$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
Import-Module (Join-Path $repoRoot 'scripts\AnimatedGlesResultContract.psm1') -Force

function New-AnimatedEvent {
    param([string]$RunId, [int]$Sequence, [string]$Type, [long]$Timestamp, [object]$Metrics)
    $event = [ordered]@{
        run_id = $RunId
        sequence = $Sequence
        clock_domain = 'guest_monotonic'
        type = $Type
        guest_monotonic_ns = $Timestamp
    }
    if ($null -ne $Metrics) { $event.metrics = $Metrics }
    return $event
}

function New-FrameMetrics {
    param([AllowNull()][object]$CallbackInterval)
    [ordered]@{
        guest_update_duration_ns = 1000
        cpu_submission_duration_ns = 2000
        draw_count = 4
        callback_interval_ns = $CallbackInterval
    }
}

function New-EventSet {
    param([string]$RunId, [switch]$Interrupted)
    $types = @('activity_created', 'display_observed', 'activity_started', 'activity_resumed', 'surface_created',
        'gl_context_created', 'surface_changed', 'scene_ready', 'measurement_started')
    $events = New-Object System.Collections.Generic.List[object]
    $sequence = 0
    foreach ($type in $types) {
        $sequence++
        $events.Add((New-AnimatedEvent $RunId $sequence $type ([long](1000 + $sequence * 100)) $null))
    }
    $sequence++
    $events.Add((New-AnimatedEvent $RunId $sequence 'frame' ([long](1000 + $sequence * 100)) (New-FrameMetrics $null)))
    if ($Interrupted) {
        $sequence++
        $events.Add((New-AnimatedEvent $RunId $sequence 'activity_paused' ([long](1000 + $sequence * 100)) $null))
        $sequence++
        $events.Add((New-AnimatedEvent $RunId $sequence 'run_interrupted' ([long](1000 + $sequence * 100)) $null))
    } else {
        $sequence++
        $events.Add((New-AnimatedEvent $RunId $sequence 'frame' ([long](1000 + $sequence * 100)) (New-FrameMetrics ([long]16666667))))
        $sequence++
        $events.Add((New-AnimatedEvent $RunId $sequence 'measurement_ended' ([long](1000 + $sequence * 100)) $null))
        $sequence++
        $events.Add((New-AnimatedEvent $RunId $sequence 'run_completed' ([long](1000 + $sequence * 100)) $null))
    }
    return ,$events.ToArray()
}

function Write-EventFile {
    param([object[]]$Events, [string]$Path)
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $lines = @($Events | ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 12 })
    $content = ($lines -join "`n") + "`n"
    [System.IO.File]::WriteAllText($Path, $content, $utf8)
    return $content
}

function Get-Sha256 {
    param([byte[]]$Bytes)
    return [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA256]::Create().ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant()
}

