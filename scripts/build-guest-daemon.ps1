<#
.SYNOPSIS
    Builds the Fang Android guest daemon cross-compiled for the guest ABI using Android NDK Clang.
.DESCRIPTION
    Validates the Android NDK r25+ toolchain, cross-compiles all real guest daemon source files
    for the specified ABI and API level, validates the resulting ELF executable headers, calculates
    the SHA-256 digest, and writes a verified guest_manifest.json manifest.
.PARAMETER NdkPath
    Path to the Android NDK root. If omitted, uses ANDROID_NDK_ROOT, ANDROID_NDK_HOME, NDK_ROOT,
    or %LOCALAPPDATA%\Android\Sdk\ndk.
.PARAMETER Abi
    Target Android ABI. Supported: x86_64 (default), arm64-v8a, x86, armeabi-v7a.
.PARAMETER ApiLevel
    Android API level (default: 26).
.PARAMETER Configuration
    Build configuration: Release (default) or Debug.
.PARAMETER OutDir
    Output directory for the compiled ELF executable and companion guest_manifest.json.
    Defaults to out/staging.
.PARAMETER MinNdkVersion
    Minimum required NDK major version (default: 25).
.PARAMETER ValidateOnly
    If set, only validates the NDK and build prerequisites without compiling.
#>
[CmdletBinding()]
param(
    [string]$NdkPath = "",
    [string]$Abi = "x86_64",
    [int]$ApiLevel = 26,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = "Release",
    [string]$OutDir = "",
    [int]$MinNdkVersion = 25,
    [switch]$ValidateOnly,
    [switch]$VerboseOutput
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Split-Path $PSScriptRoot -Parent)

if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot 'out\staging'
}

# -----------------------------------------------------------------------------
# Pre-compute target triple for provenance capture
# -----------------------------------------------------------------------------
$targetTriple = ""
$expectedElfClass = 2    # 1 = 32-bit, 2 = 64-bit
$expectedMachine = 0x3E  # x86_64

switch ($Abi.ToLowerInvariant()) {
    "x86_64" {
        $targetTriple = "x86_64-linux-android$ApiLevel"
        $expectedElfClass = 2
        $expectedMachine = 0x3E
    }
    "arm64-v8a" {
        $targetTriple = "aarch64-linux-android$ApiLevel"
        $expectedElfClass = 2
        $expectedMachine = 0xB7
    }
    "x86" {
        $targetTriple = "i686-linux-android$ApiLevel"
        $expectedElfClass = 1
        $expectedMachine = 0x03
    }
    "armeabi-v7a" {
        $targetTriple = "armv7a-linux-androideabi$ApiLevel"
        $expectedElfClass = 1
        $expectedMachine = 0x28
    }
    default {
        Write-Error "ERROR: Unsupported Android ABI '$Abi'. Supported: x86_64, arm64-v8a, x86, armeabi-v7a."
        exit 1
    }
}
$artifactName = "fang-input-daemon-android-$($Abi.ToLowerInvariant())"

