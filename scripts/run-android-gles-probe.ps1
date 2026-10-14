param(
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$Serial = 'emulator-5554',
    [ValidateRange(1, 64)]
    [int]$Iterations = 8,
    [ValidateRange(5, 120)]
    [int]$DurationSeconds = 30,
    [ValidateRange(1, 30)]
    [int]$WarmupSeconds = 5
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$sdkRoot = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
$apk = Join-Path $repoRoot 'out\android-gles-probe\fang-android-gles-probe.apk'
$emulator = Join-Path $sdkRoot 'emulator\emulator.exe'
$packageName = 'com.fang.perfprobe'
$activityName = "$packageName/.GlesProbeActivity"
$outputDirectory = Join-Path $repoRoot 'out\android-gles-probe\runs'
Import-Module (Join-Path $PSScriptRoot 'GlesProbeResultContract.psm1') -Force

function Get-AdbEvidence {
    param([string[]]$Arguments, [string]$Source, [string]$UnavailableReason)
    try {
        $output = @(& $adb -s $Serial @Arguments 2>$null)
        $exitCode = $LASTEXITCODE
        $text = (@($output | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ }) -join '; ')
        if ($exitCode -ne 0 -or [string]::IsNullOrWhiteSpace($text)) {
            return New-GlesEvidence -Value $null -Source $Source -UnavailableReason $UnavailableReason
        }
        return New-GlesEvidence -Value $text -Source $Source
    } catch {
        return New-GlesEvidence -Value $null -Source $Source -UnavailableReason $_.Exception.Message
    }
}

function Get-EmulatorArgumentEvidence {
    param(
        [string]$CommandLine,
        [string]$ArgumentName,
        [string]$UnavailableReason = 'No matching Android Emulator process command line was available.'
    )
    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return New-GlesEvidence -Value $null -Source 'Win32_Process.CommandLine' -UnavailableReason $UnavailableReason
    }
    $pattern = '(?:^|\s)' + [regex]::Escape($ArgumentName) + '\s+(?:"([^"]+)"|(\S+))'
    $match = [regex]::Match($CommandLine, $pattern)
    if (-not $match.Success) {
        return New-GlesEvidence -Value $null -Source 'Win32_Process.CommandLine' -UnavailableReason "The active command line did not expose $ArgumentName; default selection is not inferred."
    }
    $value = if ($match.Groups[1].Success) { $match.Groups[1].Value } else { $match.Groups[2].Value }
    New-GlesEvidence -Value $value -Source 'Win32_Process.CommandLine (requested setting, not active-backend proof)'
}

if (-not (Test-Path $adb)) {
    throw "ADB was not found: $adb"
}
if (-not (Test-Path $apk)) {
    throw 'Build the probe first with ./scripts/build-android-gles-probe.ps1.'
}

& $adb start-server
if ($LASTEXITCODE -ne 0) {
    throw 'Could not start ADB.'
}
$deviceState = & $adb -s $Serial get-state
if ($LASTEXITCODE -ne 0 -or $deviceState -ne 'device') {
    throw "ADB device $Serial is not online."
}
$bootCompleted = & $adb -s $Serial shell getprop sys.boot_completed
if ($LASTEXITCODE -ne 0 -or $bootCompleted -ne '1') {
    throw "Android guest $Serial has not completed boot."
}

& $adb -s $Serial install -r $apk
if ($LASTEXITCODE -ne 0) {
    throw 'Probe APK installation failed.'
}
& $adb -s $Serial shell am force-stop $packageName

