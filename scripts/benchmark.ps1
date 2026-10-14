param(
    [ValidateRange(1, 3600000)]
    [uint64]$DurationMs = 1000,
    [ValidateRange(1, 10000000)]
    [uint64]$IntervalUs = 1000,
    [ValidateRange(1, 100)]
    [uint64]$Runs = 3,
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $repoRoot 'out\build\Release\fang-host-timing.exe'
if (-not (Test-Path $executable)) {
    $executable = Join-Path $repoRoot 'out\build\Debug\fang-host-timing.exe'
}
if (-not (Test-Path $executable)) {
    throw 'Build Fang first with ./scripts/build.ps1.'
}

if (-not $OutputFile) {
    $outputDirectory = Join-Path $repoRoot 'out\benchmarks'
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputFile = Join-Path $outputDirectory "host-timing-$timestamp.json"
}

& $executable --duration-ms $DurationMs --interval-us $IntervalUs --runs $Runs --output $OutputFile
if ($LASTEXITCODE -ne 0) {
    throw "Host timing benchmark failed with exit code $LASTEXITCODE."
}
Write-Output "Wrote benchmark results to $OutputFile"