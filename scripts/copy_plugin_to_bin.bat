@echo off
rem ============================================================================
rem  post-build copy: put the freshly built plugin DLL into the TrafficMonitor
rem  plugin folder so it can be loaded without copying by hand.
rem
rem  usage: copy_plugin_to_bin.bat "<path to built dll>"
rem ============================================================================
setlocal

set "SRC=%~1"
set "DEST=E:\code\TrafficMonitor_bin\plugins"

if "%SRC%"=="" (
    echo [post-build] ERROR: no source file given.
    exit /b 1
)

if not exist "%SRC%" (
    echo [post-build] ERROR: source file not found: %SRC%
    exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%" (
    echo [post-build] ERROR: cannot create destination: %DEST%
    exit /b 1
)

copy /Y "%SRC%" "%DEST%\" >nul
if errorlevel 1 (
    echo [post-build] ERROR: copy failed to %DEST%
    exit /b 1
)

echo [post-build] copied "%~nx1"  -^>  "%DEST%"
exit /b 0
