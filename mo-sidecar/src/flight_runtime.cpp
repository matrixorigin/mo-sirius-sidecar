// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/flight_runtime.hpp"

#include "mo_sidecar/native_result.hpp"
#include "mo_sidecar/protocol.hpp"
#include "mo_sidecar/stream_input.hpp"
#include "mo_sidecar/tae_read_resolver.hpp"

#include "execution/sirius_execution_evidence.hpp"
#include "offload/substrait_execution.hpp"

#include <arrow/flight/api.h>
#include <arrow/flight/server.h>
#include <arrow/ipc/dictionary.h>
#include <arrow/ipc/writer.h>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>
#include <grpcpp/server_builder.h>

#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace matrixone::sidecar {
namespace {

namespace flight = arrow::flight;

arrow::Status flight_error(flight::FlightStatusCode code, const std::string &message, std::string detail = {}) {
	return flight::MakeFlightError(code, message, std::move(detail));
}

arrow::Status substrait_error(const sirius::offload::substrait_execution_error &error) {
	using code = sirius::offload::substrait_error_code;
	switch (error.code()) {
	case code::UNSUPPORTED_PLAN:
		return flight_error(flight::FlightStatusCode::Failed, error.what(), "UNSUPPORTED_PLAN");
	case code::INVALID_PLAN:
		return arrow::Status::Invalid(error.what());
	case code::AUTHENTICATION_FAILED:
		return flight_error(flight::FlightStatusCode::Unauthorized, error.what(), "AUTHENTICATION_FAILED");
	case code::READ_RESOLUTION_FAILED:
		return flight_error(flight::FlightStatusCode::Unavailable, error.what(), "READ_RESOLUTION_FAILED");
	case code::CANCELLED:
		return flight_error(flight::FlightStatusCode::Cancelled, error.what(), "CANCELLED");
	case code::EXECUTION_FAILED:
		return flight_error(flight::FlightStatusCode::Internal, error.what(), "EXECUTION_FAILED");
	}
	return flight_error(flight::FlightStatusCode::Internal, "unknown Sirius execution error");
}

std::string random_ticket() {
	std::string value(32, '\0');
	if (RAND_bytes(reinterpret_cast<unsigned char *>(value.data()), value.size()) != 1) {
		throw std::runtime_error("cannot generate a Flight ticket");
	}
	return value;
}

class ticket_registry;

class execution_entry final : public std::enable_shared_from_this<execution_entry> {
  public:
	using terminal_callback = std::function<void(const std::string &)>;

	execution_entry(duckdb::DatabaseInstance &database, const runtime_config &config, execute_request request,
					std::string ticket, terminal_callback on_terminal)
		: config_(config), request_(std::move(request)), ticket_(std::move(ticket)),
		  on_terminal_(std::move(on_terminal)), connection_(std::make_unique<duckdb::Connection>(database)),
		  evidence_(std::make_shared<sirius::execution_evidence>(sirius::execution_backend::SIRIUS_GPU)) {
		// Resolution creates query-local views that binding and execution must
		// observe in one transaction. The entry owns the connection through
		// quiescence; Connection rolls this read-only transaction back on destroy.
		connection_->BeginTransaction();
		matrixone_tae_read_resolver resolver(*connection_, config_, request_.query_id, request_.account_id,
											 stream_inputs_);
		execution_ = sirius::offload::prepare_substrait(*connection_->context, request_.plan, resolver, evidence_);
		schema_ = parse_native_result_schema(request_.result_schema);
		validate_native_result_schema(schema_, execution_->schema());
	}

	~execution_entry() noexcept {
		cancel(false);
		std::lock_guard worker_lock(worker_mutex_);
		if (worker_.joinable()) {
			if (worker_.get_id() == std::this_thread::get_id()) {
				worker_.detach();
			} else {
				worker_.join();
			}
		}
	}

	const std::string &ticket() const noexcept { return ticket_; }
	const std::string &schema_wire() const noexcept { return request_.result_schema; }
	std::uint64_t deadline_unix_ms() const noexcept { return request_.deadline_unix_ms; }
	const std::string &idempotency_key() const noexcept { return request_.idempotency_key; }
	std::uint64_t max_input_batch_bytes() const noexcept { return request_.max_input_batch_bytes; }

	arrow::Result<std::shared_ptr<stream_input>> attach_input(const std::string &stream_ref) {
		auto input = stream_inputs_.find(stream_ref);
		if (!input) {
			return arrow::Status::KeyError("unknown StreamRead input reference");
		}
		const auto status = input->attach();
		if (!status.ok()) {
			return status;
		}
		stream_inputs_.handler_attached();
		return input;
	}

