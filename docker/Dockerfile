# Combined MO + DuckDB Sidecar (GPU) + mo-tpch Docker image.
#
# All stages use Debian Trixie for consistency.
# MO is built without CUDA (standard CPU build).
# The sidecar is built with CUDA 13 via pixi (tae_scanner + httpserver + sirius).
# mo-tpch is shipped with a pre-built dbgen at /opt/mo-tpch.
#
# Build:
#   ./docker/build.sh                         # uses ../mo-tpch by default
#   MO_TPCH_DIR=/path/to/mo-tpch ./docker/build.sh
#
# The script runs `git archive` against MO_TPCH_DIR to feed mo-tpch source
# into the build via a named build context. Raw `podman build` works too:
#   tmpdir=$(mktemp -d) && \
#   git -C ../mo-tpch archive HEAD | tar -C $tmpdir -x && \
#   podman build --build-context mo-tpch=$tmpdir \
#     -t mo-sirius:latest -f docker/Dockerfile . && \
#   rm -rf $tmpdir
#
# Run:
#   docker run --gpus all -p 6001:6001 -p 8888:8888 -p 9999:9999 mo-sirius:latest
#
# Run TPC-H benchmark inside the container:
#   docker exec -it <container> tpch-bench 1            # SF=1, all phases
#   docker exec -it <container> env GEN=0 tpch-bench 10 # SF=10, skip dbgen

# ---------------------------------------------------------------------------
# Stage 1: Build MO (CPU only)
# ---------------------------------------------------------------------------
FROM golang:trixie AS mo-builder

ARG GOPROXY="https://proxy.golang.org,direct"
RUN go env -w GOPROXY=${GOPROXY}

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake && \
    rm -rf /var/lib/apt/lists/*

ARG MO_REPO=https://github.com/matrixorigin/matrixone.git
ARG MO_BRANCH=main

RUN git clone --depth 1 -b ${MO_BRANCH} ${MO_REPO} /mo
WORKDIR /mo
RUN go mod download && make clean && make build && strip mo-service

# ---------------------------------------------------------------------------
# Stage 2: Build sidecar (GPU via pixi)
# ---------------------------------------------------------------------------
FROM debian:trixie AS sidecar-builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates curl libnuma-dev binutils && \
    rm -rf /var/lib/apt/lists/*

# Install pixi
RUN curl -fsSL https://pixi.sh/install.sh | PIXI_VERSION=v0.59.0 bash
ENV PATH="/root/.pixi/bin:${PATH}"

WORKDIR /sidecar
COPY . .

# Let pixi resolve and install all CUDA/RAPIDS/compiler dependencies,
# then build the sidecar with the GPU extension config.
RUN cd sirius && pixi install

RUN cd sirius && pixi run -- bash -c '\
    cmake -S ../duckdb -B ../build/release-gpu -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DEXTENSION_STATIC_BUILD=1 \
        -DCMAKE_CXX_COMPILER=${CONDA_PREFIX}/bin/clang++ \
        -DCMAKE_C_COMPILER=${CONDA_PREFIX}/bin/clang \
        -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
        -DCMAKE_C_COMPILER_LAUNCHER=sccache \
        -DCMAKE_LINKER_TYPE=MOLD \
        -DDUCKDB_EXTENSION_CONFIGS=$(pwd)/../extension_config_gpu.cmake && \
    ninja -C ../build/release-gpu'

# Strip sidecar binary and large shared libs.
RUN strip /sidecar/build/release-gpu/duckdb && \
    find /sidecar/build/release-gpu -name '*.so' -exec strip --strip-unneeded {} + 2>/dev/null; true

# Collect only the pixi-provided runtime libraries needed by the sidecar binary.
COPY docker/collect-runtime-libs.sh /sidecar/collect-runtime-libs.sh
RUN cd sirius && pixi run -- bash /sidecar/collect-runtime-libs.sh \
    /sidecar/build/release-gpu/duckdb /sidecar-runtime-libs

# ---------------------------------------------------------------------------
# Stage 3: Build mo-tpch dbgen
# ---------------------------------------------------------------------------
# Source comes from a named build context "mo-tpch" populated by
# docker/build.sh via `git archive` (so only committed files, no data/).
FROM debian:trixie-slim AS mo-tpch-builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates build-essential && \
    rm -rf /var/lib/apt/lists/*

COPY --from=mo-tpch . /mo-tpch
WORKDIR /mo-tpch/dbgen
RUN make && rm -f *.o

# ---------------------------------------------------------------------------
# Stage 4: Runtime image
# ---------------------------------------------------------------------------
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgomp1 curl tini mariadb-client-core && \
    rm -rf /var/lib/apt/lists/*

# MO binary
COPY --from=mo-builder /mo/mo-service /mo-service
COPY --from=mo-builder /mo/thirdparties/install/lib/*.so /usr/local/lib/

# MO config files (maintained in this repo, includes sidecarUrl).
COPY docker/etc/launch.toml /etc/launch/launch.toml
COPY docker/etc/cn.toml     /etc/launch/cn.toml
COPY docker/etc/tn.toml     /etc/launch/tn.toml
COPY docker/etc/log.toml    /etc/launch/log.toml

# Sidecar binary + pixi runtime libs + Sirius config
COPY --from=sidecar-builder /sidecar/build/release-gpu/duckdb /sidecar/duckdb
COPY --from=sidecar-builder /sidecar-runtime-libs/ /usr/local/lib/
COPY sirius.yaml.example /etc/sidecar/sirius.yaml

# mo-tpch (dbgen, queries, schema, golden answers, run.sh)
COPY --from=mo-tpch-builder /mo-tpch /opt/mo-tpch

RUN ldconfig

WORKDIR /

# MO ports: 6001 (SQL), 8888 (debug HTTP)
# Sidecar ports: 9999 (DuckDB HTTP API)
EXPOSE 6001 8888 9999

COPY docker/entrypoint.sh /entrypoint.sh
COPY docker/tpch-bench.sh /usr/local/bin/tpch-bench
RUN chmod +x /entrypoint.sh /usr/local/bin/tpch-bench

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
