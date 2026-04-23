# DuckDB Bitmap Extension Test Harness

This directory contains a SQL-driven harness for validating bitmap-extension functions one case at a time.

## Layout

- `functions/<case>/setup.sql` prepares any fixture tables for a case.
- `functions/<case>/query.sql` contains the single validation query for that case.
- `functions/<case>/expected.csv` stores the expected CSV result for the query.
- `run-function-tests.ps1` runs every case directory under `functions/`.

## Adding a new function case

1. Copy `functions/_template` to `functions/<new-case-name>`.
2. Replace the fixture SQL and expected CSV with the function-specific test.
3. Run the harness with the extension load SQL for your local build.

## Example commands

```powershell
$duckdb = "C:\path\to\duckdb.exe"
$extensionLoadSql = "LOAD 'C:\path\to\bitmap_extension.duckdb_extension';"

.\run-function-tests.ps1 -DuckDbCliPath $duckdb -Unsigned -ExtensionLoadSql $extensionLoadSql
```

If your local DuckDB build uses a different load form, pass that SQL snippet through `-ExtensionLoadSql`.