function New-Result {
    param(
        [string]$RunId,
        [object[]]$Events,
        [string]$EventContent,
        [switch]$Interrupted,
        [switch]$ProfileMismatch,
        [switch]$DiagnosticMode
    )
    $eventBytes = [System.Text.Encoding]::UTF8.GetBytes($EventContent)
    $observedWidth = if ($ProfileMismatch) { 720 } else { 1080 }
    $frameCount = @($Events | Where-Object { $_.type -eq 'frame' }).Count
    $callbackCount = [Math]::Max(0, $frameCount - 1)
    $validity = if ($Interrupted) {
        'invalid'
    } elseif ($DiagnosticMode) {
        'diagnostic'
    } elseif ($ProfileMismatch) {
        'invalid'
    } else {
        'valid'
    }
    $status = if ($Interrupted) { 'interrupted' } else { 'completed' }
    $reason = if ($Interrupted) {
        'lifecycle_interruption'
    } elseif ($ProfileMismatch) {
        if ($DiagnosticMode) { 'diagnostic_profile_mismatch' } else { 'requested_profile_mismatch' }
    } elseif ($DiagnosticMode) {
        'diagnostic_mode'
    } else {
        $null
    }
    $callbackMetric = if ($callbackCount -eq 0) {
        [ordered]@{ status = 'unavailable'; source = 'Renderer callback timestamps'; value = $null; unit = 'ns'; clock_domain = 'guest_monotonic'; reason = 'No complete callback interval was captured.' }
    } else {
        [ordered]@{ status = 'collected'; source = 'GLSurfaceView.Renderer callback entry timestamps'; unit = 'ns'; clock_domain = 'guest_monotonic'; sample_count = $callbackCount }
    }
    $result = [ordered]@{
        schema_version = 1
        kind = 'fang-animated-gles-run'
        run_id = $RunId
        workload = [ordered]@{ identifier = 'fang-animated-gles'; version = 1; build_id = '0123456789abcdef01234567' }
        requested_profile = [ordered]@{ width_px = 1080; height_px = 2400; orientation = 'portrait'; gles_major = 3; refresh_hz = 60.0 }
        observed_profile = [ordered]@{
            surface_width_px = [ordered]@{ status = 'collected'; source = 'test surface'; value = $observedWidth }
            surface_height_px = [ordered]@{ status = 'collected'; source = 'test surface'; value = 2400 }
            orientation = [ordered]@{ status = 'collected'; source = 'test configuration'; value = 'portrait' }
            gles_major = [ordered]@{ status = 'collected'; source = 'test GL_VERSION'; value = 3 }
            gles_version = [ordered]@{ status = 'collected'; source = 'glGetString(GL_VERSION)'; value = 'OpenGL ES 3.2 test' }
            gl_vendor = [ordered]@{ status = 'collected'; source = 'glGetString(GL_VENDOR)'; value = 'fixture vendor' }
            gl_renderer = [ordered]@{ status = 'collected'; source = 'glGetString(GL_RENDERER)'; value = 'fixture renderer' }
            display_refresh_hz = [ordered]@{ status = 'collected'; source = 'Display.Mode.getRefreshRate'; value = 60.0 }
        }
        run = [ordered]@{
            status = $status
            validity = $validity
            diagnostic_mode = [bool]$DiagnosticMode
            profile_conforming = (-not [bool]$ProfileMismatch)
            invalidation_reason = $reason
        }
        metrics = [ordered]@{
            guest_update_duration_ns = [ordered]@{ status = 'collected'; source = 'renderer CPU update interval'; unit = 'ns'; clock_domain = 'guest_monotonic'; sample_count = $frameCount }
            cpu_submission_duration_ns = [ordered]@{ status = 'collected'; source = 'GLES submission API calls'; unit = 'ns'; clock_domain = 'guest_monotonic'; sample_count = $frameCount }
            draw_count = [ordered]@{ status = 'collected'; source = 'GLES draw call counter'; unit = 'calls_per_callback'; sample_count = $frameCount }
            callback_interval_ns = $callbackMetric
            gpu_execution_duration_ns = [ordered]@{ status = 'not_yet_validated'; source = $null; value = $null; unit = 'ns'; clock_domain = $null; reason = 'Timer source not validated.' }
            actual_presentation_interval_ns = [ordered]@{ status = 'not_yet_validated'; source = $null; value = $null; unit = 'ns'; clock_domain = $null; reason = 'Presentation source not validated.' }
            missed_presentation_deadlines = [ordered]@{ status = 'not_yet_validated'; source = $null; value = $null; unit = 'deadlines'; clock_domain = $null; reason = 'Deadline source not validated.' }
        }
        artifacts = [ordered]@{
            events_jsonl = [ordered]@{ path = 'events.jsonl'; sha256 = (Get-Sha256 $eventBytes); bytes = $eventBytes.Length; event_count = $Events.Count }
        }
    }
    return ($result | ConvertTo-Json -Depth 20 | ConvertFrom-Json)
}

function Require-Rejection {
    param([scriptblock]$Action, [string]$Message)
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw $Message }
}

