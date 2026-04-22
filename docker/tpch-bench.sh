#!/usr/bin/env bash
# Run TPC-H benchmark inside the mo-sirius image.
#
# Phases (each can be skipped via env var = 0):
#   GEN   — dbgen → ./data/${SF}/*.tbl   (skip with GEN=0)
#   CTAB  — create tpch_${SF}g schema    (skip with CTAB=0)
#   LOAD  — LOAD DATA INFILE all tables  (skip with LOAD=0)
#   QUERY — run all 22 queries           (skip with QUERY=0)
#
# Engine selection (which executor MO uses for queries):
#   ENGINE=native — MO native execution, no hint   (default)
#   ENGINE=cpu    — prepend /*+ SIDECAR */ to each query
#   ENGINE=gpu    — prepend /*+ SIDECAR GPU */ to each query
#
# Usage:
#   tpch-bench [SF]                        # default SF=1
#   SF=10 tpch-bench
#   ENGINE=gpu tpch-bench 10               # all 22 queries on GPU sidecar
#   GEN=0 LOAD=0 ENGINE=gpu tpch-bench 10  # reuse data, queries only
#
# Env overrides:
#   MO_HOST (127.0.0.1)  MO_PORT (6001)
#   MO_USER (dump)       MO_PASS (111)
#   DATA_DIR (/opt/mo-tpch/data/${SF})   # bind-mount for persistence at SF >= 10
#
# Notes:
# - dbgen writes *.tbl into its CWD. We run it from a writable scratch dir
#   and pass -b for the dists.dss path, then move outputs to ${DATA_DIR}.
# - The /opt/mo-tpch directory is part of the image layer; for large scale
#   factors mount a host volume at /opt/mo-tpch/data to persist .tbl files.

set -euo pipefail

SF="${1:-${SF:-1}}"
MO_HOST="${MO_HOST:-127.0.0.1}"
MO_PORT="${MO_PORT:-6001}"
MO_USER="${MO_USER:-dump}"
MO_PASS="${MO_PASS:-111}"
GEN="${GEN:-1}"
CTAB="${CTAB:-1}"
LOAD="${LOAD:-1}"
QUERY="${QUERY:-1}"
ENGINE="${ENGINE:-native}"

WORKSPACE=/opt/mo-tpch
DATA_DIR="${DATA_DIR:-${WORKSPACE}/data/${SF}}"

case "${ENGINE}" in
    native) HINT="" ;;
    cpu)    HINT="/*+ SIDECAR */" ;;
    gpu)    HINT="/*+ SIDECAR GPU */" ;;
    *)      echo "[tpch-bench] ERROR: ENGINE must be native|cpu|gpu (got '${ENGINE}')" >&2; exit 2 ;;
esac
export HINT

cd "${WORKSPACE}"

if [[ "${GEN}" == "1" ]]; then
    echo "[tpch-bench] GEN  SF=${SF} -> ${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    # dbgen writes to CWD; run it in DATA_DIR with -b pointing at the dist file
    cd "${DATA_DIR}"
    "${WORKSPACE}/dbgen/dbgen" -b "${WORKSPACE}/dbgen/dists.dss" -s "${SF}" -f
    cd "${WORKSPACE}"
fi

# Use mo-tpch's run.sh for CTAB / LOAD / QUERY since it already handles
# ${SCALE} and [SF] placeholder substitution and the right LOAD INFILE format.
RUN_ARGS=(-h "${MO_HOST}" -P "${MO_PORT}" -u "${MO_USER}" -p "${MO_PASS}" -s "${SF}")

if [[ "${CTAB}" == "1" ]]; then
    echo "[tpch-bench] CTAB SF=${SF}"
    bash run.sh -c "${RUN_ARGS[@]}"
fi

if [[ "${LOAD}" == "1" ]]; then
    echo "[tpch-bench] LOAD SF=${SF} from ${DATA_DIR}"
    bash run.sh -l -f "${DATA_DIR}" "${RUN_ARGS[@]}"
fi

if [[ "${QUERY}" == "1" ]]; then
    echo "[tpch-bench] QUERY all SF=${SF} ENGINE=${ENGINE}"
    bash run.sh -q all "${RUN_ARGS[@]}"
fi

echo "[tpch-bench] done"
