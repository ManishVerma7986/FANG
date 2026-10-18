@echo off
rem ===========================================================================
rem Fang Android Guest Daemon Cross-Compilation Pipeline Entry Point (Windows CMD)
rem ===========================================================================
setlocal

set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%build-guest-daemon.ps1"

if not exist "%PS_SCRIPT%" (
    echo [ERROR] PowerShell script not found: "%PS_SCRIPT%" 1>&2
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& '%PS_SCRIPT%' %*; exit $LASTEXITCODE"
exit /b %ERRORLEVEL%
