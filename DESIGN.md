# MatrixOne DuckDB Sidecar — Design Document

## Analytical Query Offload via DuckDB TAE Scanner

---

## 1. Executive Summary

This document describes the architecture of the **mo-sirius-sidecar**: a DuckDB-based
query sidecar for **MatrixOne** (a cloud-native HTAP database). Analytical workloads
annotated with `/*+ SIDECAR */` (CPU) or `/*+ SIDECAR GPU */` (GPU) hints in MO are rewritten and forwarded to this sidecar,
which reads TAE storage objects directly and executes queries using DuckDB's vectorized
engine.

The sidecar consists of two statically-linked DuckDB extensions:
- **tae_scanner** — reads MatrixOne TAE object files directly via DuckDB table functions
- **httpserver** — ClickHouse-compatible HTTP interface for receiving queries

**Key properties:**
- Zero-copy read path: `pread()` → LZ4 decompress → fill DuckDB vectors
- Column-level predicate pushdown via TAE zone maps
- Block-level parallelism with DuckDB's morsel-driven execution
- Transparent integration: MO handles the SQL rewriting; clients just add a hint

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     User / Application                       │
│      /*+ SIDECAR [GPU] */ SELECT ... FROM tpch.lineitem WHERE ...  │
└───────────────────────────┬─────────────────────────────────┘
                            │ MySQL protocol
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                       MatrixOne (MO)                         │
│  1. Detect /*+ SIDECAR [GPU] */ hint                         │
│  2. Rewrite: table refs → tae_scan(manifest_url)             │
│  3. If GPU: wrap in gpu_execution()                          │
│  4. POST rewritten SQL to sidecar                            │
│  5. Translate JSONCompact response to MySQL result set        │
└───────────────────────────┬─────────────────────────────────┘
                            │ HTTP POST
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                DuckDB Sidecar (this project)                  │
│  ┌─────────────────────────────────────────────────────┐     │
│  │              httpserver extension                     │     │
│  │  Receives SQL, returns JSONCompact results            │     │
│  └──────────────────────┬──────────────────────────────┘     │
│                          │ SQL execution                      │
│  ┌──────────────────────▼──────────────────────────────┐     │
│  │              tae_scan() Table Function                │     │
│  │  ┌──────┐  ┌──────────┐  ┌───────────┐  ┌────────┐  │     │
│  │  │ Bind │→│  Init    │→│  Execute  │→│ Output │  │     │
│  │  │schema│  │open files│  │read blocks│  │DataChk │  │     │
│  │  └──────┘  └──────────┘  └─────┬─────┘  └────────┘  │     │
│  └────────────────────────────────┼─────────────────────┘     │
└───────────────────────────────────┼─────────────────────────┘
                                    │ pread()
                                    ▼
┌─────────────────────────────────────────────────────────────┐
│                     TAE Object Files                         │
│  mo-data/shared/018e1234-5678-abcd-ef01-234567890abc_00001  │
│  mo-data/shared/018e1234-5678-abcd-ef01-234567890abc_00002  │
│  ...                                                         │
└─────────────────────────────────────────────────────────────┘
                                    ▲
                                    │ flush / compaction
┌─────────────────────────────────────────────────────────────┐
│                       MatrixOne TN                           │
│  INSERT/UPDATE/DELETE → WAL → Flush → TAE objects on disk    │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

1. User issues `CALL gpu_execution('SELECT ... FROM lineitem WHERE ...')`
2. Sirius parses, plans, and generates a scan over `lineitem`
3. DuckDB invokes `tae_scan()` table function (our extension)
4. `tae_scan()` reads TAE object files via `pread()`, LZ4-decompresses, decodes MO
   vectors, and fills DuckDB `DataChunk` output
5. Sirius GPU execution engine processes the chunks on GPU
6. Results returned to user

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
not alternatives. See §16 for the full integration architecture.

Embedding CUDA kernels directly in MO's Go pipeline (via CGO/FFI) was considered
and rejected — see §16.12 for why this is infeasible.

---

## 4. TAE Object Binary Format

MatrixOne stores table data as immutable **TAE objects** on the local filesystem (or S3).
Each object contains up to 256 blocks × 8192 rows = **2,097,152 rows**, stored in
columnar layout with optional LZ4 compression.

### 4.1 File Layout

```
Offset 0                                          Offset EOF
│                                                        │
▼                                                        ▼
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
  │len=N │  inline data (N bytes) │
  │ 1B   │       23B              │
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

## 13. Phased Implementation Plan

### 13.1 Local Filesystem + Static Schema ✅ Complete

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

### 13.2 S3 Support + Read Coalescing

**Goal:** Read TAE objects from S3 (production MO deployments).

**Scope:**
- ✅ Use DuckDB's `FileSystem` abstraction for transparent local/S3 routing (DONE)
- HTTP Range request coalescing (batch column reads into one request)
- Tombstone filtering

**Key change:** `TAEObjectReader` now uses `duckdb::FileSystem::Read()` instead of
`pread()`. DuckDB's httpfs extension handles S3 authentication and range requests
transparently — any `s3://` path in the manifest works out of the box.

### 13.3 MO-Side Manifest Generation (Query Offload)

**Goal:** MO generates manifests dynamically from its in-memory catalog when
offloading analytical queries to the DuckDB/Sirius sidecar.

**Scope:**
- Go serializer in MO that converts catalog state → manifest JSON
- MVCC-correct: uses transaction snapshot for object visibility
- Zone maps included from ObjectStats for fast object pruning
- Tombstones skipped (analytics use case; compacted objects have minimal deletes)
- Manifest written to tmpfile or passed inline to DuckDB

#### 13.3.1 Why MO Generates the Manifest

Two approaches were considered:

| Approach | Freshness | Consistency | Coupling | Complexity |
|----------|-----------|-------------|----------|------------|
| **A: MO serializes in-memory catalog** | Perfect | MVCC snapshot | Tight | Low |
| **B: Read checkpoint files directly** | Stale (minutes) | Checkpoint-level | Loose | High |

**Approach A is correct for query offloading** — only MO's live catalog can
guarantee the exact MVCC snapshot. Approach B is useful for offline analytics
(periodic ETL, reporting) where staleness is acceptable.

#### 13.3.2 MO Catalog APIs

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

#### 13.3.3 ObjectStats Layout (154 bytes)

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

#### 13.3.4 Manifest Serializer (Go Pseudocode)

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

#### 13.3.5 Tombstone Handling (Deferred)

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

#### 13.3.6 End-to-End Offload Flow

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

#### 13.3.7 Manifest Generation Tools

| Tool | Use Case | Data Source |
|------|----------|-------------|
| MO `/debug/tae/manifest` API | Production & dev | MO in-memory catalog (HTTP) |
| Go serializer (planned) | Production query offload | MO in-memory catalog |
| Checkpoint reader (future) | Offline analytics, MO is down | `.ckp` files on shared storage |

#### 13.3.8 Approach B: Checkpoint File Reader (Offline Analytics)

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
binary structure described in §13.3.3, containing the object name, row/block
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

### 13.4 Substrait-Based Query Routing (MO → Sirius)

**Goal:** MO offloads analytical queries to Sirius automatically via Substrait plans.

**Scope:**
- MO compiles query to its plan protobuf, converts to Substrait
- Sirius receives Substrait plan and executes on GPU
- Results returned via gRPC to MO frontend, sent to client
- See §16 for full architecture

---

## 14. Build and Test

### 14.1 Prerequisites

```bash
# LZ4
sudo apt install liblz4-dev  # Ubuntu/Debian
pacman -S lz4                 # Arch/Manjaro
brew install lz4              # macOS
```

### 14.2 Build

```bash
cd duckdb-tae-scanner
make release
```

### 14.3 Unit Tests (Catch2)

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

### 14.4 Test with TPC-H

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

### 14.5 TPC-H SF100 Benchmark Results

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

## 15. Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| TAE format changes in MO upgrades | Scanner breaks | Version field in header; pin MO version for initial release |
| Unflushed data invisible | Incorrect query results | Explicit `mo_ctl flush` after writes; document limitation |
| Zone map encoding mismatch | Wrong predicate pushdown | Validate against MO's own zone map evaluation; fuzz test |
| LZ4 version incompatibility | Decompression failure | Use system LZ4 lib; MO uses standard LZ4 frame format |
| Large varlena strings | Memory pressure | Stream blocks; don't buffer entire object |
| Decimal precision loss | Wrong numeric results | Use DuckDB's native DECIMAL(p,s); verify with TPC-H Q1 |

---

## 16. References

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

## 17. MO → Sidecar Integration Architecture

This section describes how MatrixOne routes analytical queries to the DuckDB sidecar
for accelerated execution. The implemented approach is **SQL String Forwarding via HTTP**.

### 17.1 Three Integration Paths

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

### 17.2 Sirius Sidecar Process

All integration paths (except Path 1) require a **sidecar process** — a separate
DuckDB instance running Sirius and tae_scanner extensions on a GPU-equipped machine.

```
┌──────────────────────────────────────────────────────────────┐
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
└──────────────────────────────────────────────────────────────┘
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

### 17.3 How Sirius Works Today

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

**Scan operator detection** (`sirius_engine.cpp:226`):
```cpp
if (scan_op.function.name == "parquet_scan")  → sirius_physical_parquet_scan
if (scan_op.function.name == "iceberg_scan")  → sirius_physical_iceberg_scan
if (scan_op.function.name == "seq_scan")      → sirius_physical_duckdb_scan
// We add:
if (scan_op.function.name == "tae_scan")      → sirius_physical_duckdb_scan  // generic path
```

Our `tae_scan()` function produces standard DuckDB DataChunks, so it works via
`sirius_physical_duckdb_scan` without Sirius modifications.

### 17.4 Substrait as the Plan Exchange Format

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

### 17.5 Path 2 Detail: SQL String Forwarding

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

### 17.6 Path 3 Detail: Substrait Plan Exchange

The production target. MO compiles its plan, serializes to Substrait protobuf,
and the sidecar deserializes and executes. This preserves MO's query optimization.

#### Step 1: MO Plan → Substrait

MO's query plan is a protobuf (`proto/plan.proto`). Convert MO plan nodes to Substrait
relations:

```
MO Plan Node          →  Substrait Rel
────────────────────     ──────────────────
TABLE_SCAN             →  ReadRel (ExtensionTable pointing to tae_scan)
FILTER                 →  FilterRel
PROJECT                →  ProjectRel
AGG / GROUP            →  AggregateRel
JOIN                   →  JoinRel / HashJoinRel
SORT                   →  SortRel
LIMIT                  →  FetchRel
```

The key challenge is **ReadRel**: MO's table scan must become a Substrait ReadRel
with an `ExtensionTable` that tells Sirius to use `tae_scan()`:

```protobuf
ReadRel {
  base_schema: { names: ["l_orderkey", "l_partkey", ...] }
  extension_table: {
    detail: Any {
      type_url: "matrixone.io/tae_scan"
      value: TAEScanSpec {
        manifest_path: "/mo-data/shared/tpch_lineitem.json"
        projected_columns: [0, 3, 5, 6]
      }
    }
  }
  filter: Expression { ... }  // zone map-eligible predicates
}
```

A Go library in MO (`pkg/sql/sirius/substrait_converter.go`) would walk the MO plan
tree and emit Substrait protobuf bytes.

#### Step 2: Sirius Receives Substrait Plan

Sirius already has `from_substrait()` which converts Substrait → DuckDB logical plan.
The extension table handler needs a custom `ReadRel` resolver:

```cpp
// In from_substrait.cpp, extend TransformReadOp():
if (read.has_extension_table()) {
    auto &detail = read.extension_table().detail();
    if (detail.type_url() == "matrixone.io/tae_scan") {
        TAEScanSpec spec;
        spec.ParseFromString(detail.value());
        // Create tae_scan() table function reference
        return make_shared<TableFunctionRelation>(
            context, "tae_scan", {Value(spec.manifest_path())});
    }
}
```

Then Sirius's standard pipeline: `from_substrait` → logical plan → `sirius_physical_plan_generator`
→ GPU pipelines → execute.

#### Step 3: Results Back to MO

Options:
- **Arrow IPC** over gRPC (standard, efficient for columnar data)
- **Arrow Flight** protocol (purpose-built for this use case)
- **MO Batch** format via custom serialization (matches MO's internal format)

#### Data Flow

```
MO SQL Query
    │
    ▼
