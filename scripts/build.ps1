param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Invoke-FangCMake -Arguments @('--preset', 'fang-windows')
$preset = "fang-$($Configuration.ToLowerInvariant())"
Invoke-FangCMake -Arguments @('--build', '--preset', $preset, '--parallel')