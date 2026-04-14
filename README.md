# mo-sirius-sidecar

DuckDB-based query sidecar for MatrixOne, powered by the
[Sirius](https://github.com/matrixorigin/sirius) GPU execution engine.
Queries annotated with `/*+ SIDECAR */` (CPU) or `/*+ SIDECAR GPU */` (GPU) in MO are rewritten and forwarded to this
sidecar, which reads TAE storage objects directly and returns results via HTTP.

## Extensions

| Extension | Source | Description |
|-----------|--------|-------------|
| **tae-scanner** | [duckdb-tae-scanner](https://github.com/matrixorigin/duckdb-tae-scanner) | Reads MatrixOne TAE storage objects as DuckDB table functions |
| **httpserver** | [duckdb-httpserver](https://github.com/matrixorigin/duckdb-httpserver) | ClickHouse-compatible HTTP server for accepting SQL queries |
| **sirius** | [sirius](https://github.com/matrixorigin/sirius) | GPU-accelerated SQL execution via cuCascade/cuDF |

Extensions are statically linked into the DuckDB binary — no manual `LOAD` needed.
The GPU build adds Sirius on top of the base extensions.

## Prerequisites

CMake ≥ 3.15, Ninja, Clang (recommended) or GCC ≥ 11, plus lz4 and OpenSSL dev
libraries.

**Debian / Ubuntu:**
```bash
sudo apt install clang cmake ninja-build liblz4-dev libssl-dev git
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

### Quick start

```bash
DUCKDB_HTTPSERVER_PORT=9876 ./build/release/duckdb -unsigned
```

The HTTP server auto-starts on the specified port. No `-cmd` flags needed.

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DUCKDB_HTTPSERVER_PORT` | *(none)* | Set to auto-start HTTP server on this port |
| `DUCKDB_HTTPSERVER_HOST` | `0.0.0.0` | Listen address |
| `DUCKDB_HTTPSERVER_AUTH` | *(empty)* | Auth token (X-API-Key or Basic auth) |

### Manual start (interactive)

```bash
./build/release/duckdb -unsigned \
  -cmd "SELECT httpserve_start('0.0.0.0', 9876, '')"
```

### Verify

```bash
curl 'http://localhost:9876/?default_format=JSONCompact&query=SELECT+42'
```

## MatrixOne integration

1. Start the sidecar on port 9876 (see Deploy above)
2. Start MO with `-debug-http :8888`
3. Configure the sidecar URL in MO:
   ```toml
   # etc/launch/cn.toml
   [cn.frontend]
   sidecarUrl = "http://localhost:9876"
   ```
   Or per-session: `SET sidecar_url = 'http://localhost:9876';`
4. Run queries with the sidecar hint (note: `--comments` flag needed for mariadb client):
   ```sql
   -- CPU mode (plain DuckDB):
   /*+ SIDECAR */ SELECT count(*) FROM tpch.lineitem WHERE l_shipdate < '1998-09-01';
   -- GPU mode (Sirius, wraps in gpu_execution()):
   /*+ SIDECAR GPU */ SELECT count(*) FROM tpch.lineitem WHERE l_shipdate < '1998-09-01';
   ```

## How it works

```
Client                  MatrixOne                Sidecar (DuckDB)
  │                         │                         │
  │  /*+ SIDECAR [GPU] */ ...  │                         │
  │────────────────────────>│                         │
  │                         │  GET /debug/tae/manifest│
  │                         │  (internal, for schema) │
  │                         │                         │
  │                         │  Rewrite: table refs →  │
  │                         │  tae_scan(manifest_url) │
  │                         │                         │
  │                         │  POST rewritten SQL     │
  │                         │────────────────────────>│
  │                         │                         │ tae_scan reads
  │                         │                         │ TAE objects from
  │                         │                         │ shared storage
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
│   └── src/                   ← GPU operators, cuCascade
├── extension_config.cmake     ← CPU extensions config
├── extension_config_gpu.cmake ← GPU extensions config
├── Makefile                   ← Build wrapper
├── DESIGN.md                  ← Architecture document
└── README.md
```