	void detach_input(const std::shared_ptr<stream_input> &input) noexcept {
		if (input) {
			input->detach();
			stream_inputs_.handler_detached();
		}
		condition_.notify_all();
		maybe_notify_terminal();
	}

	void fail_input(const std::string &error) noexcept {
		stream_inputs_.cancel_all(error);
		(void)cancel(false);
	}
	bool replayable() {
		std::lock_guard lock(mutex_);
		return !claimed_ && !terminal_;
	}

	bool claim() {
		std::lock_guard lock(mutex_);
		if (claimed_ || terminal_) {
			return false;
		}
		claimed_ = true;
		return true;
	}

	void start() {
		std::lock_guard lock(mutex_);
		if (!claimed_ || worker_.joinable() || terminal_) {
			return;
		}
		worker_ = std::thread([self = shared_from_this()] { self->run(); });
	}

	arrow::Status read_next(const flight::ServerCallContext &context, std::shared_ptr<arrow::Buffer> *output) {
		std::unique_lock lock(mutex_);
		const auto deadline =
			std::chrono::system_clock::time_point(std::chrono::milliseconds(request_.deadline_unix_ms));
		while (true) {
			lock.unlock();
			const bool client_cancelled = context.is_cancelled();
			lock.lock();
			if (client_cancelled && !terminal_) {
				lock.unlock();
				cancel(false);
				lock.lock();
			}
			if (frame_ || terminal_) {
				break;
			}
			const auto now = std::chrono::system_clock::now();
			if (now >= deadline) {
				lock.unlock();
				cancel(true);
				lock.lock();
				continue;
			}
			condition_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
		}
		if (frame_) {
			*output = std::exchange(frame_, nullptr);
			condition_.notify_all();
			return arrow::Status::OK();
		}
		*output = nullptr;
		return terminal_status_;
	}

	bool cancel(bool timed_out) {
		bool unstarted = false;
		{
			std::lock_guard lock(mutex_);
			if (quiesced_) {
				return false;
			}
			if (!terminal_) {
				terminal_ = true;
				terminal_status_ = timed_out ? flight_error(flight::FlightStatusCode::TimedOut,
															"sidecar execution deadline expired", "DEADLINE_EXCEEDED")
											 : flight_error(flight::FlightStatusCode::Cancelled,
															"sidecar execution was cancelled", "CANCELLED");
				frame_.reset();
			}
			unstarted = !worker_.joinable();
		}
		stream_inputs_.cancel_all(timed_out ? "sidecar execution deadline expired" : "sidecar execution was cancelled");
		execution_->cancel();
		connection_->Interrupt();
		(void)evidence_->finish(sirius::execution_outcome::CANCELLED);
		if (unstarted) {
			std::lock_guard lock(mutex_);
			quiesced_ = true;
		}
		condition_.notify_all();
		maybe_notify_terminal();
		return true;
	}

	bool cancel_and_join(bool timed_out, const std::function<bool()> &stop_waiting = {}) {
		(void)cancel(timed_out);
		const auto deadline =
			std::chrono::system_clock::time_point(std::chrono::milliseconds(request_.deadline_unix_ms));
		{
			std::unique_lock lock(mutex_);
			while (!quiesced_ || stream_inputs_.active_handlers() != 0) {
				lock.unlock();
				const bool stopped = stop_waiting && stop_waiting();
				lock.lock();
				if (stopped) {
					return false;
				}
				const auto now = std::chrono::system_clock::now();
				if (now >= deadline) {
					return false;
				}
				condition_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
			}
		}
		join();
		return true;
	}

	void join() noexcept {
		std::lock_guard worker_lock(worker_mutex_);
		if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
			worker_.join();
		}
	}

  private:
	sirius::offload::chunk_action publish(std::string payload) {
		if (payload.size() > request_.max_batch_bytes) {
			throw std::runtime_error("MO native result batch exceeds negotiated max_batch_bytes");
		}
		auto frame = arrow::Buffer::FromString(serialize_native_batch_frame(++sequence_, payload));
		std::unique_lock lock(mutex_);
		condition_.wait(lock, [&] { return !frame_ || terminal_; });
		if (terminal_) {
			return sirius::offload::chunk_action::CANCEL;
		}
		frame_ = std::move(frame);
		condition_.notify_all();
		condition_.wait(lock, [&] { return !frame_ || terminal_; });
		return terminal_ ? sirius::offload::chunk_action::CANCEL : sirius::offload::chunk_action::CONTINUE;
	}

