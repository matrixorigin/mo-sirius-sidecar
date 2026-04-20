# MatrixOne DuckDB Sidecar — Design Document

## Analytical Query Offload via DuckDB TAE Scanner

---

## 1. Executive Summary

This document describes the architecture of the **mo-sirius-sidecar**: a DuckDB-based
query sidecar for **MatrixOne** (a cloud-native HTAP database). Analytical workloads
annotated with `/*+ SIDECAR */` (CPU) or `/*+ SIDECAR GPU */` (GPU) hints in MO are rewritten and forwarded to this sidecar,
which reads TAE storage objects directly and executes queries using DuckDB's vectorized
engine.

The sidecar consists of statically-linked DuckDB extensions:
- **tae_scanner** — reads MatrixOne TAE object files directly via DuckDB table functions
- **httpserver** — ClickHouse-compatible HTTP interface for receiving queries
- **sirius** — GPU-accelerated SQL execution via cuCascade/cuDF (GPU build only)

**Key properties:**
- Zero-copy read path: `pread()` → LZ4 decompress → fill DuckDB vectors
- Column-level predicate pushdown via TAE zone maps
- Block-level parallelism with DuckDB's morsel-driven execution
- Transparent integration: MO handles the SQL rewriting; clients just add a hint
- **GPU path**: native TAE scan reads compressed data into pinned host memory, transfers
  to GPU, decompresses via nvCOMP LZ4, and decodes with CUDA kernels — bypassing DuckDB
  vectors entirely for GPU queries

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     User / Application                      │
│    /*+ SIDECAR [GPU] */ SELECT ... FROM lineitem WHERE ...  │
└───────────────────────────┬─────────────────────────────────┘
                            │ MySQL protocol
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                       MatrixOne (MO)                        │
│  1. Detect /*+ SIDECAR [GPU] */ hint                        │
│  2. Rewrite: table refs → tae_scan(manifest_url)            │
│  3. If GPU: wrap in gpu_execution()                         │
│  4. POST rewritten SQL to sidecar                           │
│  5. Translate JSONCompact response to MySQL result set      │
└───────────────────────────┬─────────────────────────────────┘
                            │ HTTP POST
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│                DuckDB Sidecar (this project)                     │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │              httpserver extension                        │    │
│  │  Receives SQL, returns JSONCompact results               │    │
│  └─────────────────────────┬────────────────────────────────┘    │
│                            │ SQL execution                       │
│           ┌────────────────┴───────────────────┐                 │
│           ▼ CPU path                           ▼ GPU path        │
│  ┌─────────────────────────┐  ┌─────────────────────────────┐    │
│  │ tae_scan() → DuckDB     │  │ tae_scan_task → pinned host │    │
│  │ pread → LZ4 (CPU) →     │  │ pread → cudaMemcpy →        │    │
│  │ DuckDB DataChunk →      │  │ nvCOMP LZ4 (GPU) →          │    │
│  │ DuckDB vectorized exec  │  │ CUDA decode kernels →       │    │
│  └─────────────────────────┘  │ cudf filter pushdown →      │    │
│                               │ Sirius GPU execution        │    │
│                               └─────────────────────────────┘    │
└──────────────────────────────────┬───────────────────────────────┘
                                   │ pread()
                                   ▼
┌─────────────────────────────────────────────────────────────┐
│                     TAE Object Files                        │
│  mo-data/shared/018e1234-5678-abcd-ef01-234567890abc_00001  │
│  mo-data/shared/018e1234-5678-abcd-ef01-234567890abc_00002  │
│  ...                                                        │
└─────────────────────────────────────────────────────────────┘
                                   ▲
                                   │ flush / compaction
┌─────────────────────────────────────────────────────────────┐
│                       MatrixOne TN                          │
│  INSERT/UPDATE/DELETE → WAL → Flush → TAE objects on disk   │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

**CPU path (DuckDB vectorized engine):**
1. User issues `/*+ SIDECAR */ SELECT ...` or `CALL gpu_execution('SELECT ...')`
2. DuckDB invokes `tae_scan()` table function
3. `tae_scan()` reads TAE object files via `pread()`, LZ4-decompresses, decodes MO
   vectors, and fills DuckDB `DataChunk` output
4. DuckDB vectorized engine processes the chunks on CPU
5. Results returned to user via JSONCompact HTTP response

**GPU path (Sirius native TAE scan):**
1. User issues `/*+ SIDECAR GPU */ SELECT ...`
2. MO wraps the query in `gpu_execution()`
3. Sirius intercepts `tae_scan()` and routes to the native GPU TAE scan operator
4. `tae_scan_task` reads TAE objects with CRC stripping into **host pinned memory**
   (compressed data stays compressed — no CPU decompression)
5. Compressed data is transferred to GPU via `cudaMemcpyAsync`
6. **nvCOMP LZ4** decompresses all column chunks in a single batched GPU call
7. **CUDA kernels** decode MO column formats (fixed-width, varchar, null mask inversion)
8. `cudf::compute_column()` evaluates pushed-down filter predicates on GPU
9. `cudf::apply_boolean_mask()` prunes filtered rows
10. Post-filter column projection removes filter-only columns
11. Sirius GPU execution engine processes the remaining plan (joins, aggregation)
12. Results returned via JSONCompact HTTP response

---

## 3. Integration Approaches Considered

| # | Approach | Latency | Complexity | Data Freshness |
|---|----------|---------|------------|----------------|
| 1 | **Parquet bridge** — MO exports to Parquet, Sirius reads Parquet | High (ETL) | Low | Stale |
| 2 | **DuckDB TAE scanner** — read MO objects directly ✅ | Low | Medium | Real-time* |
| 3 | **Substrait exchange** — MO sends Substrait plans to Sirius | Low | High | Real-time |

\* Real-time = reads committed, flushed data. In-memory (unflushed) data is not visible.

**Decision: Approach 2 + 3 (layered).** Approach 2 (TAE scanner) is the data access
layer — a DuckDB extension that reads TAE objects directly. Approach 3 (Substrait)
builds on top, adding automatic query routing from MO to Sirius. They are complementary,
not alternatives. See §18 for the full integration architecture.

Embedding CUDA kernels directly in MO's Go pipeline (via CGO/FFI) was considered
and rejected — see §18.6 for why this is infeasible.

---

## 4. TAE Object Binary Format

MatrixOne stores table data as immutable **TAE objects** on the local filesystem (or S3).
Each object contains up to 256 blocks × 8192 rows = **2,097,152 rows**, stored in
columnar layout with optional LZ4 compression.

### 4.1 File Layout

```
Offset 0                                                    Offset EOF
│                                                            │
▼                                                            ▼
┌────────┬──────────┬──────────┬─────┬───────┬──────┬────────┐
│ Header │ ColData  │ ColData  │ ... │  BF   │ Meta │ Footer │
│  64B   │ Blk0/C0  │ Blk0/C1  │     │       │      │  64B   │
└────────┴──────────┴──────────┴─────┴───────┴──────┴────────┘
```

### 4.2 Object Header (64 bytes)

```
Offset  Size  Field
  0       8   magic        uint64 = 0xFFFFFFFF
  8       2   version      uint16 = 1
 10      13   meta_extent  Extent → location of metadata section
 23      41   reserved
```

The meta extent starts at **offset 10** (after 8-byte magic + 2-byte version), not at the
end of the header. This is the most critical field — it tells us where all column/block
metadata lives.

### 4.3 Extent (13 bytes)

Every data region is located by an **Extent**:

```
Offset  Size  Field
  0       1   alg          compression algorithm (0=None, 1=LZ4)
  1       4   offset       byte offset in file
  5       4   length       compressed size in bytes
  9       4   origin_size  decompressed size in bytes
```

Read procedure: `pread(fd, buf, ext.length, ext.offset)` →
if `ext.alg == 1`: `LZ4_decompress_safe(buf, out, ext.length, ext.origin_size)`

### 4.4 IOEntryHeader (4 bytes)

Every serialized column data region is prefixed with:

```
Offset  Size  Field
  0       2   type     IOET_ObjMeta=1, IOET_ColData=2
  2       2   version  format version
```

The reader must strip these 4 bytes before decoding vector data.

### 4.5 Metadata Section

Located via `header.meta_extent`. After decompression and IOEntryHeader (4B) stripping,
the metadata uses the **objectMetaV3** format:

#### objectMetaV3 Header (32 bytes)

```
Offset  Size  Field
  0       2   dataMetaCount       number of data meta entries (always 1)
  2       4   dataMetaOffset      byte offset to DataMeta (from start of meta buf)
  6       2   tombstoneMetaCount  number of tombstone meta entries (0 or 1)
  8       4   tombstoneMetaOffset byte offset to TombstoneMeta
 12      20   dummy               reserved
```

#### DataMeta (objectDataMetaV1)

Starts at `dataMetaOffset`. Structure:

```
[BlockHeader 179B]                    ← object-level header
[ColumnMeta × metaColCnt, 124B each] ← object-level column metadata
[BlockIndex]                          ← per-block offset table
```

#### BlockIndex

```
Offset  Size  Field
  0       4   blockCount                  number of blocks
  4     8×N   entries[blockCount]         {offset 4B, length 4B} per block
```

Each entry's `offset` is relative to the start of the DataMeta region.
Following the BlockIndex, the per-block metadata is packed sequentially,
each block having the same structure:

```
[BlockHeader 179B] [ColumnMeta × metaColCnt, 124B each]
```

#### BlockHeader (179 bytes)

Source: `pkg/objectio/meta.go`

```
Offset  Size  Field
  0       8   dbID
  8       8   tableID
 16      20   blockID
 36       4   rows              row count in this block
 40       2   columnCount       user column count
 42      13   metaLocation      Extent (metadata self-reference)
 55      13   bloomFilter       Extent (bloom filter location)
 68       4   bloomChecksum     CRC32
 72      64   zoneMapArea       object-level zone map data
136       4   zoneMapChecksum   CRC32
140       2   metaColCnt        number of ColumnMeta entries (>= columnCount; includes internal cols)
142       2   maxSeq            maximum column seqnum
144       2   startID           starting seqnum
146       1   appendable        0 or 1
147       2   sortKey           sort key column index
149       1   bloomFilterType   type of bloom filter
150      29   dummy             reserved
```

