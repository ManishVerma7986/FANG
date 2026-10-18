<#
.SYNOPSIS
    Debug script to print canonical bytes for a single test vector.
#>

$ErrorActionPreference = 'Stop'

$repoRoot = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
$fixturePath = Join-Path $repoRoot 'tests\fixtures\canonical_golden_vectors.json'

$fixture = Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json

$canonicalScope = $fixture.scope
$canonicalEncoding = $fixture.canonical_format
$roleIds = @{}
$fixture.role_ids.PSObject.Properties | ForEach-Object { $roleIds[$_.Name] = $_.Value }
$kindIds = @{}
$fixture.kind_ids.PSObject.Properties | ForEach-Object { $kindIds[$_.Name] = $_.Value }

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

# Find single_file_entry vector
$vector = $fixture.test_vectors | Where-Object { $_.name -eq 'single_file_entry' }
if (-not $vector) { throw "Vector not found" }

Write-Host "Testing: $($vector.name) - $($vector.description)"

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

# Build canonical stream with debug
$canonicalStream = [System.Collections.Generic.List[byte]]::new()

$canonicalStream.AddRange([System.Text.Encoding]::ASCII.GetBytes("FANGCIM"))
$canonicalStream.Add(0x00)
$canonicalStream.Add(0x01)

Write-StringUTF8 -Str $canonicalScope -Buffer $canonicalStream
Write-StringUTF8 -Str "fang-input-daemon" -Buffer $canonicalStream
Write-StringUTF8 -Str $vector.abi -Buffer $canonicalStream
Write-U32BE -Value $vector.api_level -Buffer $canonicalStream
Write-StringUTF8 -Str $vector.configuration -Buffer $canonicalStream
Write-StringUTF8 -Str "clang" -Buffer $canonicalStream
Write-StringUTF8 -Str $vector.ndk_version -Buffer $canonicalStream
Write-StringUTF8 -Str $vector.target_triple -Buffer $canonicalStream
Write-U32BE -Value $vector.compile_args.Count -Buffer $canonicalStream
foreach ($arg in $vector.compile_args) {
    Write-StringUTF8 -Str $arg -Buffer $canonicalStream
}

$sortedEntries = $entries | Sort-Object Path
Write-U32BE -Value $sortedEntries.Count -Buffer $canonicalStream
foreach ($entry in $sortedEntries) {
    Write-StringUTF8 -Str $entry.Path -Buffer $canonicalStream
    $kindByte = $kindIds[$entry.Kind]
    Write-U8 -Value $kindByte -Buffer $canonicalStream
    $roleId = $roleIds[$entry.Role]
    Write-U8 -Value $roleId -Buffer $canonicalStream
    Write-U64BE -Value $entry.Size -Buffer $canonicalStream
    Write-HexDigest -Hex $entry.Hash -Buffer $canonicalStream
}

$canonicalArray = $canonicalStream.ToArray()
$hex = ($canonicalArray | ForEach-Object { '{0:X2}' -f $_ }) -join ''

Write-Host "Canonical bytes length: $($canonicalArray.Length)"
Write-Host "Canonical bytes (full):"
Write-Host $hex

# Compute fingerprint
$hasher = [System.Security.Cryptography.SHA256]::Create()
$hashBytes = $hasher.ComputeHash($canonicalArray)
$hasher.Dispose()
$fingerprint = [System.BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
Write-Host "Fingerprint: $fingerprint"
Write-Host "Expected:    $($vector.expected_fingerprint_sha256)"
Write-Host "Match:       $($fingerprint -eq $vector.expected_fingerprint_sha256)"