$metadataWatch = [System.Diagnostics.Stopwatch]::StartNew()
$osEvidence = $null
$cpuEvidence = $null
$gpuEvidence = $null
$displayEvidence = $null
$hypervisorEvidence = $null
try {
    $osInfo = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    $osEvidence = New-GlesEvidence -Value ([ordered]@{
        caption = $osInfo.Caption
        version = $osInfo.Version
        build = $osInfo.BuildNumber
    }) -Source 'Win32_OperatingSystem'
    $architectureEvidence = New-GlesEvidence -Value $osInfo.OSArchitecture -Source 'Win32_OperatingSystem.OSArchitecture'
} catch {
    $osEvidence = New-GlesEvidence -Value $null -Source 'Win32_OperatingSystem' -UnavailableReason $_.Exception.Message
    $architectureEvidence = New-GlesEvidence -Value $null -Source 'Win32_OperatingSystem.OSArchitecture' -UnavailableReason $_.Exception.Message
}
try {
    $cpuInfo = @(Get-CimInstance Win32_Processor -ErrorAction Stop | ForEach-Object {
        [ordered]@{ name = $_.Name; cores = $_.NumberOfCores; logical_processors = $_.NumberOfLogicalProcessors }
    })
    $cpuEvidence = New-GlesEvidence -Value $cpuInfo -Source 'Win32_Processor'
} catch {
    $cpuEvidence = New-GlesEvidence -Value $null -Source 'Win32_Processor' -UnavailableReason $_.Exception.Message
}
try {
    $gpuInfo = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
        [ordered]@{
            name = New-GlesEvidence -Value $_.Name -Source 'Win32_VideoController.Name'
            driver_version = New-GlesEvidence -Value $_.DriverVersion -Source 'Win32_VideoController.DriverVersion' -UnavailableReason 'Windows did not expose a driver version for this adapter.'
        }
    })
    $gpuEvidence = New-GlesEvidence -Value $gpuInfo -Source 'Win32_VideoController'
} catch {
    $gpuEvidence = New-GlesEvidence -Value $null -Source 'Win32_VideoController' -UnavailableReason $_.Exception.Message
}
try {
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    $displayInfo = @([System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
        [ordered]@{
            device_name = $_.DeviceName
            width = $_.Bounds.Width
            height = $_.Bounds.Height
            primary = $_.Primary
        }
    })
    $displayEvidence = New-GlesEvidence -Value $displayInfo -Source 'System.Windows.Forms.Screen'
} catch {
    $displayEvidence = New-GlesEvidence -Value $null -Source 'System.Windows.Forms.Screen' -UnavailableReason $_.Exception.Message
}

$powerCfg = Join-Path $env:SystemRoot 'System32\powercfg.exe'
if (Test-Path $powerCfg) {
    $powerOutput = @(& $powerCfg /getactivescheme 2>$null)
    if ($LASTEXITCODE -eq 0 -and $powerOutput.Count -gt 0) {
        $powerEvidence = New-GlesEvidence -Value (($powerOutput -join ' ').Trim()) -Source 'powercfg /getactivescheme'
    } else {
        $powerEvidence = New-GlesEvidence -Value $null -Source 'powercfg /getactivescheme' -UnavailableReason 'powercfg did not return the active plan.'
    }
} else {
    $powerEvidence = New-GlesEvidence -Value $null -Source 'powercfg /getactivescheme' -UnavailableReason 'powercfg.exe was not found.'
}

try {
    $computerSystem = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
    if ($null -ne $computerSystem.HypervisorPresent) {
        $hypervisorEvidence = New-GlesEvidence -Value ([bool]$computerSystem.HypervisorPresent) -Source 'Win32_ComputerSystem.HypervisorPresent'
    } else {
        $hypervisorEvidence = New-GlesEvidence -Value $null -Source 'Win32_ComputerSystem.HypervisorPresent' -UnavailableReason 'The WMI provider returned no hypervisor value.'
    }
    if ($null -ne $computerSystem.TotalPhysicalMemory -and [uint64]$computerSystem.TotalPhysicalMemory -gt 0) {
        $memoryEvidence = New-GlesEvidence -Value ([uint64]$computerSystem.TotalPhysicalMemory) -Source 'Win32_ComputerSystem.TotalPhysicalMemory'
    } else {
        $memoryEvidence = New-GlesEvidence -Value $null -Source 'Win32_ComputerSystem.TotalPhysicalMemory' -UnavailableReason 'The WMI provider returned no positive physical-memory value.'
    }
} catch {
    $hypervisorEvidence = New-GlesEvidence -Value $null -Source 'Win32_ComputerSystem.HypervisorPresent' -UnavailableReason $_.Exception.Message
    $memoryEvidence = New-GlesEvidence -Value $null -Source 'Win32_ComputerSystem.TotalPhysicalMemory' -UnavailableReason $_.Exception.Message
}

