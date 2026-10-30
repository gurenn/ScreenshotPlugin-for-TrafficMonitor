@echo off
rem QA helper: run dumpbin against the produced DLL and store the raw output.
setlocal
set "ROOT=%~dp0.."
set "DLL=%ROOT%\bin\TMPluginScreenshot.dll"
set "OUT=%ROOT%\out-test"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where dumpbin >nul 2>&1 || (echo [FAIL] dumpbin missing & exit /b 1)
dumpbin /nologo /exports    "%DLL%" > "%OUT%\qa_exports.txt" 2>&1
dumpbin /nologo /dependents "%DLL%" > "%OUT%\qa_dependents.txt" 2>&1
dumpbin /nologo /headers    "%DLL%" > "%OUT%\qa_headers.txt" 2>&1
dumpbin /nologo /imports    "%DLL%" > "%OUT%\qa_imports.txt" 2>&1
echo [OK] dumpbin output written
exit /b 0