The key field for scanning is `metaColCnt` at offset 140: this determines how many
`ColumnMeta` entries follow the BlockHeader. It may differ from `columnCount` because
MO stores internal columns (e.g., `__mo_rowid`) in the same metadata.

#### ColumnMeta (124 bytes per column per block)

Source: `pkg/objectio/column.go`

```
Offset  Size  Field
  0       1   dataType     MO type OID (types.T)
  1       2   idx          column seqnum (uint16)
  3       4   ndv          number of distinct values
  7       4   nullCnt      null count
 11      13   location     Extent pointing to column data in file
 24       4   checksum     CRC32
 28      64   zoneMap      zone map (min/max, see §8.1)
 92      32   dummy        reserved
```

Note: the `dataType` field is a single byte (MO type OID), not the full 16-byte MOType
struct. The full type information must be obtained from the manifest/catalog.

### 4.6 Vector Serialization Format

Each column data region (after IOEntryHeader) is a serialized MO Vector:

```
Offset  Size  Field
  0       1   class       0=FLAT, 1=CONSTANT
  1      16   type        MOType (native little-endian struct)
 17       4   length      row count
 21       4   data_len    size of data section
 25    data_len  data     fixed-width: N × elem_size bytes
                          varlena: N × 24B Varlena entries
 ...       4   area_len   varlena overflow area size
 ...  area_len  area      big string data (offset/length in Varlena entries)
 ...       4   nsp_len    null bitmap size
 ...  nsp_len   nsp       null bitmap (V2 format, see below)
 ...       1   sorted     1 if column is sorted
```

### 4.7 Varlena Format (24 bytes per entry)

MO uses a custom variable-length encoding:

```
Case 1 — Inline (data[0] ≤ 23):
  ┌──────┬───────────────────────┐
  │len=N │ inline data (N bytes) │
  │ 1B   │       23B             │
  └──────┴───────────────────────┘

Case 2 — Big string (data[0..3] == 0xFFFFFFFF):
  ┌──────────┬────────┬────────┬──────────┐
  │0xFFFFFFFF│ offset │ length │ reserved │
  │   4B     │  4B    │  4B    │   12B    │
  └──────────┴────────┴────────┴──────────┘
  Actual data lives in the area buffer at area[offset : offset+length]
```

### 4.8 Null Bitmap (V2 Format)

```
Offset  Size  Field
  0       8   count      int64: number of null values
  8       8   len        uint64: total bit count
 16       8   data_size  uint64: bitmap data size in bytes
 24    data_size  words  uint64[]: bit-packed bitmap (bit set = null)
```

---

## 5. Type Mapping: MatrixOne → DuckDB

### 5.1 Integer Types

| MO Type | OID | Size | DuckDB Type |
|---------|-----|------|-------------|
| T_int8 | 20 | 1B | TINYINT |
| T_int16 | 21 | 2B | SMALLINT |
| T_int32 | 22 | 4B | INTEGER |
| T_int64 | 23 | 8B | BIGINT |
| T_uint8 | 25 | 1B | UTINYINT |
| T_uint16 | 26 | 2B | USMALLINT |
| T_uint32 | 27 | 4B | UINTEGER |
| T_uint64 | 28 | 8B | UBIGINT |

### 5.2 Floating-Point Types

| MO Type | OID | Size | DuckDB Type |
|---------|-----|------|-------------|
| T_float32 | 30 | 4B | FLOAT |
| T_float64 | 31 | 8B | DOUBLE |

### 5.3 String / Binary Types

| MO Type | OID | Size | DuckDB Type |
|---------|-----|------|-------------|
| T_char | 40 | 24B (Varlena) | VARCHAR |
| T_varchar | 41 | 24B (Varlena) | VARCHAR |
| T_blob | 43 | 24B (Varlena) | BLOB |
| T_text | 44 | 24B (Varlena) | VARCHAR |
| T_json | 201 | 24B (Varlena) | VARCHAR |
| T_binary | 46 | 24B (Varlena) | BLOB |
| T_varbinary | 47 | 24B (Varlena) | BLOB |
| T_datalink | 48 | 24B (Varlena) | VARCHAR |

### 5.4 Date / Time Types

| MO Type | OID | Size | DuckDB Type | Conversion |
|---------|-----|------|-------------|------------|
| T_date | 50 | 4B (int32) | DATE | Subtract 719,162 days |
| T_time | 51 | 8B (int64) | TIME | Direct (microseconds) |
| T_datetime | 52 | 8B (int64) | TIMESTAMP | Subtract 62,135,596,800,000,000 µs |
| T_timestamp | 53 | 8B (int64) | TIMESTAMP | Subtract 62,135,596,800,000,000 µs |

**Critical epoch offset:** MO counts from January 1, year 1 AD (proleptic Gregorian).
DuckDB counts from January 1, 1970 (Unix epoch). The constant offsets are:

- **MO_UNIX_EPOCH_DAYS** = 719,162 (days between year 1 and 1970)
- **MO_UNIX_EPOCH_USEC** = 62,135,596,800,000,000 (microseconds)

### 5.5 Decimal Types

| MO Type | OID | Size | DuckDB Type | Notes |
|---------|-----|------|-------------|-------|
| T_decimal64 | 60 | 8B | DECIMAL(p,s) | p,s from MOType.Width/Scale |
| T_decimal128 | 61 | 16B | DECIMAL(p,s) | {B0_63, B64_127} uint64 pair |
| T_decimal256 | 62 | 32B | HUGEINT | Rare, approximate |

### 5.6 Boolean

| MO Type | OID | Size | DuckDB Type |
|---------|-----|------|-------------|
| T_bool | 10 | 1B | BOOLEAN |

### 5.7 Bit Type

| MO Type | OID | Size | DuckDB Type |
|---------|-----|------|-------------|
| T_bit | 210 | 8B (uint64) | UBIGINT |

### 5.8 UUID / Enum

| MO Type | OID | Size | DuckDB Type |
|---------|-----|------|-------------|
| T_uuid | 100 | 16B | UUID |
| T_enum | 200 | 2B (uint16) | USMALLINT |

### 5.9 Internal Types (Skip)

These MO-internal types should not appear in user tables:

| MO Type | OID | Size | Purpose |
|---------|-----|------|---------|
| T_TS | 70 | 12B | Transaction timestamp |
| T_Rowid | 80 | 24B | Row identifier |
| T_Blockid | 81 | 20B | Block identifier |

### 5.10 Vector Types (Future)

| MO Type | OID | DuckDB Type |
|---------|-----|-------------|
| T_array_float32 | 224 | FLOAT[] |
| T_array_float64 | 225 | DOUBLE[] |

---

## 6. Object Discovery

The scanner needs to know which TAE object files belong to a given table.

### 6.1 Method 1: mo_ctl inspect (Current)

```sql
SELECT mo_ctl('dn', 'inspect', 'object -t tpch.lineitem -vvvv');
```

Returns one line per object with base64-encoded `ObjectStats`. Decode to get:
- Object name (= filename): `018e1234-5678-abcd-ef01-234567890abc_00001`
- Row count, block count, byte size
- Zone maps per column

**Pros:** Works today, no code changes.
**Cons:** Must parse base64 ObjectStats; requires MO SQL connection.

### 6.2 Method 2: Manifest File (Recommended for Phase 1)

Export object metadata to a JSON manifest:

```json
{
  "table": "tpch.lineitem",
  "columns": [
    {"name": "l_orderkey", "oid": 22, "width": 4, "scale": 0},
    {"name": "l_partkey", "oid": 22, "width": 4, "scale": 0},
    ...
  ],
  "objects": [
    {"path": "018e1234-5678-abcd-ef01-234567890abc_00001", "rows": 2097152, "blocks": 256},
    {"path": "018e1234-5678-abcd-ef01-234567890abc_00002", "rows": 1500000, "blocks": 184},
    ...
  ]
}
```

Generated by a helper script or SQL function. The scanner reads this once at bind time.

### 6.3 Method 3: SQL Catalog Query (Future)

A new MO SQL function `mo_table_objects(db, table)` that returns a virtual table:

```sql
SELECT object_name, row_count, block_count, byte_size
FROM mo_table_objects('tpch', 'lineitem');
```

This would allow fully dynamic discovery without a manifest file.

### 6.4 File Path Resolution

Object files live under the MO shared fileservice root:

```
<data_dir>/<ObjectName>
```

Where `<ObjectName>` is a 42-character string: `<UUID>_<5-digit-num>`.
Example: `mo-data/shared/018e1234-5678-abcd-ef01-234567890abc_00001`

---

## 7. DuckDB Extension Design

### 7.1 SQL Interface

```sql
-- Load the extension
LOAD 'tae_scanner';

-- Scan a MO table (local filesystem)
SELECT * FROM tae_scan('/path/to/mo-data/shared/manifest.json');

-- With column projection
SELECT l_orderkey, l_quantity, l_extendedprice
FROM tae_scan('/path/to/manifest.json');

-- With predicates (zone map pushdown)
SELECT * FROM tae_scan('/path/to/manifest.json')
WHERE l_shipdate >= DATE '1994-01-01'
  AND l_shipdate < DATE '1995-01-01';
```

### 7.2 Three-Phase Execution

#### Phase 1: Bind

- Parse manifest JSON to discover schema and object list
- Map MO column types to DuckDB LogicalTypes
- Register output column names and types
- Compute projection mapping (which MO seqnums to read)

#### Phase 2: Init

Init processes the column list in three phases to correctly handle DuckDB's
`filter_prune` optimization (where filter-only columns are excluded from output):

