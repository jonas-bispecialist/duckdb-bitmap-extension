param(
    [string]$DuckDbCliPath = "duckdb",
    [string]$ScaleFactor = "1",
    [int]$Warmup = 1,
    [int]$Iterations = 5,
    [string]$ExtensionLoadSql = "",
    [switch]$Unsigned,
    [string]$DatabasePath = (Join-Path $PSScriptRoot "bitmap-ext-tpch.duckdb"),
    [string]$WorkloadsRoot = (Join-Path $PSScriptRoot "workloads"),
    [switch]$RebuildData
)

$ErrorActionPreference = "Stop"

function ConvertTo-SqlLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)

    return "'" + ($Value -replace "'", "''") + "'"
}

function Invoke-DuckDb {
    param(
        [Parameter(Mandatory = $true)][string]$Sql,
        [Parameter(Mandatory = $true)][string]$DatabasePath
    )

    $args = @($DatabasePath)
    if ($Unsigned) {
        $args += "-unsigned"
    }
    $args += @("-c", $Sql)

    $stdout = & $DuckDbCliPath @args 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "DuckDB exited with code $exitCode.`nOUTPUT:`n$stdout"
    }

    return [pscustomobject]@{
        StdOut = $stdout
        StdErr = ""
    }
}

function Get-WorkloadSql {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-Content -LiteralPath $Path -Raw).Trim().TrimEnd(";")
}

function Measure-Workload {
    param(
        [Parameter(Mandatory = $true)][string]$WorkloadName,
        [Parameter(Mandatory = $true)][string]$QuerySql,
        [Parameter(Mandatory = $true)][string]$DatabasePath,
        [Parameter(Mandatory = $true)][string]$OutputDir
    )

    $samples = New-Object System.Collections.Generic.List[double]
    $rowCounts = New-Object System.Collections.Generic.List[int]

    for ($i = 0; $i -lt ($Warmup + $Iterations); $i++) {
        $outputPath = Join-Path $OutputDir "$WorkloadName.$i.csv"
        if (Test-Path -LiteralPath $outputPath) {
            Remove-Item -LiteralPath $outputPath -Force
        }

        $sqlParts = New-Object System.Collections.Generic.List[string]
        if (-not [string]::IsNullOrWhiteSpace($ExtensionLoadSql)) {
            $sqlParts.Add($ExtensionLoadSql.Trim().TrimEnd(";"))
        }
        $sqlParts.Add("COPY (" + $QuerySql + ") TO " + (ConvertTo-SqlLiteral $outputPath) + " (FORMAT CSV, HEADER)")
        $sql = ($sqlParts -join ";`n") + ";"

        $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
        Invoke-DuckDb -Sql $sql -DatabasePath $DatabasePath | Out-Null
        $elapsed.Stop()

        $lineCount = (Get-Content -LiteralPath $outputPath | Measure-Object).Count
        $rows = [Math]::Max(0, $lineCount - 1)

        if ($i -ge $Warmup) {
            $samples.Add([Math]::Round($elapsed.Elapsed.TotalMilliseconds, 3))
            $rowCounts.Add($rows)
        }

        Remove-Item -LiteralPath $outputPath -Force -ErrorAction SilentlyContinue
    }

    $sorted = $samples | Sort-Object
    $avg = [Math]::Round(($samples | Measure-Object -Average).Average, 3)
    $min = [Math]::Round(($sorted | Select-Object -First 1), 3)
    $max = [Math]::Round(($sorted | Select-Object -Last 1), 3)
    $p95Index = [Math]::Min($sorted.Count - 1, [Math]::Ceiling($sorted.Count * 0.95) - 1)
    $p95 = [Math]::Round([double]$sorted[$p95Index], 3)

    return [pscustomobject]@{
        workload = $WorkloadName
        scale_factor = $ScaleFactor
        iterations = $Iterations
        warmup = $Warmup
        avg_ms = $avg
        min_ms = $min
        p95_ms = $p95
        max_ms = $max
        rows = ($rowCounts | Select-Object -First 1)
    }
}

if (-not (Test-Path -LiteralPath $WorkloadsRoot)) {
    throw "Workloads root not found: $WorkloadsRoot"
}

$workloadFiles = @(Get-ChildItem -LiteralPath $WorkloadsRoot -File -Filter *.sql |
    Where-Object { $_.Name -ne "prepare-tpch.sql" } |
    Sort-Object Name)

if ($workloadFiles.Count -eq 0) {
    throw "No workload SQL files found under $WorkloadsRoot"
}

$outputRoot = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

if ($RebuildData -or -not (Test-Path -LiteralPath $DatabasePath)) {
    $preparePath = Join-Path $WorkloadsRoot "prepare-tpch.sql"
    $prepareSql = if (Test-Path -LiteralPath $preparePath) {
        ((Get-Content -LiteralPath $preparePath -Raw).Trim() -replace '\{\{SCALE_FACTOR\}\}', $ScaleFactor)
    }
    else {
        @(
            "INSTALL tpch",
            "LOAD tpch",
            "DROP TABLE IF EXISTS customer",
            "DROP TABLE IF EXISTS lineitem",
            "DROP TABLE IF EXISTS nation",
            "DROP TABLE IF EXISTS orders",
            "DROP TABLE IF EXISTS part",
            "DROP TABLE IF EXISTS partsupp",
            "DROP TABLE IF EXISTS region",
            "DROP TABLE IF EXISTS supplier",
            "CALL dbgen(sf = $ScaleFactor)"
        ) -join ";`n"
    }

    $prepareParts = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($ExtensionLoadSql)) {
        $prepareParts.Add($ExtensionLoadSql.Trim().TrimEnd(";"))
    }
    $prepareParts.Add($prepareSql.Trim().TrimEnd(";"))

    Invoke-DuckDb -Sql (($prepareParts -join ";`n") + ";") -DatabasePath $DatabasePath | Out-Null
}

$results = New-Object System.Collections.Generic.List[object]

foreach ($workloadFile in $workloadFiles) {
    $workloadName = [System.IO.Path]::GetFileNameWithoutExtension($workloadFile.Name)
    $querySql = Get-WorkloadSql -Path $workloadFile.FullName
    $result = Measure-Workload -WorkloadName $workloadName -QuerySql $querySql -DatabasePath $DatabasePath -OutputDir $outputRoot
    $results.Add($result)
}

$results |
    Sort-Object workload |
    Format-Table -AutoSize

$reportPath = Join-Path $outputRoot ("perf-report-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".csv")
$results | Export-Csv -NoTypeInformation -Path $reportPath

Write-Host "Report written to $reportPath"
