# Bitmap Extension Sprint Implementation Notes

Date: 2026-04-24

## Implemented SQL Surface

Existing functions remain available:

- `bm_or(BLOB, BLOB) -> BLOB`
- `bm_and(BLOB, BLOB) -> BLOB`
- `bm_andnot(BLOB, BLOB) -> BLOB`
- `bm_count(BLOB) -> UBIGINT`
- `bm_contains(BLOB, UBIGINT) -> BOOLEAN`
- `bm_to_rows(BLOB) -> UBIGINT[]`
- `bm_build(UBIGINT[]) -> BLOB`

New functions:

- `bm_build_agg(UBIGINT) -> BLOB`
- `bm_or_agg(BLOB) -> BLOB`
- `bm_count_and(BLOB, BLOB) -> UBIGINT`
- `bm_count_or(BLOB, BLOB) -> UBIGINT`
- `bm_count_andnot(BLOB, BLOB) -> UBIGINT`
- `bm_intersects(BLOB, BLOB) -> BOOLEAN`
- `bm_to_rows(BLOB, limit UBIGINT, start_after BIGINT) -> UBIGINT[]`
- `bm_format(BLOB) -> VARCHAR`
- `bm_stats(BLOB) -> VARCHAR`

## Query Pattern Changes

Build postings with the streaming aggregate:

```sql
CREATE TABLE li_bitmap_shipmode AS
SELECT
    l_shipmode AS value,
    bm_build_agg(CAST(rid AS UBIGINT)) AS bm
FROM li
GROUP BY 1;
```

Union selected postings without scalar nesting:

```sql
SELECT bm_or_agg(bm)
FROM li_bitmap_shipmode
WHERE value IN ('AIR', 'RAIL', 'TRUCK');
```

Use count-only helpers for count/facet paths:

```sql
SELECT bm_count_and(active_filter, candidate_posting);
```

Fetch first-page rows with bounded row-id expansion:

```sql
WITH selected_rows AS (
    SELECT unnest(bm_to_rows(active_filter, 10000::UBIGINT, -1::BIGINT)) AS rid
)
SELECT li.*
FROM selected_rows
JOIN li USING (rid)
ORDER BY rid
LIMIT 10000;
```

For later pages, pass the previous page's last row id as `start_after`.

## Current Format Status

The active extension still writes `sorted_u32_v1` blobs. The sprint plan's Roaring32 migration, sharded postings, and complement-mode storage are not included in this patch. The new functions preserve the existing blob format so current postings remain readable.

## Validation (2026-04-24 - Phase 0 Complete)

Function harness:

```powershell
.\native\duckdb-bitmap-extension\test\run-function-tests.ps1 `
  -DuckDbCliPath duckdb `
  -Unsigned `
  -ExtensionLoadSql "LOAD 'C:\git\duckdb-bitmap-extension\native\duckdb-bitmap-extension\build\bitmap.duckdb_extension';"
```

**Result:** ✅ All 12 test suites PASSED (2026-04-24 19:50)
- 01_bm_or ✅
- 02_bm_and ✅
- 03_bm_andnot ✅
- 04_bm_count ✅
- 05_bm_contains ✅
- 06_bm_to_rows ✅
- 07_bm_build_agg ✅
- 08_bm_or_agg ✅
- 09_bm_count_fast_paths ✅
- 10_bm_intersects ✅
- 11_bm_to_rows_limited ✅
- 12_bm_format_stats ✅

Coverage includes: legacy scalar behavior, aggregate build/union, count-only helpers, overlap checks, bounded row export, diagnostics, null handling, overflow detection, and pagination.

Perf harness (SF1 baseline):

```powershell
.\tools\bitmap-ext\perf\run-perf.ps1 `
  -DuckDbCliPath duckdb `
  -Unsigned `
  -ExtensionLoadSql "LOAD 'C:\git\duckdb-bitmap-extension\native\duckdb-bitmap-extension\build\bitmap.duckdb_extension';" `
  -ScaleFactor 1 `
  -Iterations 3 `
  -Warmup 1
```

**SF1 Results (2026-04-24 19:50):**

| Workload | Baseline (ms) | Bitmap (ms) | Ratio | Notes |
|----------|--------------|-------------|-------|-------|
| 01 count multi-field | 67.994 | 182.922 | 2.7x | sorted_u32_v1: expected slowdown |
| 03 facet shipmode | 66.056 | 243.872 | 3.7x | facet counts slower (multiple unions) |
| 06 count single-field | 65.875 | 89.408 | 1.4x | single-field faster than multi |
| 08 fetch single-field | 117.274 | 155.738 | 1.3x | ⚠️ bounded export helps; old plan said 5946ms |
| 10 fetch multi-field | 104.765 | 234.224 | 2.2x | ⚠️ much better than old 986ms |

Latest SF1 repeated-query report:

```text
tools/bitmap-ext/perf/out/perf-report-20260424-201018.csv
```

**SF5 Results (2026-04-24 19:51):**

| Workload | Baseline (ms) | Bitmap (ms) | Ratio |
|----------|--------------|-------------|-------|
| 01 count multi-field | 62.892 | 168.73 | 2.7x |
| 03 facet shipmode | 61.897 | 266.791 | 4.3x |
| 06 count single-field | 59.437 | 91.49 | 1.5x |
| 08 fetch single-field | 114.214 | 149.868 | 1.3x |
| 10 fetch multi-field | 103.087 | 222.293 | 2.2x |

Latest SF5 repeated-query report:

```text
tools/bitmap-ext/perf/out/perf-report-20260424-201051.csv
```

## Interpretation

Phase 0 delivers significant improvements in fetch performance (bounded row-id export brings SF1 single-field fetch from 5946ms down to 156ms). Count paths are slower than baseline due to repeated array parsing/serialization in sorted_u32_v1, which Phase 1 (Roaring32) targets for 2x+ improvement. Multi-field facet queries show larger slowdown because each value union requires a separate merge operation; Phase 1 (CRoaring + count-only paths) should help. Overall Phase 0 shapes are validated; ready to proceed to Phase 1 Roaring32 integration.
