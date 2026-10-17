[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDirectory = Join-Path $repositoryRoot 'out\build'
$cachePath = Join-Path $buildDirectory 'CMakeCache.txt'
if (-not (Test-Path $cachePath)) {
    throw "The existing configured build tree is missing: $cachePath. Configure it separately; this script will not replace it."
}

$cache = Get-Content $cachePath
$sourceEntry = $cache | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } | Select-Object -First 1
$generatorEntry = $cache | Where-Object { $_ -match '^CMAKE_GENERATOR:INTERNAL=' } | Select-Object -First 1
$platformEntry = $cache | Where-Object { $_ -match '^CMAKE_GENERATOR_PLATFORM:INTERNAL=' } | Select-Object -First 1
$expectedSource = (($sourceEntry -replace '^CMAKE_HOME_DIRECTORY:INTERNAL=', '') -replace '/', '\').TrimEnd('\')
$normalizedRepositoryRoot = $repositoryRoot.Replace('/', '\').TrimEnd('\')
$generator = ($generatorEntry -replace '^CMAKE_GENERATOR:INTERNAL=', '')
$platform = ($platformEntry -replace '^CMAKE_GENERATOR_PLATFORM:INTERNAL=', '')
if (-not [string]::Equals($expectedSource, $normalizedRepositoryRoot,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "The existing build tree belongs to '$expectedSource', not '$repositoryRoot'."
}
if ($generator -ne 'Visual Studio 18 2026' -or $platform -ne 'x64') {
    throw "Expected the existing Visual Studio 18 2026 x64 build tree; found generator '$generator' and platform '$platform'."
}

$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) {
    throw 'Git was not found; repository validation cannot continue.'
}
$cmake = Get-FangCMake
$ctest = Get-FangCTest
$results = [System.Collections.Generic.List[object]]::new()
$script:lastExitCode = 0

function Invoke-ValidationStep {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][scriptblock]$Command
    )

    Write-Host "`n== $Name =="
    & $Command
    $exitCode = $LASTEXITCODE
    if ($null -eq $exitCode) {
        $exitCode = 1
    }
    $script:validationResults.Add([pscustomobject]@{
        Name = $Name
        ExitCode = $exitCode
        Status = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
    })
    $script:lastExitCode = $exitCode
    Write-Host "$Name exit code: $exitCode"
}

$script:validationResults = $results
$script:repositoryRoot = $repositoryRoot
$script:buildDirectory = $buildDirectory
$script:configuration = $Configuration
$script:cmake = $cmake
$script:ctest = $ctest
$script:gitPath = $git.Source

Invoke-ValidationStep -Name 'Repository branch and status' -Command {
    & $script:gitPath -C $script:repositoryRoot branch --show-current
    if ($LASTEXITCODE -ne 0) { return }
    & $script:gitPath -C $script:repositoryRoot status --short --untracked-files=all
}
$repositoryCheckPassed = $script:lastExitCode -eq 0

$targets = @(
    'fang-core-tests',
    'fang-runtime-tests',
    'fang-runtime-broker-tests',
    'fang-runtime-broker-native-tests',
    'fang-runtime-supervisor-native-tests',
    'fang-runtime-supervisor'
)
if ($repositoryCheckPassed) {
    Invoke-ValidationStep -Name "Focused $Configuration build" -Command {
        & $script:cmake --build $script:buildDirectory --config $script:configuration `
            --target @targets
    }
}
$buildPassed = $repositoryCheckPassed -and $script:lastExitCode -eq 0

if ($buildPassed) {
    $supervisorTest = Join-Path $buildDirectory "$Configuration\fang-runtime-supervisor-native-tests.exe"
    Invoke-ValidationStep -Name 'Supervisor native test executable' -Command {
        & $supervisorTest
    }
    $supervisorTestPassed = $script:lastExitCode -eq 0

    if ($supervisorTestPassed) {
        Invoke-ValidationStep -Name 'Focused Stage 2B CTest set' -Command {
            & $script:ctest --test-dir $script:buildDirectory -C $script:configuration `
                -R '^(core-unit|runtime-backend-unit|runtime-broker-unit|runtime-broker-native-unit|runtime-supervisor-native-unit)$' `
                --output-on-failure
        }
    } else {
        $results.Add([pscustomobject]@{
            Name = 'Focused Stage 2B CTest set'
            ExitCode = $null
            Status = 'SKIPPED: supervisor executable failed'
        })
    }
} else {
    $results.Add([pscustomobject]@{
        Name = 'Supervisor native test executable'
        ExitCode = $null
        Status = 'SKIPPED: repository check or build failed'
    })
    $results.Add([pscustomobject]@{
        Name = 'Focused Stage 2B CTest set'
        ExitCode = $null
        Status = 'SKIPPED: repository check or build failed'
    })
}

Invoke-ValidationStep -Name 'git diff --check' -Command {
    & $script:gitPath -C $script:repositoryRoot diff --check
}

Write-Host "`n== Validation summary =="
$results | Format-Table -AutoSize
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' -or $_.Status -like 'SKIPPED*' }).Count -gt 0
if ($failed) {
    exit 1
}
exit 0