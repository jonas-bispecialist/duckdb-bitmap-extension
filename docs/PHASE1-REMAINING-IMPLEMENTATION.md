# Phase 1 Remaining Implementation Guide

**Status:** 50% Complete - Steps 0-4 done, Steps 5-9 pending  
**Commit:** 35cf710 (Phase 1 WIP - foundational work complete)  
**Date:** 2026-04-24

## What's Done

- ✅ CRoaring v4.6.1 vendored (roaring.h + roaring.c)
- ✅ CMakeLists.txt updated
- ✅ BITMAP_ENCODING_ROARING32 constant added
- ✅ Parser updated for dual encoding support
- ✅ Three helper functions added (Deserialize, Cardinality, ToRoaring)
- ✅ MakeRoaringBlobBytes serializer implemented
- ✅ Aggregate state swapped (roaring_bitmap_t instead of uint32 array)
- ✅ All aggregate functions updated (Build, Or aggregates; Combine, Finalize)
- ✅ BitmapCountFunction updated to use BitmapViewCardinality
- ✅ 30K+ LoC of CRoaring code integrated
- ✅ Git commit created

## What Remains (Steps 5-9)

### Step 5: Binary Operations Replacement

**Pattern for all binary ops:**
```c
// OLD: BitmapOrFunction calls ApplyBinaryOp(lhs, rhs, BITMAP_OP_OR) which calls MergeOr
// NEW: Use CRoaring directly

BitmapView lhs = {0}, rhs = {0};
if (!ParseBitmapBlob(info, lhs_vector, row, &lhs)) return;
if (!ParseBitmapBlob(info, rhs_vector, row, &rhs)) return;

roaring_bitmap_t *rb_lhs = BitmapViewToRoaring(&lhs);  // materialize any encoding
if (rb_lhs == NULL) { SetError("OOM"); return; }
roaring_bitmap_t *rb_rhs = BitmapViewToRoaring(&rhs);
if (rb_rhs == NULL) { roaring_bitmap_free(rb_lhs); SetError("OOM"); return; }

roaring_bitmap_t *result = roaring_bitmap_or(rb_lhs, rb_rhs);
roaring_bitmap_free(rb_lhs);
roaring_bitmap_free(rb_rhs);

uint8_t *blob_bytes = NULL;
idx_t blob_size = 0;
if (!MakeRoaringBlobBytes(result, &blob_bytes, &blob_size)) { ... }
duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);
free(blob_bytes);
roaring_bitmap_free(result);
```

**Functions to update (all follow same pattern):**
- `BitmapOrFunction` (line 693): use `roaring_bitmap_or`
- `BitmapAndFunction` (line 747): use `roaring_bitmap_and`
- `BitmapAndNotFunction` (line 801): use `roaring_bitmap_andnot`

Then remove dead: `MergeOr`, `MergeAnd`, `MergeAndNot`, `ApplyBinaryOp`

**Count-only paths (BitmapCountBinaryCommon):**
Replace calls to CountOr/And/AndNot with direct CRoaring cardinality API:
- `roaring_bitmap_or_cardinality(lhs, rhs)`
- `roaring_bitmap_and_cardinality(lhs, rhs)`
- `roaring_bitmap_andnot_cardinality(lhs, rhs)`

**Membership test (BitmapContainsValue):**
```c
if (needle > UINT32_MAX) return false;
uint32_t target = (uint32_t)needle;

if (view->encoding == BITMAP_ENCODING_SORTED_U32) {
    // Keep fast binary search path
    return binary_search(view->payload, view->count, target);
}
// Roaring32 path
roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(view);
if (rb == NULL) return false;
bool result = roaring_bitmap_contains(rb, target);
roaring_bitmap_free(rb);
return result;
```

