param(
    [string]$DuckDbCliPath = "duckdb",
    [string]$CasesRoot = (Join-Path $PSScriptRoot "functions"),
    [string]$ExtensionLoadSql = "",
    [switch]$Unsigned,
    [switch]$UpdateExpected
)

$ErrorActionPreference = "Stop"

function ConvertTo-SqlLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)

    return "'" + ($Value -replace "'", "''") + "'"
}

function Normalize-Text {
    param([Parameter(Mandatory = $true)][string]$Value)

    return ($Value -replace "`r`n", "`n").TrimEnd("`n")
}

function Invoke-DuckDb {
    param(
        [Parameter(Mandatory = $true)][string]$DatabasePath,
        [Parameter(Mandatory = $true)][string]$Sql
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

if (-not (Test-Path -LiteralPath $CasesRoot)) {
    throw "Cases root not found: $CasesRoot"
}

$caseDirs = @(Get-ChildItem -LiteralPath $CasesRoot -Directory |
    Where-Object { $_.Name -ne "_template" } |
    Sort-Object Name)

if ($caseDirs.Count -eq 0) {
    throw "No function cases found under $CasesRoot"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("bitmap-ext-tests-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

$failures = New-Object System.Collections.Generic.List[string]

try {
    foreach ($caseDir in $caseDirs) {
        $caseName = $caseDir.Name
        $setupPath = Join-Path $caseDir.FullName "setup.sql"
        $queryPath = Join-Path $caseDir.FullName "query.sql"
        $expectedPath = Join-Path $caseDir.FullName "expected.csv"

        if (-not (Test-Path -LiteralPath $queryPath)) {
            $failures.Add("${caseName}: missing query.sql")
            continue
        }

        $setupSql = if (Test-Path -LiteralPath $setupPath) { [string](Get-Content -LiteralPath $setupPath -Raw) } else { "" }
        $querySql = [string](Get-Content -LiteralPath $queryPath -Raw)
        $expectedExists = Test-Path -LiteralPath $expectedPath

        if (-not $expectedExists -and -not $UpdateExpected) {
            $failures.Add("${caseName}: missing expected.csv (rerun with -UpdateExpected to scaffold it)")
            continue
        }

        $databasePath = Join-Path $tempRoot "$caseName.duckdb"
        $actualPath = Join-Path $tempRoot "$caseName.actual.csv"

        if (Test-Path -LiteralPath $actualPath) {
            Remove-Item -LiteralPath $actualPath -Force
        }

        $sqlParts = New-Object System.Collections.Generic.List[string]
        if (-not [string]::IsNullOrWhiteSpace($ExtensionLoadSql)) {
            $sqlParts.Add($ExtensionLoadSql.Trim().TrimEnd(";"))
        }
        if (-not [string]::IsNullOrWhiteSpace($setupSql)) {
            $sqlParts.Add($setupSql.Trim().TrimEnd(";"))
        }

        $sqlParts.Add(
            "COPY (" + $querySql.Trim().TrimEnd(";") + ") TO " + (ConvertTo-SqlLiteral $actualPath) + " (FORMAT CSV, HEADER)"
        )

        $sql = ($sqlParts -join ";`n") + ";"

        Invoke-DuckDb -DatabasePath $databasePath -Sql $sql | Out-Null

        if ($UpdateExpected) {
            Copy-Item -LiteralPath $actualPath -Destination $expectedPath -Force
            Write-Host "UPDATED $caseName"
            continue
        }

        $actualText = Normalize-Text (Get-Content -LiteralPath $actualPath -Raw)
        $expectedText = Normalize-Text (Get-Content -LiteralPath $expectedPath -Raw)

        if ($actualText -ne $expectedText) {
            $failures.Add("${caseName}: output mismatch")
            Write-Host "FAILED  $caseName"
            Write-Host "Expected:"
            Write-Host $expectedText
            Write-Host "Actual:"
            Write-Host $actualText
            continue
        }

        Write-Host "PASSED  $caseName"
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Error ($failures -join "`n")
}

Write-Host "All bitmap-extension function cases passed."