	sirius::offload::chunk_action consume_batch(const std::shared_ptr<cucascade::data_batch> &batch,
												rmm::cuda_stream_view stream) {
		if (now_unix_ms() >= request_.deadline_unix_ms) {
			cancel(true);
			return sirius::offload::chunk_action::CANCEL;
		}
		native_result_batch_encoder encoder(batch, *connection_->context, schema_, stream);
		std::size_t offset = 0;
		while (offset < encoder.rows()) {
			std::size_t rows = encoder.rows() - offset;
			while (encoder.encoded_size(offset, rows) > request_.max_batch_bytes) {
				if (rows == 1) {
					throw std::runtime_error("one MO native result row exceeds negotiated max_batch_bytes");
				}
				rows /= 2;
			}
			auto payload = encoder.encode(offset, rows);
			if (publish(std::move(payload)) == sirius::offload::chunk_action::CANCEL) {
				return sirius::offload::chunk_action::CANCEL;
			}
			offset += rows;
		}
		return sirius::offload::chunk_action::CONTINUE;
	}

	void run() noexcept {
		arrow::Status status = arrow::Status::OK();
		sirius::execution_outcome outcome = sirius::execution_outcome::SUCCEEDED;
		try {
			execution_->run_batches([this](const auto &batch, auto stream) { return consume_batch(batch, stream); });
		} catch (const sirius::offload::substrait_execution_error &error) {
			status = substrait_error(error);
			outcome = error.code() == sirius::offload::substrait_error_code::CANCELLED
						  ? sirius::execution_outcome::CANCELLED
						  : sirius::execution_outcome::FAILED;
		} catch (const std::exception &error) {
			status = flight_error(flight::FlightStatusCode::Internal,
								  std::string("sidecar result streaming failed: ") + error.what(), "EXECUTION_FAILED");
			outcome = sirius::execution_outcome::FAILED;
		} catch (...) {
			status =
				flight_error(flight::FlightStatusCode::Internal, "sidecar result streaming failed", "EXECUTION_FAILED");
			outcome = sirius::execution_outcome::FAILED;
		}
		finish(std::move(status), outcome);
	}

	void finish(arrow::Status status, sirius::execution_outcome outcome) noexcept {
		{
			std::lock_guard lock(mutex_);
			if (!terminal_) {
				terminal_ = true;
				terminal_status_ = std::move(status);
			}
			quiesced_ = true;
		}
		if (outcome == sirius::execution_outcome::SUCCEEDED) {
			stream_inputs_.mark_all_not_needed();
		} else {
			stream_inputs_.cancel_all("sidecar execution terminated");
		}
		(void)evidence_->finish(outcome);
		condition_.notify_all();
		maybe_notify_terminal();
	}

	void maybe_notify_terminal() noexcept {
		bool notify = false;
		{
			std::lock_guard lock(mutex_);
			if (quiesced_ && stream_inputs_.active_handlers() == 0 && !terminal_notified_) {
				terminal_notified_ = true;
				notify = true;
			}
		}
		if (notify) {
			on_terminal_(ticket_);
		}
	}

	const runtime_config &config_;
	execute_request request_;
	std::string ticket_;
	terminal_callback on_terminal_;
	stream_input_registry stream_inputs_;

	// Destruction is reverse declaration order: execution (and its resolution
	// tokens) is destroyed before the connection used by token cleanup.
	std::unique_ptr<duckdb::Connection> connection_;
	std::shared_ptr<sirius::execution_evidence> evidence_;
	std::unique_ptr<sirius::offload::substrait_execution> execution_;
	native_result_schema schema_;

	std::mutex mutex_;
	std::condition_variable condition_;
	std::shared_ptr<arrow::Buffer> frame_;
	std::uint64_t sequence_ = 0;
	arrow::Status terminal_status_ = arrow::Status::OK();
	bool claimed_ = false;
	bool terminal_ = false;
	bool quiesced_ = false;
	bool terminal_notified_ = false;
	// Multiple CancelExecution handlers may observe the same entry before the
	// terminal callback removes it from the registry. std::thread::join is not
	// safe to call concurrently, so serialize the one successful join.
	std::mutex worker_mutex_;
	std::thread worker_;
};

class native_result_stream final : public flight::FlightDataStream {
  public:
	native_result_stream(std::shared_ptr<execution_entry> entry, const flight::ServerCallContext &context)
		: entry_(std::move(entry)), context_(context) {
		entry_->start();
	}

	~native_result_stream() override { (void)Close(); }