1. **Phase 1 — Read columns**: Iterate `input.column_ids` (includes ALL columns:
   projected + filter-only). For each TAE column, append its seqnum to `read_seqnums`
   and record `col_ids_to_decoded[ci]` = position in `decoded_cols`.

2. **Phase 2 — Output map**: Build `output_map` from `input.projection_ids`
   (or `column_ids` if absent). Each entry stores a `decoded_pos` for direct
   indexing into `decoded_cols` — no sequential counter needed.

3. **Phase 3 — Filters**: Extract pushed-down filters using `col_ids_to_decoded`
   to map filter column indices to decoded_cols positions. Encode constants in
   MO binary format.

Also: read sampling options, build work queue, create per-thread local state.

#### Phase 3: Execute (parallel, called repeatedly until EOF)

```
work_unit = work_queue[next_idx.fetch_add(1)]  // atomic dispatch
reader = open/cache TAEObjectReader for work_unit.object_idx
reader.ReadMeta()

// Zone map check (block-level skip)
for each pushed filter:
    if zone_map_eliminates(block, filter):
        blocks_skipped++; continue to next work unit

decoded_cols = reader.ReadBlock(block_idx, read_seqnums)

// Fill output columns via output_map:
//   TAE_COLUMN  → FillColumn(output[i], decoded_cols[om.decoded_pos])
//   VCOL_FILENAME → fill_n with file path string
//   VCOL_BLOCK_ID → fill_n with block index
// (output_map.size() == DataChunk columns; decoded_cols may be larger
//  when filter_prune adds filter-only columns to read_seqnums)

// Per-row filter evaluation
filtered_count = ApplyRowFilters(filters, decoded_cols, output)

// Bernoulli sampling (if TABLESAMPLE SYSTEM)
if do_sample:
    sample_count = apply_bernoulli(filtered_count, sample_rate, rng)

output.SetCardinality(final_count)
```

### 7.3 Source Files

```
duckdb-tae-scanner/
├── CMakeLists.txt                    Build config (DuckDB + LZ4)
├── README.md                         Quick-start guide
├── include/
│   ├── tae_types.hpp                 MO→DuckDB type mapping + Varlena
│   ├── tae_object_reader.hpp         TAE binary format reader (structs + offsets)
│   ├── tae_zonemap.hpp               Zone map evaluation for predicate pushdown
│   ├── tae_scanner.hpp               State structs, OutputColumnInfo, PushedFilter
│   ├── tae_column_fill.hpp           FillColumn() — copy TAE columns into DuckDB vectors
│   ├── tae_filter.hpp                Filter helpers: extract, zone map eval, per-row filter
│   └── tae_scanner_extension.hpp     Extension class declaration
├── src/
│   ├── tae_object_reader.cpp         pread + LZ4 + vector decode + metadata parse
│   ├── tae_column_fill.cpp           Column fill: Copy{Fixed,Date,Timestamp,Varlen,Uuid}, SetNullMask
│   ├── tae_filter.cpp                Filter encode, ExtractFilter, BlockPassesFilters, ApplyRowFilters
│   ├── tae_scanner.cpp               Manifest parse, bind, init, execute, statistics, callbacks
│   └── tae_scanner_extension.cpp     Extension entry point
└── test/
    ├── gen_test_data.py              Python TAE binary writer (test fixtures)
    ├── test_scan_basic.cpp           Scan, projection, parallel, explain
    ├── test_scan_filters.cpp         LZ4, ORDER BY, filter-prune, stats, partition prune
    ├── test_scan_types.cpp           Decimal, UUID, blob, date, timestamp
    ├── test_scan_errors.cpp          Error handling
    ├── test_reader.cpp               Low-level reader, coalescing, prefetch
    ├── test_zonemap.cpp              Zone map evaluation
    └── data/                         Generated .tae files + manifest JSONs
```

### 7.4 Column Fill Dispatch

The `FillColumn()` function (in `tae_column_fill.cpp`) fills DuckDB output
vectors based on column type. Both FLAT and CONSTANT (single-value broadcast)
MO vectors are supported.

| Category | MO Types | Strategy |
|----------|----------|----------|
| Fixed-width | int8..int64, uint8..uint64, float32/64, bool, decimal64, decimal128, enum, bit | `memcpy(dst, src, N × elem_size)` |
| Date | T_date | `dst[i] = src[i] - 719162` |
| Timestamp | T_datetime, T_timestamp | `dst[i] = src[i] - 62135596800000000` |
| Varlena | char, varchar, text, blob, json, binary, varbinary | Decode Varlena → `StringVector::AddString()` |
| UUID | T_uuid | Per-row `UUID::FromBlob()` (big-endian → hugeint_t) |
| CONSTANT | (any type, vec_class=1) | Set `CONSTANT_VECTOR` type, fill single value at pos 0 |
| Null mask | (all types) | Iterate bitmap words, call `Validity.SetInvalid(i)` |

### 7.5 Filter Pushdown Pipeline

Filters are evaluated in two stages (implemented in `tae_filter.cpp`):

1. **Zone map block skip** — reject entire blocks where zone map min/max
   proves no rows can match. Supported for all numeric, decimal, date/timestamp,
   string, and UUID types.

2. **Per-row evaluation** — for blocks that pass zone maps, each row is
   checked against all pushed filters. Rows that fail are compacted out via
   `SelectionVector::Slice()`.

### 7.6 Virtual Columns

The extension exposes two virtual columns (DuckDB column IDs ≥ 2^63):

| Virtual Column | Type | Value |
|----------------|------|-------|
| `file_path` | VARCHAR | TAE object file path from manifest |
| `block_id` | INTEGER | Zero-based block index within the object |

These are populated in the Execute phase via `fill_n` on flat vectors.

### 7.7 DuckDB Callbacks

| Callback | Purpose |
|----------|---------|
| `TAEScanBind` | Parse manifest, build schema |
| `TAEScanInit` | Build output_map, extract filters, set up work queue |
| `TAEScanInitLocal` | Per-thread state (reader cache, RNG) |
| `TAEScanExecute` | Parallel block scan with atomic dispatch |
| `TAEScanCardinality` | Row count estimate for planner |
| `TAEScanStatistics` | Column min/max + null info from zone maps & metadata |
| `TAEScanProgress` | Scan completion percentage |
| `TAEScanToString` | Static EXPLAIN info |
| `TAEScanDynamicToString` | Runtime profiling info |
| `TAEScanGetVirtualColumns` | Advertise file_path/block_id |
| `TAEScanSupportsPushdownType` | Type-aware filter support |

---

## 8. Predicate Pushdown via Zone Maps

Each column in each block has a 64-byte **zone map** containing the min and max values.
The scanner evaluates DuckDB's pushed-down filters against zone maps to skip entire
blocks that cannot contain matching rows.

### 8.1 Zone Map Layout (64 bytes)

Source: `pkg/vm/engine/tae/index/zm.go`

```
Offset  Size  Field
  0      30   min_value     encoded minimum (raw little-endian for fixed types)
 30       1   min_info      lower 5 bits = length of min (for strings)
 31      30   max_value     encoded maximum
 61       1   max_info      lower 5 bits = length of max, bit 7 = truncated flag
 62       1   scale_init    lower 6 bits = decimal scale, bit 7 = initialized flag
 63       1   data_type     MO type OID (types.T)
```

**Fixed types** (int32, float64, date, timestamp, etc.): Min/max are stored as raw
little-endian bytes in the first `sizeof(T)` bytes of the 30-byte field.

**String types** (varchar, char, text, etc.): Up to 30 bytes of the min/max string
are stored. If the actual max value exceeds 30 bytes, the `truncated` flag (bit 7 of
`max_info`) is set, and comparisons must be conservative (the max might be larger).

A zone map is valid only if bit 7 of `scale_init` (the `initialized` flag) is set.

### 8.2 Filter Extraction (Bind/Init Time)

The scanner processes DuckDB's `TableFilterSet` at init time:

1. Iterate over all pushed filters (keyed by `ProjectionIndex`)
2. For each `ConstantFilter`: convert `ExpressionType` to internal `FilterOp`, encode
   the DuckDB `Value` constant as raw bytes matching zone map encoding
