Set-StrictMode -Version Latest

function Get-AnimatedProperty {
    param([object]$InputObject, [string]$Name)
    if ($null -eq $InputObject) { return $null }
    if ($InputObject -is [System.Collections.IDictionary]) {
        if ($InputObject.Contains($Name)) { return $InputObject[$Name] }
        return $null
    }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Assert-AnimatedEvidenceTree {
    param([object]$Value, [string]$Path)
    if ($null -eq $Value -or $Value -is [string] -or $Value.GetType().IsValueType) { return }
    if ($Value -is [System.Collections.IEnumerable] -and
        $Value -isnot [System.Collections.IDictionary] -and
        $Value -isnot [pscustomobject]) {
        $index = 0
        foreach ($item in $Value) {
            Assert-AnimatedEvidenceTree -Value $item -Path "$Path[$index]"
            $index++
        }
        return
    }

    $status = Get-AnimatedProperty $Value 'status'
    if ($null -ne $status) {
        $source = Get-AnimatedProperty $Value 'source'
        $evidenceValue = Get-AnimatedProperty $Value 'value'
        $reason = Get-AnimatedProperty $Value 'reason'
        if ([string]::IsNullOrWhiteSpace([string]$source)) {
            throw "$Path evidence must identify its source."
        }
        switch ([string]$status) {
            'collected' {
                if ($null -eq $evidenceValue -or
                    ($evidenceValue -is [string] -and [string]::IsNullOrWhiteSpace($evidenceValue))) {
                    throw "$Path is marked collected without a value."
                }
                Assert-AnimatedEvidenceTree -Value $evidenceValue -Path "$Path.value"
            }
            { $_ -in 'unavailable', 'not_yet_validated' } {
                if ($null -ne $evidenceValue -or [string]::IsNullOrWhiteSpace([string]$reason)) {
                    throw "$Path must explain an unavailable or unvalidated value and must not supply one."
                }
            }
            default { throw "$Path has unsupported evidence status '$status'." }
        }
        return
    }

    if ($Value -is [System.Collections.IDictionary]) {
        foreach ($key in $Value.Keys) {
            Assert-AnimatedEvidenceTree -Value $Value[$key] -Path "$Path.$key"
        }
    } else {
        foreach ($property in $Value.PSObject.Properties) {
            Assert-AnimatedEvidenceTree -Value $property.Value -Path "$Path.$($property.Name)"
        }
    }
}

function Test-AnimatedGlesResultContract {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Result,
        [Parameter(Mandatory)][string]$EventsPath
    )

    if ((Get-AnimatedProperty $Result 'schema_version') -ne 1 -or
        (Get-AnimatedProperty $Result 'kind') -ne 'fang-animated-gles-run') {
        throw 'Unsupported animated GLES result schema or kind.'
    }
    $runId = [string](Get-AnimatedProperty $Result 'run_id')
    $parsedRunId = [guid]::Empty
    if (-not [guid]::TryParse($runId, [ref]$parsedRunId)) {
        throw 'run_id must be a UUID.'
    }
    $workload = Get-AnimatedProperty $Result 'workload'
    if ((Get-AnimatedProperty $workload 'identifier') -ne 'fang-animated-gles' -or
        (Get-AnimatedProperty $workload 'version') -ne 1 -or
        [string]::IsNullOrWhiteSpace([string](Get-AnimatedProperty $workload 'build_id'))) {
        throw 'Animated workload identity and build ID are required.'
    }

    $requested = Get-AnimatedProperty $Result 'requested_profile'
    foreach ($field in @('width_px', 'height_px', 'orientation', 'gles_major', 'refresh_hz')) {
        if ($null -eq (Get-AnimatedProperty $requested $field)) {
            throw "Required requested-profile field is missing: $field."
        }
    }
    $observed = Get-AnimatedProperty $Result 'observed_profile'
    foreach ($field in @('surface_width_px', 'surface_height_px', 'orientation', 'gles_major', 'gles_version',
            'gl_vendor', 'gl_renderer', 'display_refresh_hz')) {
        $evidence = Get-AnimatedProperty $observed $field
        if ($null -eq $evidence) {
            throw "Required observed-profile evidence is missing: $field."
        }
        Assert-AnimatedEvidenceTree -Value $evidence -Path "observed_profile.$field"
    }

    $run = Get-AnimatedProperty $Result 'run'
    $status = [string](Get-AnimatedProperty $run 'status')
    $validity = [string](Get-AnimatedProperty $run 'validity')
    if ($status -notin @('completed', 'interrupted', 'failed') -or
        $validity -notin @('valid', 'diagnostic', 'invalid', 'failed')) {
        throw 'Run status or validity is missing or unsupported.'
    }
    $profileConforming = Get-AnimatedProperty $run 'profile_conforming'
    $diagnosticMode = Get-AnimatedProperty $run 'diagnostic_mode'
    if ($profileConforming -isnot [bool] -or $diagnosticMode -isnot [bool]) {
        throw 'Profile conformance and diagnostic mode must be explicit.'
    }
    if ($validity -eq 'valid' -and
        ($status -ne 'completed' -or $profileConforming -ne $true -or $diagnosticMode -eq $true)) {
        throw 'Only a completed, conforming, non-diagnostic run may be valid.'
    }
    if (($status -eq 'interrupted' -and $validity -ne 'invalid') -or
        ($status -eq 'failed' -and $validity -ne 'failed') -or
        ($status -eq 'completed' -and $validity -eq 'failed') -or
        ($validity -eq 'diagnostic' -and
            ($status -ne 'completed' -or $diagnosticMode -ne $true))) {
        throw 'Run status and validity are inconsistent.'
    }
    $profileMatches = $true
    foreach ($pair in @(
        @{ observed = 'surface_width_px'; requested = 'width_px' },
        @{ observed = 'surface_height_px'; requested = 'height_px' },
        @{ observed = 'orientation'; requested = 'orientation' },
        @{ observed = 'gles_major'; requested = 'gles_major' },
        @{ observed = 'display_refresh_hz'; requested = 'refresh_hz' }
    )) {
        $evidence = Get-AnimatedProperty $observed $pair.observed
        if ((Get-AnimatedProperty $evidence 'status') -ne 'collected' -or
            (Get-AnimatedProperty $evidence 'value') -ne (Get-AnimatedProperty $requested $pair.requested)) {
            $profileMatches = $false
        }
    }
    if ($profileConforming -ne $profileMatches) {
        throw 'Recorded profile_conforming does not agree with requested and observed profile evidence.'
    }
    if (($validity -in @('invalid', 'diagnostic', 'failed') -or $status -in @('interrupted', 'failed')) -and
        [string]::IsNullOrWhiteSpace([string](Get-AnimatedProperty $run 'invalidation_reason'))) {
        throw 'An invalid, diagnostic, interrupted, or failed run must state its reason.'
    }

    $eventsArtifact = Get-AnimatedProperty (Get-AnimatedProperty $Result 'artifacts') 'events_jsonl'
    if ((Get-AnimatedProperty $eventsArtifact 'path') -ne 'events.jsonl') {
        throw 'Raw event artifact path must be the run-local events.jsonl file.'
    }
    if (-not (Test-Path -LiteralPath $EventsPath -PathType Leaf)) {
        throw 'The raw event artifact is missing.'
    }
    $bytes = [System.IO.File]::ReadAllBytes($EventsPath)
    $actualHash = [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
    if ($actualHash -ne [string](Get-AnimatedProperty $eventsArtifact 'sha256') -or
        $bytes.Length -ne (Get-AnimatedProperty $eventsArtifact 'bytes')) {
        throw 'Raw event artifact hash or byte count does not match the result.'
    }

    $lines = [System.IO.File]::ReadAllLines($EventsPath, [System.Text.Encoding]::UTF8)
    if ($lines.Count -eq 0 -or $lines.Count -ne (Get-AnimatedProperty $eventsArtifact 'event_count')) {
        throw 'Raw event count is empty or does not match the result.'
    }
    $events = New-Object System.Collections.Generic.List[object]
    $expectedSequence = 1
    $lastGuestNs = [long]0
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { throw 'Raw event artifact contains an empty line.' }
        try { $event = ConvertFrom-Json -InputObject $line -ErrorAction Stop }
        catch { throw "Malformed raw event JSON: $($_.Exception.Message)" }
        if ($event.run_id -isnot [string] -or $event.run_id -ne $runId -or
            ($event.sequence -isnot [int] -and $event.sequence -isnot [long]) -or
            $event.sequence -ne $expectedSequence) {
            throw 'Raw event run ID or sequence is inconsistent.'
        }
        if ($event.type -isnot [string] -or [string]::IsNullOrWhiteSpace($event.type)) {
            throw 'Every raw event must identify its event type.'
        }
        if ($event.clock_domain -isnot [string] -or $event.clock_domain -ne 'guest_monotonic') {
            throw 'Every raw event must identify its guest_monotonic clock domain.'
        }
        if ($event.guest_monotonic_ns -isnot [long] -and $event.guest_monotonic_ns -isnot [int]) {
            throw 'Event guest_monotonic_ns must be an integer.'
        }
        if ([long]$event.guest_monotonic_ns -le 0 -or [long]$event.guest_monotonic_ns -lt $lastGuestNs) {
            throw 'Guest monotonic event timestamps must be nondecreasing.'
        }
        $lastGuestNs = [long]$event.guest_monotonic_ns
        $events.Add($event)
        $expectedSequence++
    }
    if ($events[0].type -ne 'activity_created') {
        throw 'The event stream must start with activity_created.'
    }
    $types = @($events | ForEach-Object { $_.type })
    foreach ($boundaryType in @('scene_ready', 'measurement_started', 'measurement_ended',
            'run_completed', 'run_interrupted')) {
        $boundaryCount = @($types | Where-Object { $_ -eq $boundaryType }).Count
        if ($boundaryCount -gt 1) {
            throw "Lifecycle boundary event '$boundaryType' must not be duplicated."
        }
    }
    if ($status -eq 'completed') {
        foreach ($requiredType in @('scene_ready', 'measurement_started', 'measurement_ended', 'run_completed')) {
            if ($types -notcontains $requiredType) {
                throw "Completed run is missing lifecycle event '$requiredType'."
            }
        }
        $sceneReadyIndex = [array]::IndexOf($types, 'scene_ready')
        $measurementStartedIndex = [array]::IndexOf($types, 'measurement_started')
        $measurementEndedIndex = [array]::IndexOf($types, 'measurement_ended')
        $runCompletedIndex = [array]::IndexOf($types, 'run_completed')
        if ($sceneReadyIndex -ge $measurementStartedIndex -or
            $measurementStartedIndex -ge $measurementEndedIndex -or
            $measurementEndedIndex -ge $runCompletedIndex) {
            throw 'Completed lifecycle boundaries must be ordered scene_ready, measurement_started, measurement_ended, run_completed.'
        }
        if ($types[-1] -ne 'run_completed' -or $types -contains 'run_interrupted') {
            throw 'run_completed must be the final recorded event.'
        }
    } elseif ($status -eq 'interrupted') {
        if ($types[-1] -ne 'run_interrupted' -or
            @($types | Where-Object { $_ -eq 'run_interrupted' }).Count -ne 1 -or
            $types -contains 'run_completed') {
            throw 'An interrupted run must end with run_interrupted.'
        }
    } elseif ($status -eq 'failed' -and
        ($types[-1] -ne 'run_interrupted' -or
            @($types | Where-Object { $_ -eq 'run_interrupted' }).Count -ne 1 -or
            $types -contains 'run_completed')) {
        throw 'A failed run must end with one run_interrupted terminal event.'
    }

    $metrics = Get-AnimatedProperty $Result 'metrics'
    $frameEvents = @($events | Where-Object { $_.type -eq 'frame' })
    $frameIndex = 0
    foreach ($metricName in @('guest_update_duration_ns', 'cpu_submission_duration_ns',
            'draw_count', 'callback_interval_ns', 'gpu_execution_duration_ns',
            'actual_presentation_interval_ns', 'missed_presentation_deadlines')) {
        $metric = Get-AnimatedProperty $metrics $metricName
        if ($null -eq $metric) { throw "Required metric availability is missing: $metricName." }
        $metricStatus = [string](Get-AnimatedProperty $metric 'status')
        if ($metricStatus -notin @('collected', 'unavailable', 'not_yet_validated')) {
            throw "Metric '$metricName' has unsupported availability status."
        }
        if ($metricStatus -eq 'collected' -and
            [string]::IsNullOrWhiteSpace([string](Get-AnimatedProperty $metric 'source'))) {
            throw "Collected metric '$metricName' must name its source."
        }
        if ($metricStatus -in @('unavailable', 'not_yet_validated') -and
            ([string]::IsNullOrWhiteSpace([string](Get-AnimatedProperty $metric 'reason')) -or
                $null -ne (Get-AnimatedProperty $metric 'value'))) {
            throw "Metric '$metricName' must provide a reason and no value when unavailable or not validated."
        }
    }
    if ($status -eq 'completed' -and $validity -eq 'valid' -and $frameEvents.Count -eq 0) {
        throw 'A valid completed capture must contain frame samples.'
    }
    foreach ($event in $frameEvents) {
        $sampleMetrics = Get-AnimatedProperty $event 'metrics'
        foreach ($sampleName in @('guest_update_duration_ns', 'cpu_submission_duration_ns', 'draw_count')) {
            if ($null -eq (Get-AnimatedProperty $sampleMetrics $sampleName)) {
                throw "Frame sample is missing metric '$sampleName'."
            }
        }
        $callbackProperty = if ($sampleMetrics -is [System.Collections.IDictionary]) {
            if ($sampleMetrics.Contains('callback_interval_ns')) { $true } else { $false }
        } else {
            $null -ne $sampleMetrics.PSObject.Properties['callback_interval_ns']
        }
        if (-not $callbackProperty) {
            throw "Frame sample is missing metric 'callback_interval_ns'."
        }
        if (($event.metrics.draw_count -isnot [int] -and $event.metrics.draw_count -isnot [long]) -or
            $event.metrics.draw_count -ne 4 -or
            ($event.metrics.guest_update_duration_ns -isnot [int] -and
                $event.metrics.guest_update_duration_ns -isnot [long]) -or
            ($event.metrics.cpu_submission_duration_ns -isnot [int] -and
                $event.metrics.cpu_submission_duration_ns -isnot [long]) -or
            $event.metrics.guest_update_duration_ns -lt 0 -or
            $event.metrics.cpu_submission_duration_ns -lt 0) {
            throw 'Frame sample contains malformed measurement values.'
        }
        if ($frameIndex -eq 0) {
            if ($null -ne $event.metrics.callback_interval_ns) {
                throw 'The first captured frame callback interval must be explicitly null.'
            }
        } elseif (($event.metrics.callback_interval_ns -isnot [int] -and
                $event.metrics.callback_interval_ns -isnot [long]) -or
            $event.metrics.callback_interval_ns -le 0) {
            throw 'A subsequent frame callback interval must be a positive guest-clock duration.'
        }
        $frameIndex++
    }
    foreach ($metricName in @('guest_update_duration_ns', 'cpu_submission_duration_ns', 'draw_count')) {
        $metric = Get-AnimatedProperty $metrics $metricName
        if ((Get-AnimatedProperty $metric 'status') -eq 'collected' -and
            (Get-AnimatedProperty $metric 'sample_count') -ne $frameEvents.Count) {
            throw "Metric '$metricName' sample count does not reconcile with raw frame events."
        }
    }
    $callbackMetric = Get-AnimatedProperty $metrics 'callback_interval_ns'
    if ((Get-AnimatedProperty $callbackMetric 'status') -eq 'collected' -and
        (Get-AnimatedProperty $callbackMetric 'sample_count') -ne [Math]::Max(0, $frameEvents.Count - 1)) {
        throw 'Callback interval count does not reconcile with raw frame events.'
    }

    return $true
}

Export-ModuleMember -Function Test-AnimatedGlesResultContract