$emulatorVersionEvidence = $null
$accelCheckEvidence = $null
if (Test-Path $emulator) {
    $versionOutput = @(& $emulator -version 2>$null)
    $versionMatch = [regex]::Match(($versionOutput -join ' '), '(?i)Android emulator version [0-9.]+ \(build_id [^)]+\)')
    if ($LASTEXITCODE -eq 0 -and $versionMatch.Success) {
        $emulatorVersionEvidence = New-GlesEvidence -Value $versionMatch.Value -Source 'emulator.exe -version'
    } else {
        $emulatorVersionEvidence = New-GlesEvidence -Value $null -Source 'emulator.exe -version' -UnavailableReason 'The emulator version command failed.'
    }
    $accelOutput = @(& $emulator -accel-check 2>$null)
    $accelLines = @($accelOutput | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ -match '^WHPX\(' })
    if ($accelLines.Count -gt 0) {
        $accelCheckEvidence = New-GlesEvidence -Value ($accelLines -join '; ') -Source 'emulator.exe -accel-check (capability check only)'
    } else {
        $accelCheckEvidence = New-GlesEvidence -Value $null -Source 'emulator.exe -accel-check' -UnavailableReason 'The emulator returned no acceleration check output.'
    }
} else {
    $emulatorVersionEvidence = New-GlesEvidence -Value $null -Source 'emulator.exe -version' -UnavailableReason "Emulator executable not found at $emulator."
    $accelCheckEvidence = New-GlesEvidence -Value $null -Source 'emulator.exe -accel-check' -UnavailableReason 'Emulator executable was not found.'
}

