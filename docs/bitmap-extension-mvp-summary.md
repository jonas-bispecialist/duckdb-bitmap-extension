# Bitmap DuckDB Extension MVP Summary

## Context and Scope

- Goal: build a Windows-only DuckDB extension MVP for bitmap-based filtering/facet workflows.
- Baseline decision: skip C# bitmap baseline and go directly to native DuckDB extension.
- DuckDB runtime target: current project package `DuckDB.NET.Data.Full` `1.5.0`.
- Delivery approach: incremental
  1. Build and verify a hello-world extension.
  2. Implement bitmap functions one by one.
  3. Validate correctness and run TPC-H-based performance tests.

## Implementation Delivered

Implemented under `native/duckdb-bitmap-extension` with build/test tooling under `tools/bitmap-ext`.

### Registered extension functions

- `bitmap_hello(VARCHAR) -> VARCHAR`
- `bm_or(BLOB, BLOB) -> BLOB`
- `bm_and(BLOB, BLOB) -> BLOB`
- `bm_andnot(BLOB, BLOB) -> BLOB`
- `bm_count(BLOB) -> UBIGINT`
- `bm_contains(BLOB, UBIGINT) -> BOOLEAN`
- `bm_to_rows(BLOB) -> UBIGINT[]`
- `bm_build(UBIGINT[]) -> BLOB` (helper for benchmark/data preparation)

### Bitmap payload format (MVP)

- Envelope: `FPBM` magic + version/encoding/reserved + payload length.
- Payload encoding in MVP: sorted unique `uint32` row ids (little-endian), exact semantics.
- Null policy:
  - Binary ops are null-preserving.
  - `bm_count(NULL)` returns `NULL`.
  - `bm_contains(NULL, x)` and `bm_contains(bm, NULL)` return `NULL`.
  - `bm_to_rows(NULL)` returns `NULL`.

## Build and Packaging

### Key change

- Build script moved to Zig-based native compilation to avoid requiring local VS/CMake.
- Extension metadata packaging configured for C-struct ABI compatibility:
  - `abi_type = C_STRUCT`
  - C API version metadata set to `v1.2.0` (required by extension loader compatibility checks).

### Main scripts

- `tools/bitmap-ext/build.ps1`
- `tools/bitmap-ext/smoke.ps1`
- `tools/bitmap-ext/append_metadata.py`

## Correctness Testing

Function test harness and cases were added:

- Harness: `native/duckdb-bitmap-extension/test/run-function-tests.ps1`
- Cases:
  - `01_bm_or`
  - `02_bm_and`
  - `03_bm_andnot`
  - `04_bm_count`
  - `05_bm_contains`
  - `06_bm_to_rows`

Result: all cases passing.

## Performance Harness and Results

TPC-H benchmark harness added under `tools/bitmap-ext/perf`:

- Data prep: `workloads/prepare-tpch.sql`
- Runner: `run-perf.ps1`
- Workloads compare baseline SQL vs bitmap workflows for count/facet/export-like queries.

Latest validated run (SF=1, warmup=1, iterations=3):

- `01_baseline_count`: `64.554 ms`
- `02_bitmap_count`: `168.165 ms`
- `03_baseline_facet_shipmode`: `62.150 ms`
- `04_bitmap_facet_shipmode`: `250.718 ms`
- `05_bitmap_export_rowids`: `766.742 ms`

CSV report:

- `tools/bitmap-ext/perf/out/perf-report-20260423-192602.csv`

## Sub-Agent Coordination Summary

Parallel agents were used for:

- extension scaffold/build plumbing,
- bitmap contract and function scaffolding,
- test + perf harness scaffolding,
- independent test execution verification.

Final independent verification by a dedicated test agent passed:

- build,
- smoke,
- function cases,
- perf run.

## Current Status

- MVP is working and testable now.
- Correctness path is in place with repeatable scripts.
- Performance in this MVP is currently slower than baseline for tested SF=1 workloads.

## Recommended Next Step

- Replace MVP sorted-array bitmap internals with CRoaring-backed operations while keeping the same SQL surface and harnesses.
- Re-run the same perf harness after each optimization pass to confirm improvements.

