cd 'A:\PYTHON PROJECT\Fang root folder\Fang'
$content = Get-Content tests/unit/stage9_runtime_tests.cpp
$count = 0
for ($i = 0; $i -lt $content.Count; $i++) {
    if ($content[$i] -match 'test_process_detection_avoids_confusion\(\);') {
        $count++
        if ($count -eq 2) {
            for ($j = $i; $j -lt $content.Count; $j++) {
                if ($content[$j] -match '^\s*\}$' -and $content[$j-1] -match 'return 1;') {
                    $newLines = @(
                        '        // Stage 15: Offline Deployment Preflight Tests',
                        '        test_preflight_artifact_integrity_pass();',
                        '        test_preflight_artifact_integrity_fail_missing();',
                        '        test_preflight_artifact_integrity_fail_sha256_mismatch();',
                        '        test_preflight_abi_match_pass();',
                        '        test_preflight_abi_match_fail();',
                        '        test_preflight_provenance_eligible();',
                        '        test_preflight_provenance_ineligible();',
                        '        test_preflight_daemon_args_valid();',
                        '        test_preflight_transport_auth_required();',
                        '        test_preflight_serial_required();',
                        '        test_preflight_structured_results();',
                        ''
                    )
                    $content = $content[0..$i] + $newLines + $content[($i+1)..$j]
                    break
                }
            }
            break
        }
    }
}
Set-Content tests/unit/stage9_runtime_tests.cpp -Value $content