MO Parser + Planner
    │  (pkg/sql/plan/build.go)
    ▼
MO Plan (protobuf)
    │
    ▼
Substrait Converter  ←── NEW: pkg/sql/sirius/substrait_converter.go
    │  MO plan nodes → Substrait relations
    │  Table references → ExtensionTable(tae_scan)
    │  Expressions → Substrait expressions
    ▼
Substrait Plan (protobuf bytes)
    │
    ▼  gRPC to Sirius sidecar
    │
Sirius: from_substrait()
    │  (substrait/src/from_substrait.cpp)
    ▼
DuckDB Logical Plan
    │  tae_scan() as ReadRel → table function
    ▼
sirius_physical_plan_generator
    │  (src/planner/sirius_physical_plan_generator.cpp)
    ▼
GPU Pipelines
    │  scan → filter → aggregate → sort → collect
    ▼
Result (Arrow IPC)
    │
    ▼  gRPC response
    │
MO Frontend → MySQL Protocol → Client
```

**Pros:** Full plan optimization on both sides. Type-safe. Extensible.
**Cons:** Significant work on MO→Substrait converter. Custom ExtensionTable handler in Sirius.

### 17.7 Unified Query Router

All three paths coexist in a single deployment. The MO router selects the best
execution path for each query:

```
                        ┌──────────────────────┐
                        │   MO receives query   │
                        └──────────┬───────────┘
                                   │
                          ┌────────▼────────┐
                          │ shouldRouteToGPU │
                          │   (heuristic)    │
                          └───┬──────────┬───┘
                          No  │          │ Yes
                              ▼          ▼
                       ┌──────────┐  ┌─────────────────┐
                       │ MO CPU   │  │ convertToSubstrait│
                       │ pipeline │  │ (MO plan → proto) │
                       └──────────┘  └──┬────────────┬──┘
                                    OK  │        Err │
                                        ▼            ▼
                                 ┌────────────┐ ┌──────────────┐
                                 │ Path 3:    │ │ Path 2:      │
                                 │ ExecutePlan│ │ ExecuteSQL   │
                                 │ (substrait)│ │ (rewritten)  │
                                 └─────┬──────┘ └──────┬───────┘
                                       │               │
                                       └───────┬───────┘
                                               ▼
                                      ┌────────────────┐
                                      │ Sirius sidecar │
                                      │ gpu_execution  │
                                      └────────────────┘
```

**Path 1** (Manual CLI) is always available independently — the user talks directly
to DuckDB+Sirius without MO involvement. It's the developer/debug interface.

**GPU routing heuristic:**

| Factor | GPU Favorable | Keep on CPU |
|--------|--------------|-------------|
| Query type | SELECT with aggregates | INSERT, UPDATE, DELETE, DDL |
| Data volume | > 100K rows | < 100K rows |
| Operators | GROUP BY, JOIN, SORT | Simple point lookups |
| Functions | SUM, AVG, COUNT, MIN, MAX | UDFs, string manipulation |
| Table count | 1-5 tables | > 5 tables (complex joins) |
| Result size | Small (aggregate output) | Large (full table scan) |

**Implementation** (`pkg/frontend/mysql_cmd_executor.go`):

```go
func (mce *MysqlCmdExecutor) executeWithGPUFallback(
    ctx context.Context,
    plan *plan.Plan,
    stmt string,
) (*Result, error) {
    if !siriusEnabled || !shouldRouteToGPU(plan) {
        return mce.dispatchStmt(ctx, plan)          // MO CPU path
    }

    // Try Path 3: Substrait plan exchange (preferred)
    if substraitBytes, err := convertToSubstrait(plan); err == nil {
        if result, err := sidecar.ExecutePlan(ctx, substraitBytes); err == nil {
            return result, nil
        }
        // Substrait execution failed — fall through to Path 2
    }

    // Path 2: SQL string forwarding (fallback)
    rewritten := rewriteTablesForTAEScan(stmt, plan)
    if result, err := sidecar.ExecuteSQL(ctx, rewritten); err == nil {
        return result, nil
    }

    // Both GPU paths failed — fall back to MO CPU
    return mce.dispatchStmt(ctx, plan)
}

func shouldRouteToGPU(p *plan.Plan) bool {
    query := p.GetQuery()
    if query == nil {
        return false // not a SELECT
    }
    hasAgg := false
    for _, node := range query.GetNodes() {
        if node.GetNodeType() == plan.Node_AGG {
            hasAgg = true
        }
    }
    return hasAgg && estimatedRows(query) > 100000
}
```

This layered approach means:
- **Day 1:** Path 2 works immediately (just SQL rewriting, no Substrait converter)
- **Incrementally:** Add Substrait coverage for more query patterns
- **Eventually:** Most queries go through Path 3; Path 2 handles edge cases
- **Always:** Path 1 available for manual/debug use

### 17.8 MO Interception Points

Based on MO's query execution flow:

```
ExecRequest (mysql_cmd_executor.go:3435)
  → doComQuery (line 3266)
    → GetComputationWrapper → parse SQL, build plan
    → Loop statements:
        → executeStmtWithResponse (line 3391)  ← INTERCEPT HERE
          → executeWithGPUFallback(plan, stmt)
            → try Path 3 (Substrait) first
            → fall back to Path 2 (SQL string)
            → fall back to MO CPU
```

MO already has a pattern for remote execution: `RemoteRun()` in
`pkg/sql/compile/scope.go:394` serializes operator trees and sends them to other
CN nodes via RPC. The Sirius routing follows the same pattern, but sends to a
GPU sidecar instead of another CN.

### 17.9 Integration Rollout (Complements §13 Extension Phases)

§13 phases the *scanner extension* (local→S3→catalog→Substrait). This section
phases the *MO↔Sirius integration* — they run in parallel:

| §13 Extension Phase | §17 Integration Step | Combined Effect |
|---------------------|---------------------|-----------------|
| §13.1: Local scan | Step 1: Path 1 (CLI) | Manual DuckDB queries on local TAE files |
| §13.2: S3 support | Step 2: Path 2 (SQL) | MO auto-routes queries, sidecar reads S3 |
| §13.3: MO manifest gen | Step 2 continues | MO generates manifest from live catalog |
| §13.4: Substrait | Step 3: Path 3 | Full plan pushdown, MO optimizer preserved |

**Step 1 (Current):** Path 1 only. tae_scan extension + manifest generator.
User manually calls `gpu_execution('SELECT ... FROM tae_scan(...)')` from DuckDB CLI.
No MO changes.

**Step 2:** Add Path 2. Sidecar gRPC service + MO query router + SQL rewriter.
MO routes analytical queries automatically. ~500 LOC in MO.

**Step 3:** Add Path 3. MO→Substrait converter + custom ExtensionTable handler in
Sirius. Path 3 preferred, Path 2 as fallback. ~2000 LOC in MO, ~100 LOC in Sirius.
All three paths coexist and the router picks the best available.

### 17.10 gRPC Service Definition

The sidecar exposes a single gRPC service that serves both Path 2 and Path 3.
Results are streamed as Arrow IPC record batches for efficient columnar transfer.

```protobuf
syntax = "proto3";
package sirius.sidecar.v1;

import "google/protobuf/any.proto";

// ─── Service ────────────────────────────────────────────────

service SiriusSidecar {
  // Path 2: Execute a SQL string. The sidecar parses, plans, and executes
  // via gpu_execution(). Table names should already be rewritten to tae_scan().
  rpc ExecuteSQL(ExecuteSQLRequest) returns (stream ResultBatch);

  // Path 3: Execute a pre-compiled Substrait plan. The sidecar deserializes
  // via from_substrait(), then feeds the logical plan to Sirius GPU engine.
  rpc ExecutePlan(ExecutePlanRequest) returns (stream ResultBatch);

  // Health check / capabilities. MO calls this at startup to discover
  // which paths the sidecar supports and which GPU is available.
  rpc GetCapabilities(CapabilitiesRequest) returns (CapabilitiesResponse);
}

// ─── Path 2: SQL Execution ──────────────────────────────────

message ExecuteSQLRequest {
  // Rewritten SQL with tae_scan() table functions.
  // Example: "SELECT l_returnflag, SUM(l_quantity)
  //           FROM tae_scan('/mo-data/tpch/lineitem.json')
  //           GROUP BY 1"
  string sql = 1;

  // Optional query ID for tracing/cancellation.
  string query_id = 2;

  // Optional session settings (e.g., timezone, memory limit).
  map<string, string> settings = 3;

  // Maximum rows to return (0 = unlimited).
  uint64 max_rows = 4;
}

// ─── Path 3: Substrait Plan Execution ───────────────────────

message ExecutePlanRequest {
  // Serialized Substrait Plan protobuf (binary, not JSON).
  // MO's substrait_converter.go produces this from the MO plan tree.
  bytes substrait_plan = 1;

  // Optional query ID for tracing/cancellation.
  string query_id = 2;

  // Optional session settings.
  map<string, string> settings = 3;

  // Maximum rows to return (0 = unlimited).
  uint64 max_rows = 4;
}