	std::shared_ptr<arrow::Schema> schema() override { return arrow::schema({}); }
	arrow::Result<flight::FlightPayload> GetSchemaPayload() override {
		flight::FlightPayload payload;
		// Flight requires a leading Arrow IPC schema even though protocol v4
		// carries no Arrow result data. Emit an empty transport schema; the exact
		// MO physical schema was already echoed in FlightInfo.schema.
		auto empty_schema = schema();
		arrow::ipc::DictionaryFieldMapper mapper(*empty_schema);
		auto status = arrow::ipc::GetSchemaPayload(*empty_schema, arrow::ipc::IpcWriteOptions::Defaults(), mapper,
												   &payload.ipc_message);
		if (!status.ok()) {
			return status;
		}
		return payload;
	}
	arrow::Result<flight::FlightPayload> Next() override {
		std::shared_ptr<arrow::Buffer> frame;
		auto status = entry_->read_next(context_, &frame);
		if (!status.ok()) {
			return status;
		}
		flight::FlightPayload payload;
		// Flight treats a null IPC metadata pointer as EOF. Carry the native
		// MOB1 frame directly as FlightData.data_header; no Arrow result body or
		// application-metadata side channel is involved.
		payload.ipc_message.type = arrow::ipc::MessageType::RECORD_BATCH;
		payload.ipc_message.metadata = std::move(frame);
		return payload;
	}
	arrow::Status Close() override {
		if (!closed_.exchange(true)) {
			entry_->cancel(false);
		}
		return arrow::Status::OK();
	}

  private:
	std::shared_ptr<execution_entry> entry_;
	const flight::ServerCallContext &context_;
	std::atomic<bool> closed_{false};
};

class ticket_registry final {
  public:
	ticket_registry(duckdb::DatabaseInstance &database, const runtime_config &config)
		: database_(database), config_(config), reaper_([this] { reap(); }) {}

	~ticket_registry() { shutdown(); }

	arrow::Result<std::shared_ptr<execution_entry>> prepare(execute_request request) {
		std::shared_ptr<execution_entry> existing;
		{
			std::unique_lock lock(mutex_);
			while (true) {
				if (stopped_) {
					return flight_error(flight::FlightStatusCode::Unavailable, "sidecar is stopping");
				}
				const auto idempotency = idempotency_.find(request.idempotency_key);
				if (idempotency == idempotency_.end()) {
					if (entries_.size() + reserved_ >= config_.max_active_tickets) {
						return flight_error(flight::FlightStatusCode::Unavailable,
											"sidecar active ticket limit reached", "RESOURCE_EXHAUSTED");
					}
					++reserved_;
					idempotency_.emplace(request.idempotency_key,
										 idempotency_record{.fingerprint = request.fingerprint,
															.ticket = {},
															.deadline_unix_ms = request.deadline_unix_ms,
															.preparing = true});
					break;
				}
				if (idempotency->second.fingerprint != request.fingerprint) {
					return flight_error(flight::FlightStatusCode::Failed,
										"idempotency key was reused for a different request", "IDEMPOTENCY_CONFLICT");
				}
				if (idempotency->second.preparing) {
					const auto deadline = std::chrono::system_clock::time_point(
						std::chrono::milliseconds(idempotency->second.deadline_unix_ms));
					if (state_changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
						return flight_error(flight::FlightStatusCode::TimedOut,
											"idempotent request did not finish preparing "
											"before its deadline",
											"IDEMPOTENCY_IN_PROGRESS");
					}
					continue;
				}
				if (idempotency->second.cancel_requested) {
					return flight_error(flight::FlightStatusCode::Failed, "idempotent request is already terminal",
										"IDEMPOTENCY_TERMINAL");
				}
				const auto found = entries_.find(idempotency->second.ticket);
				if (found == entries_.end()) {
					return flight_error(flight::FlightStatusCode::Internal,
										"idempotent request lost its prepared ticket", "IDEMPOTENCY_STATE_INVALID");
				}
				existing = found->second;
				break;
			}
		}
		if (existing) {
			if (!existing->replayable()) {
				return flight_error(flight::FlightStatusCode::Failed, "idempotent request ticket was already claimed",
									"IDEMPOTENCY_ALREADY_CLAIMED");
			}
			return existing;
		}

		std::shared_ptr<execution_entry> entry;
		const auto idempotency_key = request.idempotency_key;
		try {
			std::string ticket;
			do {
				ticket = random_ticket();
			} while (contains(ticket));
			entry = std::make_shared<execution_entry>(
				database_, config_, std::move(request), ticket,
				[this](const std::string &completed_ticket) { remove(completed_ticket); });
		} catch (...) {
			release_reservation(idempotency_key);
			throw;
		}

		bool stopping = false;
		bool cancel_requested = false;
		{
			std::lock_guard lock(mutex_);
			--reserved_;
			if (stopped_) {
				stopping = true;
				idempotency_.erase(idempotency_key);
			} else {
				entries_.emplace(entry->ticket(), entry);
				auto &idempotency = idempotency_.at(idempotency_key);
				idempotency.ticket = entry->ticket();
				idempotency.preparing = false;
				cancel_requested = idempotency.cancel_requested;
			}
		}
		state_changed_.notify_all();
		// cancel() calls the registry's terminal callback, so it must never run
		// while mutex_ is held.
		if (stopping) {
			entry->cancel(false);
			return flight_error(flight::FlightStatusCode::Unavailable, "sidecar is stopping");
		}
		if (cancel_requested) {
			(void)entry->cancel_and_join(false);
			return flight_error(flight::FlightStatusCode::Cancelled, "idempotent request was cancelled while preparing",
								"CANCELLED");
		}
		return entry;
	}