$avdNameEvidence = Get-AdbEvidence -Arguments @('shell', 'getprop', 'ro.boot.qemu.avd_name') -Source 'adb shell getprop ro.boot.qemu.avd_name' -UnavailableReason 'Guest AVD name property was empty or unavailable.'
$androidReleaseEvidence = Get-AdbEvidence -Arguments @('shell', 'getprop', 'ro.build.version.release') -Source 'adb shell getprop ro.build.version.release' -UnavailableReason 'Guest Android release was unavailable.'
$androidApiEvidence = Get-AdbEvidence -Arguments @('shell', 'getprop', 'ro.build.version.sdk') -Source 'adb shell getprop ro.build.version.sdk' -UnavailableReason 'Guest Android API was unavailable.'
$abiEvidence = Get-AdbEvidence -Arguments @('shell', 'getprop', 'ro.product.cpu.abi') -Source 'adb shell getprop ro.product.cpu.abi' -UnavailableReason 'Guest ABI was unavailable.'
$guestSizeEvidence = Get-AdbEvidence -Arguments @('shell', 'wm', 'size') -Source 'adb shell wm size' -UnavailableReason 'Guest display size was unavailable.'
$guestDensityEvidence = Get-AdbEvidence -Arguments @('shell', 'wm', 'density') -Source 'adb shell wm density' -UnavailableReason 'Guest display density was unavailable.'
$avdConfigEvidence = New-GlesEvidence -Value $null -Source 'AVD config.ini allowlist' -UnavailableReason 'The AVD config file was not found or had no allowlisted settings.'
if ($avdNameEvidence.status -eq 'collected') {
    $avdConfigRoots = @()
    if ($env:ANDROID_AVD_HOME) { $avdConfigRoots += $env:ANDROID_AVD_HOME }
    $avdConfigRoots += (Join-Path $env:USERPROFILE '.android\avd')
    foreach ($avdRoot in ($avdConfigRoots | Select-Object -Unique)) {
        $candidate = Join-Path $avdRoot "$($avdNameEvidence.value).avd\config.ini"
        if (-not (Test-Path $candidate)) { continue }
        $properties = @{}
        foreach ($line in Get-Content $candidate) {
            if ($line -match '^\s*([^#=]+?)\s*=\s*(.*?)\s*$') {
                $properties[$Matches[1].Trim()] = $Matches[2]
            }
        }
        $allowlist = @('image.sysdir.1', 'hw.cpu.arch', 'hw.cpu.ncore', 'hw.ramSize', 'hw.gpu.enabled', 'hw.gpu.mode', 'hw.lcd.width', 'hw.lcd.height', 'hw.lcd.density', 'tag.id', 'target', 'disk.dataPartition.size', 'runtime.network.speed', 'runtime.network.latency')
        $selectedProperties = [ordered]@{}
        foreach ($key in $allowlist) {
            if ($properties.ContainsKey($key)) { $selectedProperties[$key] = $properties[$key] }
        }
        if ($selectedProperties.Count -gt 0) {
            $avdConfigEvidence = New-GlesEvidence -Value $selectedProperties -Source "$candidate (allowlisted settings; may be overridden by command-line flags)"
        }
        $imageSysdir = $properties['image.sysdir.1']
        if (-not [string]::IsNullOrWhiteSpace($imageSysdir)) {
            $imageDirectory = [System.IO.Path]::GetFullPath((Join-Path $sdkRoot $imageSysdir))
            $sourcePropertiesPath = Join-Path $imageDirectory 'source.properties'
            if (Test-Path $sourcePropertiesPath) {
                $imageProperties = @{}
                foreach ($imageLine in Get-Content $sourcePropertiesPath) {
                    if ($imageLine -match '^\s*([^#=]+?)\s*=\s*(.*?)\s*$') {
                        $imageProperties[$Matches[1].Trim()] = $Matches[2]
                    }
                }
                $imageApi = $imageProperties['AndroidVersion.ApiLevel']
                $imageTag = $imageProperties['SystemImage.TagId']
                $imageAbi = $imageProperties['SystemImage.Abi']
                $imagePackage = $null
                if ($imageApi -and $imageTag -and $imageAbi) {
                    $imagePackage = "system-images;android-$imageApi;$imageTag;$imageAbi"
                }
                $systemImageEvidence = New-GlesEvidence -Value ([ordered]@{
                    package_id = New-GlesEvidence -Value $imagePackage -Source 'AVD image.sysdir.1 + system image source.properties' -UnavailableReason 'Package ID requires API, tag, and ABI from source.properties.'
                    package_revision = New-GlesEvidence -Value $imageProperties['Pkg.Revision'] -Source 'system image source.properties Pkg.Revision'
                    api_level = New-GlesEvidence -Value $imageApi -Source 'system image source.properties AndroidVersion.ApiLevel'
                    abi = New-GlesEvidence -Value $imageAbi -Source 'system image source.properties SystemImage.Abi'
                    tag = New-GlesEvidence -Value $imageTag -Source 'system image source.properties SystemImage.TagId'
                }) -Source $sourcePropertiesPath
            } else {
                $systemImageEvidence = New-GlesEvidence -Value $null -Source $sourcePropertiesPath -UnavailableReason 'System image source.properties was not found.'
            }
        } else {
            $systemImageEvidence = New-GlesEvidence -Value $null -Source 'AVD config.ini image.sysdir.1' -UnavailableReason 'The AVD config did not expose a system image directory.'
        }
        break
    }
}
if ($null -eq $systemImageEvidence) {
    $systemImageEvidence = New-GlesEvidence -Value $null -Source 'AVD config.ini image.sysdir.1' -UnavailableReason 'The AVD config was not found or its image path could not be read.'
}

$emulatorProcess = $null
$emulatorCommandLine = $null
$emulatorProcessQueryError = $null
try {
    $emulatorProcesses = @(Get-CimInstance Win32_Process -Filter "Name='emulator.exe'" -ErrorAction Stop)
    if ($emulatorProcesses.Count -eq 1) {
        $emulatorProcess = $emulatorProcesses[0]
    } elseif ($avdNameEvidence.status -eq 'collected') {
        $matches = @($emulatorProcesses | Where-Object { $_.CommandLine -match [regex]::Escape([string]$avdNameEvidence.value) })
        if ($matches.Count -eq 1) { $emulatorProcess = $matches[0] }
    }
    if ($null -ne $emulatorProcess) { $emulatorCommandLine = $emulatorProcess.CommandLine }
} catch {
    $emulatorCommandLine = $null
    $emulatorProcessQueryError = $_.Exception.Message
}
$processCommandLineUnavailableReason = if ($emulatorProcessQueryError) {
    "Could not inspect Android Emulator process command lines: $emulatorProcessQueryError"
} else {
    'No matching Android Emulator process command line was available.'
}
$requestedAccelerationEvidence = Get-EmulatorArgumentEvidence -CommandLine $emulatorCommandLine -ArgumentName '-accel' -UnavailableReason $processCommandLineUnavailableReason
$requestedGraphicsModeEvidence = Get-EmulatorArgumentEvidence -CommandLine $emulatorCommandLine -ArgumentName '-gpu' -UnavailableReason $processCommandLineUnavailableReason
$activeAcceleratorEvidence = New-GlesEvidence -Value $null -Source 'Android Emulator active accelerator' -UnavailableReason 'The runner can observe requested flags and WHPX availability, but cannot independently verify which accelerator the running VM used.'

