function Get-FangCMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $bundledCMake = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $bundledCMake) {
                return $bundledCMake
            }
        }
    }

    throw 'CMake was not found on PATH or in the installed Visual Studio Build Tools instance.'
}

function Invoke-FangCMake {
    param([Parameter(Mandatory)][string[]]$Arguments)

    $cmake = Get-FangCMake
    & $cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake failed with exit code $LASTEXITCODE."
    }
}

function Get-FangCTest {
    $command = Get-Command ctest -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $ctest = Join-Path (Split-Path (Get-FangCMake) -Parent) 'ctest.exe'
    if (Test-Path $ctest) {
        return $ctest
    }

    throw 'CTest was not found on PATH or beside CMake.'
}