3. For `ConjunctionAndFilter`: recursively extract child filters (all must pass)
4. For `IsNull`/`IsNotNull`: record as-is (zone maps can't evaluate these)
5. Store extracted `PushedFilter` structs in bind data

**Constant encoding**: DuckDB dates and timestamps use Unix epoch, while MO uses
year-1-AD epoch. The encoder adds the appropriate offset (`MO_UNIX_EPOCH_DAYS` for
dates, `MO_UNIX_EPOCH_USEC` for timestamps) so the encoded constant matches the
raw zone map bytes.

### 8.3 Block Evaluation (Execute Time)

For each block, before reading column data:

```
for each PushedFilter pf:
    zm = reader.GetZoneMap(block_idx, pf.seqnum)
    if zm is null or not initialized:
        continue   // can't evaluate → keep block
    if IsStringType(pf.mo_type_oid):
        pass = ZoneMapCheckString(zm, pf.op, pf.constant, pf.const_len)
    else:
        pass = ZoneMapCheckFixed<T>(zm, pf.op, pf.constant_as_T)
    if not pass:
        skip block   // zone map proves no rows can match
```

**Supported operators**: `=`, `!=`, `>`, `>=`, `<`, `<=`

**Skip logic per operator**:

| Operator | Skip block when |
|----------|-----------------|
| `= C` | `C < min` or `C > max` |
| `!= C` | `min == max == C` (all values are C) |
| `> C` | `max <= C` |
| `>= C` | `max < C` |
| `< C` | `min >= C` |
| `<= C` | `min > C` |

Multiple filters on the same or different columns are AND'd: if any filter rejects
the block, it is skipped.

### 8.4 DuckDB Integration

```cpp
// In GetTAEScanFunction():
func.filter_pushdown = true;   // tell DuckDB to push filters to us
func.filter_prune = true;      // allow pruning filter-only columns from output
```

**filter_prune column mapping**: When `filter_prune = true`, DuckDB's
`RemoveUnusedColumns` optimizer keeps filter-only columns in `column_ids` but
excludes them from the output via `projection_ids`:

```
Example: SELECT col_int, col_dbl FROM ... WHERE col_str = 'x'

column_ids     = [0, 1, 2]   // col_int, col_str (filter-only), col_dbl
projection_ids = [0, 2]      // output slots → positions 0 and 2 in column_ids
```

The scanner reads ALL `column_ids` columns into `decoded_cols` (for both output
and filtering), but builds `output_map` from `projection_ids` only. Each
`OutputColumnInfo` stores a `decoded_pos` so Execute can directly index
`decoded_cols[om.decoded_pos]` without a fragile sequential counter.

```cpp
// In TAEScanInit():
for (auto &entry : *input.filters) {
    ProjectionIndex col_idx = entry.GetIndex();  // position in column_ids
    auto decoded_pos = col_ids_to_decoded[col_idx];
    // ... extract ConstantFilter, ConjunctionAndFilter, IsNull/IsNotNull
    // ... encode constants, store in state->filters
}

// In TAEScanExecute():
while (current_block < block_count) {
    if (!BlockPassesFilters(bind.filters, reader, block_idx)) {
        blocks_skipped++;
        continue;  // skip block entirely — no I/O for column data
    }
    blocks_scanned++;
    // read and decode block...
}
```

The scanner tracks `blocks_scanned` and `blocks_skipped` counters for
diagnostic logging. A high skip ratio indicates effective pushdown.

### 8.5 Object-Level Partition Pruning

When filters are present, the scanner evaluates zone maps for **all blocks** of
each object at Init time, before building the work queue. If every block in an
object fails the zone map check, the entire object is excluded from the work
queue — no blocks are read and no I/O occurs for that object.

```cpp
// In TAEScanInit() — work queue build:
if (!state->filters.empty()) {
    for each object:
        reader.ReadMeta();
        for each block in object:
            if (BlockPassesFilters(...))
                add to work queue;
            else
                blocks_skipped++;
        if (no block added)
            objects_skipped++;
} else {
    // No filters — add all blocks unconditionally
}
```

This provides two levels of pruning:
1. **Object-level** (Init time): Entire objects skipped when all blocks fail
2. **Block-level** (Execute time): Remaining blocks re-checked at scan time

#### Manifest Zone Map Fast Path

When the manifest includes a `zone_map` hex field and `sort_column` name, the
scanner can prune objects **without reading metadata at all**. The zone map
encodes the sort key's min/max values in the same 64-byte format used inside
.tae files.

```json
{
  "sort_column": "col_int",
  "objects": [
    {"path": "obj1.tae", "zone_map": "0a000000...50000000...", ...}
  ]
}
```

During Init, if an object has `sort_key_zm` and the filters reference the sort
column, `ZoneMapPassesFilters()` evaluates the manifest zone map directly:

- **Fail** → skip the object entirely (no ReadMeta, no I/O)
- **Pass** → proceed to per-block pruning as usual

This is especially valuable for remote/S3 files where ReadMeta requires a
network round-trip per object.

The `objects_skipped` counter appears in `DynamicToString` profiling output
when any objects are pruned. Combined with `blocks_skipped`, this gives
complete visibility into how much I/O was avoided.

---

## 9. ORDER BY Pushdown (Scan Order Optimization)

MO objects are sorted by primary key. When DuckDB detects `ORDER BY col LIMIT N`,
its RowGroupPruner optimizer calls our `set_scan_order` callback, enabling two
optimizations:

### 9.1 Block Reordering

Work units are sorted by zone map statistics of the ORDER BY column:
- **ASC**: sort by zone map **min** value (smallest-first)
- **DESC**: sort by zone map **max** value (largest-first)

This ensures blocks most likely to satisfy the query are scanned first.

### 9.2 Early Termination (Row Limit Pruning)

When `row_limit` is provided, we truncate the work queue once enough rows are
available. For example, `LIMIT 3` on blocks of 4 rows each → only 1 block scanned.

### 9.3 Manifest Sort Column

The manifest JSON supports an optional `"sort_column"` field declaring which column
the data is sorted by. This metadata enables the optimizer to push down ORDER BY:

```json
{
  "sort_column": "pk_column",
  "columns": [...],
  "objects": [...]
}
```

### 9.4 Callback Flow

```
  RowGroupPruner optimizer
       │
       ▼
  TAESetScanOrder(options, bind_data)
       │  extract: column_idx, ASC/DESC, MIN/MAX stat, row_limit
       ▼
  TAEScanInit
       │  sort work_units by zone map of order column
       │  truncate work queue if row_limit allows
       ▼
  TAEScanExecute → scans blocks in optimal order
```

## 10. Tombstone Handling (Deleted Rows)

MO uses **row-level tombstones** to mark deleted rows. A separate tombstone object
contains `(Rowid, CommitTS)` pairs indicating which rows in which blocks have been
deleted.

### 10.1 Phase 1: Ignore Tombstones

For initial TPC-H benchmarking (read-only workload after bulk load), there are no
deletes. We can safely skip tombstone handling.

### 10.2 Phase 2: Apply Tombstones

1. During bind, also enumerate tombstone objects for the table
2. For each data block, check if any tombstone entries reference rows in that block
3. Build a deletion bitmap and merge with the null mask (or filter rows in output)

Tombstone objects use the same TAE binary format. The "data" columns are:
- Column 0: `T_Rowid` (24B) — identifies the specific row
- Column 1: `T_TS` (12B) — commit timestamp of the delete

---

## 11. Consistency Model

### 11.1 Snapshot Isolation

The scanner reads a **point-in-time snapshot** of committed, flushed data. This means:

- Rows inserted and committed before the scan starts are visible
- Rows still in the TN's in-memory buffer (not yet flushed) are **not** visible
- Concurrent modifications during the scan do not affect results (immutable files)

### 11.2 Ensuring All Data Is Flushed

For TPC-H benchmarking, after `LOAD DATA`:

```sql
-- Force flush all in-memory data to disk
SELECT mo_ctl('dn', 'flush', 'tpch.lineitem');
SELECT mo_ctl('dn', 'flush', 'tpch.orders');
-- ... for each table
```

MO's `commitWorkspaceThreshold = 1MB`. Since TPC-H tables are much larger, data is
written directly to disk during `LOAD DATA`. An explicit flush is only needed for
small tables or recent small inserts.

### 11.3 Future: MVCC Integration

For live HTAP workloads, the scanner would need to:
1. Acquire a snapshot timestamp from MO
2. Filter objects by `CreateTime <= snapshot AND (DeleteTime > snapshot OR DeleteTime == 0)`
3. Apply tombstones with `CommitTS <= snapshot`

This requires a connection to MO's catalog service, which is Phase 3 scope.

---

## 12. Performance Considerations

### 12.1 I/O Pattern

- **Sequential within object**: metadata → column data blocks (increasing offsets)
- **Random across objects**: different files, but each file read sequentially
- **Read coalescing**: batch multiple column reads into a single `pread()` covering
  the bounding range, then slice per column (reduces syscall overhead)

### 12.2 Compression

LZ4 decompression throughput: ~3-4 GB/s per core. For TPC-H SF10 lineitem (~7 GB raw),
decompression takes ~2 seconds on a single core. This is negligible compared to GPU
execution time.

### 12.3 PCIe Bandwidth

PCIe 4.0 x16: ~25 GB/s Host-to-Device. TPC-H SF10 lineitem after column projection
(typically 3-5 columns) is ~1-2 GB. Transfer time: ~80ms. Not a bottleneck.

### 12.4 Memory Layout

DuckDB DataChunk uses flat columnar layout — same as GPU `cudf::column`. After the
scanner fills a DataChunk, Sirius can `cudaMemcpyAsync` directly without reshuffling.

### 12.5 Block Size

MO blocks contain up to 8,192 rows. DuckDB's standard vector size is 2,048. The
scanner should split MO blocks into DuckDB-sized chunks (4 chunks per block) for
optimal pipeline utilization.

### 12.6 CRC-Wrapped File Handling

MO wraps TAE object files in a CRC framing format: every 2,044 bytes of logical data
are stored as a 2,048-byte physical block (4-byte CRC header + 2,044 bytes payload).
A naive approach strips the entire file on first access, but this is catastrophic for
metadata-only reads (e.g., statistics callback reads ~4 KB of metadata but would load
~33 MB per file).

**Targeted CRC reads**: `ReadBytes()` translates each logical `[offset, offset+len)`
range to the covering physical CRC blocks, reads only those blocks via `pread()`,
strips CRC headers inline, and returns a contiguous logical buffer. Formula:

```
first_block = offset / 2044
last_block  = (offset + len - 1) / 2044
phys_start  = first_block * 2048
phys_end    = (last_block + 1) * 2048
```

For a statistics callback reading 1,018 objects: ~10 MB total I/O instead of 33.6 GB.

### 12.7 count(*) Metadata Fast Path

When the query requires no column data (e.g., `SELECT count(*)`), the Execute phase
emits row counts directly from manifest metadata without any file I/O:

- **Condition**: `read_seqnums` empty AND `filters` empty AND no sampling
- **Row counts**: all blocks = 8,192 rows except the last block of each object, which
  is `total_rows - (blocks - 1) * 8192`
- **Virtual columns**: `file_path` and `block_id` are filled if projected
- 600 million rows counted in **0.2 seconds** (was ~327 seconds before targeted CRC fix)

### 12.8 Planner Statistics and String Zone Maps

The `TAEScanStatistics` callback provides DuckDB's optimizer with column-level min/max
statistics from zone maps. For VARCHAR columns, zone map bytes are raw string bytes
(not Varlena-encoded), with lengths stored at separate offsets (byte 30 for min, byte
61 for max). The callback uses `StringStats::Update()` to set truncated 8-byte min/max
ranges, and calls `ResetMaxStringLength()` since zone map values don't reflect actual
column data lengths.

A `MAX_OBJECTS_FOR_STATS` cap (default 16) prevents expensive per-object metadata reads
for large tables — the callback returns `nullptr` and DuckDB falls back to defaults.

---

## 13. GPU Native TAE Scan (Sirius)

When the sidecar receives a `/*+ SIDECAR GPU */` query, Sirius intercepts `tae_scan()`
table functions and replaces the CPU-based DuckDB scan with a dedicated GPU pipeline.
This avoids the DuckDB→host→GPU roundtrip: compressed TAE data goes directly from disk
to GPU memory, with decompression and decoding performed entirely on the GPU.

### 13.1 Architecture

```
              CPU (Host)                                     GPU (Device)
┌────────────────────────────────────┐          ┌──────────────────────────────────────┐
│           tae_scan_task            │          │            GPU Pipeline              │
│                                    │          │                                      │
│  1. Parse TAE metadata             │          │                                      │
│  2. Object-level zone-map pruning  │          │                                      │
│     (skip objects that cannot      │          │                                      │
│      match filter predicates)      │          │                                      │
│  3. Block-level zone-map pruning   │          │                                      │
│  4. Coalesced pread() — merge      │   H2D    │  6. nvCOMP LZ4 batch decompress      │
│     adjacent reads into single     │          │                                      │
│     I/O calls (cap: 32 per group)  │          │  7. CUDA decode kernels:             │
│  5. CRC-strip into pinned host    ─┼─────────►│     • fixed-width (strip header,     │
│     memory (data stays compressed) │  async   │       epoch adjust DATE/TIMESTAMP)   │
│                                    │          │     • varchar (varlena → cudf        │
│  host_tae_representation           │          │       offsets + chars via CUB)       │
│  ┌──────────────────────────────┐  │          │     • null mask XOR inversion        │
│  │ col_chunk[0]: compressed     │  │          │                                      │
│  │ col_chunk[1]: compressed     │  │          │  8. cudf::compute_column()           │
│  │ col_chunk[2]: compressed     │  │          │     (evaluate filter predicates)     │
│  │ ...                          │  │          │                                      │
│  │ filter_expression            │  │          │  9. cudf::apply_boolean_mask()       │
│  │ post_filter_projection_ids   │  │          │     (prune filtered rows)            │
│  └──────────────────────────────┘  │          │                                      │
└────────────────────────────────────┘          │ 10. Post-filter column projection    │
                                                │     (remove filter-only columns)     │
                                                │                                      │
                                                │          cudf::table                 │
                                                │              │                       │
                                                └──────────────┼───────────────────────┘
                                                               ▼
                                                      Sirius GPU engine
                                                    (joins, aggregation)
```

### 13.2 Components

**Scan operator & task:**

| File | Description |
|------|-------------|
| `src/op/sirius_physical_gpu_tae_scan.cpp` | Scan operator — translates DuckDB filter expressions to cudf AST (index-based column references) |
| `src/op/scan/tae_scan_task.cpp` | Scan task — reads TAE metadata + compressed column data into pinned host memory with zone-map pruning, coalesced I/O, and CRC stripping |
| `src/include/op/sirius_physical_gpu_tae_scan.hpp` | Scan operator header |
| `src/include/op/scan/tae_scan_task.hpp` | Scan task header (global/local state classes) |

**Data representation & conversion:**

| File | Description |
|------|-------------|
| `src/include/data/host_tae_representation.hpp` | Holds compressed column chunks with metadata (extent, width, scale, MO OID) |
| `src/data/host_tae_representation.cpp` | Representation implementation |
| `src/data/host_tae_representation_converters.cpp` | GPU pipeline: H2D transfer → nvCOMP LZ4 → CUDA decode → filter → projection |
| `src/include/data/host_tae_representation_converters.hpp` | Converter header |

**CUDA kernels:**

| File | Description |
|------|-------------|
| `src/cuda/tae/tae_decode_fixed.cu` | Fixed-width column decode: strip IOEntryHeader, epoch adjustment for DATE/TIMESTAMP |
| `src/cuda/tae/tae_decode_varchar.cu` | Varchar decode: MO varlena → cudf offsets+chars via CUB exclusive prefix sum |
| `src/cuda/tae/tae_null_mask.cu` | Null mask inversion: MO bit=1=null → cudf bit=0=null (XOR) |
| `src/include/cuda/tae/tae_decode_kernels.hpp` | Kernel declarations |

**TAE format:**

| File | Description |
|------|-------------|
| `src/tae/tae_metadata_parser.cpp` | Parses block/column metadata from TAE object headers |
| `src/include/tae/tae_format.hpp` | TAE format structs (Extent, ObjectMeta, MOType enums) |

### 13.3 Column Projection Semantics

The TAE scan task receives three projection-related inputs from DuckDB:

- **`column_ids`**: ALL columns needed by the scan (output + filter-only)
- **`projection_ids`**: Maps indices into `column_ids`. First `types.size()` entries
  are output columns; remaining entries are filter-only columns
- **`types`**: Output column DuckDB types. `types.size()` = number of output columns

The scan task builds `post_filter_projection_ids` from only the first
`min(projection_ids.size(), types.size())` entries, ensuring filter-only columns are
excluded from the final output after GPU filter evaluation.

Column ordering in the converter uses `column_ids` position (not raw column seqnum)
to match DuckDB's `batch_column_map` expectations. This is critical because the
converter's `std::map<uint16_t, ...>` is sorted by key, and using raw seqnum would
produce incorrect column ordering for queries that project columns in non-sequential
order.

### 13.4 Type Handling

- **Decimal**: Scale is extracted from DuckDB's `returned_types` via
  `DecimalType::GetScale()`, not hardcoded. cuDF uses negative scale:
  `DECIMAL(15,2)` → `cudf::data_type{DECIMAL64, -2}`
- **Date/Timestamp**: CUDA kernel adjusts epoch from MO's `0001-01-01` to
  cuDF/Unix epoch `1970-01-01`
- **Varchar**: MO stores varlena with inline length prefix. CUDA kernel extracts
  string lengths, runs CUB exclusive prefix sum to build cudf offsets array, then
  copies string data contiguously

### 13.5 Filter Pushdown

The scan operator translates DuckDB filter expressions to cudf AST expressions using
**index-based** `cudf::ast::column_reference` (not `column_name_reference`). This is
required because `cudf::compute_column()` does not support name-based references.

The filter is evaluated on GPU after decompression/decoding:
1. `cudf::compute_column(table, filter_ast)` → boolean mask
2. `cudf::apply_boolean_mask(table, mask)` → filtered table
3. Post-filter projection removes filter-only columns

### 13.6 Scan Optimizations

**Object-level zone-map pruning:**
Before opening a file, the scan task checks the sort-key zone map from the manifest
(`sort_key_zm` field, a 64-byte ZM encoding min/max for the sort column). If the
pushed-down filter predicates cannot match the object's sort-key range, the entire
object is skipped. This avoids disk I/O for objects that cannot contribute rows.
Note: MO does not populate `zone_map` in manifests at SF10; this optimization
becomes effective at larger scale factors where manifests include zone maps.

**Block-level zone-map pruning:**
After parsing object metadata, the scan task examines per-block zone maps for each
filter column. Blocks whose zone map range is disjoint from the filter predicate are
excluded from the read plan. This reduces the amount of data read and transferred to
the GPU.

**Coalesced I/O:**
After sorting the read plan by file offset, adjacent reads are merged into single
`pread()` calls. Groups are capped at `MAX_COALESCE_GROUP = 32` to bound temporary
memory (~3.3 MB per group). For CRC-wrapped files (all local MO files use
4-byte CRC prefix per 2048-byte physical block), the scan reads the physical range
once and strips CRC headers in memory. For non-CRC files (e.g., S3 objects), the
scan reads directly into the pinned buffer.

Typical coalescing at SF10 (lineitem, 72 blocks × 5 columns):
- COUNT(*): 72 reads → 3 I/O calls
- Q1 (5 columns): 360 reads → 12 I/O calls

### 13.7 Performance Characteristics

**TPC-H SF10 on RTX 3070 (8 GB VRAM):**

| Path               | Total (22 queries) | Per-query avg | Relative to native |
|--------------------|-------------------:|:-------------:|:------------------:|
| Native MatrixOne   |          4,825 ms  |     219 ms    |        1.0×        |
| CPU sidecar        |          3,463 ms  |     157 ms    |   **1.4× faster**  |
| GPU TAE sidecar    |          9,135 ms  |     415 ms    |    0.5× (slower)   |

At SF10, GPU overhead (host→GPU transfer, kernel launch latency) dominates because
per-query data volumes are small (~100 MB compressed per query). The GPU path is
designed for SF100+ workloads where parallelism and memory bandwidth overcome transfer
overhead. At SF100, a single lineitem scan processes ~5 GB compressed data where GPU
decompression at ~300 GB/s (nvCOMP LZ4) far exceeds CPU throughput of ~4 GB/s.

---

## 14. Phased Implementation Plan

### 14.1 Local Filesystem + Static Schema ✅ Complete

**Goal:** Run TPC-H queries on GPU via Sirius, reading MO data from local disk.

**Implemented:**
- ✅ Read TAE objects from `mo-data/shared/` via DuckDB FileSystem (local + S3)
- ✅ Schema provided via JSON manifest file
- ✅ Full MO type support: int8–uint64, float32/64, bool, decimal64/128, date,
  datetime, timestamp, char, varchar, text, json, blob, binary, varbinary, uuid, enum
- ✅ Zone map predicate pushdown (block-level skip)
- ✅ Per-row filter evaluation (all supported types)
- ✅ CONSTANT vector support (single-value broadcast)
- ✅ Projection pushdown (column pruning)
- ✅ Parallel block scanning (atomic work dispatch, multi-threaded)
- ✅ Planner statistics (cardinality + column min/max from zone maps + null info from metadata)
- ✅ Targeted CRC reads (reads only needed bytes from CRC-wrapped files, not entire file)
- ✅ count(*) metadata fast path (zero file I/O, resolves from manifest metadata)
- ✅ Virtual columns (file_path, block_id)
- ✅ Sampling pushdown (TABLESAMPLE SYSTEM via Bernoulli)
- ✅ ORDER BY pushdown (set_scan_order: block reorder + limit pruning)
- ✅ EXPLAIN integration (toString, dynamicToString)
- ✅ Progress reporting
- ✅ LZ4 decompression
- ✅ Catch2 tests across 6 test files

**Deliverables:**
- `tae_scanner.duckdb_extension` DuckDB extension (~1,100 lines C++)
- Manifest generation via MO's `/debug/tae/manifest` HTTP API
- `test/gen_test_data.py` binary test data generator (~900 lines Python)

### 14.2 S3 Support + Read Coalescing

**Goal:** Read TAE objects from S3 (production MO deployments).

**Scope:**
- ✅ Use DuckDB's `FileSystem` abstraction for transparent local/S3 routing (DONE)
- HTTP Range request coalescing (batch column reads into one request)
- Tombstone filtering

**Key change:** `TAEObjectReader` now uses `duckdb::FileSystem::Read()` instead of
`pread()`. DuckDB's httpfs extension handles S3 authentication and range requests
transparently — any `s3://` path in the manifest works out of the box.

### 14.3 MO-Side Manifest Generation (Query Offload)

**Goal:** MO generates manifests dynamically from its in-memory catalog when
offloading analytical queries to the DuckDB/Sirius sidecar.

**Scope:**
- Go serializer in MO that converts catalog state → manifest JSON
- MVCC-correct: uses transaction snapshot for object visibility
- Zone maps included from ObjectStats for fast object pruning
- Tombstones skipped (analytics use case; compacted objects have minimal deletes)
- Manifest written to tmpfile or passed inline to DuckDB

#### 14.3.1 Why MO Generates the Manifest

Two approaches were considered:

| Approach | Freshness | Consistency | Coupling | Complexity |
|----------|-----------|-------------|----------|------------|
| **A: MO serializes in-memory catalog** | Perfect | MVCC snapshot | Tight | Low |
| **B: Read checkpoint files directly** | Stale (minutes) | Checkpoint-level | Loose | High |

**Approach A is correct for query offloading** — only MO's live catalog can
guarantee the exact MVCC snapshot. Approach B is useful for offline analytics
(periodic ETL, reporting) where staleness is acceptable.

#### 14.3.2 MO Catalog APIs

The serializer calls these MO APIs:

```go
// 1. Get table schema
schema := tableEntry.GetLastestSchema(false) // false = data objects
for _, col := range schema.ColDefs {
    // col.Name, col.Type.Oid, col.Type.Width, col.Type.Scale, col.NullAbility
}

// 2. Iterate visible objects at transaction snapshot
it := tableEntry.MakeDataVisibleObjectIt(txn)
defer it.Release()
for it.Next() {
    obj := it.Item()
    stats := obj.GetObjectStats()
    // stats.ObjectName().String()  → "uuid_00001"
    // stats.Rows()                 → row count
    // stats.BlkCnt()               → block count
    // stats.Size()                 → compressed size
    // stats.SortKeyZoneMap()       → 64-byte zone map
}
```

Key types:
- `catalog.TableEntry` → `GetLastestSchema()`, `MakeDataVisibleObjectIt(txn)`
- `catalog.ObjectEntry` → `GetObjectStats()`, `IsVisible(txn)`
- `objectio.ObjectStats` (154 bytes) → `ObjectName()`, `Rows()`, `BlkCnt()`, `SortKeyZoneMap()`

#### 14.3.3 ObjectStats Layout (154 bytes)

```
Offset  Size  Field
  0      60   object_name  (uuid[16] + filenum[2] + namestring[42])
 60      13   extent       (alg[1] + offset[4] + length[4] + origin[4])
 73       4   row_cnt
 77       4   blk_cnt
 81      64   zone_map     (sort key min/max, same format as block zone maps)
145       4   object_size  (compressed)
149       4   origin_size  (uncompressed)
153       1   reserved     (flags: appendable, sorted, cn_created; bits 5-7: level)
```

The zone map at offset 81 is the **sort key zone map** — same 64-byte format
used inside `.tae` block metadata. MO's `/debug/tae/manifest` API extracts and
hex-encodes it into the manifest's `zone_map` field.

#### 14.3.4 Manifest Serializer (Go Pseudocode)

```go
func GenerateManifest(txn txnif.TxnReader, table *catalog.TableEntry,
                      dataDir string) ([]byte, error) {
    schema := table.GetLastestSchema(false)

    // Build column list
    columns := make([]ManifestColumn, 0, len(schema.ColDefs))
    var sortColumn string
    for _, col := range schema.ColDefs {
        if col.Hidden { continue }
        columns = append(columns, ManifestColumn{
            Name:  col.Name,
            OID:   int(col.Type.Oid),
            Width: int(col.Type.Width),
            Scale: int(col.Type.Scale),
        })
        if col.IsSortKey() { sortColumn = col.Name }
    }

    // Enumerate visible objects
    objects := make([]ManifestObject, 0)
    it := table.MakeDataVisibleObjectIt(txn)
    defer it.Release()
    for it.Next() {
        stats := it.Item().GetObjectStats()
        obj := ManifestObject{
            Path:   stats.ObjectName().String(),
            Rows:   int(stats.Rows()),
            Blocks: int(stats.BlkCnt()),
            Size:   int(stats.Size()),
        }
        zm := stats.SortKeyZoneMap()
        if !zm.IsEmpty() {
            obj.ZoneMap = hex.EncodeToString(zm[:])
        }
        objects = append(objects, obj)
    }

    manifest := Manifest{
        Database:   table.GetDB().GetName(),
        Table:      schema.Name,
        DataDir:    dataDir,
        SortColumn: sortColumn,
        Columns:    columns,
        Objects:    objects,
    }
    return json.Marshal(manifest)
}
```

**Properties:**
- MVCC-correct: `MakeDataVisibleObjectIt(txn)` filters by snapshot
- Zero I/O: all data from in-memory catalog
- Latency: microseconds (JSON serialization of ~100s of objects)
- Tombstone-aware: deleted objects filtered by visibility iterator

#### 14.3.5 Tombstone Handling (Deferred)

MO uses **soft deletes** — tombstone objects (`.tae` files with `object_type=2`)
record deleted row IDs. Each tombstone row contains:

```
Col 0: __mo_%1_delete_rowid  (Rowid, 24 bytes = blockid[20] + row_offset[4])
Col 1: __mo_%1_pk_val        (table PK type)
Col 2: __mo_%1_commit_time   (TS)
```

At read time, MO builds a deletion bitmap per block from matching tombstones.

**Current status:** Tombstones are **not handled** by the DuckDB scanner.
For analytics workloads on compacted data, this is acceptable — compaction
merges data and applies tombstones, producing clean objects with no deletes.

**Future options:**
1. MO pre-computes deletion bitmaps and includes them in the manifest
2. Scanner reads tombstone objects and applies filtering itself
3. MO only offloads queries on fully-compacted tables

#### 14.3.6 End-to-End Offload Flow

```
┌──────────────────────────────────────────────────────────┐
│ User: SELECT ... FROM lineitem WHERE l_shipdate > '1995' │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────┐
│ MO Optimizer                     │
│ • Decides to offload to sidecar  │
│ • Calls GenerateManifest(txn)    │
│ • Writes /tmp/manifest_xxx.json  │
└──────────────────────┬───────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│ DuckDB Sidecar                                           │
│ SELECT * FROM tae_scan('/tmp/manifest_xxx.json')         │
│   WHERE l_shipdate > '1995-01-01'                        │
│                                                          │
│ • Reads manifest → discovers 50 objects                  │
│ • Zone map fast path → skips 30 objects (no ReadMeta)    │
│ • Block-level pruning → skips 15 more blocks             │
│ • Scans 5 objects, returns results                       │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────┐
│ MO Frontend                      │
│ • Returns results to client      │
└──────────────────────────────────┘
```

#### 14.3.7 Manifest Generation Tools

| Tool | Use Case | Data Source |
|------|----------|-------------|
| MO `/debug/tae/manifest` API | Production & dev | MO in-memory catalog (HTTP) |
| Go serializer (planned) | Production query offload | MO in-memory catalog |
| Checkpoint reader (future) | Offline analytics, MO is down | `.ckp` files on shared storage |

#### 14.3.8 Approach B: Checkpoint File Reader (Offline Analytics)

For use cases where MO is unavailable or staleness is acceptable (periodic ETL,
reporting dashboards, backup verification), a standalone tool can generate
manifests by reading checkpoint files directly from shared storage.

**Checkpoint file structure:**

```
ckp/
├── meta_<start_ts>_<end_ts>.ckp    # Checkpoint metadata
└── <data_objects>.ckp               # Checkpoint data batches
```

Checkpoint metadata contains a **TableRange** batch with columns:

```
Col 0: table_id     (uint64)
Col 1: object_type  (int8: 1=Data, 2=Tombstone)
Col 2: start_rowid  (Rowid)
Col 3: end_rowid    (Rowid)
Col 4: objectStats  (bytes, 154-byte ObjectStats)
```

Each row represents one object. The `objectStats` field is the same 154-byte
binary structure described in §14.3.3, containing the object name, row/block
counts, zone map, and sizes.

**Reading algorithm:**

```go
func GenerateManifestFromCheckpoint(ckpPath string, tableID uint64,
                                     dataDir string) ([]byte, error) {
    // 1. List checkpoint metadata files
    metaFiles := ckputil.ListCKPMetaFiles(ckpPath)
    latestMeta := metaFiles[len(metaFiles)-1]

    // 2. Read metadata batch
    metaBatch := ckputil.ReadMetaBatch(latestMeta)

    // 3. Filter for target table's data objects
    ranges := ckputil.ExportToTableRangesByFilter(
        metaBatch, tableID, ObjectType_Data)

    // 4. Extract ObjectStats from each range
    objects := make([]ManifestObject, 0)
    for _, r := range ranges {
        stats := r.ObjectStats
        obj := ManifestObject{
            Path:   stats.ObjectName().String(),
            Rows:   int(stats.Rows()),
            Blocks: int(stats.BlkCnt()),
            Size:   int(stats.Size()),
        }
        zm := stats.SortKeyZoneMap()
        if !zm.IsEmpty() {
            obj.ZoneMap = hex.EncodeToString(zm[:])
        }
        objects = append(objects, obj)
    }

    // 5. Schema must be obtained separately (not in checkpoint)
    //    Options: hardcode, read from MO SQL, or embed in a config file
    ...
}
```

**Challenges vs Approach A:**

| Challenge | Detail |
|-----------|--------|
| **Schema not in checkpoint** | Checkpoint stores ObjectStats but not column names/types. Schema must come from MO SQL, a config file, or the `COLUMNS_META` system table in checkpoint |
| **Incremental checkpoints** | MO produces incremental checkpoints; the reader must merge global + incremental to get the full object list |
| **MVCC filtering** | Checkpoint objects have `create_ts` and `delete_ts`; the reader must apply timestamp filtering to get a consistent view |
| **Table ID discovery** | Must know the numeric `table_id`; requires either MO SQL or scanning the checkpoint's catalog tables |
| **Format versioning** | Checkpoint format changes across MO versions (currently `CheckpointVersion13`); reader must track format evolution |

**When to use Approach B:**
- MO is offline for maintenance and analytics must continue
- Data lake export: periodic snapshot of MO tables for external tools
- Backup verification: confirm checkpoint data is readable
- Development: generate manifests without running MO

**When NOT to use:**
- Real-time query offloading (use Approach A — need MVCC consistency)
- Tables with active deletes (tombstones require transaction context)

### 14.4 Substrait-Based Query Routing (MO → Sirius)

**Goal:** MO offloads analytical queries to Sirius automatically via Substrait plans.

**Scope:**
- MO compiles query to its plan protobuf, converts to Substrait
- Sirius receives Substrait plan and executes on GPU
- Results returned via gRPC to MO frontend, sent to client
- See §18 for full architecture

---

## 15. Build and Test

### 15.1 Prerequisites

```bash
# LZ4
sudo apt install liblz4-dev  # Ubuntu/Debian
pacman -S lz4                 # Arch/Manjaro
brew install lz4              # macOS
```

### 15.2 Build

```bash
cd duckdb-tae-scanner
make release
```

### 15.3 Unit Tests (Catch2)

```bash
# Generate test data (run from project root, not test/)
cd duckdb-tae-scanner
python3 test/gen_test_data.py

# Run all tests
cd build/release && ./test/tae_tests

# Run specific test tags
./test/tae_tests "[types]"        # decimal, UUID, blob tests
./test/tae_tests "[scan]"         # basic scan tests
./test/tae_tests "[filter]"       # filter pushdown tests
./test/tae_tests "[virtual]"      # virtual column tests
./test/tae_tests "[sampling]"     # sampling tests
./test/tae_tests "[constant]"     # CONSTANT vector tests
```

### 15.4 Test with TPC-H

```bash
# 1. Start MO with local filesystem
mo-service -launch etc/launch/launch.toml  # backend=DISK

# 2. Load TPC-H data
mysql -h 127.0.0.1 -P 6001 -u root < tpch_load.sql

# 3. Flush all tables
mysql -h 127.0.0.1 -P 6001 -u root -e "SELECT mo_ctl('dn', 'flush', 'tpch.lineitem')"

# 4. Generate manifest via MO debug HTTP API
curl 'http://127.0.0.1:8888/debug/tae/manifest?table=tpch.lineitem' \
    > lineitem_manifest.json

# 5. Run in DuckDB with Sirius
duckdb -cmd "LOAD 'tae_scanner'; LOAD 'sirius';"
D> CALL gpu_execution('
     SELECT l_returnflag, l_linestatus,
            SUM(l_quantity), SUM(l_extendedprice)
     FROM tae_scan('lineitem_manifest.json')
     WHERE l_shipdate <= DATE ''1998-09-02''
     GROUP BY l_returnflag, l_linestatus
   ');
```

### 15.5 TPC-H SF100 Benchmark Results

Measured on 24-core machine with local NVMe storage, DuckDB CLI (24 threads):

| Query | Wall Time | CPU Time | Description |
|-------|-----------|----------|-------------|
| count(*) | 0.2s | — | Metadata fast path, zero file I/O (600M rows) |
| Q1 | 2.6s | 51s | Single-table aggregation + group by |
| Q2 | 1.2s | 6s | Correlated subquery, 5-way join |
| Q3 | 2.8s | 39s | 3-way join (customer/orders/lineitem) |
| Q4 | 2.4s | 33s | Semi-join (EXISTS subquery) |
| Q5 | 1.9s | 35s | 6-way join with VARCHAR filter |
| Q6 | 1.0s | 20s | Single-table scan + multi-column filter |
| Q7 | 2.2s | 39s | 6-way join, two-nation shipping |
| Q8 | 2.2s | 39s | 8-way join, market share |
| Q9 | 8.2s | 121s | 6-way join, LIKE filter, profit calc |
| Q10 | 4.1s | 57s | 4-way join, returned items |
| Q11 | 0.2s | 2s | 3-way join, HAVING subquery |
| Q12 | 3.6s | 45s | 2-way join, shipping mode priority |
| Q13 | 7.8s | 125s | LEFT OUTER JOIN, NOT LIKE filter |
| Q14 | 1.5s | 25s | 2-way join, promotion revenue |
| Q15 | 1.3s | 22s | CTE with max aggregate, top supplier |
| Q16 | 1.0s | 16s | 2-way join, NOT IN anti-join |
| Q17 | 1.0s | 18s | Correlated subquery, small parts |
| Q18 | 6.7s | 99s | 3-way join, HAVING, large quantities |
| Q19 | 4.0s | 50s | 2-way join, complex OR predicates |
| Q20 | 1.3s | 21s | Nested subqueries, forest parts |
| Q21 | 6.5s | 120s | 4-way join, EXISTS + NOT EXISTS |
| Q22 | 0.8s | 16s | Subquery, NOT EXISTS, global customers |

---

## 16. Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| TAE format changes in MO upgrades | Scanner breaks | Version field in header; pin MO version for initial release |
| Unflushed data invisible | Incorrect query results | Explicit `mo_ctl flush` after writes; document limitation |
| Zone map encoding mismatch | Wrong predicate pushdown | Validate against MO's own zone map evaluation; fuzz test |
| LZ4 version incompatibility | Decompression failure | Use system LZ4 lib; MO uses standard LZ4 frame format |
| Large varlena strings | Memory pressure | Stream blocks; don't buffer entire object |
| Decimal precision loss | Wrong numeric results | Use DuckDB's native DECIMAL(p,s); verify with TPC-H Q1 |

---

## 17. References

- MatrixOne source: `pkg/objectio/` — TAE object I/O layer
- MatrixOne source: `pkg/container/vector/` — vector serialization
- MatrixOne source: `pkg/container/types/` — type definitions
- MatrixOne source: `pkg/fileservice/local_fs.go` — local filesystem backend
- MatrixOne source: `pkg/frontend/mysql_cmd_executor.go` — query dispatch
- MatrixOne source: `pkg/sql/compile/compile.go` — plan compilation to pipelines
- DuckDB extension API: `src/include/duckdb/function/table_function.hpp`
- Sirius-DB: DuckDB extension for GPU-native SQL execution
- Sirius source: `src/sirius_engine.cpp` — pipeline-based GPU execution
- Sirius source: `substrait/src/` — DuckDB↔Substrait conversion layer
- Substrait specification: https://substrait.io/
- LZ4: https://github.com/lz4/lz4

---

## 18. MO → Sidecar Integration Architecture

This section describes how MatrixOne routes analytical queries to the DuckDB sidecar
for accelerated execution. The implemented approach is **SQL String Forwarding via HTTP**.

### 18.1 Three Integration Paths

| | Path 1: Manual CLI | Path 2: SQL Forwarding | Path 3: Substrait Plan |
|---|---|---|---|
| **User interface** | DuckDB CLI directly | Transparent (MO SQL) | Transparent (MO SQL) |
| **MO involvement** | None | Query router + SQL rewriter | Query router + Substrait converter |
| **What crosses gRPC** | N/A (no gRPC) | SQL string | Binary Substrait protobuf |
| **MO optimizer used?** | No | No (DuckDB re-plans) | Yes (plan preserved) |
| **Sirius changes** | None | None | 1 line (tae_scan scan type) |
| **MO changes** | None | ~500 LOC (router + gRPC) | ~2000 LOC (router + converter) |
| **Plan quality** | DuckDB optimizer only | DuckDB optimizer only | MO + DuckDB combined |
| **Recommended for** | Dev/testing | Quick production MVP | Production target |

**Path 1: Manual CLI** — User talks directly to DuckDB+Sirius:
```sql
-- User runs this in DuckDB shell
CALL gpu_execution('SELECT l_returnflag, SUM(l_quantity)
    FROM tae_scan(''/path/to/manifest.json'')
    GROUP BY 1');
```

**Path 2: SQL String Forwarding** — MO intercepts query, rewrites table names,
forwards to sidecar:
```
User → MO → rewrites "lineitem" → "tae_scan('/manifest/lineitem.json')"
         → sends SQL string via gRPC → Sirius sidecar → GPU → results → MO → client
```

**Path 3: Substrait Plan Exchange** — MO builds plan, converts to Substrait protobuf,
sends binary plan to sidecar:
```
User → MO → builds optimized plan → converts to Substrait bytes
         → sends protobuf via gRPC → Sirius from_substrait() → GPU → results → MO → client
```

### 18.2 Sirius Sidecar Process

All integration paths (except Path 1) require a **sidecar process** — a separate
DuckDB instance running Sirius and tae_scanner extensions on a GPU-equipped machine.

```
┌────────────────────────────────────────────────────────────────┐
│                    Sirius Sidecar Process                      │
│                                                                │
│  DuckDB Runtime                                                │
│  ├── tae_scanner extension (reads TAE objects from filesystem) │
│  ├── sirius extension (GPU execution via CUDA/cuDF)            │
│  └── substrait extension (plan deserialization)                │
│                                                                │
│  gRPC Server                                                   │
│  ├── ExecuteSQL(sql_string) → Arrow IPC batches                │
│  └── ExecutePlan(substrait_bytes) → Arrow IPC batches          │
│                                                                │
│  GPU Resources                                                 │
│  ├── CUDA streams + cuDF operators                             │
│  ├── RMM memory pool (GPU + host + disk tiering)               │
│  └── cuCascade (GPU memory management)                         │
└────────────────────────────────────────────────────────────────┘
```

**Why a separate process (not embedded in MO):**
- Sirius is C++ linking CUDA/cuDF — cannot load into a Go binary
- GPU memory management (RMM, cuCascade) needs its own address space
- Crash isolation — GPU OOM in Sirius doesn't kill MO
- Independent scaling — sidecar on GPU node, MO on CPU nodes

**Deployment options:**
- **Same machine:** MO and sidecar share filesystem, gRPC over localhost
- **Separate machines:** Sidecar on GPU server, shared NFS/S3 for TAE files
- **Container:** Sidecar in `nvidia/cuda` image with DuckDB + extensions

**Startup:**
```bash
# Start sidecar (loads extensions, starts gRPC server)
sirius-sidecar --port 50051 \
    --extensions "sirius,tae_scanner" \
    --data-dir /mo-data/shared/
```

### 18.3 How Sirius Works Today

Sirius is a DuckDB extension that intercepts physical plans and routes operators to GPU.
The current entry point is:

```sql
-- User talks directly to DuckDB+Sirius
CALL gpu_execution('SELECT l_returnflag, SUM(l_quantity) FROM lineitem GROUP BY 1');
```

**Execution flow inside Sirius:**
1. `GPUExecutionBind`: Parse SQL → DuckDB logical plan → `sirius_physical_plan_generator`
2. `sirius_engine::initialize_internal`: Build multi-stage GPU pipelines with partitioning
3. Scan executor reads data (Parquet/DuckDB/Iceberg scan tasks)
4. GPU executor runs operators via CUDA/cuDF on pipelines
5. Results returned as DuckDB DataChunks

**Scan operator detection** (`sirius_engine.cpp`):
```cpp
if (scan_op.function.name == "parquet_scan")  → sirius_physical_parquet_scan
if (scan_op.function.name == "iceberg_scan")  → sirius_physical_iceberg_scan
if (scan_op.function.name == "seq_scan")      → sirius_physical_duckdb_scan
if (scan_op.function.name == "tae_scan")      → sirius_physical_tae_scan
```

`tae_scan()` has a dedicated GPU scan operator (`sirius_physical_tae_scan`) that
reads compressed TAE data into host pinned memory and decodes entirely on GPU
via nvCOMP LZ4 + custom CUDA kernels. See §13 for details.

### 18.4 Substrait as the Plan Exchange Format

Substrait is a cross-database query plan format supported by DuckDB (via extension),
Apache Arrow, Velox, and others. Sirius already bundles the DuckDB Substrait extension.

**Key Substrait concepts:**
- **Plan**: Top-level container with relations and extension references
- **Rel (Relation)**: An operation — ReadRel, FilterRel, AggregateRel, JoinRel, etc.
- **ReadRel**: Data source — NamedTable, LocalFiles (Parquet/ORC), or ExtensionTable
- **Expression**: Literals, field references, function calls, casts
- **ExtensionTable**: Custom data source — **this is where `tae_scan` maps to**

**DuckDB's Substrait SQL API** (already available in Sirius):
```sql
SELECT * FROM get_substrait('SELECT a, SUM(b) FROM t GROUP BY a');    → BLOB
SELECT * FROM get_substrait_json('SELECT a FROM t WHERE a > 5');      → JSON
CALL from_substrait(x'12090a...');                                     → results
CALL from_substrait_json('{"relations":[...]}');                       → results
```

### 18.5 Path 2 Detail: SQL String Forwarding

MO rewrites SQL, replacing table names with `tae_scan()` calls, and forwards the
SQL string to the sidecar. The sidecar calls `gpu_execution()` internally.

**MO-side changes (~500 LOC):**
- GPU query router in `pkg/frontend/mysql_cmd_executor.go` at `executeStmtWithResponse()`
- Heuristic: route `SELECT` with aggregates/joins on large tables to sidecar
- New package `pkg/sql/colexec/sirius/` with gRPC client
- Query rewriting: replace table names with `tae_scan('/path/to/manifest.json')`

**Sidecar receives:**
```
"SELECT l_returnflag, SUM(l_quantity) FROM tae_scan('/mo-data/tpch/lineitem.json') GROUP BY 1"
```

**Pros:** No Sirius code changes. Simple deployment. MO changes are minimal.
**Cons:** Extra process to manage. SQL-string interface loses type safety.

### 18.6 Why Not Embed Sirius in MO? (CGO/FFI Infeasibility)

A natural question: why not link Sirius as a shared library (`.so`) and call it
directly from MO via CGO, eliminating the sidecar and gRPC entirely?

```
# The "embed" approach (rejected)
MO (Go) ──CGO──→ libsirius.so (C++) ──CUDA──→ GPU
```

This is infeasible for multiple compounding reasons:

#### 1. C++ Runtime Dependency Chain

Sirius is not a thin C library. It embeds the entire DuckDB engine (~500K LOC C++)
plus CUDA/cuDF/RMM GPU libraries. The dependency graph:

```
libsirius.so
├── libduckdb.so          (~500K LOC: parser, optimizer, executor, buffer pool)
├── libcudf.so            (GPU columnar engine, depends on librmm, libraft)
├── librmm.so             (GPU memory pool manager)
├── libcudart.so          (CUDA runtime)
├── libnccl.so            (optional: multi-GPU communication)
└── libsubstrait.so       (protobuf-based plan deserializer)
```

CGO requires `extern "C"` wrappers. Every DuckDB/Sirius API that MO calls would
need a C shim, and complex C++ objects (`shared_ptr<DataChunk>`, `unique_ptr<QueryResult>`,
cuDF `table_view`) cannot cross the CGO boundary. You'd need to serialize/deserialize
at every call — at which point you've reinvented gRPC, but worse.

#### 2. Memory Allocator Conflict

Go's runtime manages heap via `mmap` + GC. DuckDB has its own buffer pool manager.
CUDA/RMM manages GPU memory + host-pinned memory pools. Three competing allocators
in one process:

- Go's GC scans the entire heap for pointers. DuckDB's buffer pool contains opaque
  binary data that the GC cannot understand — it would either corrupt the data
  (if it tries to move it) or waste CPU scanning it.
- RMM uses `cudaMallocManaged()` and custom memory pools. These pages must not be
  touched by Go's GC.
- CGO pins goroutines to OS threads during C calls. CUDA kernels are asynchronous
  and use their own thread pool. A long-running GPU kernel would block the goroutine's
  OS thread, consuming one of Go's limited M threads.

#### 3. Crash Isolation

GPU errors are catastrophic and unrecoverable:
- **CUDA OOM:** `cudaErrorMemoryAllocation` — no graceful recovery
- **GPU kernel timeout:** The OS watchdog kills the CUDA context
- **Driver crash:** `cudaErrorUnknown` — the entire process is corrupted
- **ECC error:** Hardware fault, all GPU state is lost

In the sidecar model, a GPU crash kills only the sidecar process. MO detects the
gRPC connection drop, returns an error to the client, and can restart the sidecar.
In the embedded model, a GPU crash kills MO — taking down all connected clients,
all in-flight transactions, and the entire CN node.

#### 4. Build System Complexity

MO builds with `go build`. Embedding Sirius would require:
- CUDA toolkit (11.x+) installed on every build machine
- cuDF/RMM C++ libraries compiled and linked
- DuckDB compiled as a shared library with matching ABI
- CGO cross-compilation flags for CUDA (`-gencode arch=compute_80,...`)
- CI/CD: GPU-capable build agents (expensive)

Every MO developer would need a GPU dev environment just to build, even if they
never touch the Sirius integration.

#### 5. Scaling and Deployment

The sidecar model allows independent scaling:
- MO CN nodes run on CPU-optimized machines (many cores, large RAM)
- Sirius sidecar runs on GPU machines (A100/H100, expensive)
- Multiple MO CN nodes can share one Sirius sidecar (or a pool of them)
- Sirius can be upgraded independently without restarting MO

In the embedded model, every MO CN node needs a GPU, which is wasteful — most
queries go through the CPU path and don't need GPU acceleration.

#### Summary

| Factor | Sidecar (gRPC) | Embedded (CGO) |
|--------|----------------|----------------|
| Integration effort | ~500 LOC gRPC client | ~5000+ LOC C shims + CGO |
| Crash isolation | ✅ sidecar dies, MO lives | ❌ GPU crash kills MO |
| Memory safety | ✅ separate address spaces | ❌ GC vs RMM conflict |
| Build requirement | Go only (MO), C++/CUDA (sidecar) | CUDA toolkit for all MO builds |
| Scaling | ✅ independent CPU/GPU nodes | ❌ every CN needs GPU |
| Latency overhead | ~0.1ms per gRPC call | ~0 (in-process) |
| Upgrade independence | ✅ restart sidecar only | ❌ rebuild MO |

The ~0.1ms gRPC latency is negligible compared to GPU execution time (typically
10ms–10s for analytical queries). The sidecar architecture is strictly superior
for production use.

