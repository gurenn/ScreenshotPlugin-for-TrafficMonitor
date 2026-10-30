@echo off
rem ===========================================================================
rem  TMPluginScreenshot - single step build (MSVC x64, static CRT)
rem
rem  Usage:  build.bat          incremental build
rem          build.bat clean    delete objects + output, then rebuild
rem
rem  Tool chain discovery (no hard coded machine specific paths):
rem    1) vswhere.exe   -> -latest -products * -requires <VC tools> -property installationPath
rem    2) scan %ProgramFiles% and %ProgramFiles(x86)% for Microsoft Visual Studio\2022\*
rem    3) last resort: a well known default location, with a [WARN] line
rem  Windows SDK: vcvars64 normally provides it; when the registry lookup it
rem  relies on is unavailable we scan Windows Kits\10\Include and take the
rem  highest version that actually contains um\windows.h.
rem
rem  This script prints ASCII only and depends on nothing but cmd builtins.
rem ===========================================================================
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"
set "VCVARS="
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"

rem ---------------------------------------------------------------- 1) vswhere
if not exist "!VSWHERE!" goto scan_vs
for /f "usebackq tokens=*" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call :try_vs "%%P"
if defined VCVARS goto have_vcvars
echo [INFO] vswhere found no installation with the C++ tools, scanning directories

rem ------------------------------------------------------------- 2) scan VS2022
:scan_vs
for /d %%D in ("!PF!\Microsoft Visual Studio\2022\*") do call :try_vs "%%D"
if defined VCVARS goto have_vcvars
for /d %%D in ("!PF86!\Microsoft Visual Studio\2022\*") do call :try_vs "%%D"
if defined VCVARS goto have_vcvars

rem ---------------------------------------------------------- 3) last resort
set "CAND=!PF86!\Microsoft Visual Studio\2022\BuildTools"
call :try_vs "!CAND!"
if defined VCVARS echo [WARN] Visual Studio located through the fallback path only
if not defined VCVARS goto no_vcvars

:have_vcvars
echo [INFO] vcvars64: !VCVARS!
call "!VCVARS!" >nul
if errorlevel 1 goto vcvars_failed

rem ------------------------------------------------------------------- SDK fix
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
if not exist "%ROOT%bin"   mkdir "%ROOT%bin"
if not exist "%ROOT%build" mkdir "%ROOT%build"

if /i "%~1" neq "clean" goto do_build

echo [INFO] clean build requested
del /q "%ROOT%build\*.obj" 2>nul
del /q "%ROOT%build\*.pdb" 2>nul
del /q "%ROOT%build\*.exp" 2>nul
del /q "%ROOT%build\*.lib" 2>nul
del /q "%ROOT%bin\TMPluginScreenshot.dll" 2>nul

:do_build
cl /nologo /LD /std:c++17 /EHsc /MT /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /I"%ROOT%include" /I"%ROOT%src" ^
   /Fo"%ROOT%build\\" /Fd"%ROOT%build\\" ^
   "%ROOT%src\PluginSettings.cpp" ^
   "%ROOT%src\CaptureEngine.cpp" ^
   "%ROOT%src\ImageWriter.cpp" ^
   "%ROOT%src\RegionSelector.cpp" ^
   "%ROOT%src\WindowPicker.cpp" ^
   "%ROOT%src\OptionsDialog.cpp" ^
   "%ROOT%src\HotkeyManager.cpp" ^
   "%ROOT%src\TMPlugin.cpp" ^
   /Fe"%ROOT%bin\TMPluginScreenshot.dll" ^
   /link /DLL /SUBSYSTEM:WINDOWS /RELEASE /MACHINE:X64 ^
   /IMPLIB:"%ROOT%build\TMPluginScreenshot.lib" ^
   windowscodecs.lib dwmapi.lib shlwapi.lib shell32.lib ole32.lib oleaut32.lib uuid.lib ^
   user32.lib gdi32.lib advapi32.lib

if errorlevel 1 goto build_failed
if not exist "%ROOT%bin\TMPluginScreenshot.dll" goto no_output

echo.
echo [OK] built bin\TMPluginScreenshot.dll
exit /b 0

rem ---------------------------------------------------------------------------
:try_vs
if defined VCVARS goto :eof
set "PROBE=%~1"
if "!PROBE!"=="" goto :eof
if exist "!PROBE!\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!PROBE!\VC\Auxiliary\Build\vcvars64.bat"
goto :eof

:no_vcvars
echo [FAIL] vcvars64.bat could not be located
echo        tried vswhere, then %PF%\Microsoft Visual Studio\2022\* and %PF86%\Microsoft Visual Studio\2022\*
exit /b 1

:vcvars_failed
echo [FAIL] vcvars64.bat returned a non zero exit code
exit /b 1

:sdk_missing
echo [FAIL] Windows SDK include directory not found and vcvars64 did not provide one
exit /b 1

:build_failed
echo.
echo [FAIL] build failed
exit /b 1

:no_output
echo [FAIL] TMPluginScreenshot.dll was not produced
exit /b 1