// ─── Result Streaming ───────────────────────────────────────

message ResultBatch {
  oneof payload {
    // First message: schema (Arrow IPC Schema message).
    // Sent exactly once before any record batches.
    bytes arrow_schema = 1;

    // Subsequent messages: data (Arrow IPC RecordBatch messages).
    // Each batch contains up to batch_size rows in columnar format.
    bytes arrow_record_batch = 2;

    // Final message: execution summary.
    ExecutionSummary summary = 3;

    // Error during execution.
    ExecutionError error = 4;
  }
}

message ExecutionSummary {
  uint64 total_rows = 1;
  uint64 total_batches = 2;

  // Timing breakdown (nanoseconds).
  uint64 plan_time_ns = 3;        // DuckDB planning (Path 2) or Substrait deserialize (Path 3)
  uint64 gpu_exec_time_ns = 4;    // Sirius GPU pipeline execution
  uint64 transfer_time_ns = 5;    // GPU→host data transfer
  uint64 total_time_ns = 6;       // Wall clock

  // Resource usage.
  uint64 gpu_memory_peak_bytes = 7;
  uint64 blocks_scanned = 8;      // tae_scan: TAE blocks read
  uint64 blocks_skipped = 9;      // tae_scan: TAE blocks skipped by zone map
}

message ExecutionError {
  enum ErrorCode {
    UNKNOWN = 0;
    PARSE_ERROR = 1;          // SQL parse failure (Path 2)
    PLAN_ERROR = 2;           // Substrait deserialization failure (Path 3)
    EXECUTION_ERROR = 3;      // Runtime error (OOM, GPU fault, etc.)
    UNSUPPORTED_FEATURE = 4;  // Query uses feature not supported by Sirius
    TIMEOUT = 5;
    CANCELLED = 6;
  }
  ErrorCode code = 1;
  string message = 2;
  string detail = 3;         // Stack trace or diagnostic info
}

// ─── Capabilities ───────────────────────────────────────────

message CapabilitiesRequest {}

message CapabilitiesResponse {
  // Which execution paths are supported.
  bool supports_sql = 1;          // Path 2
  bool supports_substrait = 2;    // Path 3

  // GPU info.
  string gpu_name = 3;            // e.g., "NVIDIA A100-SXM4-80GB"
  uint64 gpu_memory_bytes = 4;    // Total GPU memory
  uint32 gpu_count = 5;           // Number of GPUs

  // DuckDB/Sirius version info.
  string duckdb_version = 6;
  string sirius_version = 7;
  string tae_scanner_version = 8;

  // Supported Substrait features (for MO to know what it can send).
  repeated string substrait_extensions = 9;  // e.g., ["aggregate_approx", "window"]
}
```

**Wire protocol notes:**
- Arrow IPC is the standard columnar serialization. DuckDB can produce it natively
  via `duckdb::ArrowConverter`. cuDF can also produce Arrow directly.
- Streaming response (`stream ResultBatch`) allows the sidecar to push batches as
  they complete, reducing latency for large results. MO can start sending to the
  MySQL client before the full result is ready.
- The schema message is sent first so MO knows column names and types before data
  arrives. This maps directly to MySQL column definitions.
- `query_id` enables cancellation: MO can call a separate `CancelQuery(query_id)`
  RPC (not shown) if the client disconnects.

**MO client usage** (Go):

```go
package sirius

import (
    "context"
    "io"

    pb "github.com/matrixorigin/matrixone/pkg/pb/sirius"
    "google.golang.org/grpc"
)

type SidecarClient struct {
    conn   *grpc.ClientConn
    client pb.SiriusSidecarClient
}

func NewSidecarClient(addr string) (*SidecarClient, error) {
    conn, err := grpc.Dial(addr, grpc.WithInsecure())
    if err != nil {
        return nil, err
    }
    return &SidecarClient{
        conn:   conn,
        client: pb.NewSiriusSidecarClient(conn),
    }, nil
}

// ExecutePlan sends a Substrait plan (Path 3) and collects Arrow batches.
func (sc *SidecarClient) ExecutePlan(
    ctx context.Context, plan []byte, queryID string,
) (schema []byte, batches [][]byte, summary *pb.ExecutionSummary, err error) {
    stream, err := sc.client.ExecutePlan(ctx, &pb.ExecutePlanRequest{
        SubstraitPlan: plan,
        QueryId:       queryID,
    })
    if err != nil {
        return nil, nil, nil, err
    }
    for {
        msg, err := stream.Recv()
        if err == io.EOF {
            break
        }
        if err != nil {
            return nil, nil, nil, err
        }
        switch p := msg.Payload.(type) {
        case *pb.ResultBatch_ArrowSchema:
            schema = p.ArrowSchema
        case *pb.ResultBatch_ArrowRecordBatch:
            batches = append(batches, p.ArrowRecordBatch)
        case *pb.ResultBatch_Summary:
            summary = p.Summary
        case *pb.ResultBatch_Error:
            return nil, nil, nil, fmt.Errorf("sidecar error %s: %s",
                p.Error.Code, p.Error.Message)
        }
    }
    return schema, batches, summary, nil
}

// ExecuteSQL sends a SQL string (Path 2 fallback).
func (sc *SidecarClient) ExecuteSQL(
    ctx context.Context, sql string, queryID string,
) (schema []byte, batches [][]byte, summary *pb.ExecutionSummary, err error) {
    stream, err := sc.client.ExecuteSQL(ctx, &pb.ExecuteSQLRequest{
        Sql:     sql,
        QueryId: queryID,
    })
    // ... same Recv() loop as ExecutePlan ...
}
```

**Sidecar server** (C++, embedded in DuckDB process):

```cpp
// sirius_sidecar_server.cpp — gRPC server wrapping DuckDB+Sirius

class SiriusSidecarImpl final : public SiriusSidecar::Service {
    duckdb::DuckDB db;
    duckdb::Connection conn;

public:
    SiriusSidecarImpl(const string& data_dir)
        : db(data_dir), conn(db) {
        conn.Query("LOAD 'sirius'");
        conn.Query("LOAD 'tae_scanner'");
    }

    grpc::Status ExecuteSQL(grpc::ServerContext* ctx,
                            const ExecuteSQLRequest* req,
                            grpc::ServerWriter<ResultBatch>* writer) override {
        // Wrap in gpu_execution() for Sirius GPU path
        string wrapped = "CALL gpu_execution('" + EscapeSQL(req->sql()) + "')";
        auto result = conn.Query(wrapped);
        return StreamArrowResult(result, writer);
    }

    grpc::Status ExecutePlan(grpc::ServerContext* ctx,
                             const ExecutePlanRequest* req,
                             grpc::ServerWriter<ResultBatch>* writer) override {
        // Deserialize Substrait → DuckDB plan → Sirius GPU
        auto result = conn.Query(
            "CALL gpu_execution_substrait(?)",
            duckdb::Value::BLOB(req->substrait_plan()));
        return StreamArrowResult(result, writer);
    }

private:
    grpc::Status StreamArrowResult(
            unique_ptr<duckdb::MaterializedQueryResult>& result,
            grpc::ServerWriter<ResultBatch>* writer) {
        // 1. Send schema
        ResultBatch schema_msg;
        schema_msg.set_arrow_schema(SerializeArrowSchema(result));
        writer->Write(schema_msg);

        // 2. Stream record batches
        while (auto chunk = result->Fetch()) {
            ResultBatch batch_msg;
            batch_msg.set_arrow_record_batch(SerializeArrowBatch(chunk));
            writer->Write(batch_msg);
        }

        // 3. Send summary
        ResultBatch summary_msg;
        auto* s = summary_msg.mutable_summary();
        s->set_total_rows(result->RowCount());
        writer->Write(summary_msg);

        return grpc::Status::OK;
    }
};
```

### 17.11 Arrow IPC → MySQL Protocol (Result Conversion)

When the sidecar returns Arrow IPC batches, MO must convert them to MySQL wire
protocol for the client. This section describes the **direct conversion** approach
that bypasses MO's `batch.Batch` entirely.

#### MO's Current Result Pipeline

```
batch.Batch → convertBatchToSlices() → ColumnSlices → appendResultSetTextRow2() → TCP
              (output.go:687)                          (mysql_protocol.go:3160)
```

Key functions in this chain:
- `WriteResultSetRow2()` — dispatches per-row to text or binary serializer
  (`mysql_protocol.go:3516`)
- `appendResultSetTextRow2()` — COM_QUERY: each value as length-encoded string,
  NULL = `0xFB` (`mysql_protocol.go:3160`)
- `appendResultSetBinaryRow2()` — COM_STMT_EXECUTE: NULL bitmap + fixed-size
  binary values (`mysql_protocol.go:2861`)
- `SendColumnDefinitionPacket()` — sends column metadata (name, type, charset)
  (`mysql_protocol.go:2240`)

#### Direct Arrow → MySQL Conversion

Instead of: `Arrow IPC → batch.Batch → ColumnSlices → MySQL wire`
We do:      `Arrow IPC → ArrowSlices → MySQL wire`

```go
// pkg/frontend/arrow_result.go — Arrow IPC → MySQL protocol adapter

package frontend

import (
    "github.com/apache/arrow/go/v15/arrow"
    "github.com/apache/arrow/go/v15/arrow/ipc"
    "github.com/apache/arrow/go/v15/arrow/array"
)

// ArrowColumnDef converts Arrow field metadata to MysqlColumn definitions.
// Called once after receiving the arrow_schema message from the sidecar.
func ArrowColumnDefs(schema *arrow.Schema) []*MysqlColumn {
    cols := make([]*MysqlColumn, schema.NumFields())
    for i, field := range schema.Fields() {
        c := new(MysqlColumn)
        c.SetName(field.Name)
        c.SetOrgName(field.Name)

        switch field.Type.ID() {
        case arrow.BOOL:
            c.SetColumnType(defines.MYSQL_TYPE_BOOL)
        case arrow.INT8:
            c.SetColumnType(defines.MYSQL_TYPE_TINY)
        case arrow.INT16:
            c.SetColumnType(defines.MYSQL_TYPE_SHORT)
        case arrow.INT32:
            c.SetColumnType(defines.MYSQL_TYPE_LONG)
        case arrow.INT64:
            c.SetColumnType(defines.MYSQL_TYPE_LONGLONG)
        case arrow.FLOAT32:
            c.SetColumnType(defines.MYSQL_TYPE_FLOAT)
        case arrow.FLOAT64:
            c.SetColumnType(defines.MYSQL_TYPE_DOUBLE)
        case arrow.STRING, arrow.LARGE_STRING:
            c.SetColumnType(defines.MYSQL_TYPE_VARCHAR)
        case arrow.BINARY, arrow.LARGE_BINARY:
            c.SetColumnType(defines.MYSQL_TYPE_BLOB)
        case arrow.DATE32:
            c.SetColumnType(defines.MYSQL_TYPE_DATE)
        case arrow.TIMESTAMP:
            c.SetColumnType(defines.MYSQL_TYPE_DATETIME)
        case arrow.DECIMAL128:
            c.SetColumnType(defines.MYSQL_TYPE_DECIMAL)
            dt := field.Type.(*arrow.Decimal128Type)
            c.SetDecimal(dt.Scale)
        }
        cols[i] = c
    }
    return cols
}

