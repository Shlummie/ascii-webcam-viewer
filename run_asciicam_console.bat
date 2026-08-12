@echo off
setlocal
cd /d "%~dp0"

if not exist "ASCIICAM.exe" (
    echo ASCIICAM.exe was not found.
    echo Build it with build_asciicam_exe.bat first.
    pause
    exit /b 1
)

echo Starting ASCIICAM.exe...
"%~dp0ASCIICAM.exe"
if errorlevel 1 (
    echo ASCIICAM exited with an error.
    pause
    exit /b 1
)