	std::shared_ptr<execution_entry> claim(const std::string &ticket) {
		std::shared_ptr<execution_entry> result;
		{
			std::lock_guard lock(mutex_);
			if (stopped_) {
				return nullptr;
			}
			const auto found = entries_.find(ticket);
			if (found == entries_.end()) {
				return nullptr;
			}
			result = found->second;
		}
		return result->claim() ? result : nullptr;
	}

	std::shared_ptr<execution_entry> lookup(const std::string &ticket) {
		std::lock_guard lock(mutex_);
		if (stopped_) {
			return nullptr;
		}
		const auto found = entries_.find(ticket);
		return found == entries_.end() ? nullptr : found->second;
	}

	enum class cancel_result : std::uint8_t { NOT_FOUND = 0, QUIESCED, DEADLINE_EXCEEDED };

	cancel_result cancel_and_join(const std::string &ticket, const std::function<bool()> &stop_waiting = {}) {
		std::shared_ptr<execution_entry> entry;
		{
			std::lock_guard lock(mutex_);
			const auto found = entries_.find(ticket);
			if (found == entries_.end()) {
				return cancel_result::NOT_FOUND;
			}
			entry = found->second;
		}
		return entry->cancel_and_join(false, stop_waiting) ? cancel_result::QUIESCED : cancel_result::DEADLINE_EXCEEDED;
	}

	cancel_result cancel_and_join_idempotency(const std::string &idempotency_key,
											  const std::function<bool()> &stop_waiting = {}) {
		std::shared_ptr<execution_entry> entry;
		{
			std::unique_lock lock(mutex_);
			while (true) {
				if (stopped_) {
					return cancel_result::NOT_FOUND;
				}
				const auto found = idempotency_.find(idempotency_key);
				if (found == idempotency_.end()) {
					return cancel_result::NOT_FOUND;
				}
				found->second.cancel_requested = true;
				if (!found->second.preparing) {
					const auto ticket = entries_.find(found->second.ticket);
					if (ticket == entries_.end()) {
						return cancel_result::NOT_FOUND;
					}
					entry = ticket->second;
					break;
				}
				const auto deadline =
					std::chrono::system_clock::time_point(std::chrono::milliseconds(found->second.deadline_unix_ms));
				lock.unlock();
				const bool stopped_waiting = stop_waiting && stop_waiting();
				lock.lock();
				if (stopped_waiting) {
					return cancel_result::DEADLINE_EXCEEDED;
				}
				const auto now = std::chrono::system_clock::now();
				if (now >= deadline) {
					return cancel_result::DEADLINE_EXCEEDED;
				}
				state_changed_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
			}
		}
		return entry->cancel_and_join(false, stop_waiting) ? cancel_result::QUIESCED : cancel_result::DEADLINE_EXCEEDED;
	}

	void stop_admission_and_cancel() noexcept {
		std::vector<std::shared_ptr<execution_entry>> entries;
		{
			std::lock_guard lock(mutex_);
			if (!stopped_) {
				stopped_ = true;
			}
			for (const auto &[_, entry] : entries_) {
				entries.push_back(entry);
			}
		}
		wake_.notify_all();
		state_changed_.notify_all();
		if (reaper_.joinable()) {
			reaper_.join();
		}
		for (const auto &entry : entries) {
			entry->cancel(false);
		}
	}

	void shutdown() noexcept {
		stop_admission_and_cancel();
		std::vector<std::shared_ptr<execution_entry>> entries;
		{
			std::lock_guard lock(mutex_);
			for (const auto &[_, entry] : entries_) {
				entries.push_back(entry);
			}
		}
		for (const auto &entry : entries) {
			entry->join();
		}
		std::lock_guard lock(mutex_);
		entries_.clear();
		idempotency_.clear();
		state_changed_.notify_all();
	}

  private:
	bool contains(const std::string &ticket) {
		std::lock_guard lock(mutex_);
		return entries_.contains(ticket);
	}

	void release_reservation(const std::string &idempotency_key) {
		std::lock_guard lock(mutex_);
		--reserved_;
		idempotency_.erase(idempotency_key);
		state_changed_.notify_all();
	}

	void remove(const std::string &ticket) {
		std::lock_guard lock(mutex_);
		const auto found = entries_.find(ticket);
		if (found != entries_.end()) {
			idempotency_.erase(found->second->idempotency_key());
			entries_.erase(found);
		}
		state_changed_.notify_all();
	}

