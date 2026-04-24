param(
    [string]$DuckDbCliPath = "duckdb",
    [string]$ScaleFactor = "1",
    [int]$Warmup = 1,
    [int]$Iterations = 5,
    [string]$ExtensionLoadSql = "",
    [switch]$Unsigned,
    [string]$DatabasePath = (Join-Path $PSScriptRoot "bitmap-ext-tpch.duckdb"),
    [string]$WorkloadsRoot = (Join-Path $PSScriptRoot "workloads"),
    [switch]$RebuildData,
    [switch]$RebuildBitmaps
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

function Invoke-TimedStage {
    param(
        [Parameter(Mandatory = $true)][string]$StageName,
        [Parameter(Mandatory = $true)][string]$Sql,
        [Parameter(Mandatory = $true)][string]$DatabasePath
    )

    $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-DuckDb -Sql $Sql -DatabasePath $DatabasePath | Out-Null
    $elapsed.Stop()

    $ms = [Math]::Round($elapsed.Elapsed.TotalMilliseconds, 3)
    Write-Host ("{0}: {1} ms" -f $StageName, $ms)

    return [pscustomobject]@{
        workload = $StageName
        scale_factor = $ScaleFactor
        iterations = 1
        warmup = 0
        avg_ms = $ms
        min_ms = $ms
        p95_ms = $ms
        max_ms = $ms
        rows = 0
    }
}

function Get-PreparedSql {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$IncludeExtensionLoad
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "SQL file not found: $Path"
    }

    $sql = ((Get-Content -LiteralPath $Path -Raw).Trim() -replace '\{\{SCALE_FACTOR\}\}', $ScaleFactor)
    $parts = New-Object System.Collections.Generic.List[string]
    if ($IncludeExtensionLoad -and -not [string]::IsNullOrWhiteSpace($ExtensionLoadSql)) {
        $parts.Add($ExtensionLoadSql.Trim().TrimEnd(";"))
    }
    $parts.Add($sql.Trim().TrimEnd(";"))
    return (($parts -join ";`n") + ";")
}

if (-not (Test-Path -LiteralPath $WorkloadsRoot)) {
    throw "Workloads root not found: $WorkloadsRoot"
}

$workloadFiles = @(Get-ChildItem -LiteralPath $WorkloadsRoot -File -Filter *.sql |
    Where-Object { $_.Name -notin @("prepare-tpch.sql", "prepare-tpch-data.sql", "prepare-bitmaps.sql") } |
    Sort-Object Name)

if ($workloadFiles.Count -eq 0) {
    throw "No workload SQL files found under $WorkloadsRoot"
}

$outputRoot = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$results = New-Object System.Collections.Generic.List[object]

$databaseExists = Test-Path -LiteralPath $DatabasePath
if ($RebuildData -or -not $databaseExists) {
    $prepareDataPath = Join-Path $WorkloadsRoot "prepare-tpch-data.sql"
    $prepareDataSql = Get-PreparedSql -Path $prepareDataPath
    $results.Add((Invoke-TimedStage -StageName "00_prepare_data" -Sql $prepareDataSql -DatabasePath $DatabasePath))
    $RebuildBitmaps = $true
}

if ($RebuildBitmaps) {
    $prepareBitmapsPath = Join-Path $WorkloadsRoot "prepare-bitmaps.sql"
    $prepareBitmapsSql = Get-PreparedSql -Path $prepareBitmapsPath -IncludeExtensionLoad
    $results.Add((Invoke-TimedStage -StageName "00_build_bitmaps" -Sql $prepareBitmapsSql -DatabasePath $DatabasePath))
}

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