$gitCommitOutput = @(& git -C $repoRoot rev-parse HEAD 2>$null)
if ($LASTEXITCODE -eq 0 -and $gitCommitOutput.Count -gt 0) {
    $gitCommitEvidence = New-GlesEvidence -Value $gitCommitOutput[0].Trim() -Source 'git rev-parse HEAD'
    $gitStatusOutput = @(& git -C $repoRoot status --porcelain 2>$null)
    if ($LASTEXITCODE -eq 0) {
        $worktreeDirtyEvidence = New-GlesEvidence -Value ($gitStatusOutput.Count -gt 0) -Source 'git status --porcelain (dirty flag only)'
    } else {
        $worktreeDirtyEvidence = New-GlesEvidence -Value $null -Source 'git status --porcelain' -UnavailableReason 'Git status could not be read.'
    }
} else {
    $gitCommitEvidence = New-GlesEvidence -Value $null -Source 'git rev-parse HEAD' -UnavailableReason 'Git commit could not be read.'
    $worktreeDirtyEvidence = New-GlesEvidence -Value $null -Source 'git status --porcelain' -UnavailableReason 'Git status could not be read.'
}
$apkHash = Get-FileHash -LiteralPath $apk -Algorithm SHA256
$apkHashEvidence = New-GlesEvidence -Value $apkHash.Hash -Source 'Get-FileHash SHA256'
$metadataCollectionDurationMs = $metadataWatch.ElapsedMilliseconds
$runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), ([Guid]::NewGuid().ToString('N').Substring(0, 8))

$logcatInfo = New-Object System.Diagnostics.ProcessStartInfo
$logcatInfo.FileName = $adb
$logcatInfo.Arguments = "-s $Serial logcat -v raw -s FangGlesProbe:I"
$logcatInfo.UseShellExecute = $false
$logcatInfo.RedirectStandardOutput = $true
$logcatInfo.CreateNoWindow = $true
$logcat = [System.Diagnostics.Process]::Start($logcatInfo)

