function New-GlesEvidence {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Source,
        [string]$UnavailableReason = 'The source did not provide a value.'
    )

    $hasValue = $null -ne $Value
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) {
        $hasValue = $false
    } elseif ($Value -is [System.Collections.ICollection] -and $Value.Count -eq 0) {
        $hasValue = $false
    }

    if ($hasValue) {
        return [ordered]@{ status = 'collected'; source = $Source; value = $Value }
    }
    return [ordered]@{ status = 'unavailable'; source = $Source; value = $null; reason = $UnavailableReason }
}

function Get-GlesManifestPathValue {
    param([object]$InputObject, [string]$Path)
    $current = $InputObject
    foreach ($segment in $Path.Split('.')) {
        if ($null -eq $current) { return $null }
        if ($current -is [System.Collections.IDictionary]) {
            if (-not $current.Contains($segment)) { return $null }
            $current = $current[$segment]
        } else {
            $property = $current.PSObject.Properties[$segment]
            if ($null -eq $property) { return $null }
            $current = $property.Value
        }
    }
    return $current
}

function Assert-GlesEvidenceTree {
    param([object]$InputObject, [string]$Path = 'manifest')
    if ($null -eq $InputObject -or $InputObject -is [string] -or $InputObject.GetType().IsValueType) { return }
    if ($InputObject -is [System.Collections.IEnumerable] -and
        $InputObject -isnot [System.Collections.IDictionary] -and
        $InputObject -isnot [pscustomobject]) {
        $index = 0
        foreach ($item in $InputObject) {
            Assert-GlesEvidenceTree -InputObject $item -Path "$Path[$index]"
            $index++
        }
        return
    }

    if ($InputObject -is [System.Collections.IDictionary]) {
        $hasStatus = $InputObject.Contains('status')
        $status = if ($hasStatus) { [string]$InputObject['status'] } else { $null }
        $source = if ($InputObject.Contains('source')) { [string]$InputObject['source'] } else { $null }
        $hasValue = $InputObject.Contains('value')
        $value = if ($hasValue) { $InputObject['value'] } else { $null }
        $reason = if ($InputObject.Contains('reason')) { [string]$InputObject['reason'] } else { $null }
    } else {
        $statusProperty = $InputObject.PSObject.Properties['status']
        $hasStatus = $null -ne $statusProperty
        $status = if ($hasStatus) { [string]$statusProperty.Value } else { $null }
        $sourceProperty = $InputObject.PSObject.Properties['source']
        $source = if ($null -ne $sourceProperty) { [string]$sourceProperty.Value } else { $null }
        $valueProperty = $InputObject.PSObject.Properties['value']
        $hasValue = $null -ne $valueProperty
        $value = if ($hasValue) { $valueProperty.Value } else { $null }
        $reasonProperty = $InputObject.PSObject.Properties['reason']
        $reason = if ($null -ne $reasonProperty) { [string]$reasonProperty.Value } else { $null }
    }

    if ($hasStatus) {
        if ([string]::IsNullOrWhiteSpace($source) -or -not $hasValue) {
            throw "$Path must include a source and value field."
        }
        if ($status -eq 'collected') {
            if ($null -eq $value -or
                ($value -is [string] -and [string]::IsNullOrWhiteSpace($value)) -or
                ($value -is [System.Collections.ICollection] -and $value.Count -eq 0)) {
                throw "$Path is marked collected without a value."
            }
        } elseif ($status -eq 'unavailable') {
            if ($null -ne $value -or [string]::IsNullOrWhiteSpace($reason)) {
                throw "$Path is marked unavailable without an explanatory reason."
            }
        } else {
            throw "$Path has unsupported evidence status '$status'."
        }
        if ($status -eq 'collected') {
            Assert-GlesEvidenceTree -InputObject $value -Path "$Path.value"
        }
        return
    }

    if ($InputObject -is [System.Collections.IDictionary]) {
        foreach ($key in $InputObject.Keys) {
            Assert-GlesEvidenceTree -InputObject $InputObject[$key] -Path "$Path.$key"
        }
    } else {
        foreach ($property in $InputObject.PSObject.Properties) {
            Assert-GlesEvidenceTree -InputObject $property.Value -Path "$Path.$($property.Name)"
        }
    }
}

function Get-GlesProbeLogSummary {
    param([string]$Line, [string]$RunId)
    if ([string]::IsNullOrWhiteSpace($Line) -or [string]::IsNullOrWhiteSpace($RunId)) { return $null }
    $pattern = '^RUN_RESULT\s+run_id=' + [regex]::Escape($RunId) + '\s+(\{.*\})$'
    if ($Line -match $pattern) { return $Matches[1] }
    return $null
}

