@echo off
setlocal
pushd "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto missing_vcvars

call "%VCVARS%"
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++17 /EHsc /W4 /permissive- /O2 asciicam_native_app.cpp /Fe:ASCIICAM.exe /link /SUBSYSTEM:WINDOWS user32.lib comctl32.lib gdi32.lib ole32.lib oleaut32.lib strmiids.lib
set "BUILD_EXIT=%errorlevel%"

if "%BUILD_EXIT%"=="0" (
    echo Built ASCIICAM.exe
)

popd
exit /b %BUILD_EXIT%

:missing_vcvars
echo Visual Studio Build Tools were not found at:
echo %VCVARS%
popd
exit /b 1