	void reap() noexcept {
		std::unique_lock lock(mutex_);
		while (!stopped_) {
			wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopped_; });
			if (stopped_) {
				break;
			}
			const auto now = now_unix_ms();
			std::vector<std::shared_ptr<execution_entry>> expired;
			for (const auto &[_, entry] : entries_) {
				if (entry->deadline_unix_ms() <= now) {
					expired.push_back(entry);
				}
			}
			lock.unlock();
			for (const auto &entry : expired) {
				entry->cancel(true);
			}
			lock.lock();
		}
	}

	duckdb::DatabaseInstance &database_;
	const runtime_config &config_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable state_changed_;
	std::unordered_map<std::string, std::shared_ptr<execution_entry>> entries_;
	struct idempotency_record {
		std::string fingerprint;
		std::string ticket;
		std::uint64_t deadline_unix_ms = 0;
		bool preparing = false;
		bool cancel_requested = false;
	};
	std::unordered_map<std::string, idempotency_record> idempotency_;
	std::size_t reserved_ = 0;
	bool stopped_ = false;
	std::thread reaper_;
};

class input_handler_guard final {
  public:
	input_handler_guard(std::shared_ptr<execution_entry> entry, std::shared_ptr<stream_input> input)
		: entry_(std::move(entry)), input_(std::move(input)) {}
	~input_handler_guard() { entry_->detach_input(input_); }

	input_handler_guard(const input_handler_guard &) = delete;
	input_handler_guard &operator=(const input_handler_guard &) = delete;

  private:
	std::shared_ptr<execution_entry> entry_;
	std::shared_ptr<stream_input> input_;
};

class sidecar_flight_server final : public flight::FlightServerBase {
  public:
	sidecar_flight_server(duckdb::DatabaseInstance &database, const runtime_config &config)
		: config_(config), registry_(database, config) {}

	void stop_registry() noexcept { registry_.shutdown(); }
	void stop_admission() noexcept { registry_.stop_admission_and_cancel(); }

	arrow::Status GetFlightInfo(const flight::ServerCallContext &context, const flight::FlightDescriptor &descriptor,
								std::unique_ptr<flight::FlightInfo> *info) override {
		if (descriptor.type != flight::FlightDescriptor::CMD) {
			return arrow::Status::Invalid("ExecuteSubstrait requires a command descriptor");
		}
		try {
			auto request = parse_execute_request(descriptor.cmd);
			if (request.idempotency_key != execution_idempotency_key(request.account_id, request.query_id)) {
				return flight_error(flight::FlightStatusCode::Unauthorized,
									"ExecuteSubstrait idempotency key does not match its identity",
									"AUTHENTICATION_FAILED");
			}
			request.fingerprint = sha256_bytes(descriptor.cmd);
			const auto now = now_unix_ms();
			if (request.protocol_version != k_protocol_version || request.substrait_version != k_substrait_version) {
				return flight_error(flight::FlightStatusCode::Failed,
									"unsupported sidecar or Substrait protocol version", "UNSUPPORTED_VERSION");
			}
			if (request.capability_hash != capability_hash()) {
				return flight_error(flight::FlightStatusCode::Failed, "sidecar capability hash mismatch",
									"CAPABILITY_MISMATCH");
			}
			if (request.max_batch_bytes == 0 || request.max_batch_bytes > config_.max_batch_bytes) {
				return arrow::Status::Invalid("max_batch_bytes exceeds the sidecar limit");
			}
			if (request.max_input_batch_bytes == 0 || request.max_input_batch_bytes > k_max_stream_input_batch_bytes ||
				request.max_input_batch_bytes > config_.max_batch_bytes) {
				return arrow::Status::Invalid("max_input_batch_bytes exceeds the sidecar limit");
			}
			if (request.deadline_unix_ms <= now) {
				return flight_error(flight::FlightStatusCode::TimedOut, "execution deadline already expired");
			}
			const auto maximum_deadline = now + config_.ticket_ttl_ms;
			if (request.deadline_unix_ms > maximum_deadline) {
				request.deadline_unix_ms = maximum_deadline;
			}

			auto prepared = registry_.prepare(std::move(request));
			if (!prepared.ok()) {
				return prepared.status();
			}
			auto entry = std::move(prepared).ValueOrDie();
			if (context.is_cancelled()) {
				entry->cancel(false);
				return flight_error(flight::FlightStatusCode::Cancelled, "request cancelled during schema delivery");
			}

			std::vector<flight::FlightEndpoint> endpoints;
			endpoints.emplace_back(flight::Ticket(entry->ticket()), std::vector<flight::Location>{}, std::nullopt,
								   std::string{});
			flight::FlightInfo::Data data{entry->schema_wire(), descriptor, std::move(endpoints), -1, -1, false,
										  capability_hash()};
			*info = std::make_unique<flight::FlightInfo>(std::move(data));
			return arrow::Status::OK();
		} catch (const sirius::offload::substrait_execution_error &error) {
			return substrait_error(error);
		} catch (const std::invalid_argument &error) {
			return arrow::Status::Invalid(error.what());
		} catch (const std::exception &error) {
			return flight_error(flight::FlightStatusCode::Internal,
								std::string("cannot prepare sidecar execution: ") + error.what());
		}
	}