// WriteArrowBatchAsTextRows reads an Arrow record batch and writes MySQL text
// rows directly, without converting to batch.Batch or ColumnSlices.
//
// This is the hot path — one function call per sidecar response batch.
func (mp *MysqlProtocolImpl) WriteArrowBatchAsTextRows(
    rec arrow.Record,
) error {
    nRows := int(rec.NumRows())
    nCols := int(rec.NumCols())

    for row := 0; row < nRows; row++ {
        mp.beginPacket()

        for col := 0; col < nCols; col++ {
            arr := rec.Column(col)

            if arr.IsNull(row) {
                mp.appendUint8(0xFB)  // MySQL NULL marker
                continue
            }

            // Read directly from Arrow columnar buffers — no copy
            switch v := arr.(type) {
            case *array.Boolean:
                if v.Value(row) {
                    mp.appendStringLenEncOfInt64(1)
                } else {
                    mp.appendStringLenEncOfInt64(0)
                }
            case *array.Int8:
                mp.appendStringLenEncOfInt64(int64(v.Value(row)))
            case *array.Int16:
                mp.appendStringLenEncOfInt64(int64(v.Value(row)))
            case *array.Int32:
                mp.appendStringLenEncOfInt64(int64(v.Value(row)))
            case *array.Int64:
                mp.appendStringLenEncOfInt64(v.Value(row))
            case *array.Uint8:
                mp.appendStringLenEncOfUint64(uint64(v.Value(row)))
            case *array.Uint16:
                mp.appendStringLenEncOfUint64(uint64(v.Value(row)))
            case *array.Uint32:
                mp.appendStringLenEncOfUint64(uint64(v.Value(row)))
            case *array.Uint64:
                mp.appendStringLenEncOfUint64(v.Value(row))
            case *array.Float32:
                mp.appendStringLenEncOfFloat32(v.Value(row))
            case *array.Float64:
                mp.appendStringLenEncOfFloat64(v.Value(row))
            case *array.String:
                AppendStringLenEnc(mp, []byte(v.Value(row)))
            case *array.Binary:
                AppendStringLenEnc(mp, v.Value(row))
            case *array.Date32:
                // Arrow date32 = days since epoch → format as "YYYY-MM-DD"
                d := v.Value(row).ToTime()
                AppendStringLenEnc(mp, []byte(d.Format("2006-01-02")))
            case *array.Timestamp:
                t := v.Value(row).ToTime(arrow.Microsecond)
                AppendStringLenEnc(mp, []byte(t.Format("2006-01-02 15:04:05.000000")))
            case *array.Decimal128:
                AppendStringLenEnc(mp, []byte(v.Value(row).ToString(
                    v.DataType().(*arrow.Decimal128Type).Scale)))
            }
        }
        mp.finishedPacket()
    }
    return mp.flush()
}
```

#### Full Integration in the Router

```go
// In executeWithGPUFallback(), after receiving Arrow results from sidecar:

func (mce *MysqlCmdExecutor) sendArrowResultToClient(
    ctx context.Context,
    arrowSchema []byte,
    arrowBatches [][]byte,
) error {
    mp := mce.GetSession().GetMysqlProtocol()

    // 1. Decode Arrow schema → MySQL column definitions
    reader := ipc.NewReaderFromBytes(arrowSchema)
    cols := ArrowColumnDefs(reader.Schema())

    // 2. Send column count + column definitions
    mp.SendColumnCountPacket(uint64(len(cols)))
    for i, c := range cols {
        mp.SendColumnDefinitionPacket(ctx, c, i)
    }
    mp.WriteEOFIF(0, serverStatus)

    // 3. Stream each Arrow batch → MySQL rows (zero-copy from Arrow buffers)
    for _, batchBytes := range arrowBatches {
        rec := ipc.DeserializeRecordBatch(reader.Schema(), batchBytes)
        mp.WriteArrowBatchAsTextRows(rec)
        rec.Release()
    }

    // 4. Send EOF to signal end of result set
    mp.WriteEOFOrOK(0, serverStatus)
    return mp.flush()
}
```

#### Why This is Efficient

| Step | Copy? | Notes |
|------|-------|-------|
| gRPC recv → `[]byte` | zero-copy | gRPC delivers contiguous buffer |
| `[]byte` → Arrow Record | zero-copy | Arrow IPC uses buffer offsets, no deserialization |
| Arrow column → MySQL text | 1 copy | Format int/float/string into length-encoded packet |
| packet → TCP socket | kernel | `writev()` scatter-gather I/O |

Total: **one data copy** (Arrow buffer → MySQL packet buffer), same as MO's native
path (ColumnSlices → MySQL packet buffer). No intermediate `batch.Batch` allocation.

#### Streaming Behavior

The sidecar streams `ResultBatch` messages. MO can start sending MySQL column
definitions and rows to the client **before the full result is computed**:

```
Sidecar                    MO                         Client
   │                        │                            │
   │── arrow_schema ──────→│── column defs ───────────→│
   │── record_batch #1 ──→│── MySQL rows (1..N) ────→│
   │── record_batch #2 ──→│── MySQL rows (N+1..M) ──→│
   │── record_batch #3 ──→│── MySQL rows (M+1..K) ──→│
   │── summary ──────────→│── EOF packet ────────────→│
```

This gives the client first-row latency equal to one GPU pipeline stage + one
network hop, not the full query execution time.

### 17.12 Why Not Embed Sirius in MO? (CGO/FFI Infeasibility)

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

### 17.13 MO → Substrait Converter Design (Path 3)

This section specifies the Go library that converts MO's internal query plan
(`pkg/pb/plan.Plan`) to Substrait protobuf bytes consumable by Sirius's
`from_substrait()`. This is the core of Path 3.

#### MO Plan Structure (Source)

MO's plan is defined in `proto/plan.proto` (~1746 lines). Key structures:

```
Plan
└── Query
    ├── StmtType: SELECT | INSERT | UPDATE | DELETE
    ├── Steps: []int32           (root node IDs — entry points into DAG)
    ├── Nodes: []Node            (flat DAG of relational operators)
    └── Headings: []string       (result column names)

Node
├── NodeType: TABLE_SCAN(2) | PROJECT(10) | AGG(11) | FILTER(13) |
│             JOIN(14) | SORT(16) | WINDOW(17) | UNION_ALL(19) | ...
├── NodeId: int32
├── Children: []int32            (child node IDs in the DAG)
├── ProjectList: []Expr          (output column expressions)
├── FilterList: []Expr           (WHERE / HAVING predicates)
├── GroupBy: []Expr              (GROUP BY expressions)
├── AggList: []Expr              (aggregate function expressions)
├── OnList: []Expr               (JOIN conditions)
├── OrderBy: []OrderBySpec       (SORT specifications)
├── Limit / Offset: *Expr        (LIMIT / OFFSET)
├── JoinType: INNER(0) | LEFT(1) | RIGHT(2) | OUTER(3) | SEMI(4) | ANTI(5)
├── TableDef: *TableDef          (TABLE_SCAN: column defs, table name, etc.)
├── ObjRef: *ObjectRef           (TABLE_SCAN: db/table identifiers)
└── WinSpecList: []Expr          (WINDOW specifications)

Expr (oneof)
├── Lit: *Literal                (constant values)
├── Col: *ColRef                 (column reference: RelPos, ColPos, Name)
├── F: *Function                 (function call: ObjName + Args)
├── T: *TargetType               (CAST expression)
├── Sub: *SubqueryRef            (subquery)
├── List: *ExprList              (IN list)
└── Typ: Type                    (result type: Id, Width, Scale, NotNullable)

Type.Id values:
  T_bool=10, T_int8=20, T_int16=21, T_int32=22, T_int64=23,
  T_uint8=25, T_uint16=26, T_uint32=27, T_uint64=28,
  T_float32=30, T_float64=31, T_decimal64=32, T_decimal128=33,
  T_date=50, T_time=51, T_datetime=52, T_timestamp=53,
  T_char=60, T_varchar=61, T_json=62, T_binary=64, T_varbinary=65,
  T_blob=70, T_text=71
```

#### Substrait Plan Structure (Target)

Substrait plan produced by the converter, as consumed by Sirius's `from_substrait.cpp`:

```
Plan
├── Extensions: []Extension      (function anchor → name registry)
└── Relations: []PlanRel
    └── Root: RelRoot
        ├── Input: Rel           (the relation tree)
        └── Names: []string      (output column names)

Rel (oneof)
├── ReadRel     { named_table | extension_table, base_schema, filter, projection }
├── FilterRel   { input: Rel, condition: Expression }
├── ProjectRel  { input: Rel, expressions: []Expression }
├── AggregateRel { input: Rel, groupings: []Grouping, measures: []Measure }
├── JoinRel     { left: Rel, right: Rel, on: Expression, type: JoinType }
├── SortRel     { input: Rel, sorts: []SortField }
├── FetchRel    { input: Rel, offset: int64, count: int64 }
├── SetRel      { inputs: []Rel, op: UNION_ALL | MINUS | INTERSECT }
└── CrossProductRel { left: Rel, right: Rel }

