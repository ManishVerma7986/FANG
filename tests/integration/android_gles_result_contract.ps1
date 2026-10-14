$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
Import-Module (Join-Path $repoRoot 'scripts\GlesProbeResultContract.psm1') -Force

function New-TestEvidence {
    param([object]$Value, [string]$Source, [string]$Reason = 'Not available in the fixture.')
    New-GlesEvidence -Value $Value -Source $Source -UnavailableReason $Reason
}

function Require-Rejection {
    param([scriptblock]$Action, [string]$Message)
    $rejected = $false
    try {
        & $Action
    } catch {
        $rejected = $true
    }
    if (-not $rejected) {
        throw $Message
    }
}

$summary = [ordered]@{
    api = 36
    iterations = 8
    refresh_hz = 60.0
    renderer = 'Reference renderer'
    gl_version = 'OpenGL ES 3.1'
    frames = 2
    draw_ms_p50 = 1.0
    draw_ms_p95 = 2.0
    draw_ms_p99 = 3.0
    over_budget_percent = 0.0
    measurement_seconds = 5.0
} | ConvertTo-Json -Depth 8 | ConvertFrom-Json

$manifestSource = [ordered]@{
    schema_version = 1
    kind = 'fang-android-gles-probe-run'
    run_id = 'fixture-run-1'
    run_started_utc = '2026-10-14T20:00:00.0000000Z'
    collection = [ordered]@{ duration_ms = 15; completed_before_activity_start = $true }
    host = [ordered]@{
        windows_build = New-TestEvidence '10.0.26300' 'Win32_OperatingSystem'
        os_architecture = New-TestEvidence '64-bit' 'Win32_OperatingSystem.OSArchitecture'
        cpu = New-TestEvidence @{ name = 'fixture cpu' } 'Win32_Processor'
        gpu_adapters = New-TestEvidence @(@{
            name = New-TestEvidence 'fixture gpu' 'Win32_VideoController.Name'
            driver = New-TestEvidence '1.0' 'Win32_VideoController.DriverVersion'
        }) 'Win32_VideoController'
        displays = New-TestEvidence @(@{ name = 'display'; width = 1080; height = 2400 }) 'System.Windows.Forms.Screen'
        physical_memory_bytes = New-TestEvidence ([uint64]17179869184) 'Win32_ComputerSystem.TotalPhysicalMemory'
        active_power_scheme = New-TestEvidence 'Balanced' 'powercfg /getactivescheme'
        effective_power_mode = New-TestEvidence $null 'Windows power-mode API' 'The effective power-mode overlay is not queried.'
    }
    runtime = [ordered]@{
        emulator_version = New-TestEvidence 'Android Emulator 37.2.12' 'emulator -version'
    }
    virtualization = [ordered]@{
        windows_hypervisor_present = New-TestEvidence $true 'Win32_ComputerSystem.HypervisorPresent'
        emulator_accel_check = New-TestEvidence 'WHPX installed and usable' 'emulator -accel-check'
        active_accelerator = New-TestEvidence $null 'Android Emulator process' 'The active accelerator is not independently exposed.'
        requested_acceleration = New-TestEvidence 'on' 'emulator process command line'
        requested_graphics_mode = New-TestEvidence 'host' 'emulator process command line'
    }
    guest = [ordered]@{
        adb_serial = New-TestEvidence 'emulator-5554' 'adb devices'
        avd_name = New-TestEvidence 'fixture-avd' 'guest boot property'
        android_release = New-TestEvidence '16' 'getprop ro.build.version.release'
        android_api = New-TestEvidence 36 'getprop ro.build.version.sdk'
        abi = New-TestEvidence 'x86_64' 'getprop ro.product.cpu.abi'
        avd_config = New-TestEvidence ([ordered]@{ 'hw.ramSize' = '2G'; 'hw.cpu.ncore' = '4'; 'hw.gpu.mode' = 'host' }) 'temporary AVD config.ini allowlist'
        system_image = New-TestEvidence ([ordered]@{
            package_id = New-TestEvidence 'system-images;android-36;default;x86_64' 'AVD image.sysdir.1 + source.properties'
            package_revision = New-TestEvidence '2' 'source.properties Pkg.Revision'
            api_level = New-TestEvidence '36' 'source.properties AndroidVersion.ApiLevel'
            abi = New-TestEvidence 'x86_64' 'source.properties SystemImage.Abi'
            tag = New-TestEvidence 'default' 'source.properties SystemImage.TagId'
        }) 'system image source.properties'
        display_size = New-TestEvidence '1080x2400' 'adb shell wm size'
        display_density = New-TestEvidence '420' 'adb shell wm density'
        renderer = New-TestEvidence $summary.renderer 'GLES probe summary'
        graphics_api = New-TestEvidence $summary.gl_version 'GLES probe summary'
        refresh_hz = New-TestEvidence $summary.refresh_hz 'GLES probe summary'
    }
    source = [ordered]@{
        git_commit = New-TestEvidence 'abcdef0' 'git rev-parse HEAD'
        worktree_dirty = New-TestEvidence $false 'git status --porcelain'
        apk_sha256 = New-TestEvidence '0123456789abcdef' 'Get-FileHash'
    }
    workload = [ordered]@{
        identifier = 'fang-gles-fullscreen-shader-v1'
        benchmark_version = 1
        iterations = 8
        duration_seconds = 5
        warmup_seconds = 1
    }
    artifacts = [ordered]@{
        summary_json = 'gles-fixture.json'
        samples_csv = 'gles-fixture.csv'
        manifest_json = 'gles-fixture.manifest.json'
    }
}
$manifest = $manifestSource | ConvertTo-Json -Depth 20 | ConvertFrom-Json