	arrow::Status DoGet(const flight::ServerCallContext &context, const flight::Ticket &request,
						std::unique_ptr<flight::FlightDataStream> *stream) override {
		auto entry = registry_.claim(request.ticket);
		if (!entry) {
			return arrow::Status::KeyError("unknown, expired, or already claimed ticket");
		}
		*stream = std::make_unique<native_result_stream>(std::move(entry), context);
		return arrow::Status::OK();
	}

	arrow::Status DoPut(const flight::ServerCallContext &context, std::unique_ptr<flight::FlightMessageReader> reader,
						std::unique_ptr<flight::FlightMetadataWriter> writer) override {
		std::shared_ptr<execution_entry> entry;
		try {
			if (!reader || !writer || reader->descriptor().type != flight::FlightDescriptor::CMD) {
				return arrow::Status::Invalid("UploadInput requires a command descriptor");
			}
			const auto request = parse_upload_input_request(reader->descriptor().cmd);
			entry = registry_.lookup(request.ticket);
			if (!entry) {
				return arrow::Status::KeyError("unknown or expired sidecar execution ticket");
			}
			auto attached = entry->attach_input(request.stream_ref);
			if (!attached.ok()) {
				return attached.status();
			}
			auto input = std::move(attached).ValueOrDie();
			input_handler_guard guard(entry, input);
			const auto stopped = [&] { return context.is_cancelled(); };
			auto attached_ack = arrow::Buffer::FromString(serialize_upload_input_ack(upload_input_ack{.ready = true}));
			const auto attached_status = writer->WriteMetadata(*attached_ack);
			if (!attached_status.ok()) {
				entry->fail_input(attached_status.ToString());
				return attached_status;
			}

			while (true) {
				auto next = reader->Next();
				if (!next.ok()) {
					entry->fail_input(next.status().ToString());
					return next.status();
				}
				auto chunk = std::move(next).ValueOrDie();
				if (!chunk.data && !chunk.app_metadata) {
					auto completed = input->finish_upload(stopped);
					if (!completed.ok()) {
						entry->fail_input(completed.status().ToString());
						return completed.status();
					}
					auto ack = arrow::Buffer::FromString(serialize_upload_input_ack(*completed));
					return writer->WriteMetadata(*ack);
				}
				if (chunk.data || !chunk.app_metadata) {
					entry->fail_input("UploadInput accepts MO-native metadata frames only");
					return arrow::Status::Invalid("UploadInput accepts MO-native metadata frames only");
				}
				if (static_cast<std::uint64_t>(chunk.app_metadata->size()) >
					entry->max_input_batch_bytes() + k_native_batch_frame_header_bytes) {
					entry->fail_input("MO native input frame exceeds the negotiated limit");
					return arrow::Status::Invalid("MO native input frame exceeds the negotiated limit");
				}
				auto consumed = input->publish(std::move(chunk.app_metadata), stopped);
				if (!consumed.ok()) {
					entry->fail_input(consumed.status().ToString());
					return consumed.status();
				}
				auto ack_value = *consumed;
				auto ack = arrow::Buffer::FromString(serialize_upload_input_ack(ack_value));
				const auto status = writer->WriteMetadata(*ack);
				if (!status.ok()) {
					entry->fail_input(status.ToString());
					return status;
				}
				if (ack_value.complete) {
					return arrow::Status::OK();
				}
			}
		} catch (const std::invalid_argument &error) {
			if (entry) {
				entry->fail_input(error.what());
			}
			return arrow::Status::Invalid(error.what());
		} catch (const std::exception &error) {
			if (entry) {
				entry->fail_input(error.what());
			}
			return flight_error(flight::FlightStatusCode::Internal,
								std::string("native input upload failed: ") + error.what());
		}
	}

	arrow::Status ListActions(const flight::ServerCallContext &, std::vector<flight::ActionType> *actions) override {
		actions->emplace_back("GetCapabilities", "Return the canonical sidecar capability document");
		actions->emplace_back("CancelExecution", "Cancel by opaque Flight ticket or request idempotency key");
		return arrow::Status::OK();
	}