Expression (oneof)
├── Literal     { i32, i64, fp32, fp64, string, boolean, date, timestamp, decimal, ... }
├── Selection   { direct_reference.struct_field.field: uint32 }  (column index)
├── ScalarFunction { function_reference: uint32, arguments: []FunctionArgument }
├── Cast        { input: Expression, type: Type }
├── IfThen      { ifs: [{if, then}], else }   (CASE WHEN)
└── SingularOrList { value: Expression, options: []Expression }  (IN)
```

#### Node Mapping: MO → Substrait

```
MO Node_NodeType      →  Substrait Rel Type       Notes
──────────────────────────────────────────────────────────────────
Node_TABLE_SCAN (2)   →  ReadRel                  See ExtensionTable below
Node_PROJECT (10)     →  ProjectRel               ProjectList → expressions
Node_AGG (11)         →  AggregateRel             GroupBy → groupings, AggList → measures
Node_FILTER (13)      →  FilterRel                FilterList AND-ed → condition
Node_JOIN (14)        →  JoinRel or CrossProductRel
Node_SORT (16)        →  SortRel                  OrderBy → sorts
Node_WINDOW (17)      →  WindowRel                (limited support in Sirius)
Node_UNION_ALL (19)   →  SetRel (UNION_ALL)
──────────────────────────────────────────────────────────────────
LIMIT/OFFSET          →  FetchRel                 Wraps child SortRel
```

Unsupported MO nodes (fall back to Path 2):
`Node_RECURSIVE_CTE`, `Node_MATERIAL`, `Node_SINK`, `Node_SINK_SCAN`,
`Node_TIME_WINDOW`, `Node_FILL`, `Node_PARTITION`, `Node_APPLY`,
`Node_FUNCTION_SCAN`, `Node_EXTERNAL_SCAN`, `Node_SOURCE_SCAN`.

#### Expression Mapping: MO → Substrait

```
MO Expr Type          →  Substrait Expression     Notes
──────────────────────────────────────────────────────────────────
Expr_Lit (Literal)    →  Literal                  Type-specific value mapping
Expr_Col (ColRef)     →  Selection                RelPos:ColPos → struct_field index
Expr_F (Function)     →  ScalarFunction           ObjName → function anchor
Expr_T (TargetType)   →  Cast                     Type mapping below
Expr_List (ExprList)  →  SingularOrList           For IN expressions
```

**Function name mapping** (MO function name → Substrait function name):

```
MO Function.ObjName   →  Substrait Extension Function
──────────────────────────────────────────────────────────────────
Comparison operators:
  "="                  →  "equal"
  ">"                  →  "gt"
  ">="                 →  "gte"
  "<"                  →  "lt"
  "<="                 →  "lte"
  "<>"  / "!="         →  "not_equal"

Logical operators:
  "and"                →  "and"
  "or"                 →  "or"
  "not"                →  "not"

Null checks:
  "ifnull"             →  "is_null" / "is_not_null"

Arithmetic:
  "+"                  →  "add"
  "-"                  →  "subtract"
  "*"                  →  "multiply"
  "/"                  →  "divide"
  "mod" / "%"          →  "modulus"

Aggregate functions:
  "sum"                →  "sum"
  "count"              →  "count"
  "count(*)"           →  "count" (with no args → count_star in DuckDB)
  "min"                →  "min"
  "max"                →  "max"
  "avg"                →  "avg"
  "stddev"             →  "std_dev"
  "any_value"          →  "any_value"

String functions:
  "substring" / "substr" → "substring"
  "length"             →  "char_length"
  "like"               →  "like"
  "starts_with"        →  "starts_with"
  "ends_with"          →  "ends_with"

Date/Time:
  "date_part"/"extract" → "extract"
  "year"/"month"/"day" →  "extract" with enum arg
```

#### Type Mapping: MO Type.Id → Substrait Type

```
MO Type.Id            →  Substrait Type           Substrait kind_case
──────────────────────────────────────────────────────────────────
T_bool (10)           →  Type_Boolean             kBool
T_int8 (20)           →  Type_I8                  kI8
T_int16 (21)          →  Type_I16                 kI16
T_int32 (22)          →  Type_I32                 kI32
T_int64 (23)          →  Type_I64                 kI64
T_uint8 (25)          →  Type_I16                 kI16  (widen: no unsigned in Substrait)
T_uint16 (26)         →  Type_I32                 kI32  (widen)
T_uint32 (27)         →  Type_I64                 kI64  (widen)
T_uint64 (28)         →  Type_I64                 kI64  (widen, may overflow)
T_float32 (30)        →  Type_Float               kFp32
T_float64 (31)        →  Type_Double              kFp64
T_decimal64 (32)      →  Type_Decimal             kDecimal (precision, scale)
T_decimal128 (33)     →  Type_Decimal             kDecimal (precision, scale)
T_date (50)           →  Type_Date                kDate
T_time (51)           →  Type_Time                kTime
T_datetime (52)       →  Type_PrecisionTimestamp   kPrecisionTimestamp (precision=6)
T_timestamp (53)      →  Type_PrecisionTimestamp   kPrecisionTimestamp (precision=6)
T_char (60)           →  Type_VarChar             kVarchar (length = Width)
T_varchar (61)        →  Type_VarChar             kVarchar (length = Width)
T_json (62)           →  Type_VarChar             kVarchar (treat as string)
T_binary (64)         →  Type_Binary              kBinary
T_varbinary (65)      →  Type_Binary              kBinary
T_blob (70)           →  Type_Binary              kBinary
T_text (71)           →  Type_VarChar             kVarchar

Nullability: Type.NotNullable → NULLABILITY_REQUIRED, else NULLABILITY_NULLABLE
```

**Note on unsigned types:** Substrait has no unsigned integer types. MO's uint8/16/32
are widened to the next signed size. uint64 is mapped to int64 with potential overflow
for values > 2^63. This matches DuckDB's Substrait converter behavior.

#### ReadRel with ExtensionTable (tae_scan)

The critical mapping: MO's TABLE_SCAN → Substrait ReadRel that tells Sirius to
call `tae_scan()`.

```protobuf
// Custom protobuf for tae_scan parameters
// File: proto/sirius/tae_scan_spec.proto

syntax = "proto3";
package sirius.tae;

message TAEScanSpec {
  // Path to the JSON manifest for this table
  string manifest_path = 1;

  // Projected column indices (0-based, from MO plan's ProjectList)
  repeated uint32 projected_columns = 2;

  // Table metadata for provenance
  string database_name = 3;
  string table_name = 4;
  uint64 table_id = 5;
}
```

The converter wraps this into ReadRel.extension_table:

```go
func (c *Converter) convertTableScan(node *plan.Node) *substrait.Rel {
    // Build base_schema from TableDef columns
    schema := &substrait.NamedStruct{}
    for _, col := range node.TableDef.Cols {
        schema.Names = append(schema.Names, col.Name)
        schema.Struct.Types = append(schema.Struct.Types, moTypeToSubstrait(col.Typ))
    }

    // Build tae_scan spec
    spec := &siriuspb.TAEScanSpec{
        ManifestPath:     c.manifestPath(node.ObjRef),
        ProjectedColumns: c.projectedColumns(node),
        DatabaseName:     node.ObjRef.DbName,
        TableName:        node.ObjRef.ObjName,
        TableId:          node.TableDef.TblId,
    }
    specBytes, _ := proto.Marshal(spec)

    // Build ReadRel with ExtensionTable
    return &substrait.Rel{
        RelType: &substrait.Rel_Read{
            Read: &substrait.ReadRel{
                BaseSchema: schema,
                ReadType: &substrait.ReadRel_ExtensionTable_{
                    ExtensionTable: &substrait.ReadRel_ExtensionTable{
                        Detail: &anypb.Any{
                            TypeUrl: "matrixone.io/tae_scan",
                            Value:   specBytes,
                        },
                    },
                },
            },
        },
    }
}
```

**Sirius-side handler** (addition to `from_substrait.cpp:TransformReadOp()`):

```cpp
if (sget.has_extension_table()) {
    auto &detail = sget.extension_table().detail();
    if (detail.type_url() == "matrixone.io/tae_scan") {
        sirius::tae::TAEScanSpec spec;
        spec.ParseFromString(detail.value());

        // Build tae_scan() call with manifest path
        vector<Value> params;
        params.push_back(Value(spec.manifest_path()));
        auto scan = make_shared_ptr<TableFunctionRelation>(
            context, "tae_scan", std::move(params));

        // Apply projection if specified
        if (spec.projected_columns_size() > 0) {
            // ... column pruning ...
        }
        return scan;
    }
}
```

#### Converter Implementation

```go
// pkg/sql/sirius/substrait_converter.go

package sirius

import (
    "fmt"

    "github.com/matrixorigin/matrixone/pkg/pb/plan"
    substrait "github.com/substrait-io/substrait-go/proto"
    "google.golang.org/protobuf/proto"
    "google.golang.org/protobuf/types/known/anypb"
)

type Converter struct {
    plan       *plan.Plan
    funcMap    map[string]uint32   // function name → anchor ID
    nextAnchor uint32
    extensions []*substrait.Extension
    errors     []error             // unsupported features encountered
}

func NewConverter(p *plan.Plan) *Converter {
    return &Converter{
        plan:    p,
        funcMap: make(map[string]uint32),
    }
}

// Convert transforms the MO plan to Substrait bytes.
// Returns (nil, err) if the plan uses unsupported features.
func (c *Converter) Convert() ([]byte, error) {
    query := c.plan.GetQuery()
    if query == nil || query.StmtType != plan.Query_SELECT {
        return nil, fmt.Errorf("only SELECT queries supported")
    }

    // Walk the plan DAG starting from root node
    if len(query.Steps) == 0 {
        return nil, fmt.Errorf("empty plan")
    }
    rootNodeId := query.Steps[0]
    rootNode := query.Nodes[rootNodeId]

    rel, err := c.convertNode(rootNode, query.Nodes)
    if err != nil {
        return nil, err
    }

    // Build the Substrait Plan
    subPlan := &substrait.Plan{
        Extensions: c.extensions,
        Relations: []*substrait.PlanRel{{
            RelType: &substrait.PlanRel_Root{
                Root: &substrait.RelRoot{
                    Input: rel,
                    Names: query.Headings,
                },
            },
        }},
    }

    return proto.Marshal(subPlan)
}

// convertNode recursively converts a MO plan node to a Substrait Rel.
func (c *Converter) convertNode(node *plan.Node, allNodes []*plan.Node) (*substrait.Rel, error) {
    switch node.NodeType {
    case plan.Node_TABLE_SCAN:
        return c.convertTableScan(node)

    case plan.Node_FILTER:
        child, err := c.convertChild(node, allNodes)
        if err != nil {
            return nil, err
        }
        condition, err := c.convertFilterList(node.FilterList)
        if err != nil {
            return nil, err
        }
        return &substrait.Rel{
            RelType: &substrait.Rel_Filter{
                Filter: &substrait.FilterRel{
                    Input:     child,
                    Condition: condition,
                },
            },
        }, nil

    case plan.Node_PROJECT:
        child, err := c.convertChild(node, allNodes)
        if err != nil {
            return nil, err
        }
        var exprs []*substrait.Expression
        for _, e := range node.ProjectList {
            se, err := c.convertExpr(e)
            if err != nil {
                return nil, err
            }
            exprs = append(exprs, se)
        }
        return &substrait.Rel{
            RelType: &substrait.Rel_Project{
                Project: &substrait.ProjectRel{
                    Input:       child,
                    Expressions: exprs,
                },
            },
        }, nil

    case plan.Node_AGG:
        child, err := c.convertChild(node, allNodes)
        if err != nil {
            return nil, err
        }
        return c.convertAggregate(node, child)

    case plan.Node_JOIN:
        if len(node.Children) < 2 {
            return nil, fmt.Errorf("JOIN node needs 2 children")
        }
        left, err := c.convertNode(allNodes[node.Children[0]], allNodes)
        if err != nil {
            return nil, err
        }
        right, err := c.convertNode(allNodes[node.Children[1]], allNodes)
        if err != nil {
            return nil, err
        }
        return c.convertJoin(node, left, right)

    case plan.Node_SORT:
        child, err := c.convertChild(node, allNodes)
        if err != nil {
            return nil, err
        }
        return c.convertSort(node, child)

    default:
        return nil, fmt.Errorf("unsupported node type: %v", node.NodeType)
    }
}

