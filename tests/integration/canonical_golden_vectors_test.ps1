<#
.SYNOPSIS
    Tests the PowerShell canonical encoding implementation against golden vectors.
    Uses List[byte] for canonical byte construction, computes SHA-256 directly.
#>

$ErrorActionPreference = 'Stop'

$repoRoot = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
$fixturePath = Join-Path $repoRoot 'tests\fixtures\canonical_golden_vectors.json'

if (-not (Test-Path -LiteralPath $fixturePath)) {
    throw "Fixture file not found: $fixturePath"
}

$fixture = Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json

$canonicalScope = $fixture.scope
$canonicalEncoding = $fixture.canonical_format
$roleIds = @{}
$fixture.role_ids.PSObject.Properties | ForEach-Object { $roleIds[$_.Name] = $_.Value }
$kindIds = @{}
$fixture.kind_ids.PSObject.Properties | ForEach-Object { $kindIds[$_.Name] = $_.Value }

function Write-U64BE { param([uint64]$Value, [System.Collections.Generic.List[byte]]$Buffer)
    $Buffer.Add(($Value -shr 56) -band 0xFF)
    $Buffer.Add(($Value -shr 48) -band 0xFF)
    $Buffer.Add(($Value -shr 40) -band 0xFF)
    $Buffer.Add(($Value -shr 32) -band 0xFF)
    $Buffer.Add(($Value -shr 24) -band 0xFF)
    $Buffer.Add(($Value -shr 16) -band 0xFF)
    $Buffer.Add(($Value -shr 8) -band 0xFF)
    $Buffer.Add($Value -band 0xFF)
}

function Write-U32BE { param([uint32]$Value, [System.Collections.Generic.List[byte]]$Buffer)
    $Buffer.Add(($Value -shr 24) -band 0xFF)
    $Buffer.Add(($Value -shr 16) -band 0xFF)
    $Buffer.Add(($Value -shr 8) -band 0xFF)
    $Buffer.Add($Value -band 0xFF)
}

function Write-StringUTF8 { param([string]$Str, [System.Collections.Generic.List[byte]]$Buffer)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Str)
    Write-U32BE -Value $bytes.Length -Buffer $Buffer
    $Buffer.AddRange($bytes)
}

function Write-HexDigest { param([string]$Hex, [System.Collections.Generic.List[byte]]$Buffer)
    for ($i = 0; $i -lt 32 -and $i * 2 -lt $Hex.Length; $i++) {
        $byteStr = $Hex.Substring($i * 2, 2)
        $Buffer.Add([Convert]::ToByte($byteStr, 16))
    }
}

function Compute-CanonicalFingerprint {
    param(
        [string]$RepoRoot,
        [string]$SourceRevision,
        [string]$Abi,
        [uint32]$ApiLevel,
        [string]$Configuration,
        [string]$TargetTriple,
        [string]$NdkVersion,
        [string[]]$CompileArgs,
        [Object[]]$Entries
    )

    # Sort entries by UTF-8 path bytes (ordinal comparison for byte-order equivalence)
    $sortedEntries = [System.Collections.Generic.List[Object]]::new()
    $Entries | ForEach-Object { $sortedEntries.Add($_) } | Out-Null
    $sortedEntries.Sort({ param($a, $b) [System.String]::CompareOrdinal($a.Path, $b.Path) })

    # Build canonical binary stream using List[byte]
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
    Write-StringUTF8 -Str $NdkVersion -Buffer $canonicalStream

    # TOOLCHAIN: target triple
    Write-StringUTF8 -Str $TargetTriple -Buffer $canonicalStream

    # TOOLCHAIN: argument count (u32be)
    Write-U32BE -Value $CompileArgs.Count -Buffer $canonicalStream

    # TOOLCHAIN: each argument in exact order
    foreach ($arg in $CompileArgs) {
        Write-StringUTF8 -Str $arg -Buffer $canonicalStream
    }

    # ENTRIES: entry count (u32be)
    Write-U32BE -Value $sortedEntries.Count -Buffer $canonicalStream

    # ENTRIES: each entry sorted by UTF-8 path bytes
    foreach ($entry in $sortedEntries) {
        Write-StringUTF8 -Str $entry.Path -Buffer $canonicalStream

        # kind: u8 (0=file, 1=virtual)
        $kindByte = $kindIds[$entry.Kind]
        $canonicalStream.Add([byte]$kindByte)

        # role: u8 (stable numeric IDs)
        $roleId = $roleIds[$entry.Role]
        $canonicalStream.Add([byte]$roleId)

        # size_bytes: u64be
        $entrySize = [uint64]$entry.Size
        Write-U64BE -Value $entrySize -Buffer $canonicalStream

        # sha256: 32 raw bytes
        Write-HexDigest -Hex $entry.Hash -Buffer $canonicalStream
    }

    # Final fingerprint = SHA-256 of all preceding bytes
    $canonicalArray = $canonicalStream.ToArray()
    $hex = ($canonicalArray | ForEach-Object { '{0:X2}' -f $_ }) -join ''
    Write-Host "  DEBUG: Canonical bytes length: $($canonicalArray.Length)"
    Write-Host "  DEBUG: Canonical bytes (FULL): $hex"
    
    # Debug: manually verify SHA-256
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($canonicalArray)
    $hasher.Dispose()
    $computedHash = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
    Write-Host "  DEBUG: Computed SHA-256: $computedHash"
    
    return $computedHash
}

$passed = 0
$failed = 0

foreach ($vector in $fixture.test_vectors) {
    Write-Host "Testing: $($vector.name) - $($vector.description)"

    # Convert entries to PSCustomObject array
    $entries = @()
    foreach ($e in $vector.entries) {
        $entries += [PSCustomObject]@{
            Path = $e.path
            Role = $e.role
            Kind = $e.kind
            Size = $e.size
            Hash = $e.content_hash
        }
    }

    $fingerprint = Compute-CanonicalFingerprint `
        -RepoRoot $vector.repo_root `
        -SourceRevision $vector.source_revision `
        -Abi $vector.abi `
        -ApiLevel $vector.api_level `
        -Configuration $vector.configuration `
        -TargetTriple $vector.target_triple `
        -NdkVersion $vector.ndk_version `
        -CompileArgs $vector.compile_args `
        -Entries $entries

    $expected = $vector.expected_fingerprint_sha256
    if ($fingerprint -eq $expected) {
        Write-Host "  PASS: $($fingerprint.Substring(0,16))..."
        $passed++
    } else {
        Write-Host "  FAIL: Expected $expected"
        Write-Host "  FAIL: Got      $fingerprint"
        $failed++
    }
}

Write-Host ""
Write-Host "Results: $passed passed, $failed failed"
if ($failed -gt 0) {
    exit 1
}
exit 0