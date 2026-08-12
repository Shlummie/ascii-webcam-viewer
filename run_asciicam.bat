@echo off
setlocal
cd /d "%~dp0"

if "%~1"=="--console" (
    call "%~dp0run_asciicam_console.bat"
    exit /b %errorlevel%
)

if exist "%~dp0ASCIICAM.exe" (
    start "" "%~dp0ASCIICAM.exe"
    exit /b 0
)

if not exist "%~dp0run_asciicam_hidden.vbs" (
    call "%~dp0run_asciicam_console.bat"
    exit /b %errorlevel%
)

wscript.exe "%~dp0run_asciicam_hidden.vbs"
exit /b 0
