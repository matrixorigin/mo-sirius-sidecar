#!/usr/bin/env bash
# Build the mo-sirius image with a clean snapshot of mo-tpch source.
#
# mo-tpch is consumed via a named build context populated by `git archive`,
# so only committed files are included (no data/, no .o, no built binaries).
#
# Usage:
#   ./docker/build.sh                          # ../mo-tpch @ mo-sirius-bench
#   MO_TPCH_DIR=/path/to/mo-tpch ./docker/build.sh
#   MO_TPCH_REF=main ./docker/build.sh         # archive a different ref
#   IMAGE_TAG=mo-sirius:dev ./docker/build.sh
#   BUILD_ENGINE=docker ./docker/build.sh
#
set -euo pipefail

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
MO_TPCH_DIR=${MO_TPCH_DIR:-${REPO_DIR}/../mo-tpch}
MO_TPCH_REF=${MO_TPCH_REF:-mo-sirius-bench}
IMAGE_TAG=${IMAGE_TAG:-mo-sirius:latest}
BUILD_ENGINE=${BUILD_ENGINE:-podman}

if [[ ! -d "${MO_TPCH_DIR}/.git" ]]; then
    echo "error: ${MO_TPCH_DIR} is not a git repo" >&2
    exit 1
fi
if ! git -C "${MO_TPCH_DIR}" rev-parse --verify --quiet "${MO_TPCH_REF}" >/dev/null; then
    echo "error: ref '${MO_TPCH_REF}' not found in ${MO_TPCH_DIR}" >&2
    exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "${tmpdir}"' EXIT

echo "[build] extracting mo-tpch ${MO_TPCH_REF} from ${MO_TPCH_DIR} -> ${tmpdir}"
git -C "${MO_TPCH_DIR}" archive "${MO_TPCH_REF}" | tar -C "${tmpdir}" -x

echo "[build] ${BUILD_ENGINE} build -t ${IMAGE_TAG}"
cd "${REPO_DIR}"
"${BUILD_ENGINE}" build \
    --build-context mo-tpch="${tmpdir}" \
    -t "${IMAGE_TAG}" \
    -f docker/Dockerfile \
    "$@" \
    .