// convertChild converts the first (and usually only) child of a node.
func (c *Converter) convertChild(node *plan.Node, allNodes []*plan.Node) (*substrait.Rel, error) {
    if len(node.Children) == 0 {
        return nil, fmt.Errorf("node %d has no children", node.NodeId)
    }
    return c.convertNode(allNodes[node.Children[0]], allNodes)
}

// registerFunction ensures a function has an anchor ID and extension entry.
func (c *Converter) registerFunction(name string) uint32 {
    if id, ok := c.funcMap[name]; ok {
        return id
    }
    id := c.nextAnchor
    c.nextAnchor++
    c.funcMap[name] = id
    c.extensions = append(c.extensions, &substrait.Extension{
        MappingType: &substrait.Extension_ExtensionFunction_{
            ExtensionFunction: &substrait.Extension_ExtensionFunction{
                FunctionAnchor: id,
                Name:           name,
            },
        },
    })
    return id
}

// convertExpr converts a single MO expression to Substrait.
func (c *Converter) convertExpr(expr *plan.Expr) (*substrait.Expression, error) {
    switch e := expr.Expr.(type) {

    case *plan.Expr_Lit:
        return c.convertLiteral(e.Lit, &expr.Typ)

    case *plan.Expr_Col:
        // ColRef.ColPos → Substrait field reference (0-based)
        return &substrait.Expression{
            RexType: &substrait.Expression_Selection_{
                Selection: &substrait.Expression_FieldReference{
                    ReferenceType: &substrait.Expression_FieldReference_DirectReference{
                        DirectReference: &substrait.Expression_ReferenceSegment{
                            ReferenceType: &substrait.Expression_ReferenceSegment_StructField_{
                                StructField: &substrait.Expression_ReferenceSegment_StructField{
                                    Field: e.Col.ColPos,
                                },
                            },
                        },
                    },
                },
            },
        }, nil

    case *plan.Expr_F:
        return c.convertFunction(e.F, &expr.Typ)

    case *plan.Expr_T:
        // CAST
        child, err := c.convertExpr(e.T.GetExpr())  // source expression
        if err != nil {
            return nil, err
        }
        return &substrait.Expression{
            RexType: &substrait.Expression_Cast_{
                Cast: &substrait.Expression_Cast{
                    Input: child,
                    Type:  moTypeToSubstrait(&expr.Typ),
                },
            },
        }, nil

    default:
        return nil, fmt.Errorf("unsupported expression type: %T", expr.Expr)
    }
}

// convertFunction converts a MO Function call to Substrait ScalarFunction.
func (c *Converter) convertFunction(fn *plan.Function, resultType *plan.Type) (*substrait.Expression, error) {
    moName := fn.Func.ObjName
    subName := moFuncToSubstrait(moName)
    anchor := c.registerFunction(subName)

    var args []*substrait.FunctionArgument
    for _, arg := range fn.Args {
        se, err := c.convertExpr(arg)
        if err != nil {
            return nil, err
        }
        args = append(args, &substrait.FunctionArgument{
            ArgType: &substrait.FunctionArgument_Value{Value: se},
        })
    }

    return &substrait.Expression{
        RexType: &substrait.Expression_ScalarFunction_{
            ScalarFunction: &substrait.Expression_ScalarFunction{
                FunctionReference: anchor,
                Arguments:         args,
                OutputType:        moTypeToSubstrait(resultType),
            },
        },
    }, nil
}

// convertAggregate converts a MO AGG node to Substrait AggregateRel.
func (c *Converter) convertAggregate(
    node *plan.Node, child *substrait.Rel,
) (*substrait.Rel, error) {
    // Grouping expressions
    var groupExprs []*substrait.Expression
    for _, g := range node.GroupBy {
        se, err := c.convertExpr(g)
        if err != nil {
            return nil, err
        }
        groupExprs = append(groupExprs, se)
    }

    // Measures (aggregate functions)
    var measures []*substrait.AggregateRel_Measure
    for _, agg := range node.AggList {
        fn, ok := agg.Expr.(*plan.Expr_F)
        if !ok {
            return nil, fmt.Errorf("aggregate expression is not a function")
        }
        moName := fn.F.Func.ObjName
        subName := moFuncToSubstrait(moName)
        anchor := c.registerFunction(subName)

        var args []*substrait.FunctionArgument
        for _, arg := range fn.F.Args {
            se, err := c.convertExpr(arg)
            if err != nil {
                return nil, err
            }
            args = append(args, &substrait.FunctionArgument{
                ArgType: &substrait.FunctionArgument_Value{Value: se},
            })
        }

        measures = append(measures, &substrait.AggregateRel_Measure{
            Measure: &substrait.AggregateFunction{
                FunctionReference: anchor,
                Arguments:         args,
                OutputType:        moTypeToSubstrait(&agg.Typ),
            },
        })
    }

    return &substrait.Rel{
        RelType: &substrait.Rel_Aggregate{
            Aggregate: &substrait.AggregateRel{
                Input: child,
                Groupings: []*substrait.AggregateRel_Grouping{{
                    GroupingExpressions: groupExprs,
                }},
                Measures: measures,
            },
        },
    }, nil
}

// convertJoin converts a MO JOIN node to Substrait JoinRel.
func (c *Converter) convertJoin(
    node *plan.Node, left, right *substrait.Rel,
) (*substrait.Rel, error) {
    joinType := moJoinToSubstrait(node.JoinType)

    // AND together all join conditions
    condition, err := c.convertFilterList(node.OnList)
    if err != nil {
        return nil, err
    }

    return &substrait.Rel{
        RelType: &substrait.Rel_Join{
            Join: &substrait.JoinRel{
                Left:       left,
                Right:      right,
                Expression: condition,
                Type:       joinType,
            },
        },
    }, nil
}

// convertSort converts a MO SORT node to Substrait SortRel, optionally
// wrapped in a FetchRel if LIMIT/OFFSET are present.
func (c *Converter) convertSort(
    node *plan.Node, child *substrait.Rel,
) (*substrait.Rel, error) {
    var sorts []*substrait.SortField
    for _, ob := range node.OrderBy {
        se, err := c.convertExpr(ob.Expr)
        if err != nil {
            return nil, err
        }
        dir := substrait.SortField_SORT_DIRECTION_ASC_NULLS_LAST
        if ob.Flag == plan.OrderBySpec_DESC {
            dir = substrait.SortField_SORT_DIRECTION_DESC_NULLS_FIRST
        }
        sorts = append(sorts, &substrait.SortField{
            Expr:      se,
            Direction: dir,
        })
    }

    sortRel := &substrait.Rel{
        RelType: &substrait.Rel_Sort{
            Sort: &substrait.SortRel{
                Input: child,
                Sorts: sorts,
            },
        },
    }

    // Wrap in FetchRel if LIMIT present
    if node.Limit != nil {
        limitVal := extractInt64(node.Limit)
        var offsetVal int64
        if node.Offset != nil {
            offsetVal = extractInt64(node.Offset)
        }
        return &substrait.Rel{
            RelType: &substrait.Rel_Fetch{
                Fetch: &substrait.FetchRel{
                    Input:  sortRel,
                    Offset: offsetVal,
                    Count:  limitVal,
                },
            },
        }, nil
    }

    return sortRel, nil
}

// convertFilterList AND-s together a list of filter expressions.
func (c *Converter) convertFilterList(filters []*plan.Expr) (*substrait.Expression, error) {
    if len(filters) == 0 {
        return nil, fmt.Errorf("empty filter list")
    }
    if len(filters) == 1 {
        return c.convertExpr(filters[0])
    }
    // AND together: filters[0] AND filters[1] AND ...
    left, err := c.convertExpr(filters[0])
    if err != nil {
        return nil, err
    }
    for i := 1; i < len(filters); i++ {
        right, err := c.convertExpr(filters[i])
        if err != nil {
            return nil, err
        }
        anchor := c.registerFunction("and")
        left = &substrait.Expression{
            RexType: &substrait.Expression_ScalarFunction_{
                ScalarFunction: &substrait.Expression_ScalarFunction{
                    FunctionReference: anchor,
                    Arguments: []*substrait.FunctionArgument{
                        {ArgType: &substrait.FunctionArgument_Value{Value: left}},
                        {ArgType: &substrait.FunctionArgument_Value{Value: right}},
                    },
                    OutputType: &substrait.Type{
                        Kind: &substrait.Type_Bool{
                            Bool: &substrait.Type_Boolean{
                                Nullability: substrait.Type_NULLABILITY_NULLABLE,
                            },
                        },
                    },
                },
            },
        }
    }
    return left, nil
}
```

#### Helper Functions

```go
// moFuncToSubstrait maps MO function names to Substrait names.
func moFuncToSubstrait(name string) string {
    mapping := map[string]string{
        "=": "equal", ">": "gt", ">=": "gte", "<": "lt", "<=": "lte",
        "<>": "not_equal", "!=": "not_equal",
        "and": "and", "or": "or", "not": "not",
        "+": "add", "-": "subtract", "*": "multiply", "/": "divide",
        "mod": "modulus", "%": "modulus",
        "sum": "sum", "count": "count", "min": "min", "max": "max",
        "avg": "avg", "stddev": "std_dev", "any_value": "any_value",
        "substring": "substring", "substr": "substring",
        "length": "char_length", "like": "like",
        "starts_with": "starts_with", "ends_with": "ends_with",
        "date_part": "extract", "extract": "extract",
        "abs": "abs", "ceil": "ceil", "floor": "floor", "round": "round",
        "upper": "upper", "lower": "lower", "trim": "trim",
        "concat": "concat", "coalesce": "coalesce",
    }
    if sub, ok := mapping[name]; ok {
        return sub
    }
    return name  // pass through unknown functions
}

