# Compute PowerShell canonical bytes for file_and_virtual_entries_mixed
$canonicalScope = "fang-guest-daemon-inputs-v1"

$vector = @{
    repo_root = "C:/test/repo"
    source_revision = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    abi = "x86"
    api_level = 26
    configuration = "Release"
    target_triple = "i686-linux-android26"
    ndk_version = "r27.0.12345678"
    compile_args = @(
        "--target=i686-linux-android26",
        "-std=c++20",
        "-fPIE",
        "-pie",
        "-Wall",
        "-Wextra",
        "-D__ANDROID__",
        "-I", "C:/test/repo/apps/fang-input-daemon",
        "-I", "C:/test/repo/apps/fang-studio",
        "-O2",
        "-DNDEBUG",
        "-march=i686",
        "-mtune=generic"
    )
    entries = @(
        @{path="apps/fang-input-daemon/main.cpp"; role="source"; kind="file"; size=1500; hash="deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
        @{path="CMakeLists.txt"; role="configuration"; kind="file"; size=800; hash="beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"},
        @{path="compile_args/arg_000"; role="compile-arg"; kind="virtual"; size=27; hash="cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe"},
        @{path="compile_args/arg_001"; role="compile-arg"; kind="virtual"; size=11; hash="feedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedface"},
        @{path="compile_args/arg_010"; role="compile-arg"; kind="virtual"; size=13; hash="abcdef01abcdef01abcdef01abcdef01abcdef01abcdef01abcdef01abcdef01"},
        @{path="compile_args/arg_011"; role="compile-arg"; kind="virtual"; size=14; hash="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        @{path="target/abi"; role="target-config"; kind="virtual"; size=3; hash="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
        @{path="target/compiler_target"; role="target-config"; kind="virtual"; size=23; hash="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}
    )
}

$roleIds = @{"source"=1; "header"=2; "build-script"=3; "configuration"=4; "target-config"=5; "compile-arg"=6; "git-revision"=7}
$kindIds = @{"file"=0; "virtual"=1}

function Write-U32BE { param([uint32]$Value, [System.Collections.Generic.List[byte]]$Buffer); $Buffer.Add(($Value -shr 24) -band 0xFF); $Buffer.Add(($Value -shr 16) -band 0xFF); $Buffer.Add(($Value -shr 8) -band 0xFF); $Buffer.Add($Value -band 0xFF) }
function Write-U64BE { param([uint64]$Value, [System.Collections.Generic.List[byte]]$Buffer); $Buffer.Add(($Value -shr 56) -band 0xFF); $Buffer.Add(($Value -shr 48) -band 0xFF); $Buffer.Add(($Value -shr 40) -band 0xFF); $Buffer.Add(($Value -shr 32) -band 0xFF); $Buffer.Add(($Value -shr 24) -band 0xFF); $Buffer.Add(($Value -shr 16) -band 0xFF); $Buffer.Add(($Value -shr 8) -band 0xFF); $Buffer.Add($Value -band 0xFF) }
function Write-U8 { param([byte]$Value, [System.Collections.Generic.List[byte]]$Buffer); $Buffer.Add($Value) }
function Write-StringUTF8 { param([string]$Str, [System.Collections.Generic.List[byte]]$Buffer); $bytes = [System.Text.Encoding]::UTF8.GetBytes($Str); Write-U32BE -Value $bytes.Length -Buffer $Buffer; $Buffer.AddRange($bytes) }
function Write-HexDigest { param([string]$Hex, [System.Collections.Generic.List[byte]]$Buffer); for ($i = 0; $i -lt 32 -and $i * 2 -lt $Hex.Length; $i++) { $byteStr = $Hex.Substring($i * 2, 2); $Buffer.Add([Convert]::ToByte($byteStr, 16)) } }

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
foreach ($arg in $vector.compile_args) { Write-StringUTF8 -Str $arg -Buffer $canonicalStream }

$sortedEntries = $vector.entries | Sort-Object path
Write-U32BE -Value $sortedEntries.Count -Buffer $canonicalStream
foreach ($entry in $sortedEntries) {
    Write-StringUTF8 -Str $entry.path -Buffer $canonicalStream
    $kindByte = $kindIds[$entry.kind]
    Write-U8 -Value $kindByte -Buffer $canonicalStream
    $roleId = $roleIds[$entry.role]
    Write-U8 -Value $roleId -Buffer $canonicalStream
    Write-U64BE -Value $entry.size -Buffer $canonicalStream
    Write-HexDigest -Hex $entry.hash -Buffer $canonicalStream
}

$canonicalArray = $canonicalStream.ToArray()
$hex = ($canonicalArray | ForEach-Object { '{0:X2}' -f $_ }) -join ''
Write-Host "Canonical bytes length: $($canonicalArray.Length)"
Write-Host "Canonical bytes (first 400): $($hex.Substring(0, [Math]::Min(400, $hex.Length)))"

$hasher = [System.Security.Cryptography.SHA256]::Create()
$hash = $hasher.ComputeHash($canonicalArray)
$hasher.Dispose()
$fingerprint = [System.BitConverter]::ToString($hash).Replace('-', '').ToLowerInvariant()
Write-Host "Fingerprint: $fingerprint"