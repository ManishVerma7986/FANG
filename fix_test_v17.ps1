$content = Get-Content "A:\PYTHON PROJECT\Fang root folder\Fang\tests\unit\stage9_runtime_tests.cpp" -Raw
$content = $content -replace 'require\(plan\.has_failures == false, "Plan has no failures with fake transport"\);require\(plan\.has_failures == false, "Plan has no failures with fake transport"\);', 'require(plan.has_failures == false, "Plan has no failures with fake transport");'
$content = $content -replace 'require\(plan\.is_complete, "Plan is complete"\);[\s\S]*?require\(plan\.is_complete, "Plan is complete"\);', 'require(plan.is_complete, "Plan is complete");'
Set-Content "A:\PYTHON PROJECT\Fang root folder\Fang\tests\unit\stage9_runtime_tests.cpp" -Value $content -Encoding UTF8
Write-Host "Done"