// moJoinToSubstrait maps MO join types to Substrait.
func moJoinToSubstrait(jt plan.Node_JoinType) substrait.JoinRel_JoinType {
    switch jt {
    case plan.Node_INNER:
        return substrait.JoinRel_JOIN_TYPE_INNER
    case plan.Node_LEFT:
        return substrait.JoinRel_JOIN_TYPE_LEFT
    case plan.Node_RIGHT:
        return substrait.JoinRel_JOIN_TYPE_RIGHT
    case plan.Node_OUTER:
        return substrait.JoinRel_JOIN_TYPE_OUTER
    case plan.Node_SEMI:
        return substrait.JoinRel_JOIN_TYPE_LEFT_SEMI
    case plan.Node_ANTI:
        return substrait.JoinRel_JOIN_TYPE_LEFT_ANTI
    default:
        return substrait.JoinRel_JOIN_TYPE_INNER
    }
}

// moTypeToSubstrait maps MO Type to Substrait Type.
func moTypeToSubstrait(typ *plan.Type) *substrait.Type {
    nullability := substrait.Type_NULLABILITY_NULLABLE
    if typ.NotNullable {
        nullability = substrait.Type_NULLABILITY_REQUIRED
    }

    switch types.T(typ.Id) {
    case types.T_bool:
        return &substrait.Type{Kind: &substrait.Type_Bool{
            Bool: &substrait.Type_Boolean{Nullability: nullability}}}
    case types.T_int8:
        return &substrait.Type{Kind: &substrait.Type_I8{
            I8: &substrait.Type_I8{Nullability: nullability}}}
    case types.T_int16, types.T_uint8:
        return &substrait.Type{Kind: &substrait.Type_I16{
            I16: &substrait.Type_I16{Nullability: nullability}}}
    case types.T_int32, types.T_uint16:
        return &substrait.Type{Kind: &substrait.Type_I32{
            I32: &substrait.Type_I32{Nullability: nullability}}}
    case types.T_int64, types.T_uint32, types.T_uint64:
        return &substrait.Type{Kind: &substrait.Type_I64{
            I64: &substrait.Type_I64{Nullability: nullability}}}
    case types.T_float32:
        return &substrait.Type{Kind: &substrait.Type_Fp32{
            Fp32: &substrait.Type_FP32{Nullability: nullability}}}
    case types.T_float64:
        return &substrait.Type{Kind: &substrait.Type_Fp64{
            Fp64: &substrait.Type_FP64{Nullability: nullability}}}
    case types.T_decimal64, types.T_decimal128:
        return &substrait.Type{Kind: &substrait.Type_Decimal{
            Decimal: &substrait.Type_Decimal{
                Precision: typ.Width, Scale: typ.Scale,
                Nullability: nullability}}}
    case types.T_date:
        return &substrait.Type{Kind: &substrait.Type_Date{
            Date: &substrait.Type_Date{Nullability: nullability}}}
    case types.T_time:
        return &substrait.Type{Kind: &substrait.Type_Time{
            Time: &substrait.Type_Time{Nullability: nullability}}}
    case types.T_datetime, types.T_timestamp:
        return &substrait.Type{Kind: &substrait.Type_PrecisionTimestamp_{
            PrecisionTimestamp: &substrait.Type_PrecisionTimestamp{
                Precision: 6, Nullability: nullability}}}
    case types.T_char, types.T_varchar, types.T_text, types.T_json:
        return &substrait.Type{Kind: &substrait.Type_Varchar{
            Varchar: &substrait.Type_VarChar{
                Length: uint32(typ.Width), Nullability: nullability}}}
    case types.T_binary, types.T_varbinary, types.T_blob:
        return &substrait.Type{Kind: &substrait.Type_Binary_{
            Binary: &substrait.Type_Binary{Nullability: nullability}}}
    default:
        return &substrait.Type{Kind: &substrait.Type_Varchar{
            Varchar: &substrait.Type_VarChar{Nullability: nullability}}}
    }
}
```

#### Converter Scope and Limitations

The converter handles the **analytical query subset** that Sirius can accelerate:

| Feature | Supported | Notes |
|---------|-----------|-------|
| SELECT with WHERE | ✅ | FilterRel |
| GROUP BY + aggregates | ✅ | AggregateRel |
| INNER/LEFT/RIGHT/OUTER JOIN | ✅ | JoinRel |
| ORDER BY + LIMIT/OFFSET | ✅ | SortRel + FetchRel |
| CASE WHEN | ✅ | IfThen expression |
| CAST | ✅ | Cast expression |
| IN list | ✅ | SingularOrList |
| Subqueries | ❌ | Fall back to Path 2 |
| CTEs (WITH) | ❌ | Fall back to Path 2 |
| Window functions | ⚠️ | Limited Sirius support |
| UDFs | ❌ | Fall back to Path 2 |
| INSERT/UPDATE/DELETE | ❌ | Not applicable for GPU |

When the converter encounters an unsupported feature, it returns an error and the
router falls back to Path 2 (SQL string forwarding) automatically — as shown in
§16.7's `executeWithGPUFallback()`.

#### File Layout

```
pkg/sql/sirius/
├── substrait_converter.go     // Converter struct + Convert() entry point
├── substrait_node.go          // convertNode, convertTableScan, convertAggregate, etc.
├── substrait_expr.go          // convertExpr, convertLiteral, convertFunction
├── substrait_type.go          // moTypeToSubstrait, moFuncToSubstrait
├── substrait_converter_test.go // Unit tests with known MO plans → expected Substrait
└── client.go                  // SidecarClient (gRPC, from §16.10)
```

Estimated size: ~2000 LOC Go (converter) + ~200 LOC test.

### 17.14 Monitoring and Observability

GPU-accelerated queries span two processes (MO + sidecar) and two hardware domains
(CPU + GPU). Observability must cover both to diagnose performance issues, failures,
and capacity planning.

#### Metrics (Prometheus)

**MO-side metrics** (exported by the GPU router in MO):

```
# Query routing decisions
sirius_queries_total{path="2|3|cpu"}                  counter
sirius_queries_errors_total{path="2|3",error="..."}   counter
sirius_fallback_total{from="3",to="2|cpu"}            counter

# Latency (histogram, seconds)
sirius_query_duration_seconds{path="2|3"}             histogram
sirius_grpc_roundtrip_seconds                         histogram
sirius_arrow_to_mysql_seconds                         histogram

# Result sizes
sirius_result_rows_total                              counter
sirius_result_bytes_total                             counter
sirius_result_batches_total                           counter

# Sidecar health
sirius_sidecar_up                                     gauge (0 or 1)
sirius_sidecar_reconnects_total                       counter
```

**Sidecar-side metrics** (exported by the gRPC server in the sidecar):

```
# Execution
sirius_sidecar_queries_total{method="sql|plan"}       counter
sirius_sidecar_query_duration_seconds{phase="plan|gpu|transfer|total"}  histogram

# tae_scan I/O
tae_scan_blocks_scanned_total                         counter
tae_scan_blocks_skipped_total                         counter
tae_scan_bytes_read_total                             counter
tae_scan_objects_read_total                           counter

# GPU resources
sirius_gpu_memory_used_bytes                          gauge
sirius_gpu_memory_peak_bytes                          gauge
sirius_gpu_utilization_percent                        gauge
sirius_gpu_memory_pool_size_bytes{pool="device|host"} gauge

# DuckDB internals
sirius_duckdb_buffer_pool_size_bytes                  gauge
sirius_duckdb_temp_directory_size_bytes               gauge
```

**Collection:** Both MO and sidecar expose `/metrics` HTTP endpoints. Prometheus
scrapes both. For the sidecar, a lightweight HTTP server runs alongside gRPC:

```cpp
// In sirius_sidecar_server.cpp
httplib::Server metrics_server;
metrics_server.Get("/metrics", [](const auto& req, auto& res) {
    res.set_content(prometheus_registry.Serialize(), "text/plain");
});
metrics_server.listen("0.0.0.0", 9090);  // Prometheus scrape port
```

#### Distributed Tracing (OpenTelemetry)

Each GPU query gets a trace spanning both MO and the sidecar:

```
Trace: query-abc123
├── [MO] executeWithGPUFallback          12.5ms
│   ├── [MO] shouldRouteToGPU            0.1ms
│   ├── [MO] convertToSubstrait          0.3ms   (Path 3 only)
│   ├── [MO] grpc.ExecutePlan            11.8ms
│   │   ├── [Sidecar] from_substrait     0.5ms   (deserialize plan)
│   │   ├── [Sidecar] plan_generator     0.2ms   (DuckDB → Sirius physical plan)
│   │   ├── [Sidecar] gpu_execute        8.1ms   (GPU pipeline)
│   │   │   ├── tae_scan                 2.3ms   (read TAE objects)
│   │   │   ├── filter                   0.8ms   (GPU predicate eval)
│   │   │   ├── aggregate                3.2ms   (GPU hash aggregate)
│   │   │   └── collect                  1.8ms   (GPU → host transfer)
│   │   └── [Sidecar] serialize_arrow    0.4ms   (Arrow IPC)
│   └── [MO] arrowToMySQL               0.3ms   (result conversion)
```

**Implementation:** MO already uses OpenTelemetry (`pkg/util/trace/`). The `query_id`
passed in gRPC requests carries the trace context. The sidecar extracts it from
gRPC metadata and creates child spans:

```go
// MO side: inject trace context into gRPC call
ctx = otel.InjectTraceContext(ctx)
stream, err := sidecar.ExecutePlan(ctx, &pb.ExecutePlanRequest{
    SubstraitPlan: planBytes,
    QueryId:       span.SpanContext().TraceID().String(),
})
```

```cpp
// Sidecar side: extract trace context from gRPC metadata
auto trace_id = ctx->client_metadata().find("traceparent");
auto span = tracer->StartSpan("gpu_execute", {trace_id});
```

#### Logging

**Structured log events** at key decision points:

```
MO Router:
  level=info  msg="GPU query routed"  path=3  query_id=abc123  table=lineitem  est_rows=6M
  level=info  msg="Substrait conversion failed, falling back"  path=2  error="unsupported: CTE"
  level=warn  msg="Sidecar unavailable, using CPU"  addr=gpu01:50051  retry=3

Sidecar:
  level=info  msg="Executing plan"  method=substrait  query_id=abc123
  level=info  msg="tae_scan stats"  blocks_scanned=142  blocks_skipped=89  skip_ratio=0.385
  level=info  msg="GPU execution complete"  duration=8.1ms  gpu_mem_peak=1.2GB  rows=1500
  level=error msg="GPU OOM"  query_id=abc123  gpu_mem_used=78GB  gpu_mem_total=80GB
```

#### Health Checks

The sidecar implements a health check protocol for MO to detect failures:

```protobuf
// Added to SiriusSidecar service definition
service SiriusSidecar {
  // ... existing RPCs ...

  // Health check — MO calls periodically (every 5s)
  rpc HealthCheck(HealthCheckRequest) returns (HealthCheckResponse);
}

