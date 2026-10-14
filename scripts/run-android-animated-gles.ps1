param(
    [Parameter(Mandatory)][string]$ApkPath,
    [string]$SdkRoot = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }),
    [string]$AvdName = 'FANG-StageA-API36-x86_64',
    [Parameter(Mandatory)][ValidateRange(1, 30)][int]$WarmupSeconds,
    [Parameter(Mandatory)][ValidateRange(2, 120)][int]$DurationSeconds,
    [switch]$Headed,
    [switch]$ReplaceInstalledWorkload,
    [ValidateRange(0, 120)][int]$InterruptAfterSeconds = 0,
    [switch]$DiagnosticMode
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$packageName = 'com.fang.animatedgles'
$activityName = "$packageName/.AnimatedGlesActivity"
$runId = [guid]::NewGuid().ToString()
$serial = 'emulator-5554'
$runDirectory = Join-Path $repoRoot "out\android-animated-gles\runs\$runId"
$adb = Join-Path $SdkRoot 'platform-tools\adb.exe'
$emulator = Join-Path $SdkRoot 'emulator\emulator.exe'
$aapt = Join-Path $SdkRoot 'build-tools\36.0.0\aapt.exe'
$avdRoot = if ($env:ANDROID_AVD_HOME) { $env:ANDROID_AVD_HOME } else { Join-Path $env:USERPROFILE '.android\avd' }
$avdDirectory = Join-Path $avdRoot "$AvdName.avd"
$avdConfigPath = Join-Path $avdDirectory 'config.ini'

foreach ($path in @($ApkPath, $adb, $emulator, $aapt, $avdConfigPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required local runtime/build input is missing: $path"
    }
}
if (Test-Path -LiteralPath $runDirectory) {
    throw "Refusing to overwrite an existing run-artifact directory: $runDirectory"
}
if ($InterruptAfterSeconds -ge ($WarmupSeconds + $DurationSeconds) -and $InterruptAfterSeconds -gt 0) {
    throw 'InterruptAfterSeconds must be shorter than the requested warm-up plus measurement duration.'
}

