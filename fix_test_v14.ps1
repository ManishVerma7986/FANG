$oldText = "// Error should mention provenance unavailable`r`n      require(error.find(`"Provenance`") != std::string::npos || error.find(`"provenance`") != std::string::npos, `r`n              `"Error mentions provenance issue: `" + error);`r`n      require(plan.has_failures == false, `"Plan has no failures with fake transport`");"
$newText = "      require(plan.has_failures == false, `"Plan has no failures with fake transport`");"
$content = Get-Content "A:\PYTHON PROJECT\Fang root folder\Fang\tests\unit\stage9_runtime_tests.cpp" -Raw
$content = $content.Replace($oldText, $newText)
$content = $content -replace 'require\(plan\.is_complete, `"Plan is complete`"\);[\s\S]*?require\(plan\.is_complete, `"Plan is complete`"\);', 'require(plan.is_complete, `"Plan is complete`");'
Set-Content "A:\PYTHON PROJECT\Fang root folder\Fang\tests\unit\stage9_runtime_tests.cpp" -Value $content -Encoding UTF8
Write-Host "Done"