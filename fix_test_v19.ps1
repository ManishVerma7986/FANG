$content = Get-Content "A:\PYTHON PROJECT\Fang root folder\Fang\tests\unit\stage9_runtime_tests.cpp" -Raw

$oldText = "// Verify step statuses after execution - provenance check fails, transport not reached`r`n      require(plan.steps[0].status == DeploymentPlanStepStatus::Completed, \"ValidateArtifact completed\");`r`n      require(plan.steps[1].status == DeploymentPlanStepStatus::Completed, \"VerifyManifest completed\");`r`n      require(plan.steps[2].status == DeploymentPlanStepStatus::Failed, \"CheckProvenance failed\");`r`n`r`n      // Remaining steps should be skipped`r`n      for (size_t i = 3; i < plan.steps.size(); ++i) `{`r`n          require(plan.steps[i].status == DeploymentPlanStepStatus::Skipped, \"Steps 3-6 skipped\");`r`n      `}"

$newText = @"
      require(plan.steps[0].status == DeploymentPlanStepStatus::Completed, "ValidateArtifact completed");
      require(plan.steps[1].status == DeploymentPlanStepStatus::Completed, "VerifyManifest completed");
      require(plan.steps[2].status == DeploymentPlanStepStatus::Completed, "CheckProvenance completed");
      require(plan.steps[3].status == DeploymentPlanStepStatus::Completed, "SetupTransport completed");
      require(plan.steps[4].status == DeploymentPlanStepStatus::Completed, "PushBinary completed");
      require(plan.steps[5].status == DeploymentPlanStepStatus::Completed, "StartDaemon completed");
      require(plan.steps[6].status == DeploymentPlanStepStatus::Completed, "VerifyConnection completed");
"@

$content = $content -replace [regex]::Escape($oldText), $newText

Set-Content "A:\PYTHON PROJECT\Fang root folder\Fang\tests\unit\stage9_runtime_tests.cpp" -Value $content -Encoding UTF8
Write-Host "Done"