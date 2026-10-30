@echo off
setlocal
set "ROOT=%~dp0.."
call "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" >nul 2>&1
dumpbin /nologo /disasm "%ROOT%\bin\TMPluginScreenshot.dll" > "%ROOT%\out-test\qa_disasm.txt" 2>&1
echo done