function Test-GlesProbeResultContract {
    param(
        [Parameter(Mandatory)][object]$Manifest,
        [Parameter(Mandatory)][object]$Summary,
        [Parameter(Mandatory)][string[]]$CsvLines,
        [switch]$RequireCompleteManifest
    )

    if ($Manifest.schema_version -ne 1 -or $Manifest.kind -ne 'fang-android-gles-probe-run') {
        throw 'Unsupported GLES probe manifest schema or kind.'
    }
    if ([string]::IsNullOrWhiteSpace([string]$Manifest.run_id)) {
        throw 'Manifest run_id is required.'
    }
    $runTimestamp = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse([string]$Manifest.run_started_utc, [ref]$runTimestamp)) {
        throw 'Manifest run_started_utc must be an ISO-8601 timestamp.'
    }
    if ($runTimestamp.Offset -ne [TimeSpan]::Zero) {
        throw 'Manifest run_started_utc must identify a UTC timestamp.'
    }
    if ($Manifest.collection.completed_before_activity_start -ne $true -or $Manifest.collection.duration_ms -lt 0) {
        throw 'Metadata collection must complete before Activity launch with a nonnegative duration.'
    }

    $requiredEvidence = @(
        'host.windows_build', 'host.cpu', 'host.gpu_adapters', 'host.displays',
        'host.active_power_scheme', 'host.effective_power_mode', 'runtime.emulator_version',
        'virtualization.windows_hypervisor_present', 'virtualization.emulator_accel_check',
        'virtualization.active_accelerator', 'virtualization.requested_acceleration',
        'virtualization.requested_graphics_mode', 'guest.adb_serial', 'guest.avd_name',
        'guest.android_release', 'guest.android_api', 'guest.abi', 'guest.display_size',
        'guest.display_density', 'guest.renderer', 'guest.graphics_api', 'source.git_commit',
        'source.worktree_dirty', 'source.apk_sha256'
    )
    if ($RequireCompleteManifest) {
        $requiredEvidence += @(
            'host.os_architecture', 'host.physical_memory_bytes', 'guest.avd_config',
            'guest.system_image', 'guest.refresh_hz'
        )
    }
    foreach ($requiredPath in $requiredEvidence) {
        if ($null -eq (Get-GlesManifestPathValue -InputObject $Manifest -Path $requiredPath)) {
            throw "Required manifest evidence field is missing: $requiredPath"
        }
    }
    foreach ($section in @('host', 'runtime', 'virtualization', 'guest', 'source')) {
        Assert-GlesEvidenceTree -InputObject $Manifest.$section -Path "manifest.$section"
    }

    if ([string]::IsNullOrWhiteSpace([string]$Manifest.workload.identifier) -or
        $Manifest.workload.benchmark_version -lt 1 -or
        $Manifest.workload.iterations -ne $Summary.iterations -or
        $Manifest.workload.duration_seconds -ne $Summary.measurement_seconds -or
        $Manifest.workload.warmup_seconds -lt 1) {
        throw 'Manifest workload fields do not match the GLES summary.'
    }
    if ($Manifest.guest.android_api.value -ne $Summary.api -or
        $Manifest.guest.renderer.value -ne $Summary.renderer -or
        $Manifest.guest.graphics_api.value -ne $Summary.gl_version) {
        throw 'Manifest guest identity does not match the GLES summary.'
    }
    $guestRefreshEvidence = Get-GlesManifestPathValue -InputObject $Manifest -Path 'guest.refresh_hz'
    if ($null -ne $guestRefreshEvidence -and $guestRefreshEvidence.status -eq 'collected' -and
        $guestRefreshEvidence.value -ne $Summary.refresh_hz) {
        throw 'Manifest guest refresh rate does not match the GLES summary.'
    }
    if ($Summary.frames -lt 1 -or $Summary.refresh_hz -le 0 -or
        $Summary.draw_ms_p50 -lt 0 -or $Summary.draw_ms_p95 -lt $Summary.draw_ms_p50 -or
        $Summary.draw_ms_p99 -lt $Summary.draw_ms_p95 -or
        $Summary.over_budget_percent -lt 0 -or $Summary.over_budget_percent -gt 100) {
        throw 'GLES summary values are outside the result contract.'
    }
    if ([string]::IsNullOrWhiteSpace([string]$Manifest.artifacts.summary_json) -or
        [string]::IsNullOrWhiteSpace([string]$Manifest.artifacts.samples_csv) -or
        [string]::IsNullOrWhiteSpace([string]$Manifest.artifacts.manifest_json)) {
        throw 'Manifest artifact paths are required.'
    }

    $nonemptyLines = @($CsvLines | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($nonemptyLines.Count -lt 3 -or $nonemptyLines[0] -ne 'elapsed_ms,draw_completion_ms') {
        throw 'CSV header or required summary row is missing.'
    }
    $summaryRows = @($nonemptyLines | Where-Object { $_ -match '^SUMMARY,' })
    $sampleRows = @($nonemptyLines | Where-Object { $_ -ne 'elapsed_ms,draw_completion_ms' -and $_ -notmatch '^SUMMARY,' })
    if ($summaryRows.Count -ne 1 -or $sampleRows.Count -ne $Summary.frames) {
        throw 'Raw CSV sample count does not match the GLES summary frame count.'
    }
    try {
        $csvSummary = $summaryRows[0].Substring('SUMMARY,'.Length) | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw 'CSV summary row is not valid JSON.'
    }
    foreach ($field in @('api', 'iterations', 'refresh_hz', 'renderer', 'gl_version', 'frames', 'draw_ms_p50', 'draw_ms_p95', 'draw_ms_p99', 'over_budget_percent', 'measurement_seconds')) {
        if ($csvSummary.$field -ne $Summary.$field) {
            throw "CSV summary field '$field' does not match the summary JSON."
        }
    }
    foreach ($row in $sampleRows) {
        if ($row -notmatch '^\d+(?:\.\d+)?,-?\d+(?:\.\d+)?$') {
            throw "Malformed GLES sample row: $row"
        }
    }
    return $true
}

Export-ModuleMember -Function New-GlesEvidence, Get-GlesProbeLogSummary, Test-GlesProbeResultContract