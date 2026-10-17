<#
.SYNOPSIS
    Runs the automated Fang Reference Runtime End-to-End Smoke Test against an Android API 36 reference AVD.

.DESCRIPTION
    Validates the complete reference runtime lifecycle:
    Supervisor launch -> Windows Job ownership verified -> Android guest boot-ready (sys.boot_completed=1)
    -> first-party validation APK installed and launched -> observable rendering evidence verified
    -> graceful shutdown -> verified-empty ownership state -> second start/stop cycle verified.

.PARAMETER Configuration
    Build configuration to use ('Debug' or 'Release'). Defaults to 'Debug'.

.PARAMETER AvdName
    Name of the reference AVD to launch. Defaults to 'FANG-StageA-API36-x86_64-headed'.

.PARAMETER ApkPath
    Relative or absolute path to the first-party validation APK.
    Defaults to 'out\android-animated-gles\builds\b5d9e68b44a891932987eb06\fang-animated-gles-validation.apk'.

.PARAMETER TimeoutMs
    Optional timeout in milliseconds for guest boot readiness. Defaults to 240000 (4 minutes).

.EXAMPLE
    .\scripts\run-reference-runtime-smoke.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$AvdName = 'FANG-StageA-API36-x86_64-headed',

    [string]$ApkPath = 'out\android-animated-gles\builds\b5d9e68b44a891932987eb06\fang-animated-gles-validation.apk',

    [uint32]$TimeoutMs = 0
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

Write-Host "================================================================================"
Write-Host " Fang Reference Runtime Smoke Test Invocation"
Write-Host " Repository:    $repositoryRoot"
Write-Host " Configuration: $Configuration"
Write-Host " AVD Name:      $AvdName"
Write-Host " APK Path:      $ApkPath"
Write-Host "================================================================================"

# Verify smoke test executable
$smokeExecutable = Join-Path $repositoryRoot "out\build\$Configuration\fang-reference-runtime-smoke.exe"
if (-not (Test-Path $smokeExecutable)) {
    throw "The smoke test executable was not found at '$smokeExecutable'. Build it first via: cmake --build out\build --config $Configuration --target fang-reference-runtime-smoke"
}

# Verify supervisor executable
$supervisorExecutable = Join-Path $repositoryRoot "out\build\$Configuration\fang-runtime-supervisor.exe"
if (-not (Test-Path $supervisorExecutable)) {
    throw "The supervisor executable was not found at '$supervisorExecutable'. Build it first via: cmake --build out\build --config $Configuration --target fang-runtime-supervisor"
}

# Verify APK
$resolvedApk = if ([System.IO.Path]::IsPathRooted($ApkPath)) { $ApkPath } else { Join-Path $repositoryRoot $ApkPath }
if (-not (Test-Path $resolvedApk)) {
    throw "The validation APK artifact was not found at '$resolvedApk'."
}

# Run the test
$arguments = @(
    '--avd', $AvdName,
    '--apk', $resolvedApk
)
if ($TimeoutMs -gt 0) {
    $arguments += @('--timeout-ms', $TimeoutMs.ToString())
}

Write-Host "`nExecuting: $smokeExecutable $arguments`n"
& $smokeExecutable @arguments
$testExitCode = $LASTEXITCODE

Write-Host "`nTest completed with exit code: $testExitCode"
exit $testExitCode
