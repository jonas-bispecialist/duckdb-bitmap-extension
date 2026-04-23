# Bitmap Extension Next Sprint Plan

## Sprint Objective

Deliver a production-leaning bitmap path that performs well on 50M-150M rows and remains correct for larger data volumes, with a clear path to optional 64-bit row-id support.

Primary outcomes for this sprint:

- replace list-materialization build pattern with streaming aggregate build
- switch bitmap payload internals from sorted-u32 arrays to Roaring (with run optimization)
- add count-focused operations to avoid unnecessary row-id expansion
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
- row-id domain is limited to `UINT32_MAX` (`4,294,967,295`)

This is correct but not optimized for large skewed postings.

## Scope For This Sprint

In scope:

- new encoding path: `ROARING32`
- backward-compatible reader for legacy `SORTED_U32` blobs
- new aggregate functions for streaming build and bitmap union
- count-only fast paths
- skew-aware storage mode (`EXACT` vs `COMPLEMENT`)
- benchmark and correctness expansion

Out of scope:

- optimizer/planner rewrite rules
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
- `bm_intersects(BLOB,BLOB) -> BOOLEAN`
- `bm_format(BLOB) -> VARCHAR` (debug helper: `sorted_u32_v1`, `roaring32_v2`, etc.)
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

## Workstream C - Skew-Aware Strategy For Low Cardinality

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

## Workstream D - Fast Count Paths

### Problem

`bm_to_rows` and row-id expansion is expensive when only counts are needed.

### Additions

- `bm_count_and` and `bm_count_or`
- `bm_intersects` for short-circuit checks

### Guidance

- use count functions for UI counters/facets
- use `bm_to_rows` only for exports/drill-down

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
3. Apply skew-aware complement mode for dominant values.
4. Use roaring run optimization after build/finalize.
5. Cache frequently used union/intersection results in app/service layer keyed by filter signature.
6. Avoid unnecessary deserialization/serialization loops in SQL plans (combine in one function call path where possible).
7. Precompute and persist domain bitmap per table to make complement operations constant-shape.
8. Add a build-time threshold policy per column:
- do not bitmap-index columns with poor selectivity value for the UI
- or only index values under cardinality/selectivity thresholds
9. Keep row ids dense and monotonic where possible to maximize run compression quality.
10. Benchmark with skewed synthetic datasets, not only uniform TPC-H patterns.

## Concrete Implementation Tasks

1. Add encoding constants and compatible decode/encode paths in extension source.
2. Vendor CRoaring and integrate into build script and CMake.
3. Implement roaring-backed internal ops for AND/OR/ANDNOT/COUNT/CONTAINS/TO_ROWS.
4. Implement `bm_build_agg` aggregate function registration and tests.
5. Implement `bm_or_agg` aggregate function registration and tests.
6. Implement `bm_count_and`, `bm_count_or`, `bm_intersects`.
7. Add envelope/format helper function (`bm_format`).
8. Add migration helper (`bm_reencode_roaring`) or SQL script to rebuild postings.
9. Extend perf harness with skew scenarios and large-posting scenarios.
10. Document usage and query templates.

## Test Plan

Add new function cases under `native/duckdb-bitmap-extension/test/functions`:

- `07_bm_build_agg`
- `08_bm_or_agg`
- `09_bm_count_and_or`
- `10_bm_intersects`
- `11_roaring_roundtrip`
- `12_compat_sorted_u32_decode`
- `13_skew_complement_strategy`

Add negative/error tests:

- out-of-range ids
- null handling for new functions
- malformed roaring payloads

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

Acceptance targets:

- 2x+ faster index build for large group postings
- 2x+ faster union/intersection for large postings
- materially lower peak memory in build and union paths
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