# -----------------------------------------------------------------------------
# 1. NDK Toolchain Discovery & Validation (MUST run before provenance capture)
# -----------------------------------------------------------------------------
function Find-NdkPath {
    param([string]$ExplicitPath)
    if ($ExplicitPath) {
        # Explicit path supplied: validate it strictly, no fallback.
        if (-not (Test-Path -LiteralPath $ExplicitPath)) {
            Write-Error "ERROR: Explicit -NdkPath '$ExplicitPath' does not exist."
            exit 1
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    # Automatic discovery (no explicit path).
    if ($env:ANDROID_NDK_ROOT -and (Test-Path -LiteralPath $env:ANDROID_NDK_ROOT)) {
        return (Resolve-Path -LiteralPath $env:ANDROID_NDK_ROOT).Path
    }
    if ($env:ANDROID_NDK_HOME -and (Test-Path -LiteralPath $env:ANDROID_NDK_HOME)) {
        return (Resolve-Path -LiteralPath $env:ANDROID_NDK_HOME).Path
    }
    if ($env:NDK_ROOT -and (Test-Path -LiteralPath $env:NDK_ROOT)) {
        return (Resolve-Path -LiteralPath $env:NDK_ROOT).Path
    }
    $sdkNdkRoot = Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk'
    if (Test-Path -LiteralPath $sdkNdkRoot) {
        $versions = Get-ChildItem -LiteralPath $sdkNdkRoot -Directory | Sort-Object Name -Descending
        if ($versions.Count -gt 0) {
            return $versions[0].FullName
        }
    }
    return $null
}

$resolvedNdk = Find-NdkPath -ExplicitPath $NdkPath
if (-not $resolvedNdk) {
    Write-Error "ERROR: Android NDK not found. Please provide -NdkPath or set ANDROID_NDK_ROOT environment variable."
    exit 1
}

$sourceProperties = Join-Path $resolvedNdk 'source.properties'
if (-not (Test-Path -LiteralPath $sourceProperties)) {
    Write-Error "ERROR: Invalid Android NDK at '$resolvedNdk': missing source.properties."
    exit 1
}

$propContent = Get-Content -LiteralPath $sourceProperties -Raw
$ndkVersion = ""
$ndkMajor = 0
if ($propContent -match 'Pkg\.Revision\s*=\s*([0-9]+)\.([0-9]+)\.([0-9]+)') {
    $ndkMajor = [int]$matches[1]
    $ndkVersion = "$($matches[1]).$($matches[2]).$($matches[3])"
} elseif ($propContent -match 'Pkg\.Revision\s*=\s*([0-9]+)') {
    $ndkMajor = [int]$matches[1]
    $ndkVersion = "$($matches[1])"
}

if ($ndkMajor -lt $MinNdkVersion) {
    Write-Error "ERROR: Android NDK version '$ndkVersion' at '$resolvedNdk' is below the minimum required version r$MinNdkVersion."
    exit 1
}

# Locate LLVM toolchain Clang++ compiler
$llvmPrebuiltDir = Join-Path $resolvedNdk 'toolchains\llvm\prebuilt'
if (-not (Test-Path -LiteralPath $llvmPrebuiltDir)) {
    Write-Error "ERROR: LLVM toolchain directory not found at '$llvmPrebuiltDir'."
    exit 1
}

$prebuiltHosts = Get-ChildItem -LiteralPath $llvmPrebuiltDir -Directory
if ($prebuiltHosts.Count -eq 0) {
    Write-Error "ERROR: No host prebuilt directory found under '$llvmPrebuiltDir'."
    exit 1
}

$clangBinDir = Join-Path $prebuiltHosts[0].FullName 'bin'
$clangCxx = Join-Path $clangBinDir 'clang++.exe'
if (-not (Test-Path -LiteralPath $clangCxx)) {
    $clangCxx = Join-Path $clangBinDir 'clang++'
    if (-not (Test-Path -LiteralPath $clangCxx)) {
        Write-Error "ERROR: clang++ compiler not found in '$clangBinDir'."
        exit 1
    }
}

Write-Host "Discovered Android NDK: $resolvedNdk (version r$ndkVersion)"
Write-Host "Target Clang++: $clangCxx"

if ($ValidateOnly) {
    Write-Host "Toolchain validation succeeded."
    exit 0
}

# -----------------------------------------------------------------------------
# Helper functions for big-endian encoding (fang-cim-c14n-v1)
# -----------------------------------------------------------------------------
function Write-U32BE {
    param([uint32]$Value, [System.Collections.Generic.List[byte]]$Buffer)
    $Buffer.Add(($Value -shr 24) -band 0xFF)
    $Buffer.Add(($Value -shr 16) -band 0xFF)
    $Buffer.Add(($Value -shr 8) -band 0xFF)
    $Buffer.Add($Value -band 0xFF)
}

function Write-U64BE {
    param([uint64]$Value, [System.Collections.Generic.List[byte]]$Buffer)
    $Buffer.Add(($Value -shr 56) -band 0xFF)
    $Buffer.Add(($Value -shr 48) -band 0xFF)
    $Buffer.Add(($Value -shr 40) -band 0xFF)
    $Buffer.Add(($Value -shr 32) -band 0xFF)
    $Buffer.Add(($Value -shr 24) -band 0xFF)
    $Buffer.Add(($Value -shr 16) -band 0xFF)
    $Buffer.Add(($Value -shr 8) -band 0xFF)
    $Buffer.Add($Value -band 0xFF)
}

function Write-U8 {
    param([byte]$Value, [System.Collections.Generic.List[byte]]$Buffer)
    $Buffer.Add($Value)
}

function Write-StringUTF8 {
    param([string]$Str, [System.Collections.Generic.List[byte]]$Buffer)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Str)
    Write-U32BE -Value $bytes.Length -Buffer $Buffer
    $Buffer.AddRange($bytes)
}

function Write-HexDigest {
    param([string]$Hex, [System.Collections.Generic.List[byte]]$Buffer)
    for ($i = 0; $i -lt 32 -and $i * 2 -lt $Hex.Length; $i++) {
        $byteStr = $Hex.Substring($i * 2, 2)
        $Buffer.Add([Convert]::ToByte($byteStr, 16))
    }
}

# -----------------------------------------------------------------------------
# 0. Capture Canonical Input Manifest at Build Start
# -----------------------------------------------------------------------------
$sourceRevision = ""
$sourceTreeState = "unavailable"
$sourceTreeDirtyFiles = @()
$sourceFingerprint = ""
$sourceEvidenceStatus = "unavailable"
$sourceEvidenceUnavailableReasons = @()
$sourceEvidenceCapturedAtUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

# Canonical input manifest scope and encoding identifiers
$canonicalScope = "fang-guest-daemon-inputs-v1"
$canonicalEncoding = "fang-cim-c14n-v1"

try {
    $sourceRevision = (git -C $repoRoot rev-parse HEAD 2>$null).Trim()
    if ($sourceRevision) {
        # Use porcelain v2 with NUL delimiter for robust path handling (spaces, unicode)
        $gitStatusOutput = git -C $repoRoot status --porcelain=v2 -z 2>$null
        if ($LASTEXITCODE -eq 0 -and $gitStatusOutput) {
            $records = $gitStatusOutput -split "`0" | Where-Object { $_ }
            $gitChangedPaths = @()
            foreach ($record in $records) {
                if ($record -match '^\? (.+)$') {
                    $gitChangedPaths += $matches[1]
                } elseif ($record -match '^[12] .') {
                    $fields = $record -split ' '
                    if ($fields.Count -ge 9) {
                        $path = $fields[8]
                        if ($fields[2] -eq 'R' -or $fields[2] -eq 'C') {
                            if ($fields.Count -ge 11 -and $fields[9] -eq '->') {
                                $path = $fields[10]
                            }
                        }
                        $gitChangedPaths += $path
                    }
                }
            }
            $sourceTreeDirtyFiles = $gitChangedPaths | Sort-Object -Unique
            $sourceTreeState = if ($sourceTreeDirtyFiles.Count -eq 0) { "clean" } else { "dirty" }
        } elseif ($LASTEXITCODE -eq 0) {
            $sourceTreeState = "clean"
        } else {
            $sourceTreeState = "unavailable"
        }
    } else {
        $sourceTreeState = "unavailable"
    }
} catch {
    $sourceTreeState = "unavailable"
    $sourceEvidenceUnavailableReasons += "Exception during git status: $($_.Exception.Message)"
}

# -----------------------------------------------------------------------------
# Enumerate all declared build inputs for canonical input manifest
# -----------------------------------------------------------------------------
$canonicalEntries = @()
$hashFailures = @()

# Helper to add a file entry with role
function Add-CanonicalEntry {
    param(
        [string]$Path,
        [string]$Role,
        [string]$Kind = "file"
    )
    $fullPath = Join-Path $repoRoot $Path
    if (-not (Test-Path -LiteralPath $fullPath)) {
        $hashFailures += "Build input not found: $Path"
        return
    }
    $bytes = [System.IO.File]::ReadAllBytes($fullPath)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($bytes)
    $hasher.Dispose()
    $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
    $canonicalEntries += [PSCustomObject]@{
        Path = $Path
        Role = $Role
        Kind = $Kind
        Size = $bytes.Length
        Hash = $hashHex
    }
}

# 1. Compiled source files (translation units)
$sources = @(
    'apps\fang-input-daemon\main.cpp',
    'apps\fang-input-daemon\guest_input_backend.cpp',
    'apps\fang-input-daemon\guest_input_daemon.cpp',
    'apps\fang-input-daemon\guest_manifest.cpp',
    'apps\fang-studio\input_ipc_protocol.cpp',
    'apps\fang-studio\input_transport.cpp',
    'apps\fang-studio\socket_input_transport.cpp'
)
foreach ($src in $sources) { Add-CanonicalEntry -Path $src -Role "source" }

# 2. Project headers (include all headers in the two component directories)
$headerDirs = @('apps\fang-input-daemon', 'apps\fang-studio')
foreach ($dir in $headerDirs) {
    $headers = Get-ChildItem -LiteralPath (Join-Path $repoRoot $dir) -Filter "*.hpp" -Recurse -File -ErrorAction SilentlyContinue
    foreach ($h in $headers) {
        $relPath = $h.FullName.Substring($repoRoot.Length + 1).Replace('\', '/')
        Add-CanonicalEntry -Path $relPath -Role "header"
    }
}

# 3. Build scripts and configuration
$buildScripts = @(
    'scripts\build-guest-daemon.ps1',
    'scripts\build-guest-daemon.sh',
    'scripts\build-guest-daemon.cmd'
)
foreach ($script in $buildScripts) { Add-CanonicalEntry -Path $script -Role "build-script" }

# 4. CMake configuration
if (Test-Path -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt')) {
    Add-CanonicalEntry -Path 'CMakeLists.txt' -Role "configuration"
}

# 5. Target configuration parameters (as virtual entries)
$targetConfig = @{
    "target.abi" = $Abi
    "target.api_level" = $ApiLevel.ToString()
    "target.configuration" = $Configuration
    "target.compiler_target" = $targetTriple
}
foreach ($kv in $targetConfig.GetEnumerator()) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($kv.Value)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($bytes)
    $hasher.Dispose()
    $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
    $canonicalEntries += [PSCustomObject]@{
        Path = "target/$($kv.Key)"
        Role = "target-config"
        Kind = "virtual"
        Size = $bytes.Length
        Hash = $hashHex
    }
}

# 6. Compiler arguments (normalized, in exact invocation order)
$compileArgsForManifest = @(
    "--target=$targetTriple",
    "-std=c++20",
    "-fPIE",
    "-pie",
    "-Wall",
    "-Wextra",
    "-D__ANDROID__",
    "-I", (Join-Path $repoRoot 'apps\fang-input-daemon').Replace('\', '/'),
    "-I", (Join-Path $repoRoot 'apps\fang-studio').Replace('\', '/')
)
if ($Configuration -eq "Release") {
    $compileArgsForManifest += @("-O2", "-DNDEBUG")
} else {
    $compileArgsForManifest += @("-O0", "-g", "-DFANG_DEBUG")
}
$argIndex = 0
foreach ($arg in $compileArgsForManifest) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($arg)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($bytes)
    $hasher.Dispose()
    $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
    $canonicalEntries += [PSCustomObject]@{
        Path = "compile_args/arg_$([string]::Format('{0:D3}', $argIndex))"
        Role = "compile-arg"
        Kind = "virtual"
        Size = $bytes.Length
        Hash = $hashHex
    }
    $argIndex++
}

# 7. Git revision (as virtual entry)
if ($sourceRevision) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($sourceRevision)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($bytes)
    $hasher.Dispose()
    $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
    $canonicalEntries += [PSCustomObject]@{
        Path = "git/revision"
        Role = "git-revision"
        Kind = "virtual"
        Size = $bytes.Length
        Hash = $hashHex
    }
}

# -----------------------------------------------------------------------------
# Build canonical byte stream (fang-cim-c14n-v1)
# -----------------------------------------------------------------------------
if ($hashFailures.Count -gt 0) {
    $sourceEvidenceStatus = "unavailable"
    $sourceEvidenceUnavailableReasons = $hashFailures
} else {
    # Sort entries by UTF-8 path bytes
    $canonicalEntries = $canonicalEntries | Sort-Object Path
    
    # Build canonical binary stream
    $canonicalStream = [System.Collections.Generic.List[byte]]::new()
    
    # MAGIC: "FANGCIM" + NUL + format-version(1)
    $canonicalStream.AddRange([System.Text.Encoding]::ASCII.GetBytes("FANGCIM"))
    $canonicalStream.Add(0x00)
    $canonicalStream.Add(0x01)
    
    # SCOPE: u32be length + UTF-8 bytes
    Write-StringUTF8 -Str $canonicalScope -Buffer $canonicalStream
    
    # TARGET component
    Write-StringUTF8 -Str "fang-input-daemon" -Buffer $canonicalStream
    
    # TARGET abi
    Write-StringUTF8 -Str $Abi -Buffer $canonicalStream
    
    # TARGET api_level (u32be)
    Write-U32BE -Value $ApiLevel -Buffer $canonicalStream
    
    # TARGET configuration
    Write-StringUTF8 -Str $Configuration -Buffer $canonicalStream
    
    # TOOLCHAIN: compiler identity
    Write-StringUTF8 -Str "clang" -Buffer $canonicalStream
    
    # TOOLCHAIN: compiler version (NDK version)
    $ndkVerStr = "r$ndkVersion"
    Write-StringUTF8 -Str $ndkVerStr -Buffer $canonicalStream
    
    # TOOLCHAIN: target triple
    Write-StringUTF8 -Str $targetTriple -Buffer $canonicalStream
    
    # TOOLCHAIN: argument count (u32be)
    Write-U32BE -Value $compileArgsForManifest.Count -Buffer $canonicalStream
    
    # TOOLCHAIN: each argument in exact order
    foreach ($arg in $compileArgsForManifest) {
        Write-StringUTF8 -Str $arg -Buffer $canonicalStream
    }
    
    # ENTRIES: entry count (u32be)
    Write-U32BE -Value $canonicalEntries.Count -Buffer $canonicalStream
    
    # ENTRIES: each entry sorted by UTF-8 path bytes (already sorted)
    foreach ($entry in $canonicalEntries) {
        # path: u32be length + UTF-8 bytes
        Write-StringUTF8 -Str $entry.Path -Buffer $canonicalStream
        
        # kind: u8 (0=file, 1=virtual)
        $kindByte = if ($entry.Kind -eq "file") { 0 } else { 1 }
        Write-U8 -Value $kindByte -Buffer $canonicalStream
        
        # role: u8 (stable numeric IDs)
        $roleId = switch ($entry.Role) {
            "source" { 1 }
            "header" { 2 }
            "build-script" { 3 }
            "configuration" { 4 }
            "target-config" { 5 }
            "compile-arg" { 6 }
            "git-revision" { 7 }
            default { 255 }
        }
        Write-U8 -Value $roleId -Buffer $canonicalStream
        
        # size_bytes: u64be
        Write-U64BE -Value $entry.Size -Buffer $canonicalStream
        
        # sha256: 32 raw bytes
        Write-HexDigest -Hex $entry.Hash -Buffer $canonicalStream
    }
    
    # Final fingerprint = SHA-256 of all preceding bytes
    $canonicalArray = $canonicalStream.ToArray()
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($canonicalArray)
    $hasher.Dispose()
    $sourceFingerprint = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
    $sourceEvidenceStatus = "collected"
}

# -----------------------------------------------------------------------------
# 3. Source Dependencies & Output Directory Preparation
# -----------------------------------------------------------------------------
$sources = @(
    (Join-Path $repoRoot 'apps\fang-input-daemon\main.cpp'),
    (Join-Path $repoRoot 'apps\fang-input-daemon\guest_input_backend.cpp'),
    (Join-Path $repoRoot 'apps\fang-input-daemon\guest_input_daemon.cpp'),
    (Join-Path $repoRoot 'apps\fang-input-daemon\guest_manifest.cpp'),
    (Join-Path $repoRoot 'apps\fang-studio\input_ipc_protocol.cpp'),
    (Join-Path $repoRoot 'apps\fang-studio\input_transport.cpp'),
    (Join-Path $repoRoot 'apps\fang-studio\socket_input_transport.cpp')
)

foreach ($src in $sources) {
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Error "ERROR: Required source file missing: $src"
        exit 1
    }
}