try {
    $runStartedUtc = [DateTimeOffset]::UtcNow.ToString('o')
    & $adb -s $Serial shell am start -W -n $activityName `
        --es runId $runId `
        --ei durationSeconds $DurationSeconds `
        --ei warmupSeconds $WarmupSeconds `
        --ei iterations $Iterations
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not launch the probe Activity.'
    }

    $timeoutAt = [DateTime]::UtcNow.AddSeconds($DurationSeconds + $WarmupSeconds + 120)
    $lineTask = $logcat.StandardOutput.ReadLineAsync()
    $summaryJson = $null
    while ([DateTime]::UtcNow -lt $timeoutAt) {
        $remainingMs = [int][Math]::Min(1000, [Math]::Max(1, ($timeoutAt - [DateTime]::UtcNow).TotalMilliseconds))
        if ($lineTask.Wait($remainingMs)) {
            $line = $lineTask.Result
            if ($null -eq $line) {
                break
            }
            $runSummary = Get-GlesProbeLogSummary -Line $line -RunId $runId
            if ($runSummary) {
                $summaryJson = $runSummary
                break
            }
            if ($line -match '^START\s') {
                Write-Output $line
            }
            $lineTask = $logcat.StandardOutput.ReadLineAsync()
        } elseif ($logcat.HasExited) {
            break
        }
    }

    if (-not $summaryJson) {
        throw 'Probe did not emit a RESULT line before the timeout.'
    }

    $csvLines = @(& $adb -s $Serial exec-out run-as $packageName cat files/frames.csv)
    if ($LASTEXITCODE -ne 0 -or $csvLines.Count -lt 2) {
        throw 'Could not retrieve raw frame samples from the guest.'
    }

    $summary = $summaryJson | ConvertFrom-Json
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $summaryPath = Join-Path $outputDirectory "gles-$runId.json"
    $samplesPath = Join-Path $outputDirectory "gles-$runId.csv"
    $manifestPath = Join-Path $outputDirectory "gles-$runId.manifest.json"

    $manifest = [ordered]@{
        schema_version = 1
        kind = 'fang-android-gles-probe-run'
        run_id = $runId
        run_started_utc = $runStartedUtc
        collection = [ordered]@{
            duration_ms = $metadataCollectionDurationMs
            completed_before_activity_start = $true
            timing_note = 'Host/guest metadata queries complete before am start; the GLES Activity timer begins in onCreate.'
        }
        host = [ordered]@{
            windows_build = $osEvidence
            os_architecture = $architectureEvidence
            cpu = $cpuEvidence
            gpu_adapters = $gpuEvidence
            displays = $displayEvidence
            physical_memory_bytes = $memoryEvidence
            active_power_scheme = $powerEvidence
            effective_power_mode = New-GlesEvidence -Value $null -Source 'Windows power-mode overlay' -UnavailableReason 'powercfg reports the active power scheme, not the effective Windows power-mode slider or transient thermal policy.'
        }
        runtime = [ordered]@{
            emulator_version = $emulatorVersionEvidence
        }
        virtualization = [ordered]@{
            windows_hypervisor_present = $hypervisorEvidence
            emulator_accel_check = $accelCheckEvidence
            active_accelerator = $activeAcceleratorEvidence
            requested_acceleration = $requestedAccelerationEvidence
            requested_graphics_mode = $requestedGraphicsModeEvidence
        }
        guest = [ordered]@{
            adb_serial = New-GlesEvidence -Value $Serial -Source 'ADB selected serial'
            avd_name = $avdNameEvidence
            avd_config = $avdConfigEvidence
            system_image = $systemImageEvidence
            android_release = $androidReleaseEvidence
            android_api = $androidApiEvidence
            abi = $abiEvidence
            display_size = $guestSizeEvidence
            display_density = $guestDensityEvidence
            renderer = New-GlesEvidence -Value $summary.renderer -Source 'GLES probe GL_RENDERER'
            graphics_api = New-GlesEvidence -Value $summary.gl_version -Source 'GLES probe GL_VERSION'
            refresh_hz = New-GlesEvidence -Value $summary.refresh_hz -Source 'Android Display.getRefreshRate'
        }
        source = [ordered]@{
            git_commit = $gitCommitEvidence
            worktree_dirty = $worktreeDirtyEvidence
            apk_sha256 = $apkHashEvidence
        }
        workload = [ordered]@{
            identifier = 'fang-gles-fullscreen-shader-v1'
            benchmark_version = 1
            package = $packageName
            iterations = $Iterations
            warmup_seconds = $WarmupSeconds
            duration_seconds = $DurationSeconds
        }
        artifacts = [ordered]@{
            summary_json = [System.IO.Path]::GetFileName($summaryPath)
            samples_csv = [System.IO.Path]::GetFileName($samplesPath)
            manifest_json = [System.IO.Path]::GetFileName($manifestPath)
        }
    }
    if (-not (Test-GlesProbeResultContract -Manifest $manifest -Summary $summary -CsvLines $csvLines -RequireCompleteManifest)) {
        throw 'The GLES result failed its manifest/sample contract.'
    }

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($summaryPath, $summaryJson + [Environment]::NewLine, $utf8)
    [System.IO.File]::WriteAllLines($samplesPath, [string[]]$csvLines, $utf8)
    [System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 20) + [Environment]::NewLine, $utf8)

    Write-Output "RESULT $summaryJson"
    Write-Output "Summary: $summaryPath"
    Write-Output "Samples: $samplesPath"
    Write-Output "Manifest: $manifestPath"
} finally {
    if ($logcat -and -not $logcat.HasExited) {
        $logcat.Kill()
        $logcat.WaitForExit()
    }
    if ($logcat) {
        $logcat.Dispose()
    }
}