$buildManifestPath = Join-Path (Split-Path $ApkPath -Parent) 'build-manifest.json'
if (-not (Test-Path -LiteralPath $buildManifestPath -PathType Leaf)) {
    throw "Build provenance file is missing: $buildManifestPath"
}
$buildManifest = Get-Content -LiteralPath $buildManifestPath -Raw | ConvertFrom-Json
$apkHash = (Get-FileHash -LiteralPath $ApkPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($buildManifest.package_name -ne $packageName -or $buildManifest.apk_sha256 -ne $apkHash) {
    throw 'APK package identity or hash does not match its build manifest.'
}
$badging = & $aapt dump badging $ApkPath 2>&1
if ($LASTEXITCODE -ne 0 -or ($badging -join "`n") -notmatch "package: name='$packageName'") {
    throw 'The APK package name could not be verified as the first-party animated GLES package.'
}

$adbDevices = & $adb devices
if ($LASTEXITCODE -ne 0) { throw 'ADB device discovery failed.' }
if (($adbDevices -join "`n") -match "(?m)^$([regex]::Escape($serial))\s") {
    throw "$serial is already in use; refusing to interact with an emulator not started by this script."
}
$existingEmulatorProcess = Get-Process -Name emulator -ErrorAction SilentlyContinue
if ($existingEmulatorProcess) {
    throw 'An Android Emulator process is already running; refusing to select or stop an existing guest.'
}

New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
$runStartedUtc = [DateTime]::UtcNow.ToString('o')
$request = [ordered]@{
    run_id = $runId
    workload = 'fang-animated-gles'
    warmup_seconds = $WarmupSeconds
    duration_seconds = $DurationSeconds
    diagnostic_mode = [bool]$DiagnosticMode
    emulator_window = if ($Headed) { 'headed' } else { 'headless' }
    replace_installed_workload = [bool]$ReplaceInstalledWorkload
    interrupt_after_seconds = $InterruptAfterSeconds
    requested_profile = [ordered]@{
        width_px = 1080
        height_px = 2400
        orientation = 'portrait'
        gles_major = 3
        refresh_hz = 60.0
    }
    run_started_utc = $runStartedUtc
    runtime = $AvdName
}
[System.IO.File]::WriteAllText(
    (Join-Path $runDirectory 'request.json'),
    ($request | ConvertTo-Json -Depth 8),
    $utf8
)

function New-Evidence {
    param([AllowNull()][object]$Value, [string]$Source, [string]$Reason)
    $isAvailable = $null -ne $Value
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { $isAvailable = $false }
    if ($Value -is [array] -and $Value.Count -eq 0) { $isAvailable = $false }
    if ($isAvailable) {
        return [ordered]@{ status = 'collected'; source = $Source; value = $Value }
    }
    return [ordered]@{
        status = 'unavailable'
        source = $Source
        value = $null
        reason = if ($Reason) { $Reason } else { 'The selected local source returned no value.' }
    }
}

function Get-HostEnvironment {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $processors = @(Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue)
    $controllers = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)
    $gpuValues = @($controllers | ForEach-Object {
        [ordered]@{
            name = New-Evidence $_.Name 'Win32_VideoController.Name' 'No video-controller name was returned.'
            driver_version = New-Evidence $_.DriverVersion 'Win32_VideoController.DriverVersion' 'No driver version was returned.'
            driver_date = New-Evidence $_.DriverDate 'Win32_VideoController.DriverDate' 'No driver date was returned.'
        }
    })
    $displayValues = @($controllers | ForEach-Object {
        [ordered]@{
            adapter = New-Evidence $_.Name 'Win32_VideoController.Name' 'No adapter name was returned.'
            width_px = New-Evidence $_.CurrentHorizontalResolution 'Win32_VideoController.CurrentHorizontalResolution' 'The controller did not expose the active width.'
            height_px = New-Evidence $_.CurrentVerticalResolution 'Win32_VideoController.CurrentVerticalResolution' 'The controller did not expose the active height.'
            refresh_hz = New-Evidence $_.CurrentRefreshRate 'Win32_VideoController.CurrentRefreshRate' 'The controller did not expose the active refresh rate.'
        }
    })
    $powerOutput = & powercfg.exe /getactivescheme 2>&1
    $powerText = ($powerOutput | Out-String).Trim()
    $powerValue = if ($LASTEXITCODE -eq 0 -and $powerText) { $powerText } else { $null }
    $hypervisor = Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue
    return [ordered]@{
        windows_build = New-Evidence $(if ($os) { "$($os.Caption) $($os.Version) build $($os.BuildNumber)" } else { $null }) 'Win32_OperatingSystem Caption/Version/BuildNumber' 'Win32_OperatingSystem query failed.'
        os_architecture = New-Evidence $(if ($os) { $os.OSArchitecture } else { $null }) 'Win32_OperatingSystem.OSArchitecture' 'Win32_OperatingSystem query failed.'
        cpu = [ordered]@{
            model = New-Evidence @($processors | ForEach-Object { $_.Name }) 'Win32_Processor.Name' 'No CPU model was returned.'
            physical_cores = New-Evidence @($processors | ForEach-Object { $_.NumberOfCores }) 'Win32_Processor.NumberOfCores' 'No CPU core count was returned.'
            logical_processors = New-Evidence @($processors | ForEach-Object { $_.NumberOfLogicalProcessors }) 'Win32_Processor.NumberOfLogicalProcessors' 'No logical processor count was returned.'
        }
        gpu_adapters = New-Evidence $gpuValues 'Win32_VideoController' 'No video controllers were returned.'
        displays = New-Evidence $displayValues 'Win32_VideoController active mode fields' 'No active display mode fields were returned.'
        active_power_scheme = New-Evidence $powerValue 'powercfg.exe /getactivescheme' 'The active Windows power scheme could not be queried.'
        effective_power_mode = New-Evidence $null 'Windows effective power-mode API' 'Effective power-mode overlays are not queried by this runner.'
        hypervisor_present = New-Evidence $(if ($hypervisor) { [bool]$hypervisor.HypervisorPresent } else { $null }) 'Win32_ComputerSystem.HypervisorPresent' 'Hypervisor presence could not be queried.'
        background_load = New-Evidence $null 'No attributable host-wide workload sampler' 'The runner does not measure concurrent host workload.'
        host_qpc_capture = New-Evidence $null 'Existing independent host-timing benchmark' 'Host QPC samples are not captured or combined with this guest run.'
    }
}

function Invoke-AdbText {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $processInfo = New-Object System.Diagnostics.ProcessStartInfo
    $processInfo.FileName = $adb
    $processInfo.Arguments = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ }
    }) -join ' '
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $processInfo.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $processInfo
    if (-not $process.Start()) { throw "Could not start ADB: $($Arguments -join ' ')" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    if ($process.ExitCode -ne 0) {
        throw "ADB failed ($($process.ExitCode)): $($Arguments -join ' ') $stderr"
    }
    return $stdout
}