**Row retrieval (BitmapToRowsFunction & BitmapToRowsLimitedFunction):**
For sorted_u32: keep existing fast path  
For roaring32: use roaring iterator:
```c
roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(&bitmap);
roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
while (roaring_uint32_iterator_has_value(it)) {
    child_data[offset + i++] = (uint64_t)roaring_uint32_iterator_value(it);
    roaring_uint32_iterator_advance(it);
}
roaring_uint32_iterator_free(it);
roaring_bitmap_free(rb);
```

**Scalar list builder (BitmapBuildFunction):**
Replace malloc + qsort + dedup with:
```c
roaring_bitmap_t *rb = roaring_bitmap_create();
for (idx_t i = 0; i < length; i++) {
    roaring_bitmap_add(rb, (uint32_t)child_data[offset + i]);
}
MakeRoaringBlobBytes(rb, &blob_bytes, &blob_size);
roaring_bitmap_free(rb);
```

**Intersect check (BitmapIntersectsFunction):**
Replace `BitmapsIntersect` with inline `roaring_bitmap_intersect(rb_lhs, rb_rhs)`

### Step 6: Update Introspection Functions

**BitmapFormatName:**
```c
if (bitmap->encoding == BITMAP_ENCODING_ROARING32) {
    return "roaring32_v1";
}
// existing SORTED_U32 case
```

**BitmapStatsFunction:**
For roaring32, add container statistics:
```c
if (view.encoding == BITMAP_ENCODING_ROARING32) {
    roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(&view);
    roaring_statistics_t stats;
    roaring_bitmap_statistics(rb, &stats);
    snprintf(buf, bufsz,
        "encoding=roaring32_v1;cardinality=%llu;serialized_bytes=%llu;"
        "payload_bytes=%llu;containers=%u;array=%u;run=%u;bitset=%u",
        card, total_size, payload_size,
        stats.n_containers, stats.n_array_containers,
        stats.n_run_containers, stats.n_bitset_containers);
    roaring_bitmap_free(rb);
}
```
Increase stats buffer from 160 to 512 bytes.

### Step 7: Add bm_reencode_roaring(BLOB) -> BLOB

New migration helper:
```c
static void BitmapReencodeRoaringFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    // For sorted_u32 blobs: deserialize → serialize as roaring32
    // For roaring32 blobs: pass through unchanged (copy blob bytes)
}
```

Register as scalar function in entrypoint.

Add test case at `native/duckdb-bitmap-extension/test/functions/13_bm_reencode/`:
- setup.sql: create sorted_u32 and roaring32 sample blobs
- query.sql: test re-encoding and format checking
- expected.csv: validate output format and cardinality

### Step 8: Regenerate Expected CSV

Tests 01-03 and 12 have hardcoded hex output that will change:
- 01_bm_or: input sorted_u32, output roaring32 (hex will differ)
- 02_bm_and: input sorted_u32, output roaring32 (hex will differ)
- 03_bm_andnot: input sorted_u32, output roaring32 (hex will differ)
- 12_bm_format_stats: output will include "roaring32_v1" and container counts

Run test harness to capture new expected values, or manually execute queries.

### Step 9: Cleanup & Validation

