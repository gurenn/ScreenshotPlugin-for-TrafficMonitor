@echo off
rem ===========================================================================
rem  Builds the developer/QA harnesses that live next to the plugin.
rem  Not part of the shipped DLL, but handy to regenerate them:
rem    bin\smoketest.exe   - ITMPlugin contract + real capture + dialog check
rem    bin\fix_unload.exe  - 20 x (load -> capture -> FreeLibrary) regression
rem  Tool chain discovery is the same as build.bat (vswhere -> scan -> fallback).
rem ===========================================================================
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"
set "VCVARS="
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "!VSWHERE!" goto scan_vs
for /f "usebackq tokens=*" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call :try_vs "%%P"
if defined VCVARS goto have_vcvars
:scan_vs
for /d %%D in ("!PF!\Microsoft Visual Studio\2022\*") do call :try_vs "%%D"
if defined VCVARS goto have_vcvars
for /d %%D in ("!PF86!\Microsoft Visual Studio\2022\*") do call :try_vs "%%D"
if defined VCVARS goto have_vcvars
set "CAND=!PF86!\Microsoft Visual Studio\2022\BuildTools"
call :try_vs "!CAND!"
if not defined VCVARS goto no_vcvars

:have_vcvars
echo [INFO] vcvars64: !VCVARS!
call "!VCVARS!" >nul
if errorlevel 1 goto vcvars_failed

if defined WindowsSdkDir goto env_ready
set "SDKROOT=!PF86!\Windows Kits\10"
if not exist "!SDKROOT!\Include" goto sdk_missing
set "SDKVER="
for /f "delims=" %%I in ('dir /b /ad /o-n "!SDKROOT!\Include" 2^>nul') do (
    if not defined SDKVER if exist "!SDKROOT!\Include\%%I\um\windows.h" set "SDKVER=%%I"
)
if not defined SDKVER goto sdk_missing
set "INCLUDE=%INCLUDE%;!SDKROOT!\Include\!SDKVER!\ucrt;!SDKROOT!\Include\!SDKVER!\shared;!SDKROOT!\Include\!SDKVER!\um;!SDKROOT!\Include\!SDKVER!\winrt"
set "LIB=%LIB%;!SDKROOT!\Lib\!SDKVER!\ucrt\x64;!SDKROOT!\Lib\!SDKVER!\um\x64"
echo [INFO] Windows SDK paths added manually: !SDKVER!

:env_ready
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%build" mkdir "%ROOT%build"
set "CFLAGS=/nologo /EHsc /MT /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
set "LIBS=user32.lib gdi32.lib"

echo [test] building bin\smoketest.exe
cl %CFLAGS% /I"%ROOT%\include" /Fo"%ROOT%\build\\" "%ROOT%\tools\smoketest.cpp" /Fe"%ROOT%\bin\smoketest.exe" /link /SUBSYSTEM:CONSOLE %LIBS%
if errorlevel 1 goto build_failed

echo [test] building bin\fix_unload.exe
cl %CFLAGS% /I"%ROOT%\include" /Fo"%ROOT%\build\\" "%ROOT%\tools\fix_unload.cpp" /Fe"%ROOT%\bin\fix_unload.exe" /link /SUBSYSTEM:CONSOLE %LIBS%
if errorlevel 1 goto build_failed

echo [test] OK
exit /b 0

:try_vs
if defined VCVARS goto :eof
set "PROBE=%~1"
if "!PROBE!"=="" goto :eof
if exist "!PROBE!\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!PROBE!\VC\Auxiliary\Build\vcvars64.bat"
goto :eof

:no_vcvars
echo [FAIL] vcvars64.bat could not be located
exit /b 1
:vcvars_failed
echo [FAIL] vcvars64.bat returned a non zero exit code
exit /b 1
:sdk_missing
echo [FAIL] Windows SDK include directory not found and vcvars64 did not provide one
exit /b 1
:build_failed
echo [FAIL] building the test harnesses failed
exit /b 1
