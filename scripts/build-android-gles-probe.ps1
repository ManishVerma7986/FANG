param(
    [string]$SdkRoot = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }),
    [string]$ApiLevel = '36',
    [string]$BuildToolsVersion = '36.0.0'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$sourceDir = Join-Path $repoRoot 'benchmarks\android-gles-probe'
$outputDir = Join-Path $repoRoot 'out\android-gles-probe'
$platformDir = Join-Path $SdkRoot "platforms\android-$ApiLevel"
$buildToolsDir = Join-Path $SdkRoot "build-tools\$BuildToolsVersion"
$androidJar = Join-Path $platformDir 'android.jar'
$manifest = Join-Path $sourceDir 'AndroidManifest.xml'

$javaHome = $env:JAVA_HOME
if (-not $javaHome -or -not (Test-Path (Join-Path $javaHome 'bin\javac.exe'))) {
    $studioJbr = Join-Path $env:ProgramFiles 'Android\Android Studio\jbr'
    if (-not (Test-Path (Join-Path $studioJbr 'bin\javac.exe'))) {
        throw 'A JDK was not found. Set JAVA_HOME or install Android Studio with its bundled JBR.'
    }
    $javaHome = $studioJbr
}

$requiredFiles = @(
    (Join-Path $buildToolsDir 'aapt2.exe'),
    (Join-Path $buildToolsDir 'd8.bat'),
    (Join-Path $buildToolsDir 'zipalign.exe'),
    (Join-Path $buildToolsDir 'apksigner.bat'),
    $androidJar,
    $manifest
)
foreach ($file in $requiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required Android build tool or platform file not found: $file"
    }
}

function Invoke-Native {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Executable"
    }
}

$classesDir = Join-Path $outputDir 'classes'
$dexDir = Join-Path $outputDir 'dex'
New-Item -ItemType Directory -Path $classesDir, $dexDir -Force | Out-Null

$unsignedApk = Join-Path $outputDir 'probe-unsigned.apk'
$alignedApk = Join-Path $outputDir 'probe-aligned.apk'
$apk = Join-Path $outputDir 'fang-android-gles-probe.apk'
$keystore = Join-Path $outputDir 'debug.keystore'

Invoke-Native (Join-Path $buildToolsDir 'aapt2.exe') @(
    'link', '--manifest', $manifest, '--min-sdk-version', '24',
    '--target-sdk-version', $ApiLevel, '-I', $androidJar, '-o', $unsignedApk
)

$javaSource = Join-Path $sourceDir 'src\com\fang\perfprobe\GlesProbeActivity.java'
Invoke-Native (Join-Path $javaHome 'bin\javac.exe') @(
    '--release', '8', '-classpath', $androidJar, '-d', $classesDir, $javaSource
)

$classFiles = @(Get-ChildItem $classesDir -Filter '*.class' -Recurse | ForEach-Object FullName)
if ($classFiles.Count -eq 0) {
    throw 'Java compilation produced no class files.'
}
$d8Arguments = @(
    '--min-api', '24', '--lib', $androidJar, '--output', $dexDir
) + $classFiles
Invoke-Native (Join-Path $buildToolsDir 'd8.bat') $d8Arguments

$jar = Join-Path $javaHome 'bin\jar.exe'
Invoke-Native $jar @('--update', '--file', $unsignedApk, '-C', $dexDir, 'classes.dex')
Invoke-Native (Join-Path $buildToolsDir 'zipalign.exe') @('-f', '-p', '4', $unsignedApk, $alignedApk)

if (-not (Test-Path $keystore)) {
    Invoke-Native (Join-Path $javaHome 'bin\keytool.exe') @(
        '-genkeypair', '-keystore', $keystore, '-storepass', 'android', '-keypass', 'android',
        '-alias', 'androiddebugkey', '-dname', 'CN=Fang GLES Probe,O=Fang,C=US',
        '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-noprompt'
    )
}

Invoke-Native (Join-Path $buildToolsDir 'apksigner.bat') @(
    'sign', '--ks', $keystore, '--ks-key-alias', 'androiddebugkey',
    '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', $apk, $alignedApk
)
Invoke-Native (Join-Path $buildToolsDir 'apksigner.bat') @('verify', '--verbose', $apk)
Write-Output "Built $apk"