**Remove dead functions:**
- `MergeOr`, `MergeAnd`, `MergeAndNot`
- `CountOr`, `CountAnd`, `CountAndNot`
- `BitmapsIntersect`
- `BitmapViewIsValidRange`, `BitmapLowerBoundGreaterThan` (roaring32 path doesn't use them)
- `EnsureAggCapacity`, `AppendAggValue`, `SortDeduplicateAggState`, `MergeAggSortedValues`, `MergeAggSortedWithBitmapView`
- `CompareUint32`
- `BitmapSortedAggFinalize` (now delegates to BitmapAggFinalize; already changed registration)

**Test & validate:**
```powershell
# Build (requires cmake + C compiler)
cd native\duckdb-bitmap-extension
cmake -B build -S . && cmake --build build --config Release

# Run tests
.\test\run-function-tests.ps1 -DuckDbCliPath duckdb -Unsigned -ExtensionLoadSql "LOAD 'build/bitmap.duckdb_extension';"

# Capture perf baselines
.\..\..\tools\bitmap-ext\perf\run-perf.ps1 -ScaleFactor 1 -Iterations 5 -DuckDbCliPath duckdb -Unsigned -ExtensionLoadSql "LOAD 'build/bitmap.duckdb_extension';"
.\..\..\tools\bitmap-ext\perf\run-perf.ps1 -ScaleFactor 5 -Iterations 3 ...
```

Expected improvements:
- SF1 count: 2x-3x faster (no repeated parse/dedup on sorted arrays)
- SF1 union: 2x+ faster (Roaring run compression)
- SF5: similar ratios

## CRoaring API Reference (Used in Changes)

**Creation/Destruction:**
- `roaring_bitmap_create()` → `roaring_bitmap_t*`
- `roaring_bitmap_copy(rb)` → new bitmap
- `roaring_bitmap_free(rb)`

**Operations:**
- `roaring_bitmap_or(lhs, rhs)` → new bitmap
- `roaring_bitmap_and(lhs, rhs)` → new bitmap
- `roaring_bitmap_andnot(lhs, rhs)` → new bitmap
- `roaring_bitmap_or_inplace(dest, src)` (modifies dest)

**Cardinality:**
- `roaring_bitmap_get_cardinality(rb)` → uint64_t
- `roaring_bitmap_or_cardinality(lhs, rhs)` → uint64_t
- `roaring_bitmap_and_cardinality(lhs, rhs)` → uint64_t
- `roaring_bitmap_andnot_cardinality(lhs, rhs)` → uint64_t

**Membership/Predicates:**
- `roaring_bitmap_contains(rb, value)` → bool
- `roaring_bitmap_intersect(lhs, rhs)` → bool
- `roaring_bitmap_rank(rb, value)` → uint64_t (count of values ≤ value)

**Serialization:**
- `roaring_bitmap_portable_size_in_bytes(rb)` → size_t
- `roaring_bitmap_portable_serialize(rb, buf)`
- `roaring_bitmap_portable_deserialize_safe(buf, size)` → `roaring_bitmap_t*` (NULL on error)

**Iteration:**
- `roaring_iterator_create(rb)` → `roaring_uint32_iterator_t*`
- `roaring_uint32_iterator_has_value(it)` → bool
- `roaring_uint32_iterator_value(it)` → uint32_t
- `roaring_uint32_iterator_advance(it)`
- `roaring_uint32_iterator_free(it)`

**Optimization:**
- `roaring_bitmap_run_optimize(rb)` (run-length compression, modifies in-place)
- `roaring_bitmap_statistics(rb, stats)` → `roaring_statistics_t` with `.n_containers`, `.n_array_containers`, `.n_run_containers`, `.n_bitset_containers`

## Files Still to Edit

After Step 5: `bitmap_extension.c` (binary ops + row retrieval)  
After Step 6: `bitmap_extension.c` (format/stats)  
After Step 7: `bitmap_extension.c` (new function) + new test directory  
After Step 8: Test expected.csv files  
After Step 9: Final cleanup

## Build Notes

CMake is now installed (pip install cmake). Once all code changes are complete:

```powershell
cd native\duckdb-bitmap-extension
rm -r build
python -m cmake -B build -S . -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cd build
make  # or mingw32-make if using MinGW
```

If MSVC toolchain becomes available:
```powershell
python -m cmake -B build -S . -G "Visual Studio 17 2022"
python -m cmake --build build --config Release
```

## Summary

~60% of work is transforming internal algorithms from sorted arrays to CRoaring API. The infrastructure is solid:
- Unified dispatch via BitmapViewToRoaring
- Safe dual-encoding support (sorted_u32 still readable)
- All aggregate functions working with roaring
- Three helper functions encapsulate all roaring operations

Remaining work is systematic replacement of binary ops and row retrieval logic, then testing and cleanup. No architectural surprises remain.