$csvLines = @(
    'elapsed_ms,draw_completion_ms',
    '1100.000,1.000',
    '1200.000,2.000',
    ('SUMMARY,' + ($summary | ConvertTo-Json -Compress -Depth 8))
)

$matchingLog = 'RUN_RESULT run_id=fixture-run-1 {"frames":2}'
if ((Get-GlesProbeLogSummary -Line $matchingLog -RunId 'fixture-run-1') -ne '{"frames":2}') {
    throw 'Matching run-id result log was not parsed.'
}
if (Get-GlesProbeLogSummary -Line 'RUN_RESULT run_id=old-run {"frames":1}' -RunId 'fixture-run-1') {
    throw 'A stale result from another run id was accepted.'
}

if (-not (Test-GlesProbeResultContract -Manifest $manifestSource -Summary $summary -CsvLines $csvLines)) {
    throw 'Ordered-dictionary GLES manifest was rejected.'
}
if (-not (Test-GlesProbeResultContract -Manifest $manifest -Summary $summary -CsvLines $csvLines)) {
    throw 'Valid GLES result was rejected.'
}
if (-not (Test-GlesProbeResultContract -Manifest $manifest -Summary $summary -CsvLines $csvLines -RequireCompleteManifest)) {
    throw 'Complete GLES provenance was rejected by strict validation.'
}

$legacyManifest = $manifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$legacyManifest.host.PSObject.Properties.Remove('os_architecture')
$legacyManifest.guest.PSObject.Properties.Remove('system_image')
if (-not (Test-GlesProbeResultContract -Manifest $legacyManifest -Summary $summary -CsvLines $csvLines)) {
    throw 'A backward-compatible historical schema-v1 manifest was rejected.'
}
Require-Rejection { Test-GlesProbeResultContract -Manifest $legacyManifest -Summary $summary -CsvLines $csvLines -RequireCompleteManifest } 'A manifest missing new provenance fields passed strict validation.'

$badTimestamp = $manifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$badTimestamp.run_started_utc = '2026-10-14T21:00:00+01:00'
Require-Rejection { Test-GlesProbeResultContract -Manifest $badTimestamp -Summary $summary -CsvLines $csvLines } 'A non-UTC timestamp was accepted as UTC.'

$badRefreshRate = $manifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$badRefreshRate.guest.refresh_hz.value = 59.0
Require-Rejection { Test-GlesProbeResultContract -Manifest $badRefreshRate -Summary $summary -CsvLines $csvLines } 'A manifest refresh rate that disagrees with the summary was accepted.'

$unavailable = New-GlesEvidence -Value $null -Source 'fixture unavailable source' -UnavailableReason 'Fixture value was deliberately omitted.'
if ($unavailable.status -ne 'unavailable' -or $unavailable.value -ne $null -or [string]::IsNullOrWhiteSpace($unavailable.reason)) {
    throw 'Unavailable evidence was not represented explicitly.'
}

$missingUnavailableReason = $manifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$missingUnavailableReason.host.effective_power_mode.reason = ''
Require-Rejection { Test-GlesProbeResultContract -Manifest $missingUnavailableReason -Summary $summary -CsvLines $csvLines } 'Unavailable evidence without a reason was accepted.'

$badStatus = $manifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$badStatus.host.gpu_adapters.status = 'guessed'
Require-Rejection { Test-GlesProbeResultContract -Manifest $badStatus -Summary $summary -CsvLines $csvLines } 'Unsupported evidence status was accepted.'

$badNestedStatus = $manifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$badNestedStatus.host.gpu_adapters.value[0].driver.status = 'guessed'
Require-Rejection { Test-GlesProbeResultContract -Manifest $badNestedStatus -Summary $summary -CsvLines $csvLines } 'Unsupported nested adapter evidence status was accepted.'

$badCount = @($csvLines[0], $csvLines[1], $csvLines[3])
Require-Rejection { Test-GlesProbeResultContract -Manifest $manifest -Summary $summary -CsvLines $badCount } 'CSV/frame count mismatch was accepted.'

$badCsvSummary = $csvLines.Clone()
$badCsvSummary[3] = 'SUMMARY,' + (($summary | Select-Object * | ForEach-Object { $_.frames = 1; $_ } | ConvertTo-Json -Compress -Depth 8))
Require-Rejection { Test-GlesProbeResultContract -Manifest $manifest -Summary $summary -CsvLines $badCsvSummary } 'CSV summary mismatch was accepted.'

$badRenderer = $summary | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$badRenderer.renderer = ''
Require-Rejection { Test-GlesProbeResultContract -Manifest $manifest -Summary $badRenderer -CsvLines $csvLines } 'Missing renderer was accepted.'

Write-Output 'Android GLES result contract tests passed: strict and legacy manifests, unavailable provenance, UTC timestamp, refresh consistency, invalid status, CSV integrity, and missing renderer.'