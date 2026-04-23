# Bitmap Extension Perf Harness

This folder contains a DuckDB TPC-H based benchmark harness for facet-style workflows, including bitmap-index queries.

## What it does

- Creates a local DuckDB database with TPC-H tables via `CALL dbgen(sf = <scale>)`.
- Builds bitmap postings for selected low-cardinality `lineitem` columns using `bm_build`.
- Runs baseline SQL workloads and bitmap-based workloads for count/facet/export-style queries.
- Reports per-query latency across warmup and measured iterations.

## Example commands

```powershell
$duckdb = "C:\path\to\duckdb.exe"
$extensionLoadSql = "LOAD 'C:\path\to\bitmap_extension.duckdb_extension';"

.\run-perf.ps1 -DuckDbCliPath $duckdb -Unsigned -ExtensionLoadSql $extensionLoadSql -ScaleFactor 1 -Iterations 5 -Warmup 1 -RebuildData
```

If the extension is not built yet, keep the harness in place and pass the load SQL later.

## TPC-H scale recommendation

- Default: `sf=1` for a local laptop/desktop.
- If you want more stable timings and have memory headroom, try `sf=5`.
