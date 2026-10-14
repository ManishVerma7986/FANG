param(
    [string]$SdkRoot = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }),
    [string]$ApiLevel = '36',
    [string]$BuildToolsVersion = '36.0.0'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$sourceDir = Join-Path $repoRoot 'benchmarks\android-animated-gles'
$activitySource = Join-Path $sourceDir 'src\com\fang\animatedgles\AnimatedGlesActivity.java'
$manifest = Join-Path $sourceDir 'AndroidManifest.xml'
$platformDir = Join-Path $SdkRoot "platforms\android-$ApiLevel"
$buildToolsDir = Join-Path $SdkRoot "build-tools\$BuildToolsVersion"
$androidJar = Join-Path $platformDir 'android.jar'

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
    $manifest,
    $activitySource
)
foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required Android build input or tool not found: $file"
    }
}

function Invoke-Native {
    param([Parameter(Mandatory)][string]$Executable, [string[]]$Arguments)
    $priorErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Executable @Arguments
        $nativeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $priorErrorActionPreference
    }
    if ($nativeExitCode -ne 0) {
        throw "Command failed with exit code ${nativeExitCode}: $Executable"
    }
}

$env:JAVA_HOME = $javaHome
$sourceInput = (Get-FileHash -LiteralPath $activitySource -Algorithm SHA256).Hash.ToLowerInvariant() +
    '|' + (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.ToLowerInvariant() +
    '|' + (Get-FileHash -LiteralPath $MyInvocation.MyCommand.Path -Algorithm SHA256).Hash.ToLowerInvariant() +
    "|api=$ApiLevel|build-tools=$BuildToolsVersion|javac=$(& (Join-Path $javaHome 'bin\javac.exe') -version 2>&1)"
$sourceHasher = [System.Security.Cryptography.SHA256]::Create()
$sourceBytes = [System.Text.Encoding]::UTF8.GetBytes($sourceInput)
$sourceHash = [System.BitConverter]::ToString($sourceHasher.ComputeHash($sourceBytes)).Replace('-', '').ToLowerInvariant()
$buildId = $sourceHash.Substring(0, 24)
$sourceHasher.Dispose()
$buildRoot = Join-Path $repoRoot "out\android-animated-gles\builds\$buildId"
$apk = Join-Path $buildRoot 'fang-animated-gles-validation.apk'
$buildManifestPath = Join-Path $buildRoot 'build-manifest.json'

if (Test-Path -LiteralPath $buildRoot) {
    if (-not (Test-Path -LiteralPath $apk) -or -not (Test-Path -LiteralPath $buildManifestPath)) {
        throw "Build directory already exists but is incomplete; refusing to overwrite: $buildRoot"
    }
    $existingBuild = Get-Content -LiteralPath $buildManifestPath -Raw | ConvertFrom-Json
    if ($existingBuild.build_id -ne $buildId -or
        (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant() -ne $existingBuild.apk_sha256) {
        throw "Existing build output failed its identity/hash check: $buildRoot"
    }
    Write-Output "Reusing verified build $buildId at $apk"
    return
}

$classesDir = Join-Path $buildRoot 'classes'
$dexDir = Join-Path $buildRoot 'dex'
New-Item -ItemType Directory -Path $classesDir, $dexDir -Force | Out-Null
$unsignedApk = Join-Path $buildRoot 'unsigned.apk'
$alignedApk = Join-Path $buildRoot 'aligned.apk'

Invoke-Native (Join-Path $buildToolsDir 'aapt2.exe') @(
    'link', '--manifest', $manifest, '--min-sdk-version', '26',
    '--target-sdk-version', $ApiLevel, '-I', $androidJar, '-o', $unsignedApk
)
Invoke-Native (Join-Path $javaHome 'bin\javac.exe') @(
    '--release', '8', '-classpath', $androidJar, '-d', $classesDir, $activitySource
)

$classFiles = @(Get-ChildItem -LiteralPath $classesDir -Filter '*.class' -Recurse | ForEach-Object FullName)
if ($classFiles.Count -eq 0) {
    throw 'Java compilation produced no class files.'
}
$d8Arguments = @('--min-api', '26', '--lib', $androidJar, '--output', $dexDir) + $classFiles
Invoke-Native (Join-Path $buildToolsDir 'd8.bat') $d8Arguments
Invoke-Native (Join-Path $javaHome 'bin\jar.exe') @(
    '--update', '--file', $unsignedApk, '-C', $dexDir, 'classes.dex'
)
Invoke-Native (Join-Path $buildToolsDir 'zipalign.exe') @('-f', '-p', '4', $unsignedApk, $alignedApk)

$debugKeystore = Join-Path $env:USERPROFILE '.android\debug.keystore'
if (-not (Test-Path -LiteralPath $debugKeystore)) {
    throw "Existing local Android debug keystore is required; refusing to create or replace a signing key: $debugKeystore"
}
$keytool = Join-Path $javaHome 'bin\keytool.exe'
& $keytool -list -keystore $debugKeystore -storepass android -alias androiddebugkey *> $null
if ($LASTEXITCODE -ne 0) {
    throw 'The existing local Android debug keystore does not contain the expected debug alias; no key was created or replaced.'
}
Invoke-Native (Join-Path $buildToolsDir 'apksigner.bat') @(
    'sign', '--ks', $debugKeystore, '--ks-key-alias', 'androiddebugkey',
    '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', $apk, $alignedApk
)
$priorErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $signatureOutput = & (Join-Path $buildToolsDir 'apksigner.bat') verify --verbose --print-certs $apk 2>&1
    $signatureExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $priorErrorActionPreference
}
if ($signatureExitCode -ne 0) {
    throw 'APK signature verification failed.'
}
$signatureOutputText = $signatureOutput -join "`n"
$certificateMatch = [regex]::Match($signatureOutputText, 'Signer #1 certificate SHA-256 digest:\s*([0-9a-fA-F]+)')
if (-not $certificateMatch.Success) {
    throw 'APK signer certificate fingerprint was not reported.'
}

$gitCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Could not record the source Git revision.'
}
$apkHash = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
$buildManifest = [ordered]@{
    schema_version = 1
    build_id = $buildId
    workload = 'fang-animated-gles'
    package_name = 'com.fang.animatedgles'
    source_sha256 = $sourceHash
    git_commit = $gitCommit
    api_level = [int]$ApiLevel
    build_tools_version = $BuildToolsVersion
    java_home = $javaHome
    apk_sha256 = $apkHash
    signing_certificate_sha256 = $certificateMatch.Groups[1].Value.ToLowerInvariant()
    signing_key_source = 'Existing local Android debug keystore; not a release signing identity.'
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    $buildManifestPath,
    ($buildManifest | ConvertTo-Json -Depth 8),
    $utf8
)
Write-Output "Built and verified $apk"
Write-Output "Build ID: $buildId"
Write-Output "APK SHA-256: $apkHash"
Write-Output "Signer SHA-256: $($certificateMatch.Groups[1].Value.ToLowerInvariant())"
