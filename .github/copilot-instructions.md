# Copilot instructions

## Build, test, and lint commands

- Initialize submodules in a fresh clone or worktree before doing anything else:
  ```bash
  git submodule update --init --recursive
  ```
- CPU sidecar build from the repo root:
  ```bash
  cmake -S duckdb -B build/release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDUCKDB_EXTENSION_CONFIGS="$(pwd)/extension_config.cmake"
  ninja -C build/release
  ```
- GPU sidecar build adds `sirius` on top of the CPU extensions. Build it from inside the Sirius pixi environment so CUDA/cuDF/OpenSSL/lz4 resolve correctly:
  ```bash
  git -C sirius submodule update --init cucascade
  cd sirius && pixi install && cd ..
  SIDECAR_DIR=$(pwd)
  cd sirius && pixi run -- bash -c "
    cmake -S $SIDECAR_DIR/duckdb -B $SIDECAR_DIR/build/release-gpu -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DDUCKDB_EXTENSION_CONFIGS=$SIDECAR_DIR/extension_config_gpu.cmake
  "
  cd sirius && pixi run -- bash -c "ninja -C $SIDECAR_DIR/build/release-gpu"
  ```
- Sidecar smoke run:
  ```bash
  DUCKDB_HTTPSERVER_PORT=9876 ./build/release/duckdb
  curl 'http://localhost:9876/?default_format=JSONCompact&query=SELECT+42'
  ```

### Area-specific tests

- `tae-scanner/` uses Catch2 and requires generated fixtures:
  ```bash
  cd tae-scanner
  python3 test/gen_test_data.py
  make test
  ./build/release/test/tae_tests "[filter]"
  ```
- `httpserver/` has DuckDB SQLLogicTests plus Python HTTP end-to-end tests:
  ```bash
  cd httpserver
  make test
  pip install -r requirements.txt
  pytest test_http_api
  ```
- `sirius/` uses pixi + CMake presets. `make test` only prints a reminder because the SQLLogicTest path still exercises legacy `gpu_processing`; the main validation path is the C++ unit test binary:
  ```bash
  cd sirius
  CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) make
  build/release/extension/sirius/test/cpp/sirius_unittest
  build/release/extension/sirius/test/cpp/sirius_unittest "[cpu_cache]"
  build/release/extension/sirius/test/cpp/sirius_unittest "test_cpu_cache_basic_string_single_col"
  build/release/test/unittest --test-dir . test/sql/tpch-sirius.test
  ```

### Lint / formatting

- `tae-scanner/` follows the DuckDB extension template targets:
  ```bash
  cd tae-scanner
  make format-check
  make format
  make tidy-check
  ```
- `sirius/` uses pre-commit rather than the DuckDB formatter wrapper:
  ```bash
  cd sirius
  pixi run pre-commit run -a
  ```

## High-level architecture

- The repo root is mostly an integration layer over pinned submodules, not the main implementation surface. `.gitmodules` pins:
  - `duckdb/`
  - `extension-ci-tools/`
  - `tae-scanner/`
  - `httpserver/`
  - `sirius/`
- `extension_config.cmake` is the CPU composition point: it statically links `tae_scanner` and `httpserver` into one DuckDB build. `extension_config_gpu.cmake` includes that CPU config and then adds `sirius`, so the GPU build extends the CPU sidecar instead of defining a separate stack.
- Runtime query flow spans the docs and submodules:
  1. MatrixOne sees `/*+ SIDECAR */` or `/*+ SIDECAR GPU */`.
  2. MatrixOne rewrites table references to `tae_scan(manifest_url)`.
  3. GPU queries are wrapped in `gpu_execution()`.
  4. MatrixOne POSTs the rewritten SQL to the sidecar's `httpserver`.
  5. `httpserver` returns ClickHouse-style `JSONCompact`, which MatrixOne translates back to a MySQL result set.
- `tae-scanner` is the data access layer. It reads MatrixOne TAE objects directly from manifest metadata, performs bind/init/execute/statistics as a DuckDB table function, and handles projection pruning, zone-map filtering, object pruning, and direct `pread()` + LZ4 decode into DuckDB vectors.
- `sirius` is additive to that path rather than separate from it: it consumes DuckDB plans/chunks through `gpu_execution()`. That means scanner output and sidecar wiring should stay DuckDB-native even when the change is motivated by GPU execution.

## Key conventions

- Treat the root repo as wiring plus documentation. Most behavioral changes belong in the submodules (`tae-scanner/`, `httpserver/`, `sirius/`); root changes usually mean updating `extension_config*.cmake`, build wiring, or integration docs.
- Preserve the extension layering:
  - CPU sidecar = `tae_scanner` + `httpserver`
  - GPU sidecar = CPU sidecar + `sirius`
- Submodule state matters. Fresh worktrees do not auto-initialize submodules, and Sirius has an extra required nested submodule (`sirius/cucascade`) for GPU builds.
- `httpserver` behavior is part of the MatrixOne contract here: the sidecar is expected to expose `httpserve_start(...)`, support `X-API-Key` / Basic auth, and return `JSONCompact` responses. Avoid changing response format or auth/header behavior casually.
- `sirius/` development should target Super Sirius (`gpu_execution`) rather than the legacy `gpu_processing` path. The existing SQLLogicTest flow still reflects the legacy path, which is why `sirius/Makefile` points developers to the C++ unit test binary instead.
- `tae-scanner/` source and tests are organized by scan pipeline responsibilities (`tae_object_reader`, `tae_filter`, `tae_column_fill`, `tae_scanner`). Keep changes and validation aligned with that split; scanner feature work is usually paired with the matching Catch2 file and tag.
