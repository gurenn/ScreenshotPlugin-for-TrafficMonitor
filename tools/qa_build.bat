@echo off
rem ===========================================================================
rem  QA test build - compiles tools\qa_*.cpp into bin\qa_*.exe
rem  Same compiler flags as build.bat (x64, /MT, /O2, C++17).
rem ===========================================================================
setlocal
set "ROOT=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%" >nul 2>&1

if defined WindowsSdkDir goto env_ready
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
if not exist "%SDKROOT%\Include" goto sdk_missing
set "SDKVER=10.0.26100.0"
if exist "%SDKROOT%\Include\%SDKVER%\um\windows.h" goto env_ready_ver
set "SDKVER="
for /f "delims=" %%V in ('dir /b /ad /o-n "%SDKROOT%\Include"') do if not defined SDKVER if exist "%SDKROOT%\Include\%%V\um\windows.h" set "SDKVER=%%V"
if not defined SDKVER goto sdk_missing
:env_ready_ver
set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt"
set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"

:env_ready
if not exist "%ROOT%\bin" mkdir "%ROOT%\bin"
if not exist "%ROOT%\build" mkdir "%ROOT%\build"

set CFLAGS=/nologo /std:c++17 /EHsc /MT /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 -I"%ROOT%\include"
set LIBS=user32.lib gdi32.lib shell32.lib psapi.lib ole32.lib advapi32.lib

echo [qa] building qa_contract.exe
cl %CFLAGS% /Fo"%ROOT%\build\qa_contract.obj" /Fd"%ROOT%\build\qa_contract.pdb" /Fe"%ROOT%\bin\qa_contract.exe" "%ROOT%\tools\qa_contract.cpp" /link /SUBSYSTEM:CONSOLE /MACHINE:X64 %LIBS%
if errorlevel 1 goto failed

echo [qa] building qa_runtime.exe
cl %CFLAGS% /Fo"%ROOT%\build\qa_runtime.obj" /Fd"%ROOT%\build\qa_runtime.pdb" /Fe"%ROOT%\bin\qa_runtime.exe" "%ROOT%\tools\qa_runtime.cpp" /link /SUBSYSTEM:CONSOLE /MACHINE:X64 %LIBS%
if errorlevel 1 goto failed


echo [qa] building qa_wic.exe
cl %CFLAGS% /Fo"%ROOT%\build\qa_wic.obj" /Fd"%ROOT%\build\qa_wic.pdb" /Fe"%ROOT%\bin\qa_wic.exe" "%ROOT%\tools\qa_wic.cpp" /link /SUBSYSTEM:CONSOLE /MACHINE:X64 %LIBS% windowscodecs.lib oleaut32.lib
if errorlevel 1 goto failed

echo [qa] OK
exit /b 0

:no_vcvars
echo [qa][FAIL] vcvars64.bat not found: %VCVARS%
exit /b 1
:sdk_missing
echo [qa][FAIL] Windows SDK include directory not found
exit /b 1
:failed
echo [qa][FAIL] build failed
exit /b 1
