# Bitmap Extension Next Sprint Plan

## Sprint Objective

Deliver a production-leaning bitmap path that performs well on 50M-150M rows and remains correct for larger data volumes, with a clear path to optional 64-bit row-id support.

Primary outcomes for this sprint:

- replace list-materialization build pattern with streaming aggregate build
- switch bitmap payload internals from sorted-u32 arrays to Roaring (with run optimization)
- add count-focused operations to avoid unnecessary row-id expansion
- replace full `UBIGINT[]` row-id expansion with streaming/chunked row retrieval
- leverage DuckDB's vectorized chunk execution instead of fighting it with scalar/list materialization
- evaluate row-group-local/sharded bitmap layouts that improve pruning, locality, and CPU-cache behavior
- add skew-aware posting strategy for very low-cardinality columns (including 2-value columns)
- define and implement a 64-bit strategy decision, not just discussion

## Non-Functional Goals

1. Define explicit SLOs for key user paths:
- `count`, `facet`, and `row-id expansion`
- measured at 50M, 150M, and skewed 95/5 datasets
- tracked as p50 and p95 latency
2. Define memory budgets for build and query paths:
- peak RSS thresholds per workload
- clear fail-fast behavior with actionable errors on budget breach
3. Guarantee backward compatibility:
- legacy `SORTED_U32` blobs remain readable
- include migration path and verification for re-encoding to roaring
4. Add robustness hardening:
- malformed payload and boundary-id test coverage
- fuzz/property-style tests for parser/envelope handling
5. Validate concurrent workload behavior:
- correctness and latency checks with parallel query sessions
- not only single-session microbenchmarks
6. Add regression guardrails:
- CI perf smoke suite with threshold checks
- fail build on major regressions in critical workloads
7. Operationalize skew strategy:
- automatic `EXACT` vs `COMPLEMENT` selection thresholds
- benchmark-driven default thresholds and documented override knobs
8. Improve observability/debuggability:
- expose encoding/cardinality/serialized-size diagnostics
- make tuning decisions based on collected metrics rather than ad-hoc inspection

## Current Baseline (MVP)

- payload encoding is sorted unique `uint32` ids in a custom envelope
- `bm_build` takes a `LIST<UBIGINT>` and sorts/deduplicates in extension code
- binary ops allocate temporary arrays sized by input cardinalities
- `bm_to_rows(BLOB) -> UBIGINT[]` materializes the complete matching row-id list before callers can apply `LIMIT`
- row-id domain is limited to `UINT32_MAX` (`4,294,967,295`)

This is correct but not optimized for large skewed postings.

## SF1 Benchmark Findings To Explain

The expanded SF1 workload suite separates one-time setup from repeated query latency:

- one-time TPC-H/data setup: about `12.1s`
- one-time bitmap posting build: about `2.1s`
- repeated query latency is measured after bitmap postings already exist

Observed repeated-query results at SF1:

- multi-field count: baseline `~63ms`, bitmap `~162ms`
- single-field count: baseline `~61ms`, bitmap `~81ms`
- single-field fetch of first 10k fact rows: baseline `~114ms`, bitmap `~5946ms`
- multi-field fetch of first 10k fact rows: baseline `~112ms`, bitmap `~986ms`

Interpretation:

- bitmap creation cost is now measured separately, so the query comparison is not unfairly charging each query for index build
- count paths are closer than fetch paths, but still slower because the MVP repeatedly parses, allocates, combines, and serializes sorted arrays
- fetch paths are structurally poor because `bm_to_rows` returns a full list; DuckDB expands all matching row ids before the final join/order/limit can return the first page
- single-field fetch is worst because a broad selection can produce millions of row ids and then discard most of them after `LIMIT 10000`
- the next sprint must optimize both bitmap algebra and row retrieval shape; Roaring alone is not enough if row-id expansion remains all-at-once

## DuckDB And Bitmap Research Findings To Apply

DuckDB-specific findings:

