@echo off
setlocal
cd /d "%~dp0.."
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build\offset-tests mkdir build\offset-tests
pushd build\offset-tests
cl /nologo /O1 /W4 /MT ..\..\tests\test_offsets.c ..\..\src\offset.c /Fe:c-offset.exe || exit /b 1
c-offset.exe || exit /b 1
ml64 /nologo /c /Foasm-offset.obj ..\..\src\offset.asm || exit /b 1
cl /nologo /O1 /W4 /MT ..\..\tests\test_offsets.c asm-offset.obj /Fe:asm-offset.exe || exit /b 1
asm-offset.exe || exit /b 1
popd
