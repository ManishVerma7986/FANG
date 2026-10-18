# =============================================================================
# Guest Daemon Build Pipeline Contract Test
# Validates NDK discovery, version enforcement, ABI validation, and CMD wrapper.
# =============================================================================
$ErrorActionPreference = 'Stop'
$repoRoot = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
$buildScriptPs1 = Join-Path $repoRoot 'scripts\build-guest-daemon.ps1'
$buildScriptCmd = Join-Path $repoRoot 'scripts\build-guest-daemon.cmd'

if (-not (Test-Path -LiteralPath $buildScriptPs1)) {
    throw "Build script missing: $buildScriptPs1"
}
if (-not (Test-Path -LiteralPath $buildScriptCmd)) {
    throw "Build script CMD missing: $buildScriptCmd"
}

$tempBase = Join-Path ([System.IO.Path]::GetTempPath()) ("fang_build_contract_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempBase -Force | Out-Null

function Run-BuildScriptPs1([string]$ScriptArgs) {
    $proc = Start-Process -FilePath "powershell.exe" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-Command", "& '$buildScriptPs1' $ScriptArgs; exit `$LASTEXITCODE"
    ) -NoNewWindow -Wait -PassThru
    return $proc.ExitCode
}

try {
    Write-Host "[TEST 1] Missing NDK Path Rejection..."
    $exitCode = Run-BuildScriptPs1 "-NdkPath '$tempBase\nonexistent_ndk_dir'"
    if ($exitCode -eq 0) {
        throw "Expected non-zero exit code for nonexistent NDK, got 0"
    }
    Write-Host "  -> PASSED (Exit code: $exitCode)"

    Write-Host "[TEST 2] Missing source.properties Rejection..."
    $emptyNdk = Join-Path $tempBase "empty_ndk"
    New-Item -ItemType Directory -Path $emptyNdk -Force | Out-Null
    $exitCode = Run-BuildScriptPs1 "-NdkPath '$emptyNdk'"
    if ($exitCode -eq 0) {
        throw "Expected non-zero exit code for empty NDK missing source.properties, got 0"
    }
    Write-Host "  -> PASSED (Exit code: $exitCode)"

    Write-Host "[TEST 3] Under-Versioned NDK (r23 < r25) Rejection..."
    $oldNdk = Join-Path $tempBase "old_ndk"
    New-Item -ItemType Directory -Path $oldNdk -Force | Out-Null
    $sourcePropsOld = Join-Path $oldNdk "source.properties"
    Set-Content -LiteralPath $sourcePropsOld -Value "Pkg.Desc = Android NDK`nPkg.Revision = 23.1.7779620`n" -Encoding UTF8
    $exitCode = Run-BuildScriptPs1 "-NdkPath '$oldNdk'"
    if ($exitCode -eq 0) {
        throw "Expected non-zero exit code for NDK r23 < minimum r25, got 0"
    }
    Write-Host "  -> PASSED (Exit code: $exitCode)"

    Write-Host "[TEST 4] Unsupported ABI Selection Rejection..."
    $validNdk = Join-Path $tempBase "valid_ndk"
    New-Item -ItemType Directory -Path $validNdk -Force | Out-Null
    $sourcePropsValid = Join-Path $validNdk "source.properties"
    Set-Content -LiteralPath $sourcePropsValid -Value "Pkg.Desc = Android NDK`nPkg.Revision = 26.1.10909125`n" -Encoding UTF8
    $binDir = Join-Path $validNdk "toolchains\llvm\prebuilt\windows-x86_64\bin"
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
    $mockClang = Join-Path $binDir "clang++.exe"
    Set-Content -LiteralPath $mockClang -Value "" -Encoding UTF8

    $exitCode = Run-BuildScriptPs1 "-NdkPath '$validNdk' -Abi 'unsupported_mips'"
    if ($exitCode -eq 0) {
        throw "Expected non-zero exit code for unsupported ABI, got 0"
    }
    Write-Host "  -> PASSED (Exit code: $exitCode)"

    Write-Host "[TEST 5] Synthetic Valid NDK r26 with -ValidateOnly Succeeded..."
    $exitCode = Run-BuildScriptPs1 "-NdkPath '$validNdk' -ValidateOnly"
    if ($exitCode -ne 0) {
        throw "Expected exit code 0 for valid synthetic NDK r26 with -ValidateOnly, got $exitCode"
    }
    Write-Host "  -> PASSED (Exit code: $exitCode)"

    Write-Host "[TEST 6] CMD Wrapper Preserves Exit Code and Arguments..."
    $proc = Start-Process -FilePath $buildScriptCmd -ArgumentList @(
        "-NdkPath", $validNdk, "-ValidateOnly"
    ) -NoNewWindow -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "Expected CMD wrapper exit code 0, got $($proc.ExitCode)"
    }

    $procFail = Start-Process -FilePath $buildScriptCmd -ArgumentList @(
        "-NdkPath", $oldNdk
    ) -NoNewWindow -Wait -PassThru
    if ($procFail.ExitCode -eq 0) {
        throw "Expected CMD wrapper non-zero exit code for invalid NDK, got 0"
    }
    Write-Host "  -> PASSED (CMD wrapper exit codes: success=0, failure=$($procFail.ExitCode))"

    Write-Host "[TEST 7] Repository and Output Paths Containing Spaces..."
    $spacedOutDir = Join-Path $tempBase "output path with spaces"
    $exitCode = Run-BuildScriptPs1 "-NdkPath '$validNdk' -Abi 'x86_64' -ApiLevel 26 -Configuration 'Release' -OutDir '$spacedOutDir' -VerboseOutput"
    if ($exitCode -eq 0) {
        throw "Expected non-zero exit code for mock compiler failure with spaced output path, got 0"
    }
    $spacedBinary = Join-Path $spacedOutDir "fang-input-daemon-android-x86_64"
    if (Test-Path -LiteralPath $spacedBinary) {
        throw "Partial target binary was not removed after spaced-path compilation failure: $spacedBinary"
    }
    $spacedManifest = Join-Path $spacedOutDir "guest_manifest.json"
    if (Test-Path -LiteralPath $spacedManifest) {
        throw "Manifest was created despite spaced-path compilation failure: $spacedManifest"
    }
    $scriptText = Get-Content -LiteralPath $buildScriptPs1 -Raw
    if ($scriptText -notmatch '&\s*\$clangCxx\s+@compileArgs') {
        throw "Build script does not use robust native argument splatting"
    }
    if ($scriptText -notmatch '\$LASTEXITCODE') {
        throw "Build script does not check the native compiler exit code"
    }
    Write-Host "  -> PASSED (Exit code: $exitCode; spaced paths handled)"

    Write-Host "[TEST 8] Source Fingerprint Computed and Included in Manifest (Real NDK)..."
    # Use the real NDK to build and verify fingerprint is included
    $realNdkPath = (Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk\30.0.16248370')
    if (-not (Test-Path -LiteralPath $realNdkPath)) {
        Write-Host "  -> SKIPPED (Real NDK not found at $realNdkPath)"
    } else {
        $exitCode = Run-BuildScriptPs1 "-NdkPath '$realNdkPath' -Abi 'x86_64' -ApiLevel 26 -Configuration 'Release' -OutDir '$tempBase\test8_out'"
        if ($exitCode -ne 0) {
            throw "Build failed for fingerprint test with real NDK, exit code: $exitCode"
        }
        $manifestPath = Join-Path $tempBase "test8_out\guest_manifest.json"
        if (-not (Test-Path -LiteralPath $manifestPath)) {
            throw "Manifest not created for fingerprint test"
        }
        $manifestJson = Get-Content -LiteralPath $manifestPath -Raw
        $manifest = $manifestJson | ConvertFrom-Json
        if (-not $manifest.source_evidence) {
            throw "Manifest missing source_evidence object"
        }
        if ($manifest.source_evidence.status -ne "collected") {
            throw "source_evidence.status should be 'collected', got '$($manifest.source_evidence.status)'"
        }
        if ($manifest.source_evidence.algorithm -ne "sha256") {
            throw "source_evidence.algorithm should be 'sha256', got '$($manifest.source_evidence.algorithm)'"
        }
        if (-not $manifest.source_evidence.fingerprint -or $manifest.source_evidence.fingerprint.Length -ne 64) {
            throw "source_evidence.fingerprint should be 64-char hex, got '$($manifest.source_evidence.fingerprint)'"
        }
        if ($manifest.source_evidence.scope -ne "fang-guest-daemon-inputs-v1") {
            throw "source_evidence.scope should be 'fang-guest-daemon-inputs-v1', got '$($manifest.source_evidence.scope)'"
        }
        if ($manifest.source_evidence.encoding -ne "fang-cim-c14n-v1") {
            throw "source_evidence.encoding should be 'fang-cim-c14n-v1', got '$($manifest.source_evidence.encoding)'"
        }
        if (-not $manifest.source_evidence.captured_at_utc -or $manifest.source_evidence.captured_at_utc -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') {
            throw "source_evidence.captured_at_utc should be ISO 8601 UTC, got '$($manifest.source_evidence.captured_at_utc)'"
        }
        if ($manifest.provenance_collected -notcontains "source_evidence.fingerprint") {
            throw "provenance_collected should include 'source_evidence.fingerprint'"
        }
        Write-Host "  -> PASSED (Fingerprint: $($manifest.source_evidence.fingerprint.Substring(0,12))...)"
    }

    Write-Host "[TEST 9] Fingerprint Stability Across Builds (Real NDK)..."
    if (-not (Test-Path -LiteralPath $realNdkPath)) {
        Write-Host "  -> SKIPPED (Real NDK not found)"
    } else {
        $exitCode = Run-BuildScriptPs1 "-NdkPath '$realNdkPath' -Abi 'x86_64' -ApiLevel 26 -Configuration 'Release' -OutDir '$tempBase\test9_out'"
        if ($exitCode -ne 0) {
            throw "Second build failed with real NDK, exit code: $exitCode"
        }
        $manifestPath2 = Join-Path $tempBase "test9_out\guest_manifest.json"
        $manifest2 = (Get-Content -LiteralPath $manifestPath2 -Raw) | ConvertFrom-Json
        $manifest1 = (Get-Content -LiteralPath (Join-Path $tempBase "test8_out\guest_manifest.json") -Raw) | ConvertFrom-Json
        if ($manifest2.source_evidence.fingerprint -ne $manifest1.source_evidence.fingerprint) {
            throw "Fingerprint should be stable across builds with same source"
        }
        Write-Host "  -> PASSED (Fingerprint stable: $($manifest2.source_evidence.fingerprint.Substring(0,12))...)"
    }
}
finally {
    if (Test-Path -LiteralPath $tempBase) {
        Remove-Item -LiteralPath $tempBase -Recurse -Force -ErrorAction SilentlyContinue
    }
}
