param(
    [string]$DuckDbCapiVersion = "v1.2.0",
    [string]$DuckDbPlatform = "windows_amd64",
    [string]$ExtensionVersion = "0.1.0",
    [string]$ZigExe = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$extensionRoot = Join-Path $repoRoot "native\duckdb-bitmap-extension"
$buildRoot = Join-Path $extensionRoot "build"
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

function Resolve-ZigPath {
    param([string]$ConfiguredPath)

    if ($ConfiguredPath -and (Test-Path -LiteralPath $ConfiguredPath)) {
        return (Resolve-Path -LiteralPath $ConfiguredPath).Path
    }

    $zigCommand = Get-Command zig -ErrorAction SilentlyContinue
    if ($zigCommand) {
        return $zigCommand.Source
    }

    $wingetRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    if (Test-Path -LiteralPath $wingetRoot) {
        $found = Get-ChildItem -Path $wingetRoot -Recurse -Filter zig.exe -ErrorAction SilentlyContinue |
            Sort-Object FullName |
            Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }

    throw "Could not locate zig.exe. Install Zig (winget install zig.zig) or pass -ZigExe."
}

$zigPath = Resolve-ZigPath -ConfiguredPath $ZigExe
$sourceDir = Join-Path $extensionRoot "src"
$includeDir = Join-Path $extensionRoot "duckdb_capi"
$dllPath = Join-Path $buildRoot "bitmap.dll"

$sourceFiles = @(Get-ChildItem -Path $sourceDir -Recurse -Filter *.c | Sort-Object FullName | ForEach-Object { $_.FullName })
if ($sourceFiles.Count -eq 0) {
    throw "No C source files found under $sourceDir"
}

$zigArgs = @(
    "cc",
    "-shared",
    "-O2",
    "-std=c99",
    "-D", "DUCKDB_EXTENSION_NAME=bitmap",
    "-D", "DUCKDB_EXTENSION_API_VERSION_MAJOR=1",
    "-D", "DUCKDB_EXTENSION_API_VERSION_MINOR=2",
    "-D", "DUCKDB_EXTENSION_API_VERSION_PATCH=0",
    "-I", $includeDir
)
$zigArgs += $sourceFiles
$zigArgs += @("-o", $dllPath)

& $zigPath @zigArgs
if ($LASTEXITCODE -ne 0) {
    throw "zig compilation failed"
}

$output = Join-Path $buildRoot "bitmap.duckdb_extension"
python (Join-Path $PSScriptRoot "append_metadata.py") `
    --library-file $dllPath `
    --extension-name bitmap `
    --out-file $output `
    --duckdb-version $DuckDbCapiVersion `
    --duckdb-platform $DuckDbPlatform `
    --extension-version $ExtensionVersion `
    --abi-type C_STRUCT

Write-Host $output