$tempRoot = Join-Path $env:TEMP ("fang-animated-gles-contract-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
try {
    $runId = [guid]::NewGuid().ToString()
    $eventPath = Join-Path $tempRoot 'events.jsonl'
    $events = New-EventSet $runId
    $eventContent = Write-EventFile $events $eventPath
    $validResult = New-Result $runId $events $eventContent
    if (-not (Test-AnimatedGlesResultContract -Result $validResult -EventsPath $eventPath)) {
        throw 'Valid animated GLES result fixture was rejected.'
    }

    $unsupportedSchema = $validResult | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $unsupportedSchema.schema_version = 2
    Require-Rejection { Test-AnimatedGlesResultContract -Result $unsupportedSchema -EventsPath $eventPath } 'An unsupported schema version was accepted.'

    $missingTerminalEvents = New-EventSet $runId
    $missingTerminalEvents = @($missingTerminalEvents | Where-Object { $_.type -ne 'run_completed' })
    $missingTerminalContent = Write-EventFile $missingTerminalEvents $eventPath
    $missingTerminalResult = New-Result $runId $missingTerminalEvents $missingTerminalContent
    Require-Rejection { Test-AnimatedGlesResultContract -Result $missingTerminalResult -EventsPath $eventPath } 'A completed run missing its terminal event was accepted.'

    $backwardsTimestampEvents = New-EventSet $runId
    $backwardsTimestampEvents[9].guest_monotonic_ns = $backwardsTimestampEvents[8].guest_monotonic_ns - 1
    $backwardsTimestampContent = Write-EventFile $backwardsTimestampEvents $eventPath
    $backwardsTimestampResult = New-Result $runId $backwardsTimestampEvents $backwardsTimestampContent
    Require-Rejection { Test-AnimatedGlesResultContract -Result $backwardsTimestampResult -EventsPath $eventPath } 'A backwards guest-monotonic timestamp was accepted.'

    $nonpositiveTimestampEvents = New-EventSet $runId
    $nonpositiveTimestampEvents[0].guest_monotonic_ns = 0
    $nonpositiveTimestampContent = Write-EventFile $nonpositiveTimestampEvents $eventPath
    $nonpositiveTimestampResult = New-Result $runId $nonpositiveTimestampEvents $nonpositiveTimestampContent
    Require-Rejection { Test-AnimatedGlesResultContract -Result $nonpositiveTimestampResult -EventsPath $eventPath } 'A nonpositive guest-monotonic timestamp was accepted.'

    $diagnosticEvents = New-EventSet $runId
    $diagnosticContent = Write-EventFile $diagnosticEvents $eventPath
    $diagnosticResult = New-Result $runId $diagnosticEvents $diagnosticContent -DiagnosticMode
    if (-not (Test-AnimatedGlesResultContract -Result $diagnosticResult -EventsPath $eventPath)) {
        throw 'A conforming run explicitly marked diagnostic was rejected.'
    }

    $missingMetric = $validResult | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $missingMetric.metrics.PSObject.Properties.Remove('draw_count')
    Require-Rejection { Test-AnimatedGlesResultContract -Result $missingMetric -EventsPath $eventPath } 'A result missing draw_count was accepted.'

    $duplicateEvents = New-EventSet $runId
    $duplicateEvents[9].type = 'scene_ready'
    $duplicateContent = Write-EventFile $duplicateEvents $eventPath
    $duplicateResult = New-Result $runId $duplicateEvents $duplicateContent
    Require-Rejection { Test-AnimatedGlesResultContract -Result $duplicateResult -EventsPath $eventPath } 'A duplicated scene_ready boundary was accepted.'

    $misorderedEvents = New-EventSet $runId
    $misorderedEvents[7].type = 'measurement_started'
    $misorderedEvents[8].type = 'scene_ready'
    $misorderedContent = Write-EventFile $misorderedEvents $eventPath
    $misorderedResult = New-Result $runId $misorderedEvents $misorderedContent
    Require-Rejection { Test-AnimatedGlesResultContract -Result $misorderedResult -EventsPath $eventPath } 'Misordered capture boundaries were accepted.'

    $malformedMetricEvents = New-EventSet $runId
    $malformedMetricEvents[9].metrics.guest_update_duration_ns = '1000'
    $malformedMetricContent = Write-EventFile $malformedMetricEvents $eventPath
    $malformedMetricResult = New-Result $runId $malformedMetricEvents $malformedMetricContent
    Require-Rejection { Test-AnimatedGlesResultContract -Result $malformedMetricResult -EventsPath $eventPath } 'A string-valued duration was accepted.'

    $malformedSequenceEvents = New-EventSet $runId
    $malformedSequenceEvents[1].sequence = '2'
    $malformedSequenceContent = Write-EventFile $malformedSequenceEvents $eventPath
    $malformedSequenceResult = New-Result $runId $malformedSequenceEvents $malformedSequenceContent
    Require-Rejection { Test-AnimatedGlesResultContract -Result $malformedSequenceResult -EventsPath $eventPath } 'A string-valued event sequence was accepted.'

    $malformed = "{not-json`n"
    [System.IO.File]::WriteAllText($eventPath, $malformed, (New-Object System.Text.UTF8Encoding($false)))
    $malformedResult = $validResult | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $badBytes = [System.Text.Encoding]::UTF8.GetBytes($malformed)
    $malformedResult.artifacts.events_jsonl.sha256 = Get-Sha256 $badBytes
    $malformedResult.artifacts.events_jsonl.bytes = $badBytes.Length
    $malformedResult.artifacts.events_jsonl.event_count = 1
    Require-Rejection { Test-AnimatedGlesResultContract -Result $malformedResult -EventsPath $eventPath } 'Malformed event JSON was accepted.'

    $events = New-EventSet $runId
    $eventContent = Write-EventFile $events $eventPath
    $mismatchEvents = New-EventSet $runId
    $mismatchContent = Write-EventFile $mismatchEvents $eventPath
    $mismatchResult = New-Result $runId $mismatchEvents $mismatchContent -ProfileMismatch
    if (-not (Test-AnimatedGlesResultContract -Result $mismatchResult -EventsPath $eventPath)) {
        throw 'Explicit profile-mismatch invalid-run fixture was rejected.'
    }

    $interruptedEvents = New-EventSet $runId -Interrupted
    $interruptedContent = Write-EventFile $interruptedEvents $eventPath
    $interruptedResult = New-Result $runId $interruptedEvents $interruptedContent -Interrupted
    if (-not (Test-AnimatedGlesResultContract -Result $interruptedResult -EventsPath $eventPath)) {
        throw 'Explicit interrupted-run fixture was rejected.'
    }
    $failedEvents = New-EventSet $runId -Interrupted
    $failedEvents[-2].type = 'renderer_error'
    $failedContent = Write-EventFile $failedEvents $eventPath
    $failedResult = New-Result $runId $failedEvents $failedContent -Interrupted
    $failedResult.run.status = 'failed'
    $failedResult.run.validity = 'failed'
    $failedResult.run.invalidation_reason = 'renderer_initialization_failed'
    if (-not (Test-AnimatedGlesResultContract -Result $failedResult -EventsPath $eventPath)) {
        throw 'Explicit failed-run fixture with a run_interrupted terminal event was rejected.'
    }

    $contradictoryResult = $interruptedResult | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $contradictoryResult.run.validity = 'failed'
    Require-Rejection { Test-AnimatedGlesResultContract -Result $contradictoryResult -EventsPath $eventPath } 'An interrupted run marked failed was accepted.'
    $missingFailureReason = $failedResult | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $missingFailureReason.run.invalidation_reason = $null
    Require-Rejection { Test-AnimatedGlesResultContract -Result $missingFailureReason -EventsPath $eventPath } 'A failed run without a reason was accepted.'

    $tamperedEvents = New-EventSet $runId
    $tamperedContent = Write-EventFile $tamperedEvents $eventPath
    $tamperedResult = New-Result $runId $tamperedEvents $tamperedContent
    [System.IO.File]::AppendAllText($eventPath, ' ')
    Require-Rejection { Test-AnimatedGlesResultContract -Result $tamperedResult -EventsPath $eventPath } 'An event artifact changed after hashing was accepted.'

    Write-Output 'Animated GLES result-contract fixtures passed.'
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
