param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration

$ctest = Get-FangCTest
$preset = "fang-$($Configuration.ToLowerInvariant())"
& $ctest --preset $preset
if ($LASTEXITCODE -ne 0) {
    throw "CTest failed with exit code $LASTEXITCODE."
}