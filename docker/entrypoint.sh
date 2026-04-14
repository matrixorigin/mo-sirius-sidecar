#!/usr/bin/env bash
set -euo pipefail

MO_DEBUG_HTTP="${MO_DEBUG_HTTP:-:8888}"
MO_LAUNCH_CONF="${MO_LAUNCH_CONF:-/etc/launch/launch.toml}"

export DUCKDB_HTTPSERVER_HOST="${DUCKDB_HTTPSERVER_HOST:-0.0.0.0}"
export DUCKDB_HTTPSERVER_PORT="${DUCKDB_HTTPSERVER_PORT:-9999}"
export DUCKDB_HTTPSERVER_AUTH="${DUCKDB_HTTPSERVER_AUTH:-}"
export DUCKDB_HTTPSERVER_FOREGROUND=1

# Start MO in the background.
echo "[entrypoint] Starting mo-service ..."
/mo-service -debug-http="${MO_DEBUG_HTTP}" -launch "${MO_LAUNCH_CONF}" &
MO_PID=$!

# Start DuckDB sidecar — httpserver auto-starts via env vars and blocks
# in foreground mode (DUCKDB_HTTPSERVER_FOREGROUND=1).
echo "[entrypoint] Starting DuckDB sidecar on ${DUCKDB_HTTPSERVER_HOST}:${DUCKDB_HTTPSERVER_PORT} ..."
/sidecar/duckdb < /dev/null &
SIDECAR_PID=$!

cleanup() {
    echo "[entrypoint] Shutting down ..."
    kill "$MO_PID" "$SIDECAR_PID" 2>/dev/null || true
    wait
}
trap cleanup SIGTERM SIGINT SIGQUIT

# Wait for either process to exit; if one dies, tear down the other.
wait -n "$MO_PID" "$SIDECAR_PID" || true
EXIT_CODE=$?

cleanup
exit $EXIT_CODE
