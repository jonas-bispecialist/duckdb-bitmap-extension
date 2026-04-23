param(
    [string]$DuckDbExe = "duckdb"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$sqlFile = Join-Path $repoRoot "native\duckdb-bitmap-extension\test\hello.sql"

& $DuckDbExe -unsigned -c (Get-Content -Raw $sqlFile)