message HealthCheckRequest {}

message HealthCheckResponse {
  enum Status {
    HEALTHY = 0;         // Ready to accept queries
    DEGRADED = 1;        // Working but GPU memory pressure
    UNHEALTHY = 2;       // Cannot process queries
  }
  Status status = 1;
  string message = 2;

  // Current load
  uint32 active_queries = 3;
  uint32 max_concurrent_queries = 4;
  uint64 gpu_memory_available_bytes = 5;
}
```

MO maintains a connection pool and health state for each sidecar:

```go
type SidecarPool struct {
    mu       sync.RWMutex
    sidecars []*SidecarClient
    healthy  map[string]bool       // addr → healthy
    metrics  map[string]*SidecarMetrics
}

func (p *SidecarPool) PickHealthy() *SidecarClient {
    p.mu.RLock()
    defer p.mu.RUnlock()
    // Round-robin among healthy sidecars with available capacity
    for _, sc := range p.sidecars {
        if p.healthy[sc.addr] && sc.ActiveQueries() < sc.MaxConcurrent() {
            return sc
        }
    }
    return nil  // all busy or unhealthy → fall back to CPU
}
```

#### Dashboard (Grafana)

Key panels for the GPU acceleration dashboard:

```
Row 1: Overview
├── GPU Query Rate (queries/sec by path)
├── GPU Query Latency (p50, p95, p99)
├── Fallback Rate (Path 3→2, Path 2→CPU)
└── Sidecar Health (up/down per instance)

Row 2: GPU Performance
├── GPU Utilization (%)
├── GPU Memory Usage (used vs total)
├── GPU Execution Time Breakdown (scan / filter / aggregate / transfer)
└── Zone Map Skip Ratio (blocks skipped / total)

Row 3: Data Flow
├── Arrow Bytes Transferred (MO ↔ sidecar)
├── Result Rows per Query (histogram)
├── TAE Blocks Scanned vs Skipped
└── gRPC Roundtrip Latency

Row 4: Errors & Alerts
├── Substrait Conversion Errors (by reason)
├── GPU OOM Events
├── Sidecar Restarts
└── Query Timeout Rate
```

**Alerting rules:**

```yaml
# Prometheus alerting rules
groups:
  - name: sirius
    rules:
      - alert: SidecarDown
        expr: sirius_sidecar_up == 0
        for: 30s
        labels: { severity: critical }
        annotations: { summary: "Sirius sidecar {{ $labels.instance }} is down" }

      - alert: GPUMemoryHigh
        expr: sirius_gpu_memory_used_bytes / sirius_gpu_memory_peak_bytes > 0.9
        for: 5m
        labels: { severity: warning }
        annotations: { summary: "GPU memory usage above 90%" }

      - alert: HighFallbackRate
        expr: rate(sirius_fallback_total[5m]) / rate(sirius_queries_total[5m]) > 0.3
        for: 10m
        labels: { severity: warning }
        annotations: { summary: "More than 30% of GPU queries falling back" }

      - alert: GPUQueryLatencyHigh
        expr: histogram_quantile(0.99, sirius_query_duration_seconds) > 30
        for: 5m
        labels: { severity: warning }
        annotations: { summary: "p99 GPU query latency above 30s" }
```

### 17.15 Security and Authentication

The gRPC channel between MO and the Sirius sidecar carries SQL queries and result
data. This section specifies the security model for each deployment topology.

#### Threat Model

| Threat | Risk | Mitigation |
|--------|------|------------|
| Eavesdropping on query/result data | High (data exfiltration) | mTLS encryption |
| Unauthorized sidecar access | High (arbitrary query execution) | mTLS client certs |
| Spoofed sidecar (MITM) | High (result tampering) | mTLS server cert validation |
| Denial of service (query flood) | Medium | Rate limiting + query concurrency cap |
| Privilege escalation via SQL injection | Medium (Path 2) | Parameterized queries, read-only mode |
| Resource exhaustion (GPU OOM) | Medium | Memory limits + query timeout |

#### Transport Security: mTLS

All gRPC connections use mutual TLS (mTLS). Both MO and sidecar present certificates
signed by the same internal CA.

```
┌──────────────────┐        mTLS (TLS 1.3)        ┌──────────────────┐
│       MO CN      │◄────────────────────────────►│  Sirius Sidecar  │
│                  │                                │                  │
│ client cert:     │                                │ server cert:     │
│   mo-cn-01.pem   │                                │   sirius-01.pem  │
│ client key:      │                                │ server key:      │
│   mo-cn-01-key.pem│                               │   sirius-01-key.pem│
│ CA cert:         │                                │ CA cert:         │
│   ca.pem         │                                │   ca.pem         │
└──────────────────┘                                └──────────────────┘
```

**MO client configuration:**

```go
func NewSecureSidecarClient(addr string, cfg *TLSConfig) (*SidecarClient, error) {
    cert, err := tls.LoadX509KeyPair(cfg.CertFile, cfg.KeyFile)
    if err != nil {
        return nil, err
    }

    caCert, err := os.ReadFile(cfg.CAFile)
    if err != nil {
        return nil, err
    }
    caPool := x509.NewCertPool()
    caPool.AppendCertsFromPEM(caCert)

    tlsCfg := &tls.Config{
        Certificates: []tls.Certificate{cert},
        RootCAs:      caPool,
        MinVersion:   tls.VersionTLS13,
        ServerName:   cfg.ServerName,  // must match sidecar cert CN/SAN
    }

    conn, err := grpc.Dial(addr,
        grpc.WithTransportCredentials(credentials.NewTLS(tlsCfg)),
        grpc.WithKeepaliveParams(keepalive.ClientParameters{
            Time:    30 * time.Second,
            Timeout: 10 * time.Second,
        }),
    )
    if err != nil {
        return nil, err
    }
    return &SidecarClient{conn: conn, client: pb.NewSiriusSidecarClient(conn)}, nil
}
```

**Sidecar server configuration:**

```cpp
grpc::SslServerCredentialsOptions ssl_opts;
ssl_opts.pem_root_certs = ReadFile("ca.pem");                    // CA cert
ssl_opts.pem_key_cert_pairs.push_back({
    ReadFile("sirius-01-key.pem"),  // server key
    ReadFile("sirius-01.pem")       // server cert
});
ssl_opts.client_certificate_request =
    GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;  // enforce mTLS

auto creds = grpc::SslServerCredentials(ssl_opts);
builder.AddListeningPort("0.0.0.0:50051", creds);
```

#### Deployment-Specific Policies

**Same machine (localhost):** mTLS is optional but recommended. Alternative: Unix
domain sockets with filesystem permissions (`chmod 0600 /var/run/sirius.sock`).

**Same Kubernetes cluster:** Use a service mesh (Istio/Linkerd) for automatic mTLS
between pods. No application-level TLS needed — the sidecar proxy handles it.

**Cross-network:** mTLS is mandatory. Certificates issued by a private CA (e.g.,
Vault PKI, cert-manager). Rotate certificates automatically with short TTLs (24h).

#### Authorization

The sidecar operates in **read-only mode** — it can only execute SELECT queries
via `gpu_execution()`. No DDL, DML, or system commands are allowed.

```cpp
// Sidecar enforces read-only at the DuckDB level
conn.Query("SET access_mode = 'read_only'");

// Additional check before execution
if (!IsSelectQuery(sql)) {
    return grpc::Status(grpc::PERMISSION_DENIED, "only SELECT queries allowed");
}
```

**Per-CN identity:** The sidecar extracts the client certificate's Common Name (CN)
from the mTLS handshake. This identifies which MO CN node is making the request,
enabling per-CN rate limiting and audit logging:

```cpp
auto peer_identity = ctx->auth_context()->GetPeerIdentity();  // "mo-cn-01"
LOG_INFO << "Query from " << peer_identity << ": " << query_id;
```

#### Rate Limiting and Resource Protection

```cpp
// Sidecar-side limits
const int MAX_CONCURRENT_QUERIES = 8;       // GPU can handle ~8 parallel queries
const int MAX_QUERY_TIMEOUT_SEC = 300;      // 5 minute hard timeout
const size_t MAX_GPU_MEMORY_PER_QUERY = 8ULL * 1024 * 1024 * 1024;  // 8 GB

std::atomic<int> active_queries{0};

grpc::Status ExecuteSQL(...) override {
    if (active_queries.load() >= MAX_CONCURRENT_QUERIES) {
        return grpc::Status(grpc::RESOURCE_EXHAUSTED,
            "too many concurrent queries");
    }
    active_queries++;
    defer { active_queries--; };

    // Set per-query timeout
    conn.Query("SET statement_timeout = '300s'");

    // Set per-query memory limit (DuckDB + RMM)
    conn.Query("SET max_memory = '8GB'");

    // ... execute query ...
}
```

#### Audit Log

All queries routed to the sidecar are logged with full provenance:

```json
{
  "timestamp": "2024-12-15T10:30:45.123Z",
  "event": "gpu_query",
  "query_id": "abc123",
  "trace_id": "def456",
  "source_cn": "mo-cn-01",
  "path": 3,
  "database": "tpch",
  "tables": ["lineitem", "orders"],
  "duration_ms": 125,
  "gpu_memory_peak_mb": 1200,
  "rows_returned": 1500,
  "blocks_scanned": 142,
  "blocks_skipped": 89,
  "status": "ok"
}
```

MO-side audit entry (for the routing decision):

```json
{
  "timestamp": "2024-12-15T10:30:45.100Z",
  "event": "gpu_route_decision",
  "query_id": "abc123",
  "user": "admin",
  "client_ip": "10.0.1.50",
  "decision": "gpu",
  "path": 3,
  "reason": "has_aggregate, est_rows=6000000",
  "sidecar": "gpu01:50051"
}
```

#### Configuration

```toml
# mo.toml — Sirius integration settings
[sirius]
enabled = true
sidecar_addresses = ["gpu01:50051", "gpu02:50051"]

  [sirius.tls]
  enabled = true
  cert_file = "/etc/mo/certs/mo-cn.pem"
  key_file = "/etc/mo/certs/mo-cn-key.pem"
  ca_file = "/etc/mo/certs/ca.pem"
  server_name = "sirius-sidecar"

  [sirius.routing]
  min_estimated_rows = 100000     # Don't GPU-route small queries
  max_concurrent_gpu_queries = 16 # Per CN limit
  query_timeout = "300s"
  fallback_enabled = true         # Fall back to CPU on sidecar failure

  [sirius.pool]
  max_idle_conns = 4
  health_check_interval = "5s"
  reconnect_backoff_max = "30s"
```