	arrow::Status DoAction(const flight::ServerCallContext &context, const flight::Action &action,
						   std::unique_ptr<flight::ResultStream> *result) override {
		std::vector<flight::Result> results;
		if (action.type == "GetCapabilities") {
			if (action.body && action.body->size() != 0) {
				return arrow::Status::Invalid("GetCapabilities body must be empty");
			}
			results.emplace_back(arrow::Buffer::FromString(std::string(capability_document())));
		} else if (action.type == "CancelExecution") {
			if (!action.body) {
				return arrow::Status::Invalid("CancelExecution body is required");
			}
			cancel_request request;
			try {
				request = parse_cancel_request(
					std::string_view(reinterpret_cast<const char *>(action.body->data()), action.body->size()));
			} catch (const std::invalid_argument &error) {
				return arrow::Status::Invalid(error.what());
			}
			const auto stop_waiting = [&context] { return context.is_cancelled(); };
			const auto cancelled = request.ticket.empty()
									   ? registry_.cancel_and_join_idempotency(request.idempotency_key, stop_waiting)
									   : registry_.cancel_and_join(request.ticket, stop_waiting);
			if (cancelled == ticket_registry::cancel_result::DEADLINE_EXCEEDED) {
				return flight_error(flight::FlightStatusCode::TimedOut,
									"sidecar execution did not quiesce before its deadline", "CANCEL_NOT_QUIESCED");
			}
			results.emplace_back(arrow::Buffer::FromString(
				cancelled == ticket_registry::cancel_result::QUIESCED ? "quiesced" : "not-found"));
		} else {
			return arrow::Status::NotImplemented("unknown sidecar action: ", action.type);
		}
		*result = std::make_unique<flight::SimpleResultStream>(std::move(results));
		return arrow::Status::OK();
	}

  private:
	const runtime_config &config_;
	ticket_registry registry_;
};

} // namespace

class flight_runtime::impl {
  public:
	impl(duckdb::DatabaseInstance &database, runtime_config config)
		: config(std::move(config)), server(database, this->config) {}

	runtime_config config;
	sidecar_flight_server server;
	std::thread server_thread;
	std::mutex stop_mutex;
	std::atomic<bool> started{false};
};

flight_runtime::flight_runtime(duckdb::DatabaseInstance &database, runtime_config config)
	: impl_(std::make_unique<impl>(database, std::move(config))) {}

flight_runtime::~flight_runtime() noexcept { stop(); }

void flight_runtime::start() {
	// Arrow's conda jemalloc backend conflicts with DuckDB/Sirius allocator
	// ownership in this process. Install the process-wide Arrow backend before
	// any Flight worker can initialize the default pool.
	if (::setenv("ARROW_DEFAULT_MEMORY_POOL", "system", 1) != 0) {
		throw std::runtime_error("cannot configure Arrow system memory pool");
	}
	auto location = flight::Location::ForGrpcTls(impl_->config.flight_host, impl_->config.flight_port);
	if (!location.ok()) {
		throw std::runtime_error(location.status().ToString());
	}
	flight::FlightServerOptions options(std::move(location).ValueOrDie());
	options.tls_certificates.push_back(
		{read_secret_file(impl_->config.flight_cert_path), read_secret_file(impl_->config.flight_key_path)});
	options.verify_client = true;
	options.root_certificates = read_secret_file(impl_->config.flight_client_ca_path);
	const auto maximum_receive = static_cast<int>(k_max_plan_bytes + 1024U * 1024U);
	const auto maximum_send = static_cast<int>(impl_->config.max_batch_bytes + 1024U * 1024U);
	options.builder_hook = [maximum_receive, maximum_send](void *raw_builder) {
		auto *builder = static_cast<grpc::ServerBuilder *>(raw_builder);
		builder->SetMaxReceiveMessageSize(maximum_receive);
		builder->SetMaxSendMessageSize(maximum_send);
	};
	const auto initialized = impl_->server.Init(options);
	if (!initialized.ok()) {
		throw std::runtime_error(initialized.ToString());
	}
	impl_->started.store(true);
	impl_->server_thread = std::thread([this] {
		const auto status = impl_->server.Serve();
		(void)status;
		impl_->started.store(false);
	});
}

void flight_runtime::stop() noexcept {
	if (!impl_) {
		return;
	}
	std::lock_guard stop_lock(impl_->stop_mutex);
	if (impl_->started.exchange(false)) {
		impl_->server.stop_admission();
		const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(1);
		const auto status = impl_->server.Shutdown(&deadline);
		(void)status;
	}
	if (impl_->server_thread.joinable()) {
		impl_->server_thread.join();
	}
	// No Flight handler can create or claim an entry after Shutdown/Serve join.
	// The registry can now cancel and join every execution worker it owns.
	impl_->server.stop_registry();
}

} // namespace matrixone::sidecar
