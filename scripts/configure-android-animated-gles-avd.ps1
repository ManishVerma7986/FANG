param(
    [string]$SdkRoot = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }),
    [string]$AvdName = 'FANG-StageA-API36-x86_64'
)

$ErrorActionPreference = 'Stop'
$javaHome = $env:JAVA_HOME
if (-not $javaHome -or -not (Test-Path (Join-Path $javaHome 'bin\java.exe'))) {
    $studioJbr = Join-Path $env:ProgramFiles 'Android\Android Studio\jbr'
    if (-not (Test-Path (Join-Path $studioJbr 'bin\java.exe'))) {
        throw 'A local JDK is required by avdmanager; no SDK components will be installed.'
    }
    $javaHome = $studioJbr
}
$env:JAVA_HOME = $javaHome
$avdManager = Join-Path $SdkRoot 'cmdline-tools\latest\bin\avdmanager.bat'
$imagePackage = 'system-images;android-36;default;x86_64'
$imagePath = Join-Path $SdkRoot 'system-images\android-36\default\x86_64\source.properties'
if (-not (Test-Path -LiteralPath $avdManager) -or -not (Test-Path -LiteralPath $imagePath)) {
    throw "The already-installed API 36 x86_64 image and avdmanager are required. No download was attempted."
}

$avdRoot = if ($env:ANDROID_AVD_HOME) { $env:ANDROID_AVD_HOME } else { Join-Path $env:USERPROFILE '.android\avd' }
$avdDirectory = Join-Path $avdRoot "$AvdName.avd"
$avdIni = Join-Path $avdRoot "$AvdName.ini"
if ((Test-Path -LiteralPath $avdDirectory) -or (Test-Path -LiteralPath $avdIni)) {
    throw "Refusing to overwrite an existing AVD or AVD metadata file: $AvdName"
}

$devices = & $avdManager list device -c
if ($LASTEXITCODE -ne 0 -or $devices -notcontains 'pixel_6') {
    throw 'The local Android SDK does not report the requested Pixel 6 hardware profile.'
}
& $avdManager create avd --name $AvdName --package $imagePackage --device pixel_6
if ($LASTEXITCODE -ne 0) {
    throw 'avdmanager could not create the new AVD from the installed image.'
}
if (-not (Test-Path -LiteralPath $avdDirectory)) {
    throw "avdmanager reported success but did not create the expected AVD directory: $avdDirectory"
}

$configPath = Join-Path $avdDirectory 'config.ini'
$config = [ordered]@{}
foreach ($line in Get-Content -LiteralPath $configPath) {
    if ($line -match '^\s*([^#=]+?)\s*=\s*(.*?)\s*$') {
        $config[$Matches[1]] = $Matches[2]
    }
}
$config['hw.lcd.width'] = '1080'
$config['hw.lcd.height'] = '2400'
$config['hw.lcd.density'] = '420'
$config['hw.lcd.depth'] = '24'
$config['hw.gpu.enabled'] = 'yes'
$config['hw.gpu.mode'] = 'host'
$config['hw.cpu.ncore'] = '4'
$config['hw.ramSize'] = '4096'
$config['vm.heapSize'] = '512'
$config['fastboot.forceColdBoot'] = 'yes'
$config['PlayStore.enabled'] = 'false'
$configText = @($config.Keys | Sort-Object | ForEach-Object { "$_=$($config[$_])" }) -join "`r`n"
[System.IO.File]::WriteAllText(
    $configPath,
    $configText + "`r`n",
    (New-Object System.Text.UTF8Encoding($false))
)

$verified = @{}
foreach ($line in Get-Content -LiteralPath $configPath) {
    if ($line -match '^([^=]+)=(.*)$') { $verified[$Matches[1]] = $Matches[2] }
}
foreach ($key in @('hw.lcd.width', 'hw.lcd.height', 'hw.gpu.mode', 'hw.gpu.enabled', 'image.sysdir.1')) {
    if (-not $verified.ContainsKey($key)) {
        throw "Created AVD is missing required configuration field: $key"
    }
}
Write-Output "Created isolated AVD $AvdName using installed $imagePackage."
Write-Output "AVD config: $configPath"
Write-Output "No existing AVD was replaced; no SDK component or image was downloaded."
