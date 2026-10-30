@echo off
rem ===========================================================================
rem  build-checks.bat - build all functional check tools into bin\
rem  (lang_check / hotkey2_check / browse_check / picker_check)
rem ===========================================================================
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "PF86=%ProgramFiles(x86)%"
set "VCVARS="
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" goto scan_vs
for /f "usebackq tokens=*" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do if not defined VCVARS if exist "%%P\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%P\VC\Auxiliary\Build\vcvars64.bat"
:scan_vs
if not defined VCVARS for /d %%D in ("!ProgramFiles!\Microsoft Visual Studio\2022\*") do if exist "%%D\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%D\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS for /d %%D in ("!PF86!\Microsoft Visual Studio\2022\*") do if exist "%%D\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%D\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS set "VCVARS=!PF86!\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "!VCVARS!" >nul 2>&1
if not defined WindowsSdkDir (
    set "SDKROOT=!PF86!\Windows Kits\10"
    set "SDKVER="
    for /f "delims=" %%I in ('dir /b /ad /o-n "!SDKROOT!\Include" 2^>nul') do (
        if not defined SDKVER if exist "!SDKROOT!\Include\%%I\um\windows.h" set "SDKVER=%%I"
    )
    if not defined SDKVER goto sdk_missing
    set "INCLUDE=%INCLUDE%;!SDKROOT!\Include\!SDKVER!\ucrt;!SDKROOT!\Include\!SDKVER!\shared;!SDKROOT!\Include\!SDKVER!\um;!SDKROOT!\Include\!SDKVER!\winrt"
    set "LIB=%LIB%;!SDKROOT!\Lib\!SDKVER!\ucrt\x64;!SDKROOT!\Lib\!SDKVER!\um\x64"
)
if not exist "%ROOT%\bin" mkdir "%ROOT%\bin"
if not exist "%ROOT%\build" mkdir "%ROOT%\build"

set "CHECKS=lang_check hotkey2_check browse_check picker_check"
for %%C in (%CHECKS%) do (
    echo [BUILD] %%C
    cl /nologo /MT /EHsc /std:c++17 /utf-8 /W3 ^
       /I"%ROOT%\include" /Fo"%ROOT%\build\\" ^
       "%ROOT%\tools\%%C.cpp" ^
       /Fe"%ROOT%\bin\%%C.exe" ^
       /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib ole32.lib dwmapi.lib shell32.lib
    if errorlevel 1 goto fail
)
echo [OK] all check tools built
exit /b 0
:sdk_missing
echo [FAIL] Windows SDK not found
exit /b 1
:fail
echo [FAIL] check tool build failed
exit /b 1
