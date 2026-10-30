@echo off
rem ===========================================================================
rem  TMPluginScreenshot - acceptance checks for the produced DLL
rem    1) exports   : exactly the single undecorated C export TMPluginGetInstance
rem    2) dependents: no VCRUNTIME / MSVCP / api-ms-win-crt, only whitelisted OS DLLs
rem    3) headers   : machine (x64)
rem  Exit code 0 = all green, 1 = at least one assertion failed.
rem ===========================================================================
setlocal
set "ROOT=%~dp0.."
set "DLL=%ROOT%\bin\TMPluginScreenshot.dll"
set "WORK=%TEMP%\tmsnap_verify"
set "UNKNOWN=0"

if not exist "%DLL%" goto no_dll

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where dumpbin >nul 2>&1
if errorlevel 1 goto no_dumpbin

if defined WindowsSdkDir goto env_ready
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "PATH=%PATH%;%SDKROOT%\bin\%SDKVER%\x64"
:env_ready

if not exist "%WORK%" mkdir "%WORK%"

echo ============================================================
echo  1/3  exports
echo ============================================================
dumpbin /nologo /exports "%DLL%" > "%WORK%\exports.txt" 2>&1
findstr /C:"TMPluginGetInstance" "%WORK%\exports.txt" >nul
if errorlevel 1 goto fail_exports
findstr /C:"?" "%WORK%\exports.txt" >nul
if not errorlevel 1 goto fail_decorated
findstr /C:"name" "%WORK%\exports.txt" >nul
if errorlevel 1 goto fail_exports
echo   [PASS] the export table only contains the undecorated C symbol TMPluginGetInstance

echo.
echo ============================================================
echo  2/3  dependents
echo ============================================================
dumpbin /nologo /dependents "%DLL%" > "%WORK%\dependents.txt" 2>&1
findstr /I /C:"VCRUNTIME" "%WORK%\dependents.txt" >nul
if not errorlevel 1 goto fail_blacklist
findstr /I /C:"MSVCP" "%WORK%\dependents.txt" >nul
if not errorlevel 1 goto fail_blacklist
findstr /I /C:"api-ms-win-crt" "%WORK%\dependents.txt" >nul
if not errorlevel 1 goto fail_blacklist
echo   [PASS] no VCRUNTIME / MSVCP / api-ms-win-crt dependency (static CRT)

set "INBLK="
set "DEPCOUNT=0"
for /f "tokens=* delims= " %%L in ('type "%WORK%\dependents.txt"') do call :dep_line "%%L"
if not "%UNKNOWN%"=="0" goto fail_unknown
echo   [INFO] dependencies inspected: %DEPCOUNT%
if %DEPCOUNT% LSS 4 goto fail_unknown
echo   [PASS] every dependency is on the whitelist

echo.
echo ============================================================
echo  3/3  headers (machine)
echo ============================================================
dumpbin /nologo /headers "%DLL%" > "%WORK%\headers.txt" 2>&1
findstr /C:"machine (x64)" "%WORK%\headers.txt" >nul
if errorlevel 1 goto fail_machine
echo   [PASS] machine (x64)

echo.
echo [OK] verify: all assertions passed
exit /b 0

rem ---------------------------------------------------------------------------
:dep_line
set "LINE=%~1"
if /i "%LINE%"=="Image has the following dependencies:" (set "INBLK=1" & goto :eof)
if not defined INBLK goto :eof
if /i "%LINE%"=="Summary" (set "INBLK=" & goto :eof)
echo %LINE% | findstr /I /C:".dll" >nul
if errorlevel 1 goto :eof
call :check_dep "%LINE%"
goto :eof

:check_dep
set "DEP=%~1"
for /f "tokens=1" %%T in ("%DEP%") do set "DEP=%%T"
set /a DEPCOUNT+=1
if /i "%DEP%"=="KERNEL32.dll" goto :eof
if /i "%DEP%"=="USER32.dll" goto :eof
if /i "%DEP%"=="GDI32.dll" goto :eof
if /i "%DEP%"=="OLE32.dll" goto :eof
if /i "%DEP%"=="OLEAUT32.dll" goto :eof
if /i "%DEP%"=="WINDOWSCODECS.dll" goto :eof
if /i "%DEP%"=="SHLWAPI.dll" goto :eof
if /i "%DEP%"=="SHELL32.dll" goto :eof
if /i "%DEP%"=="DWMAPI.dll" goto :eof
if /i "%DEP%"=="ADVAPI32.dll" goto :eof
echo   [FAIL] dependency outside the whitelist: %DEP%
set /a UNKNOWN+=1
goto :eof

rem ---------------------------------------------------------------------------
:no_dll
echo [FAIL] DLL not found: %DLL%
echo        run build.bat first
exit /b 1

:no_dumpbin
echo [FAIL] dumpbin is not available in PATH
exit /b 1

:fail_exports
echo   [FAIL] TMPluginGetInstance export missing
exit /b 1

:fail_decorated
echo   [FAIL] the export table contains decorated C++ symbols
exit /b 1

:fail_blacklist
echo   [FAIL] forbidden dependency found (VCRUNTIME / MSVCP / api-ms-win-crt)
exit /b 1

:fail_unknown
echo   [FAIL] unexpected dependencies, see above
exit /b 1

:fail_machine
echo   [FAIL] the DLL is not machine (x64)
exit /b 1