if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# -----------------------------------------------------------------------------
# Safe Build/Publish: Compile to temporary location, validate, then publish
# -----------------------------------------------------------------------------
$tempDir = Join-Path $env:TEMP ("fang_build_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

$tempBinary = Join-Path $tempDir $artifactName
$tempManifest = Join-Path $tempDir 'guest_manifest.json'

$outputBinary = Join-Path $OutDir $artifactName
$manifestFile = Join-Path $OutDir 'guest_manifest.json'

# -----------------------------------------------------------------------------
# 4. Compilation & Linking (to temporary location)
# -----------------------------------------------------------------------------
$compileArgs = @(
    "--target=$targetTriple",
    "-std=c++20",
    "-fPIE",
    "-pie",
    "-Wall",
    "-Wextra",
    "-D__ANDROID__",
    "-I", (Join-Path $repoRoot 'apps\fang-input-daemon'),
    "-I", (Join-Path $repoRoot 'apps\fang-studio')
)

if ($Configuration -eq "Release") {
    $compileArgs += @("-O2", "-DNDEBUG")
} else {
    $compileArgs += @("-O0", "-g", "-DFANG_DEBUG")
}

$compileArgs += $sources
$compileArgs += @("-o", $tempBinary)

Write-Host "Compiling guest daemon for $targetTriple ($Configuration)..."
if ($VerboseOutput) {
    $displayArgs = $compileArgs | ForEach-Object {
        if ($_ -match '\s') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }
    Write-Host "$clangCxx $($displayArgs -join ' ')"
}

$compileExitCode = 0
try {
    & $clangCxx @compileArgs
    $compileExitCode = $LASTEXITCODE
} catch {
    Write-Error "ERROR: Failed to invoke compiler '$clangCxx': $($_.Exception.Message)"
    $compileExitCode = 1
}
if ($compileExitCode -ne 0) {
    Write-Error "ERROR: Compilation failed with exit code $compileExitCode."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

if (-not (Test-Path -LiteralPath $tempBinary)) {
    Write-Error "ERROR: Compiler reported success but output artifact '$tempBinary' was not created."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# -----------------------------------------------------------------------------
# 5. ELF Artifact Validation (on temporary binary)
# -----------------------------------------------------------------------------
$bytes = [System.IO.File]::ReadAllBytes($tempBinary)
if ($bytes.Length -lt 64) {
    Write-Error "ERROR: Compiled artifact is truncated ($($bytes.Length) bytes)."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# Check ELF magic: 0x7F, 'E', 'L', 'F'
if ($bytes[0] -ne 0x7F -or $bytes[1] -ne 0x45 -or $bytes[2] -ne 0x4C -or $bytes[3] -ne 0x46) {
    Write-Error "ERROR: Artifact does not contain valid ELF magic."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# -----------------------------------------------------------------------------
# Snapshot Consistency Check: Re-capture source evidence after compilation
# -----------------------------------------------------------------------------
if ($sourceEvidenceStatus -eq "collected") {
    Write-Host "Performing snapshot consistency check..."
    $postCompileEntries = @()
    $postCompileFailures = @()
    
    function Add-PostCompileEntry {
        param([string]$Path, [string]$Role, [string]$Kind = "file")
        $fullPath = Join-Path $repoRoot $Path
        if (-not (Test-Path -LiteralPath $fullPath)) {
            $postCompileFailures += "Build input not found (post-compile): $Path"
            return
        }
        $fileBytes = [System.IO.File]::ReadAllBytes($fullPath)
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        $hashBytes = $hasher.ComputeHash($fileBytes)
        $hasher.Dispose()
        $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
        $postCompileEntries += [PSCustomObject]@{
            Path = $Path
            Role = $Role
            Kind = $Kind
            Size = $fileBytes.Length
            Hash = $hashHex
        }
    }
    
    # Re-enumerate all source files and headers
    foreach ($src in $sources) { Add-PostCompileEntry -Path $src -Role "source" }
    foreach ($dir in $headerDirs) {
        $headers = Get-ChildItem -LiteralPath (Join-Path $repoRoot $dir) -Filter "*.hpp" -Recurse -File -ErrorAction SilentlyContinue
        foreach ($h in $headers) {
            $relPath = $h.FullName.Substring($repoRoot.Length + 1).Replace('\', '/')
            Add-PostCompileEntry -Path $relPath -Role "header"
        }
    }
    foreach ($script in $buildScripts) { Add-PostCompileEntry -Path $script -Role "build-script" }
    if (Test-Path -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt')) {
        Add-PostCompileEntry -Path 'CMakeLists.txt' -Role "configuration"
    }
    # Virtual entries (target config, compile args, git revision) - these shouldn't change
    foreach ($kv in $targetConfig.GetEnumerator()) {
        $valueBytes = [System.Text.Encoding]::UTF8.GetBytes($kv.Value)
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        $hashBytes = $hasher.ComputeHash($valueBytes)
        $hasher.Dispose()
        $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
        $postCompileEntries += [PSCustomObject]@{
            Path = "target/$($kv.Key)"
            Role = "target-config"
            Kind = "virtual"
            Size = $valueBytes.Length
            Hash = $hashHex
        }
    }
    $argIndex = 0
    foreach ($arg in $compileArgsForManifest) {
        $valueBytes = [System.Text.Encoding]::UTF8.GetBytes($arg)
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        $hashBytes = $hasher.ComputeHash($valueBytes)
        $hasher.Dispose()
        $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
        $postCompileEntries += [PSCustomObject]@{
            Path = "compile_args/arg_$([string]::Format('{0:D3}', $argIndex))"
            Role = "compile-arg"
            Kind = "virtual"
            Size = $valueBytes.Length
            Hash = $hashHex
        }
        $argIndex++
    }
    if ($sourceRevision) {
        $valueBytes = [System.Text.Encoding]::UTF8.GetBytes($sourceRevision)
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        $hashBytes = $hasher.ComputeHash($valueBytes)
        $hasher.Dispose()
        $hashHex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
        $postCompileEntries += [PSCustomObject]@{
            Path = "git/revision"
            Role = "git-revision"
            Kind = "virtual"
            Size = $valueBytes.Length
            Hash = $hashHex
        }
    }
    
    # Sort and compare
    $postCompileEntries = $postCompileEntries | Sort-Object Path
    $canonicalEntries = $canonicalEntries | Sort-Object Path
    
    $consistent = $true
    if ($postCompileEntries.Count -ne $canonicalEntries.Count) {
        $consistent = $false
        $sourceEvidenceUnavailableReasons += "Snapshot inconsistency: entry count changed ($($canonicalEntries.Count) -> $($postCompileEntries.Count))"
    } else {
        for ($i = 0; $i -lt $canonicalEntries.Count; $i++) {
            $pre = $canonicalEntries[$i]
            $post = $postCompileEntries[$i]
            if ($pre.Path -ne $post.Path -or $pre.Hash -ne $post.Hash -or $pre.Size -ne $post.Size) {
                $consistent = $false
                $sourceEvidenceUnavailableReasons += "Snapshot inconsistency: $($pre.Path) changed (hash: $($pre.Hash) -> $($post.Hash), size: $($pre.Size) -> $($post.Size))"
                break
            }
        }
    }
    
    if (-not $consistent) {
        Write-Warning "SOURCE SNAPSHOT INCONSISTENCY DETECTED: Source files changed during build. Provenance marked unavailable."
        $sourceEvidenceStatus = "unavailable"
        $sourceFingerprint = ""
    } else {
        Write-Host "Snapshot consistency check passed."
    }
}

$actualElfClass = [int]$bytes[4]
$actualDataEncoding = [int]$bytes[5]
$actualType = [int]($bytes[16] -bor ($bytes[17] -shl 8))
$actualMachine = [int]($bytes[18] -bor ($bytes[19] -shl 8))

if ($actualElfClass -ne $expectedElfClass) {
    Write-Error "ERROR: ELF class mismatch: got $actualElfClass, expected $expectedElfClass for ABI $Abi."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

if ($actualDataEncoding -ne 1) { # 1 = little-endian
    Write-Error "ERROR: ELF data encoding mismatch: expected little-endian (1), got $actualDataEncoding."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

if ($actualType -ne 2 -and $actualType -ne 3) { # ET_EXEC or ET_DYN (PIE)
    Write-Error "ERROR: ELF object type is not executable (got $actualType, expected ET_EXEC=2 or ET_DYN=3)."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

if ($actualMachine -ne $expectedMachine) {
    Write-Error "ERROR: ELF machine architecture mismatch: got 0x$($actualMachine.ToString('X2')), expected 0x$($expectedMachine.ToString('X2')) for ABI $Abi."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

$programHeadersCount = if ($actualElfClass -eq 2) {
    [int]($bytes[56] -bor ($bytes[57] -shl 8))
} else {
    [int]($bytes[44] -bor ($bytes[45] -shl 8))
}

if ($programHeadersCount -le 0) {
    Write-Error "ERROR: ELF executable contains 0 program headers."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# -----------------------------------------------------------------------------
# 6. SHA-256 Digest & Truthful Manifest Generation
# -----------------------------------------------------------------------------
$fileSize = (Get-Item -LiteralPath $tempBinary).Length
$hasher = [System.Security.Cryptography.SHA256]::Create()
$fileStream = [System.IO.File]::OpenRead($tempBinary)
$hashBytes = $hasher.ComputeHash($fileStream)
$fileStream.Dispose()
$hasher.Dispose()
$sha256Hex = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()

$provenanceCollected = @("artifact_name","target_abi","android_api_level","build_configuration","compiler_id","ndk_version","artifact_size_bytes","artifact_sha256","build_result","elf_machine","elf_class","elf_data","source_revision","source_tree_state","source_tree_dirty_files","source_evidence.fingerprint")
$provenanceUnavailable = @()
if (-not $sourceRevision) { $provenanceUnavailable += "source_revision" }
if ($sourceTreeState -eq "unavailable") { $provenanceUnavailable += "source_tree_state"; $provenanceUnavailable += "source_tree_dirty_files" }
if ($sourceEvidenceStatus -eq "unavailable") { $provenanceUnavailable += "source_evidence.fingerprint" }

$manifestData = [PSCustomObject]@{
    artifact_name = $artifactName
    artifact_format = "ELF"
    target_os = "android"
    target_abi = $Abi
    protocol_version = 1
    compiler_id = "Android NDK Clang r$ndkVersion"
    build_configuration = $Configuration
    build_config = $Configuration
    backend_capability = "RecordingStub"
    privileged_backend_enabled = $false
    is_cross_compiled = $true
    is_android_elf_binary = $true
    toolchain_status = "ToolchainAvailable"
    artifact_path = $outputBinary  # Final destination path
    android_api_level = $ApiLevel
    ndk_version = "r$ndkVersion"
    artifact_size_bytes = $fileSize
    artifact_sha256 = $sha256Hex
    build_result = "Success"
    elf_machine = "0x$($actualMachine.ToString('X2'))"
    elf_class = $actualElfClass
    elf_data = $actualDataEncoding
    build_rule_version = "stage9-v1"
    manifest_schema_version = "stage9-v3"
    source_revision = $sourceRevision
    source_tree_state = $sourceTreeState
    source_tree_dirty_files = $sourceTreeDirtyFiles
    source_evidence = @{
        status = $sourceEvidenceStatus
        algorithm = "sha256"
        fingerprint = $sourceFingerprint
        scope = "fang-guest-daemon-inputs-v1"
        encoding = "fang-cim-c14n-v1"
        captured_at_utc = $sourceEvidenceCapturedAtUtc
        unavailable_reasons = $sourceEvidenceUnavailableReasons
    }
    provenance_collected = $provenanceCollected
    provenance_unavailable = $provenanceUnavailable
}

$manifestJson = $manifestData | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($tempManifest, $manifestJson, [System.Text.Encoding]::UTF8)

# -----------------------------------------------------------------------------
# 7. Candidate Validation: Re-verify temp artifact against manifest
# -----------------------------------------------------------------------------
$verifyBytes = [System.IO.File]::ReadAllBytes($tempBinary)
$verifyHasher = [System.Security.Cryptography.SHA256]::Create()
$verifyStream = [System.IO.File]::OpenRead($tempBinary)
$verifyHash = $verifyHasher.ComputeHash($verifyStream)
$verifyStream.Dispose()
$verifyHasher.Dispose()
$verifySha256 = [System.BitConverter]::ToString($verifyHash).Replace('-', '').ToLowerInvariant()

if ($verifySha256 -ne $sha256Hex) {
    Write-Error "ERROR: Candidate validation failed - SHA-256 mismatch after manifest generation."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}
if ($verifyBytes.Length -ne $fileSize) {
    Write-Error "ERROR: Candidate validation failed - size mismatch after manifest generation."
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# -----------------------------------------------------------------------------
# 8. Atomic Publish: Replace verified artifact and manifest
# -----------------------------------------------------------------------------
# Backup existing verified files (if any) for rollback
$backupDir = Join-Path $env:TEMP ("fang_backup_" + [System.Guid]::NewGuid().ToString("N"))
$backupCreated = $false
if ((Test-Path -LiteralPath $outputBinary) -or (Test-Path -LiteralPath $manifestFile)) {
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    if (Test-Path -LiteralPath $outputBinary) {
        Copy-Item -LiteralPath $outputBinary -Destination $backupDir -Force
    }
    if (Test-Path -LiteralPath $manifestFile) {
        Copy-Item -LiteralPath $manifestFile -Destination $backupDir -Force
    }
    $backupCreated = $true
}

try {
    # Publish: move temp files to final destination
    if (Test-Path -LiteralPath $outputBinary) { Remove-Item -LiteralPath $outputBinary -Force }
    if (Test-Path -LiteralPath $manifestFile) { Remove-Item -LiteralPath $manifestFile -Force }
    Move-Item -LiteralPath $tempBinary -Destination $outputBinary -Force
    Move-Item -LiteralPath $tempManifest -Destination $manifestFile -Force
} catch {
    Write-Error "ERROR: Failed to publish artifact: $($_.Exception.Message)"
    # Attempt rollback
    if ($backupCreated) {
        if (Test-Path -LiteralPath (Join-Path $backupDir $artifactName)) {
            Copy-Item -LiteralPath (Join-Path $backupDir $artifactName) -Destination $outputBinary -Force
        }
        if (Test-Path -LiteralPath (Join-Path $backupDir 'guest_manifest.json')) {
            Copy-Item -LiteralPath (Join-Path $backupDir 'guest_manifest.json') -Destination $manifestFile -Force
        }
    }
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    if ($backupCreated) { Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue }
    exit 1
}

# Cleanup temp and backup on success
Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
if ($backupCreated) { Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue }

Write-Host "Successfully compiled and validated Android guest daemon artifact:"
Write-Host "  Artifact: $outputBinary ($fileSize bytes)"
Write-Host "  SHA-256:  $sha256Hex"
Write-Host "  ABI:      $Abi (API $ApiLevel, Machine: 0x$($actualMachine.ToString('X2')))"
Write-Host "  Manifest: $manifestFile"
Write-Host "  Source revision: $sourceRevision"
Write-Host "  Source tree state: $sourceTreeState"
if ($sourceTreeState -eq "dirty") {
    Write-Host "  Dirty files: $($sourceTreeDirtyFiles.Count)"
}
Write-Host "  Source fingerprint: $sourceFingerprint"
Write-Host "  Source evidence status: $sourceEvidenceStatus"
if ($sourceEvidenceStatus -eq "unavailable") {
    Write-Host "  Unavailable reasons: $($sourceEvidenceUnavailableReasons.Count)"
}

exit 0
