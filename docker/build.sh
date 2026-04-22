#!/usr/bin/env bash
# Build the mo-sirius image with a clean snapshot of mo-tpch source.
#
# mo-tpch is consumed via a named build context populated by `git archive`,
# so only committed files are included (no data/, no .o, no built binaries).
#
# Usage:
#   ./docker/build.sh                          # ../mo-tpch, mo-sirius:latest
#   MO_TPCH_DIR=/path/to/mo-tpch ./docker/build.sh
#   IMAGE_TAG=mo-sirius:dev ./docker/build.sh
#
set -euo pipefail

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
MO_TPCH_DIR=${MO_TPCH_DIR:-${REPO_DIR}/../mo-tpch}
IMAGE_TAG=${IMAGE_TAG:-mo-sirius:latest}
ENGINE=${ENGINE:-podman}

if [[ ! -d "${MO_TPCH_DIR}/.git" ]]; then
    echo "error: ${MO_TPCH_DIR} is not a git repo" >&2
    exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "${tmpdir}"' EXIT

echo "[build] extracting mo-tpch HEAD from ${MO_TPCH_DIR} -> ${tmpdir}"
git -C "${MO_TPCH_DIR}" archive HEAD | tar -C "${tmpdir}" -x

echo "[build] ${ENGINE} build -t ${IMAGE_TAG}"
cd "${REPO_DIR}"
"${ENGINE}" build \
    --build-context mo-tpch="${tmpdir}" \
    -t "${IMAGE_TAG}" \
    -f docker/Dockerfile \
    "$@" \
    .