- DuckDB executes through `DataChunk`s of column `Vector`s, with a default vector size of `2048` rows. Extension functions should process the whole input chunk through raw vector data and validity masks, not row-at-a-time APIs. Source: [DuckDB execution format](https://duckdb.org/docs/lts/internals/vector.html).
- The stable extension path is the C API. Aggregate and table-function callbacks are available there and map directly to bitmap build/combine/finalize and chunked row-id output. Source: [DuckDB C API](https://duckdb.org/docs/current/clients/c/api.html).
- DuckDB storage is already columnar and row-grouped. Row groups are roughly `120K` rows in native storage, and zonemaps are automatic for general-purpose columns. Ordered data improves both compression and pruning. Sources: [DuckDB storage/compression](https://duckdb.org/2022/10/28/lightweight-compression.html), [DuckDB indexing/zonemaps](https://duckdb.org/docs/current/guides/performance/indexing.html).
- DuckDB's buffer manager caches persistent database pages under `memory_limit`, but extension heap memory is still a risk if large bitmap states are allocated outside DuckDB's accounting. Source: [DuckDB memory management](https://duckdb.org/2024/07/09/memory-management.html).
- DuckDB already has `BITSTRING`, `bitstring_agg`, `bit_count`, and bitwise operators. These should be benchmark baselines for dense integer domains before custom code is considered a win. Source: [DuckDB bitstring functions](https://duckdb.org/docs/stable/sql/functions/bitstring).

Other database lessons:

- PostgreSQL bitmap scans combine index results in bitmap space, then visit heap pages in physical order for locality. It also has exact/lossy memory behavior under pressure. Source: [PostgreSQL bitmap index scans](https://www.postgresql.org/docs/current/indexes-bitmap-scans.html).
- ClickHouse exposes bitmap aggregates/functions with cardinality-only operations and subset/range helpers; its text index uses Roaring posting lists and uses smaller representations for tiny postings. Sources: [ClickHouse bitmap functions](https://clickhouse.com/docs/sql-reference/functions/bitmap-functions), [ClickHouse text-index posting lists](https://clickhouse.com/blog/clickhouse-full-text-search-object-storage).
- Pinot and Druid both store segment-local value-to-document-id bitmaps and avoid building bitmap indexes for every column/value when selectivity and cardinality do not justify it. Sources: [Pinot inverted index](https://docs.pinot.apache.org/functions/indexing/inverted-index), [Druid segments](https://druid.apache.org/docs/latest/design/segments/).

Implication: the implementation should not try to bypass DuckDB's executor or internal column storage in this sprint. It should expose bitmap work as aggregate and table functions, keep normal SQL predicates visible so DuckDB can still use scans/zonemaps, store postings in normal DuckDB tables so the buffer manager can cache them, and make row-id output chunked and ordered.

## Scope For This Sprint

In scope:

- new encoding path: `ROARING32`
- backward-compatible reader for legacy `SORTED_U32` blobs
- new aggregate functions for streaming build and bitmap union
- count-only fast paths
- streaming/chunked row-id table function or equivalent limited export path
- DuckDB vector/chunk-aware implementation paths for build, combine, and row export
- row-group-local or container-sharded bitmap prototype and benchmark decision
- baselines against DuckDB `BITSTRING`, zonemap-pruned SQL, and ordinary indexed/selective queries
- skew-aware storage mode (`EXACT` vs `COMPLEMENT`)
- benchmark and correctness expansion

Out of scope:

- optimizer/planner rewrite rules
- direct replacement of DuckDB's table scan or internal column-storage operators
- distributed execution
- mutable incremental bitmap maintenance (full CDC/upserts)
- shipping multiple OS packages

## Proposed SQL Surface (End of Sprint)

Keep existing functions working:

- `bm_or(BLOB,BLOB) -> BLOB`
- `bm_and(BLOB,BLOB) -> BLOB`
- `bm_andnot(BLOB,BLOB) -> BLOB`
- `bm_count(BLOB) -> UBIGINT`
- `bm_contains(BLOB,UBIGINT) -> BOOLEAN`
- `bm_to_rows(BLOB) -> UBIGINT[]`
- `bm_build(UBIGINT[]) -> BLOB`

Additions:

- `bm_build_agg(UBIGINT) -> BLOB` (streaming aggregate build, preferred for index build)
- `bm_or_agg(BLOB) -> BLOB` (streaming union of selected postings)
- `bm_count_and(BLOB,BLOB) -> UBIGINT`
- `bm_count_or(BLOB,BLOB) -> UBIGINT`
- `bm_count_andnot(BLOB,BLOB) -> UBIGINT`
- `bm_intersects(BLOB,BLOB) -> BOOLEAN`
- `bm_to_rows_table(BLOB)` or `bm_to_rows(BLOB, limit, start_after) -> UBIGINT[]` (streaming/chunked row retrieval for paging/export batches)
- `bm_format(BLOB) -> VARCHAR` (debug helper: `sorted_u32_v1`, `roaring32_v2`, etc.)
- `bm_stats(BLOB) -> VARCHAR` or `STRUCT` (debug helper: encoding, cardinality, serialized bytes, container/shard counts where available)
- `bm_reencode_roaring(BLOB) -> BLOB` (optional migration helper)

## Workstream A - Encoding Upgrade To Roaring32

### Design

- extend envelope version/encoding values to include `ROARING32`
- keep existing `SORTED_U32` decoder for backward compatibility
- encode new outputs as `ROARING32` by default
- run `run_optimize` on finalized roaring bitmaps to compress long runs

### Expected impact

- better compression for dense ranges
- faster set operations on large postings
- lower memory pressure during operation chains

## Workstream B - Streaming Build And Streaming Union

### Why

`bm_build(list(id))` materializes full lists per group and then sorts again in extension code.

### Implementation

- implement `bm_build_agg(id)` using DuckDB aggregate-function API
- aggregate state stores a mutable roaring bitmap and updates per input row
- aggregate update processes the full DuckDB input chunk through vector data and validity masks
- branch once for all-valid vs nullable inputs; skip NULLs unless later product semantics require a NULL posting
- use bulk/range insertion where possible for dense or monotonic row-id input
- combine partial aggregate states by in-place roaring union; finalize serializes once
- add aggregate state destructor for heap-owned CRoaring objects
- finalize returns one serialized bitmap blob
- implement `bm_or_agg(bitmap)` to union selected postings without `unnest + list + rebuild`

### Query pattern change

Current:

```sql
SELECT bm_build(list(id ORDER BY id)) FROM ...
```

Target:

```sql
SELECT bm_build_agg(id) FROM ...
```

For filter unions:

```sql
SELECT bm_or_agg(bitmap) FROM postings WHERE value IN (...);
```

This should replace nested scalar unions such as `bm_or(bm_or(a, b), c)` in benchmark and app query templates.

## Workstream C - Streaming Row Retrieval And Fact Fetch

### Problem

The current fetch path expands all row ids:

```sql
SELECT *
FROM unnest(bm_to_rows(bitmap)) r
JOIN fact USING (rid)
ORDER BY rid
LIMIT 10000;
```

This is slow because `bm_to_rows` returns a full list before the join and limit can reduce the result. For broad single-field selections, millions of row ids may be materialized to return only the first 10k rows.

### Target behavior

Provide a row-id retrieval shape that can stop early and page through a bitmap:

```sql
SELECT *
FROM bm_to_rows_table(?) r
JOIN fact USING (rid)
ORDER BY rid
LIMIT 10000;
```

Or, if a scalar-list helper is simpler for the C API MVP:

```sql
SELECT *
FROM unnest(bm_to_rows(?, 10000, ?)) r(rid)
JOIN fact USING (rid)
ORDER BY rid;
```

Where the arguments mean:

- `bitmap`: active filtered set
- `limit`: maximum row ids to emit
- `start_after`: resume cursor for next page/export batch

### Implementation options

Option 1 - DuckDB table function:

- preferred final shape
- streams row ids in vectors/chunks
- fills each output `DataChunk` up to DuckDB's vector size instead of building a giant child list
- tracks iterator state across calls and supports `limit`/`start_after`
- sets exact cardinality at bind time when the bitmap is constant and cardinality is known
- emits row ids in ascending `rid` order to preserve locality for join-back/fetch
- lets DuckDB pipeline the join more naturally
- avoids allocating one giant `UBIGINT[]`

Option 2 - limited scalar helper:

- easier intermediate step if table-function C API work is larger
- returns at most N row ids
- supports paging/export batches without full expansion

### Acceptance target

For SF1 first-page fetch workloads:

- bitmap single-field fetch should be within 2x of baseline `WHERE ... LIMIT 10000`
- bitmap multi-field fetch should be within 2x of baseline `WHERE ... LIMIT 10000`
- no query should expand more row ids than needed for the requested page/batch unless explicitly running a full export

## Workstream D - Skew-Aware Strategy For Low Cardinality

### Problem

If a column has very few values (for example 2), one posting can be extremely large and dominate runtime and memory.

### Strategy

Store posting metadata with mode:

- `EXACT`: bitmap represents matching row ids
- `COMPLEMENT`: bitmap represents excluded row ids; matching set is `domain_bitmap ANDNOT excluded`

Suggested metadata fields:

- `posting_mode` (`EXACT` or `COMPLEMENT`)
- `row_count`
- `domain_row_count`

### Selection rules

- if selectivity is low enough, store/select `EXACT`
- if value is dominant (for example > 70%-85%), store/select `COMPLEMENT`
- for binary columns, optionally store only the smaller side and derive the other via complement

### Result

Large dominant selections become cheap, especially in 2-value columns.

## Workstream E - Fast Count Paths

### Problem

`bm_to_rows` and row-id expansion is expensive when only counts are needed.

### Additions

- `bm_count_and` and `bm_count_or`
- `bm_intersects` for short-circuit checks
- optional multi-input helpers if profiling shows binary chaining still creates too many intermediate blobs

### Guidance

- use count functions for UI counters/facets
- use `bm_to_rows` only for exports/drill-down
- never use row-id expansion for count-only UI paths

## Workstream F - DuckDB-Native Storage, Chunk, And Cache Strategy

### Chunk/vector strategy

- keep the extension on DuckDB's stable C API for this sprint
- process scalar and aggregate inputs as full chunks, using `duckdb_vector_get_data`, `duckdb_vector_get_validity`, and `duckdb_data_chunk_get_size`
- avoid per-row allocation and repeated parse/serialize loops inside a single chunk
- where a future pinned C++ build is justified, evaluate `UnifiedVectorFormat` and selection-vector-aware paths for dictionary/constant vectors

### Row-group-local or sharded bitmap layout

Prototype a posting representation that can be stored as either:

- one global bitmap per `(column, value)`, current conceptual model
- multiple shards per `(column, value)`, such as `(column, value, shard_id, base_rid, bitmap, cardinality, min_rid, max_rid, posting_mode)`

Benchmark two shard choices:

- DuckDB-row-group-sized shards, roughly `120K` rows, for alignment with storage pruning/fetch locality
- Roaring-container-sized shards, `65,536` rows, for simpler container-local operations

Expected benefits:

- lower peak memory during operations because large postings can be combined shard-by-shard
- faster negative checks by skipping shards with zero cardinality or non-overlapping min/max ranges
- better CPU-cache locality because hot operations touch smaller containers
- more natural complement handling because dominant values can be represented per shard
- more physical locality for fact fetches when row ids are dense and ordered

Constraints:

- persisted correctness must depend on an explicit stable dense `rid`, not DuckDB's physical row position unless table lifecycle is controlled and postings are rebuilt
- do not hide ordinary fact-table filters inside opaque bitmap UDFs; keep them visible so DuckDB can still apply predicate pushdown and zonemaps
- direct hooks into DuckDB's internal column storage/table scan are out of scope for this sprint

### Column storage and buffer-cache leverage

- store posting tables as normal DuckDB tables sorted by `(column_name, value, shard_id)` or one physical table per indexed column, so zonemaps and columnar compression help posting lookup
- keep posting metadata columns separate from BLOB payloads so lookups can read value/cardinality/min/max metadata without touching payload BLOBs
- rely on DuckDB's buffer manager to cache persisted posting pages across repeated queries; do not add an unbounded extension-global decoded bitmap cache in V1
- if decoded-result caching is needed, prefer explicit temp tables or app/service-layer cache keyed by filter signature and source-table version

### CPU-cache guidance

- design hot loops around chunk-sized and shard-sized working sets, not whole-table arrays
- prefer cardinality-only CRoaring APIs and word-wise popcount paths when the result does not need row-id materialization
- run Roaring `run_optimize` and `shrink_to_fit` at finalize/persist time, not after every intermediate operation
- keep row ids dense and monotonic to encourage Roaring run containers and DuckDB zonemap-friendly physical order

### Baselines to include

- `bit_count(bitstring_agg(rid))` for dense integer domains
- ordinary DuckDB SQL with visible predicates, including sorted vs unsorted data to measure zonemap impact
- ART/index-scan behavior for highly selective equality predicates where DuckDB may already be competitive

## 64-bit Representation: Tradeoffs And Recommendation

## Option 1 - Keep roaring32 + surrogate dense row id (recommended default)

Approach:

- keep extension internals 32-bit roaring
- map natural keys to dense surrogate `fp_row_id` in `[0, 2^32-1]`

Pros:

- best performance and compression in practice
- simplest operations and smallest payloads
- mature tooling ecosystem

Cons:

- requires surrogate-id pipeline
- hard ceiling at ~4.29B rows per row-id domain

## Option 2 - Native 64-bit bitmap representation

Approach:

- store 64-bit row ids directly in bitmap structure

Pros:

- no 32-bit ceiling concerns
- direct modeling of very large domains

Cons:

- larger memory/storage footprint
- slower operations relative to 32-bit in most workloads
- more complex implementation and migration path

## Option 3 - Sharded 32-bit domains

Approach:

- split row-id space into shards by high bits; each shard uses roaring32

Pros:

- extends beyond 32-bit while retaining 32-bit container performance per shard
- can be a middle ground between options 1 and 2

Cons:

- more complex query logic and metadata
- extra orchestration overhead

## Recommendation

For next sprint, implement Option 1 and design envelopes/metadata to permit Option 3 later if needed. Only move to Option 2 when a real requirement exceeds `2^32-1` row ids per domain.

## Performance Tuning Tactics For 50M-150M+ Rows

1. Use `bm_build_agg` and `bm_or_agg` to eliminate list materialization and rebuild cycles.
2. Keep all count/facet paths in bitmap space (`bm_count_*`), avoid fact-table scans.
3. Use streaming/chunked row-id retrieval for fact fetches; do not expand full matching sets for first-page queries.
4. Process DuckDB input vectors chunk-at-a-time and branch on validity once per vector when possible.
5. Store and combine large postings by shard/row-group when benchmarks show lower memory or better locality than a global bitmap.
6. Apply skew-aware complement mode for dominant values.
7. Use roaring run optimization after build/finalize, not after every scalar operation.
8. Cache frequently used union/intersection results in app/service layer or explicit temp tables keyed by filter signature and table version.
9. Avoid unnecessary deserialization/serialization loops in SQL plans (combine in one function call path where possible).
10. Precompute and persist domain bitmap per table/shard to make complement operations constant-shape.
11. Add a build-time threshold policy per column:
- do not bitmap-index columns with poor selectivity value for the UI
- or only index values under cardinality/selectivity thresholds
12. Keep row ids dense and monotonic where possible to maximize run compression quality and ordered fetch locality.
13. Keep ordinary SQL predicates visible so DuckDB can still use zonemaps and predicate pushdown.
14. Benchmark with skewed synthetic datasets, not only uniform TPC-H patterns.
15. Compare against DuckDB-native `BITSTRING` and zonemap-pruned SQL before declaring custom bitmap wins.

## Concrete Implementation Tasks

1. Add encoding constants and compatible decode/encode paths in extension source.
2. Vendor CRoaring and integrate into build script and CMake.
3. Implement roaring-backed internal ops for AND/OR/ANDNOT/COUNT/CONTAINS/TO_ROWS.
4. Implement `bm_build_agg` aggregate function registration and tests.
5. Implement `bm_or_agg` aggregate function registration and tests.
6. Implement `bm_count_and`, `bm_count_or`, `bm_count_andnot`, `bm_intersects`.
7. Implement streaming/chunked row-id retrieval (`bm_to_rows_table` or limited `bm_to_rows(bitmap, limit, start_after)`).
8. Refactor scalar/aggregate paths to process DuckDB chunks through vector data and validity masks without avoidable per-row allocation.
9. Add envelope/format helper function (`bm_format`).
10. Add bitmap diagnostics helper (`bm_stats`) with cardinality, encoding, serialized size, and container/shard metadata where available.
11. Add row-group-sized or `65,536`-row shard prototype behind explicit SQL/template path.
12. Add posting-table metadata shape for shard cardinality/min/max/mode without forcing BLOB reads.
13. Add migration helper (`bm_reencode_roaring`) or SQL script to rebuild postings.
14. Extend perf harness with skew scenarios and large-posting scenarios.
15. Extend perf harness with DuckDB-native baselines (`BITSTRING`, sorted/unsorted zonemap-pruned SQL, selective equality/index scenarios).
16. Document usage and query templates.

## Test Plan

Add new function cases under `native/duckdb-bitmap-extension/test/functions`:

- `07_bm_build_agg`
- `08_bm_or_agg`
- `09_bm_count_and_or`
- `10_bm_intersects`
- `11_roaring_roundtrip`
- `12_compat_sorted_u32_decode`
- `13_skew_complement_strategy`
- `14_bm_to_rows_limited_or_table`
- `15_bm_stats`
- `16_sharded_posting_roundtrip`

Add negative/error tests:

- out-of-range ids
- null handling for new functions
- malformed roaring payloads
- malformed sorted-u32 payloads that are unsorted or contain duplicates, if legacy strict validation is enabled

Add vector/chunk behavior tests:

- all-valid inputs with no validity mask
- nullable inputs with skipped NULL row ids
- boundary row ids around shard/container edges (`65535`, `65536`, row-group boundary, `UINT32_MAX`)
- table-function pagination with `limit` and `start_after`

## Perf Plan (Must Pass Before Sprint Close)

Datasets:

- TPC-H SF1 and SF5
- synthetic skewed table:
  - 100M rows
  - one 2-value column with 95/5 distribution
  - one medium-cardinality column

Required comparisons:

- MVP sorted-u32 vs roaring32 implementation
- list build vs streaming aggregate build
- unnest/rebuild unions vs `bm_or_agg`
- scalar full-list row expansion vs streaming/chunked row retrieval
- global bitmap vs row-group-sized or `65,536`-row sharded bitmap
- custom bitmap count/facet paths vs DuckDB `BITSTRING`/`bitstring_agg`/`bit_count`
- custom bitmap predicates vs ordinary DuckDB SQL on sorted and unsorted data to quantify zonemap effects
- baseline `WHERE field IN (...)` fact fetch vs bitmap row-id fetch and join-back
- baseline `WHERE A IN (...) AND B = ...` count/fetch vs bitmap multi-field count/fetch
- memory profile using peak RSS plus `duckdb_memory()` observations for base-table/cache/intermediate pressure

Acceptance targets:

- 2x+ faster index build for large group postings
- 2x+ faster union/intersection for large postings
- repeated count workloads at SF1/SF5 should beat or closely match baseline SQL after bitmap postings exist
- first-page fact fetch should avoid full row-id materialization and be within 2x of baseline filtered `LIMIT 10000`
- materially lower peak memory in build and union paths
- no extension path should exceed the configured bitmap memory budget without a clear fail-fast error
- row-group/sharded layout should ship only if it demonstrates lower memory or latency in at least one target workload without regressing simple counts materially
- correct count parity against fact-table SQL checks in all harness scenarios

## Deliverables In Repo

- extension code updates under `native/duckdb-bitmap-extension/src`
- updated build tooling under `tools/bitmap-ext`
- new tests under `native/duckdb-bitmap-extension/test/functions`
- updated perf workloads under `tools/bitmap-ext/perf/workloads`
- docs:
  - this sprint plan
  - migration/usage notes for new functions

## Definition Of Done

1. New functions compile and load with local unsigned extension flow.
2. Existing function behavior remains backward compatible.
3. All function harness cases pass.
4. Perf harness demonstrates measurable improvements on large/skewed workloads.
5. Documentation includes recommended query patterns for build, filter, and validation.
