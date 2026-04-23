#!/usr/bin/env bash
set -euo pipefail

MO_DEBUG_HTTP="${MO_DEBUG_HTTP:-:8888}"
MO_LAUNCH_CONF="${MO_LAUNCH_CONF:-/etc/launch/launch.toml}"

export SIRIUS_CONFIG_FILE="${SIRIUS_CONFIG_FILE:-/etc/sidecar/sirius.yaml}"
export DUCKDB_HTTPSERVER_HOST="${DUCKDB_HTTPSERVER_HOST:-0.0.0.0}"
export DUCKDB_HTTPSERVER_PORT="${DUCKDB_HTTPSERVER_PORT:-9999}"
export DUCKDB_HTTPSERVER_AUTH="${DUCKDB_HTTPSERVER_AUTH:-}"
export DUCKDB_HTTPSERVER_FOREGROUND=1

# MO and the sidecar are both very chatty (debug-level by default). When the
# container's log driver is journald (podman/docker default) every line ends
# up in the host's syslog, which is unpleasant for benchmarking sessions.
# Redirect their stdout/stderr to files under /log so the container's own
# stdout (captured by the log driver) only carries [entrypoint] lifecycle
# messages. Bind-mount /log if you want the files on the host.
LOG_DIR="${LOG_DIR:-/log}"
LOG_TS="$(date +%Y%m%d-%H%M%S)"
mkdir -p "${LOG_DIR}/tpch/${LOG_TS}"
SIDECAR_LOG="${LOG_DIR}/sidecar-${LOG_TS}.log"
MO_LOG="${LOG_DIR}/mo-${LOG_TS}.log"

# Point mo-tpch's report dir + run.log at this run's timestamped subdir so
# repeated container starts don't clobber each other's TPC-H output. mo-tpch's
# run.sh writes to ${WORKSPACE}/run.log and ${WORKSPACE}/report/<CASE>/ via
# two independent paths, hence two symlinks landing in the same dir; the
# touch makes the run.log symlink live from the start (tee -a / >> would
# create on first write anyway, but a dead symlink confuses ls/tail).
touch "${LOG_DIR}/tpch/${LOG_TS}/run.log"
ln -sfn "${LOG_DIR}/tpch/${LOG_TS}"          /opt/mo-tpch/report
ln -sfn "${LOG_DIR}/tpch/${LOG_TS}/run.log"  /opt/mo-tpch/run.log

MO_PID=""
SIDECAR_PID=""

cleanup() {
    echo "[entrypoint] Shutting down ..."
    # Both MO and the sidecar httpserver use SIGINT for graceful shutdown.
    [ -n "$MO_PID" ]      && kill -INT "$MO_PID"      2>/dev/null || true
    [ -n "$SIDECAR_PID" ] && kill -INT "$SIDECAR_PID"  2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# --- Start sidecar first and wait for it to be ready ---
echo "[entrypoint] Starting DuckDB sidecar on ${DUCKDB_HTTPSERVER_HOST}:${DUCKDB_HTTPSERVER_PORT} (logs: ${SIDECAR_LOG}) ..."
/sidecar/duckdb < /dev/null >>"${SIDECAR_LOG}" 2>&1 &
SIDECAR_PID=$!

SIDECAR_URL="http://127.0.0.1:${DUCKDB_HTTPSERVER_PORT}"
echo "[entrypoint] Waiting for sidecar to be ready ..."
for i in $(seq 1 30); do
    if curl -sf --noproxy '*' "${SIDECAR_URL}/ping" >/dev/null 2>&1; then
        echo "[entrypoint] Sidecar ready."
        break
    fi
    if ! kill -0 "$SIDECAR_PID" 2>/dev/null; then
        echo "[entrypoint] ERROR: Sidecar process exited before becoming ready."
        exit 1
    fi
    sleep 1
done

if ! curl -sf --noproxy '*' "${SIDECAR_URL}/ping" >/dev/null 2>&1; then
    echo "[entrypoint] ERROR: Sidecar did not become ready within 30 seconds."
    exit 1
fi

# --- Start MO ---
echo "[entrypoint] Starting mo-service (logs: ${MO_LOG}) ..."
/mo-service -debug-http="${MO_DEBUG_HTTP}" -launch "${MO_LAUNCH_CONF}" >>"${MO_LOG}" 2>&1 &
MO_PID=$!

# Wait for either process to exit; if one dies, the EXIT trap tears down the other.
set +e
wait -n "$MO_PID" "$SIDECAR_PID"
EXIT_CODE=$?
set -e

echo "[entrypoint] A process exited (code=$EXIT_CODE), shutting down."
exit $EXIT_CODE
