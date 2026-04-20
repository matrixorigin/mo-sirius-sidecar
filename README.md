# mo-sirius-sidecar

DuckDB-based query sidecar for MatrixOne, powered by the
[Sirius](https://github.com/sirius-db/sirius) GPU execution engine.
[MatrixOne](https://github.com/matrixorigin/matrixone) rewrites and forwards queries annotated with `/*+ SIDECAR */` (DuckDB on CPU) or `/*+ SIDECAR GPU */` (SiriusDB on GPU) to this sidecar to take advantage of GPU for analytic query processing.  

MatrixOne distinguishes itself from other SiriusDB integrations through its fundamental architecture as a Hybrid Transactional/Analytical Processing (HTAP) system. This dual-capability framework allows MatrixOne to consistently maintain high-throughput transactional performance, executing tens of thousands of transactions per second on modest CPU configurations. By integrating with SiriusDB, the system facilitates the offloading of complex analytical workloads to high-performance GPUs. This synergy enables the processing of near-instantaneous, transaction-consistent data, effectively bridging the gap between real-time operational state and deep computational analysis without the latency typically associated with traditional data movement.

## Execution Paths

| Path | Hint | Engine | Scan Pipeline |
|------|------|--------|---------------|
| **CPU** | `/*+ SIDECAR */` | DuckDB vectorized | `tae_scan()` → pread → LZ4 (CPU) → DuckDB vectors |
| **GPU** | `/*+ SIDECAR GPU */` | Sirius + cuDF | `tae_scan_task` → pread → pinned host → cudaMemcpy → nvCOMP LZ4 (GPU) → CUDA decode → cudf tables |

The GPU path bypasses DuckDB execution engine entirely — compressed TAE data goes directly from
disk to GPU memory, with decompression and column decoding performed by CUDA kernels.
Filter predicates are pushed down and evaluated on GPU via `cudf::compute_column()`.
See [DESIGN.md §13](DESIGN.md#13-gpu-native-tae-scan-sirius) for full architecture.

## Extensions

| Extension | Source | Description |
|-----------|--------|-------------|
| **tae-scanner** | [duckdb-tae-scanner](https://github.com/matrixorigin/duckdb-tae-scanner) | Reads MatrixOne TAE storage objects as DuckDB table functions |
| **httpserver** | [duckdb-httpserver](https://github.com/matrixorigin/duckdb-httpserver) | DuckDB HTTP server for accepting SQL queries |
| **sirius** | [sirius](https://github.com/matrixorigin/sirius) | GPU-accelerated SQL execution via cuCascade/cuDF |

Extensions are statically linked into the DuckDB binary — no manual `LOAD` needed.
The GPU build adds Sirius on top of the base extensions.

## Prerequisites

CMake ≥ 3.15, Ninja, Clang (recommended) or GCC ≥ 11, plus lz4 and OpenSSL dev
libraries.

**Debian / Ubuntu:**
```bash
sudo apt install clang cmake ninja-build liblz4-dev libssl-dev git libcurl4-openssl-dev
```

**Fedora / RHEL / Rocky:**
```bash
sudo dnf install clang cmake ninja-build lz4-devel openssl-devel git
```

**Arch Linux:**
```bash
sudo pacman -S clang cmake ninja lz4 openssl git
```

## Build

```bash
git clone --recurse-submodules https://github.com/matrixorigin/mo-sirius-sidecar.git
cd mo-sirius-sidecar

# For GPU builds, also init sirius's internal deps:
git -C sirius submodule update --init cucascade

# Configure (first time only)
cmake -S duckdb -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$(pwd)/extension_config.cmake"

# Build
ninja -C build/release
```

Artifacts:
- `build/release/duckdb` — DuckDB shell with all extensions linked
- `build/release/extension/tae_scanner/tae_scanner.duckdb_extension` — loadable
- `build/release/extension/httpserver/httpserver.duckdb_extension` — loadable
- `build/release/extension/sirius/sirius.duckdb_extension` — loadable (GPU build only)

### GPU build (requires CUDA)

The Sirius extension uses [pixi](https://pixi.sh) to manage CUDA toolkit,
cuDF, and other RAPIDS dependencies.  Install pixi first, then initialize
the Sirius conda environment:

```bash
# Install pixi (one-time)
curl -fsSL https://pixi.sh/install.sh | bash

# Initialize Sirius submodule deps (cucascade is required at build time)
git -C sirius submodule update --init cucascade

# Install CUDA + RAPIDS toolchain into sirius/.pixi/
cd sirius && pixi install && cd ..
```

Build from within the pixi environment so the compiler can find CUDA, cuDF,
lz4, and OpenSSL:

```bash
SIDECAR_DIR=$(pwd)

# Configure (first time only)
cd sirius && pixi run -- bash -c "
  cmake -S $SIDECAR_DIR/duckdb -B $SIDECAR_DIR/build/release-gpu -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDUCKDB_EXTENSION_CONFIGS=$SIDECAR_DIR/extension_config_gpu.cmake
" && cd ..

# Build
cd sirius && pixi run -- bash -c "
  ninja -C $SIDECAR_DIR/build/release-gpu
" && cd ..
```

> **Note:** The pixi compiler wrapper (conda-forge GCC) does not see system
> `/usr/include`, so system libraries like lz4 and OpenSSL are declared as
> pixi dependencies in `sirius/pixi.toml`.

> **Note:** On machines without an NVIDIA GPU the build succeeds but the
> binary will print "NVML not available" and refuse GPU queries at runtime.

This adds the Sirius GPU execution engine on top of tae_scanner + httpserver.

## Deploy

### CPU sidecar

```bash
DUCKDB_HTTPSERVER_FOREGROUND=1 \
DUCKDB_HTTPSERVER_PORT=9876 \
  ./build/release/duckdb
```

The HTTP server auto-starts on the specified port.
`DUCKDB_HTTPSERVER_FOREGROUND=1` keeps the process running as a daemon (blocks
after startup instead of dropping to the interactive shell).

### GPU sidecar

The GPU build **must** run inside the pixi environment so that CUDA and cuDF
runtime libraries are on `LD_LIBRARY_PATH`:

```bash
cd sirius && pixi run -- bash -c "
  cd .. && \
  DUCKDB_HTTPSERVER_FOREGROUND=1 \
  DUCKDB_HTTPSERVER_PORT=9876 \
  SIRIUS_LOG_LEVEL=info \
    ./build/release-gpu/duckdb
"
```

Set `SIRIUS_LOG_LEVEL=debug` for verbose GPU execution logs (very noisy).

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DUCKDB_HTTPSERVER_PORT` | *(none)* | Set to auto-start HTTP server on this port |
| `DUCKDB_HTTPSERVER_HOST` | `0.0.0.0` | Listen address |
| `DUCKDB_HTTPSERVER_AUTH` | *(empty)* | Auth token (X-API-Key or Basic auth) |
| `DUCKDB_HTTPSERVER_FOREGROUND` | `0` | Set to `1` to block after startup (daemon mode) |
| `SIRIUS_LOG_LEVEL` | `warn` | Sirius GPU engine log level (`info`, `debug`, `trace`) |

### Manual start (interactive)

```bash
./build/release/duckdb \
  -cmd "SELECT httpserve_start('0.0.0.0', 9876, '')"
```

### Verify

```bash
curl 'http://localhost:9876/?default_format=JSONCompact&query=SELECT+42'
# Expected: {"meta":[{"name":"42","type":"Int32"}],"data":[[42]],"rows":1}
```

## MatrixOne integration


### 1. Start the sidecar

Start the CPU or GPU sidecar on port 9876 (see [Deploy](#deploy) above).

### 2. Start MatrixOne

MatrixOne has integrated SiriusDB sidecar.  At this moment, MO must be started with 
the `-debug-http` flag — this enables the internal
`/debug/tae/manifest` endpoint that the sidecar uses to discover TAE objects:

```bash
cd /path/to/matrixone
./mo-service -debug-http :8888 -launch etc/launch/launch.toml
```

### 3. Configure the sidecar URL

Add to `cn.toml`:

```toml
[cn.frontend]
sidecarUrl = "http://localhost:9876"
```

Or set it per-session (useful for testing):

```sql
SET sidecar_url = 'http://localhost:9876';
```

### 4. Run queries

TPCH dataset can be loaded from [mo-tpch](https://github.com/matrixorigin/mo-tpch).

```sql
-- CPU sidecar (DuckDB vectorized engine):
/*+ SIDECAR */ SELECT count(*) FROM tpch.lineitem WHERE l_shipdate < '1998-09-01';

-- GPU sidecar (Sirius + cuDF, wraps query in gpu_execution()):
/*+ SIDECAR GPU */ SELECT count(*) FROM tpch.lineitem WHERE l_shipdate < '1998-09-01';
```

If the sidecar is not configured or not reachable, MO silently falls back to
native execution (the hint is stripped).

NOTE: MatrixOne use a mysql compatible client protocol.  User can use any mysql client
to connect to MatrixOne and run queries.  If using the `mariadb` client, user need to 
use `--comments` so that SQL hints are preserved:

```bash
mariadb --skip-ssl -h 127.0.0.1 -P 6001 -u dump -p111 --comments
```

### Known issues

- **MO HTTP timeout:** MO's `fileservice` package overrides `http.DefaultTransport`
  with a 20-second `ResponseHeaderTimeout`. The sidecar HTTP client in MO must use
  a dedicated `http.Transport` to avoid this — see `pkg/frontend/sidecar_offload.go`.
- **GPU VRAM limits:** Multi-table joins at SF100+ may hang if the GPU has
  insufficient VRAM (tested: RTX 3070 8GB handles SF10 fully, SF100 Q1-Q2 only).
- **GPU scan overhead at small scale:** At SF10, the GPU TAE scan path is ~2.5× slower
  than CPU due to host→GPU transfer overhead dominating. The GPU path is designed for
  SF100+ where nvCOMP LZ4 throughput (~300 GB/s) far exceeds CPU decompression (~4 GB/s).

## How it works

```
Client                  MatrixOne                Sidecar (DuckDB + Sirius)
  │                         │                         │
  │  /*+ SIDECAR [GPU] */ ...  │                         │
  │────────────────────────>│                         │
  │                         │  GET /debug/tae/manifest│
  │                         │  (internal, for schema) │
  │                         │                         │
  │                         │  Rewrite: table refs →  │
  │                         │  tae_scan(manifest_url) │
  │                         │  GPU: wrap in           │
  │                         │  gpu_execution()        │
  │                         │                         │
  │                         │  POST rewritten SQL     │
  │                         │────────────────────────>│
  │                         │                         │
  │                         │                         │ CPU path:
  │                         │                         │  tae_scan → pread →
  │                         │                         │  LZ4 decompress (CPU) →
  │                         │                         │  DuckDB vectors →
  │                         │                         │  DuckDB engine
  │                         │                         │
  │                         │                         │ GPU path:
  │                         │                         │  tae_scan_task → pread →
  │                         │                         │  pinned host memory →
  │                         │                         │  cudaMemcpy to GPU →
  │                         │                         │  nvCOMP LZ4 decompress →
  │                         │                         │  CUDA decode kernels →
  │                         │                         │  cudf filter pushdown →
  │                         │                         │  Sirius GPU engine
  │                         │                         │
  │                         │  JSONCompact response   │
  │                         │<────────────────────────│
  │  MySQL result set       │                         │
  │<────────────────────────│                         │
```

## Architecture

```
mo-sirius-sidecar/
├── duckdb/                    ← DuckDB v1.5.1 (submodule)
├── extension-ci-tools/        ← DuckDB build helpers (submodule)
├── tae-scanner/               ← TAE storage reader (submodule)
│   ├── src/                   ← Scanner, filter, object reader
│   └── include/               ← Headers
├── httpserver/                ← HTTP query server (submodule)
│   └── src/                   ← Server, serializers
├── sirius/                    ← GPU SQL engine (submodule)
│   └── src/
│       ├── op/scan/           ← tae_scan_task (GPU native TAE reader)
│       ├── data/              ← host_tae→gpu_table converter (nvCOMP + CUDA)
│       ├── cuda/tae/          ← CUDA kernels (fixed decode, varchar, null mask)
│       ├── tae/               ← TAE metadata parser
│       └── ...                ← GPU operators, cuCascade, planner
├── extension_config.cmake     ← CPU extensions config
├── extension_config_gpu.cmake ← GPU extensions config
├── Makefile                   ← Build wrapper
├── DESIGN.md                  ← Architecture document
└── README.md
```
