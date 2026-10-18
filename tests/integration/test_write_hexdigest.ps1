# Test Write-HexDigest function
function Write-HexDigest {
    param([string]$Hex, [System.Collections.Generic.List[byte]]$Buffer)
    for ($i = 0; $i -lt 32 -and $i * 2 -lt $Hex.Length; $i++) {
        $byteStr = $Hex.Substring($i * 2, 2)
        Write-Host "  i=$i, byteStr='$byteStr', byte=$([Convert]::ToByte($byteStr, 16))"
        $Buffer.Add([Convert]::ToByte($byteStr, 16))
    }
}

$hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
Write-Host "Input hash: $hash"
Write-Host "Length: $($hash.Length)"

$buffer = [System.Collections.Generic.List[byte]]::new()
Write-HexDigest -Hex $hash -Buffer $buffer

$hex = ($buffer.ToArray() | ForEach-Object { '{0:X2}' -f $_ }) -join ''
Write-Host "Output: $hex"
Write-Host "Expected: E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"
Write-Host "Match: $($hex -eq 'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855')"