function Save-GuestFile {
    param([string]$GuestPath, [string]$LocalPath)
    $text = Invoke-AdbText @('-s', $serial, 'exec-out', 'run-as', $packageName, 'cat', $GuestPath)
    [System.IO.File]::WriteAllText($LocalPath, $text, $utf8)
}

$emulatorProcess = $null
$emulatorStarted = $false
try {
    $emulatorArguments = @(
        '-avd', $AvdName,
        '-no-snapshot',
        '-no-audio',
        '-no-boot-anim',
        '-gpu', 'host',
        '-accel', 'on',
        '-port', '5554'
    )
    if (-not $Headed) { $emulatorArguments += '-no-window' }
    $windowStyle = if ($Headed) { 'Normal' } else { 'Hidden' }
    $emulatorProcess = Start-Process -FilePath $emulator -ArgumentList $emulatorArguments -PassThru -WindowStyle $windowStyle
    $emulatorStarted = $true

    $bootDeadline = [DateTime]::UtcNow.AddMinutes(4)
    $booted = $false
    while ([DateTime]::UtcNow -lt $bootDeadline) {
        if ($emulatorProcess.HasExited) {
            throw "The newly launched emulator exited with code $($emulatorProcess.ExitCode)."
        }
        $devices = & $adb devices
        if ($LASTEXITCODE -eq 0 -and ($devices -join "`n") -match "(?m)^$([regex]::Escape($serial))\s+device$") {
            $state = & $adb -s $serial shell getprop sys.boot_completed 2>$null
            if ($LASTEXITCODE -eq 0 -and ($state -join '').Trim() -eq '1') {
                $booted = $true
                break
            }
        }
        Start-Sleep -Seconds 2
    }
    if (-not $booted) {
        throw 'The new reference AVD did not reach sys.boot_completed within four minutes.'
    }

    $installedPath = & $adb -s $serial shell pm path $packageName 2>$null
    $workloadAlreadyInstalled = $LASTEXITCODE -eq 0 -and ($installedPath -join '').Trim()
    if ($workloadAlreadyInstalled -and -not $ReplaceInstalledWorkload) {
        throw 'The isolated AVD already contains the workload package; refusing to overwrite app data.'
    }
    if ($workloadAlreadyInstalled -and $ReplaceInstalledWorkload) {
        Write-Output 'Replacing the existing first-party workload APK in the isolated AVD; package signing-key mismatch will be rejected by Android.'
    }
    $null = & $adb -s $serial shell input keyevent 224
    if ($LASTEXITCODE -ne 0) { throw 'Could not wake the isolated reference AVD display.' }
    Start-Sleep -Seconds 1
    $null = & $adb -s $serial shell input keyevent 82
    if ($LASTEXITCODE -ne 0) { throw 'Could not request dismissal of the isolated reference AVD keyguard.' }
    Start-Sleep -Seconds 1
    $displayPowerState = Invoke-AdbText @('-s', $serial, 'shell', 'dumpsys', 'power')
    [System.IO.File]::WriteAllText(
        (Join-Path $runDirectory 'guest-display-power-before.txt'),
        $displayPowerState,
        $utf8
    )
    $beforeState = Invoke-AdbText @('-s', $serial, 'shell', 'dumpsys', 'battery')
    [System.IO.File]::WriteAllText((Join-Path $runDirectory 'guest-state-before.txt'), $beforeState, $utf8)
    $installArguments = @('install', '-t')
    if ($workloadAlreadyInstalled) { $installArguments += '-r' }
    $installArguments += $ApkPath
    $installOutput = & $adb -s $serial @installArguments 2>&1
    if ($LASTEXITCODE -ne 0 -or ($installOutput -join "`n") -notmatch 'Success') {
        throw "First-party APK installation failed: $($installOutput -join ' ')"
    }

    $startUtc = [DateTime]::UtcNow.ToString('o')
    $startArguments = @(
        '-s', $serial, 'shell', 'am', 'start', '-W', '-n', $activityName,
        '--es', 'runId', $runId,
        '--es', 'buildId', [string]$buildManifest.build_id,
        '--ei', 'warmupSeconds', [string]$WarmupSeconds,
        '--ei', 'durationSeconds', [string]$DurationSeconds,
        '--ez', 'diagnosticMode', ([bool]$DiagnosticMode).ToString().ToLowerInvariant()
    )
    $startOutput = & $adb @startArguments 2>&1
    if ($LASTEXITCODE -ne 0 -or ($startOutput -join "`n") -match 'Error:') {
        throw "Could not launch the first-party validation Activity: $($startOutput -join ' ')"
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $runDirectory 'activity-start.txt'),
        ($startOutput | Out-String),
        $utf8
    )

    if ($InterruptAfterSeconds -gt 0) {
        Start-Sleep -Seconds $InterruptAfterSeconds
        $null = & $adb -s $serial shell input keyevent 3
        if ($LASTEXITCODE -ne 0) { throw 'Could not inject the requested HOME interruption into the isolated reference AVD.' }
    }

    $resultGuestPath = "files/runs/$runId/guest-result.json"
    $resultDeadline = [DateTime]::UtcNow.AddSeconds($WarmupSeconds + $DurationSeconds + 90)
    $resultExists = $false
    while ([DateTime]::UtcNow -lt $resultDeadline) {
        if ($emulatorProcess.HasExited) {
            throw 'The reference emulator exited while the first-party run was in progress.'
        }
        & $adb -s $serial shell run-as $packageName test -f $resultGuestPath 2>$null
        if ($LASTEXITCODE -eq 0) {
            $resultExists = $true
            break
        }
        Start-Sleep -Seconds 2
    }
    if (-not $resultExists) {
        $logcat = & $adb -s $serial logcat -d -v threadtime -s FangAnimatedGles:E AndroidRuntime:E 2>&1
        [System.IO.File]::WriteAllText((Join-Path $runDirectory 'failure-logcat.txt'), ($logcat | Out-String), $utf8)
        throw 'No guest result was produced before the run deadline; the attempt remains failed and un-repaired.'
    }

    $eventsPath = Join-Path $runDirectory 'events.jsonl'
    $guestResultPath = Join-Path $runDirectory 'guest-result.json'
    Save-GuestFile "files/runs/$runId/events.jsonl" $eventsPath
    Save-GuestFile $resultGuestPath $guestResultPath
    $guestResult = Get-Content -LiteralPath $guestResultPath -Raw | ConvertFrom-Json
    if ($guestResult.run_id -ne $runId -or $guestResult.workload.build_id -ne $buildManifest.build_id) {
        throw 'Guest result identity does not match the requested run and APK build.'
    }

    $afterState = Invoke-AdbText @('-s', $serial, 'shell', 'dumpsys', 'battery')
    [System.IO.File]::WriteAllText((Join-Path $runDirectory 'guest-state-after.txt'), $afterState, $utf8)
    $logcatOutput = & $adb -s $serial logcat -d -v threadtime -s FangAnimatedGles:I FangAnimatedGles:E AndroidRuntime:E 2>&1
    [System.IO.File]::WriteAllText((Join-Path $runDirectory 'logcat.txt'), ($logcatOutput | Out-String), $utf8)

    $osVersion = Invoke-AdbText @('-s', $serial, 'shell', 'getprop', 'ro.build.version.release')
    $apiLevel = Invoke-AdbText @('-s', $serial, 'shell', 'getprop', 'ro.build.version.sdk')
    $abi = Invoke-AdbText @('-s', $serial, 'shell', 'getprop', 'ro.product.cpu.abi')
    $fingerprint = Invoke-AdbText @('-s', $serial, 'shell', 'getprop', 'ro.build.fingerprint')
    $displaySize = Invoke-AdbText @('-s', $serial, 'shell', 'wm', 'size')
    $displayDensity = Invoke-AdbText @('-s', $serial, 'shell', 'wm', 'density')
    $brightness = Invoke-AdbText @('-s', $serial, 'shell', 'settings', 'get', 'system', 'screen_brightness')
    $brightnessMode = Invoke-AdbText @('-s', $serial, 'shell', 'settings', 'get', 'system', 'screen_brightness_mode')
    $lowPower = Invoke-AdbText @('-s', $serial, 'shell', 'settings', 'get', 'global', 'low_power')
    $avdProperties = @{}
    foreach ($line in Get-Content -LiteralPath $avdConfigPath) {
        if ($line -match '^([^=]+)=(.*)$') { $avdProperties[$Matches[1]] = $Matches[2] }
    }
    $imageRelative = [string]$avdProperties['image.sysdir.1']
    $imageSourcePath = Join-Path $SdkRoot ($imageRelative -replace '/', '\')
    $imageProperties = @{}
    $imageSourceProperties = Join-Path $imageSourcePath 'source.properties'
    if (Test-Path -LiteralPath $imageSourceProperties) {
        foreach ($line in Get-Content -LiteralPath $imageSourceProperties) {
            if ($line -match '^([^=]+)=(.*)$') { $imageProperties[$Matches[1]] = $Matches[2] }
        }
    }
    $hostEnvironment = Get-HostEnvironment
    $accelOutput = & $emulator -accel-check 2>&1
    $accelEvidence = New-Evidence $(if ($LASTEXITCODE -eq 0) { ($accelOutput | Out-String).Trim() } else { $null }) 'emulator -accel-check' 'The Emulator acceleration check failed.'
    $versionOutput = & $emulator -version 2>&1
    $emulatorVersion = New-Evidence $(if ($LASTEXITCODE -eq 0) { [string]$versionOutput[0] } else { $null }) 'emulator -version' 'The emulator version query failed.'
    $sourceRevision = (& git -C $repoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not record source Git revision.' }
    $gitStatus = & git -C $repoRoot status --porcelain
    if ($LASTEXITCODE -ne 0) { throw 'Could not record source worktree status.' }
    $eventsBytes = [System.IO.File]::ReadAllBytes($eventsPath)
    $eventsHash = [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA256]::Create().ComputeHash($eventsBytes)).Replace('-', '').ToLowerInvariant()
    $guestResultBytes = [System.IO.File]::ReadAllBytes($guestResultPath)
    $guestResultHash = [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA256]::Create().ComputeHash($guestResultBytes)).Replace('-', '').ToLowerInvariant()

    $guestResult | Add-Member -NotePropertyName provenance -NotePropertyValue ([ordered]@{
        capture_started_utc = $startUtc
        host = $hostEnvironment
        runtime = [ordered]@{
            emulator_version = $emulatorVersion
            adb_version = New-Evidence ((& $adb version | Out-String).Trim()) 'adb version' 'ADB version could not be queried.'
            emulator_accel_check = $accelEvidence
            active_accelerator = New-Evidence $null 'Android Emulator active accelerator' 'The active accelerator is not independently exposed by the available process evidence.'
            requested_acceleration = New-Evidence 'on' 'emulator launch argument -accel on' 'The launch argument was not recorded.'
            requested_graphics_mode = New-Evidence 'host' 'emulator launch argument -gpu host' 'The launch argument was not recorded.'
            emulator_window = New-Evidence $request.emulator_window 'runner launch arguments' 'The requested emulator window mode was not recorded.'
            replaced_existing_first_party_package = New-Evidence $request.replace_installed_workload 'runner package-update option' 'The package replacement decision was not recorded.'
            injected_home_interruption_after_seconds = New-Evidence $request.interrupt_after_seconds 'runner-controlled ADB HOME key event' 'No controlled lifecycle interruption was requested.'
        }
        guest = [ordered]@{
            adb_serial = New-Evidence $serial 'adb devices' 'No connected ADB serial was found.'
            avd_name = New-Evidence $AvdName 'new isolated AVD configuration' 'No AVD name was recorded.'
            android_release = New-Evidence $osVersion.Trim() 'getprop ro.build.version.release' 'Android release property was unavailable.'
            android_api = New-Evidence ([int]$apiLevel.Trim()) 'getprop ro.build.version.sdk' 'Android API property was unavailable.'
            abi = New-Evidence $abi.Trim() 'getprop ro.product.cpu.abi' 'Guest ABI property was unavailable.'
            build_fingerprint = New-Evidence $fingerprint.Trim() 'getprop ro.build.fingerprint' 'Guest build fingerprint was unavailable.'
            display_size = New-Evidence $displaySize.Trim() 'adb shell wm size' 'Guest display size query was unavailable.'
            display_density = New-Evidence $displayDensity.Trim() 'adb shell wm density' 'Guest density query was unavailable.'
            display_brightness = New-Evidence $brightness.Trim() 'settings get system screen_brightness' 'Guest display brightness was unavailable.'
            adaptive_brightness_mode = New-Evidence $brightnessMode.Trim() 'settings get system screen_brightness_mode' 'Adaptive-brightness setting was unavailable.'
            low_power_mode = New-Evidence $lowPower.Trim() 'settings get global low_power' 'Guest low-power setting was unavailable.'
            avd_config = New-Evidence $avdProperties 'AVD config.ini before emulator launch' 'AVD config was unavailable.'
            system_image = New-Evidence ([ordered]@{
                package_id = New-Evidence 'system-images;android-36;default;x86_64' 'AVD image.sysdir.1 and installed SDK image path' 'System image package identity was not resolved.'
                package_revision = New-Evidence $imageProperties['Pkg.Revision'] 'system image source.properties' 'System image revision was unavailable.'
                api_level = New-Evidence $imageProperties['AndroidVersion.ApiLevel'] 'system image source.properties' 'System image API metadata was unavailable.'
                abi = New-Evidence $imageProperties['SystemImage.Abi'] 'system image source.properties' 'System image ABI metadata was unavailable.'
                tag = New-Evidence $imageProperties['SystemImage.TagId'] 'system image source.properties' 'System image tag was unavailable.'
            }) 'system image source.properties' 'System image metadata was unavailable.'
            guest_state_before_sha256 = New-Evidence (Get-FileHash -LiteralPath (Join-Path $runDirectory 'guest-state-before.txt') -Algorithm SHA256).Hash.ToLowerInvariant() 'Get-FileHash guest-state-before.txt' 'Pre-run guest-state capture was unavailable.'
            guest_state_after_sha256 = New-Evidence (Get-FileHash -LiteralPath (Join-Path $runDirectory 'guest-state-after.txt') -Algorithm SHA256).Hash.ToLowerInvariant() 'Get-FileHash guest-state-after.txt' 'Post-run guest-state capture was unavailable.'
            display_power_before_sha256 = New-Evidence (Get-FileHash -LiteralPath (Join-Path $runDirectory 'guest-display-power-before.txt') -Algorithm SHA256).Hash.ToLowerInvariant() 'Get-FileHash guest-display-power-before.txt' 'Pre-run display power-state capture was unavailable.'
            observed_profile = $guestResult.observed_profile
            thermal_state = $guestResult.thermal_state
        }
        source = [ordered]@{
            git_commit = New-Evidence $sourceRevision 'git rev-parse HEAD' 'Git revision could not be queried.'
            worktree_dirty = New-Evidence ([bool]$gitStatus.Count) 'git status --porcelain' 'Git status could not be queried.'
            apk_sha256 = New-Evidence $apkHash 'Get-FileHash APK' 'APK hash was unavailable.'
            signer_certificate_sha256 = New-Evidence $buildManifest.signing_certificate_sha256 'APK build manifest signature verification' 'Signer fingerprint was unavailable.'
            build_manifest_sha256 = New-Evidence (Get-FileHash -LiteralPath $buildManifestPath -Algorithm SHA256).Hash.ToLowerInvariant() 'Get-FileHash build-manifest.json' 'Build-manifest hash was unavailable.'
        }
    })
    $guestResult.artifacts | Add-Member -NotePropertyName guest_result_json -NotePropertyValue ([ordered]@{
        path = 'guest-result.json'
        sha256 = $guestResultHash
        bytes = $guestResultBytes.Length
    })
    $guestResult.artifacts | Add-Member -NotePropertyName logcat -NotePropertyValue ([ordered]@{
        path = 'logcat.txt'
        sha256 = (Get-FileHash -LiteralPath (Join-Path $runDirectory 'logcat.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
    })
    $resultPath = Join-Path $runDirectory 'result.json'
    [System.IO.File]::WriteAllText($resultPath, ($guestResult | ConvertTo-Json -Depth 30), $utf8)
    Import-Module (Join-Path $repoRoot 'scripts\AnimatedGlesResultContract.psm1') -Force
    $validatedResult = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    Test-AnimatedGlesResultContract -Result $validatedResult -EventsPath $eventsPath | Out-Null
    Write-Output "Run ID: $runId"
    Write-Output "Run validity: $($validatedResult.run.validity)"
    Write-Output "Profile conforming: $($validatedResult.run.profile_conforming)"
    Write-Output "Event samples: $($validatedResult.metrics.guest_update_duration_ns.sample_count)"
    Write-Output "Result: $resultPath"
} finally {
    if ($emulatorStarted) {
        & $adb -s $serial emu kill 2>$null | Out-Null
        if ($emulatorProcess -and -not $emulatorProcess.HasExited) {
            try { $emulatorProcess.WaitForExit(15000) | Out-Null } catch { }
        }
        if ($emulatorProcess -and -not $emulatorProcess.HasExited) {
            Stop-Process -Id $emulatorProcess.Id -Force
        }
    }
}
