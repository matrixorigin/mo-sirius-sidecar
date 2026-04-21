#!/usr/bin/env bash
set -euo pipefail

MO_DEBUG_HTTP="${MO_DEBUG_HTTP:-:8888}"
MO_LAUNCH_CONF="${MO_LAUNCH_CONF:-/etc/launch/launch.toml}"

export SIRIUS_CONFIG_FILE="${SIRIUS_CONFIG_FILE:-/etc/sidecar/sirius.yaml}"
export DUCKDB_HTTPSERVER_HOST="${DUCKDB_HTTPSERVER_HOST:-0.0.0.0}"
export DUCKDB_HTTPSERVER_PORT="${DUCKDB_HTTPSERVER_PORT:-9999}"
export DUCKDB_HTTPSERVER_AUTH="${DUCKDB_HTTPSERVER_AUTH:-}"
export DUCKDB_HTTPSERVER_FOREGROUND=1

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
echo "[entrypoint] Starting DuckDB sidecar on ${DUCKDB_HTTPSERVER_HOST}:${DUCKDB_HTTPSERVER_PORT} ..."
/sidecar/duckdb < /dev/null &
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
echo "[entrypoint] Starting mo-service ..."
/mo-service -debug-http="${MO_DEBUG_HTTP}" -launch "${MO_LAUNCH_CONF}" &
MO_PID=$!

# Wait for either process to exit; if one dies, the EXIT trap tears down the other.
set +e
wait -n "$MO_PID" "$SIDECAR_PID"
EXIT_CODE=$?
set -e

echo "[entrypoint] A process exited (code=$EXIT_CODE), shutting down."
exit $EXIT_CODE
