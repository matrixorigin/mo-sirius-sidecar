# Combined MO + DuckDB Sidecar (GPU) Docker image.
#
# All stages use Debian Trixie for consistency.
# MO is built without CUDA (standard CPU build).
# The sidecar is built with CUDA 13 via pixi (tae_scanner + httpserver + sirius).
#
# Build:
#   docker build -t mo-sirius:latest -f docker/Dockerfile \
#     --build-arg MO_REPO=https://github.com/matrixorigin/matrixone.git \
#     --build-arg MO_BRANCH=main \
#     .
#
# Run:
#   docker run --gpus all -p 6001:6001 -p 8888:8888 -p 9999:9999 mo-sirius:latest

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
# Stage 3: Runtime image
# ---------------------------------------------------------------------------
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libnuma1 libgomp1 tini && \
    rm -rf /var/lib/apt/lists/*

# MO binary
COPY --from=mo-builder /mo/mo-service /mo-service
COPY --from=mo-builder /mo/thirdparties/install/lib/*.so /usr/local/lib/

# MO config files (maintained in this repo, includes sidecarUrl).
COPY docker/etc/launch.toml /etc/launch/launch.toml
COPY docker/etc/cn.toml     /etc/launch/cn.toml
COPY docker/etc/tn.toml     /etc/launch/tn.toml
COPY docker/etc/log.toml    /etc/launch/log.toml

# Sidecar binary + pixi runtime libs
COPY --from=sidecar-builder /sidecar/build/release-gpu/duckdb /sidecar/duckdb
COPY --from=sidecar-builder /sidecar-runtime-libs/ /usr/local/lib/

RUN ldconfig

WORKDIR /

# MO ports: 6001 (SQL), 8888 (debug HTTP)
# Sidecar ports: 9999 (DuckDB HTTP API)
EXPOSE 6001 8888 9999

COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
