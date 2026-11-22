@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "variant=c"
if /i "%~1"=="asm" set "variant=asm"
if not "%~1"=="" if /i not "%~1"=="asm" exit /b 2
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" goto :notools
for /f "usebackq delims=" %%I in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto :notools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "build\%variant%" mkdir "build\%variant%" || exit /b 1
if not exist bin mkdir bin || exit /b 1
set "app=noteChad"
if "%variant%"=="asm" set "app=noteChad-asm"
pushd "build\%variant%"
set "flags=/nologo /std:c11 /utf-8 /W4 /O1 /GL /Gy /Gw /GF /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS"
if "%variant%"=="asm" (ml64 /nologo /c /Fooffset.obj ..\..\src\offset.asm) else (cl %flags% /c ..\..\src\offset.c)
if errorlevel 1 exit /b 1
cl %flags% /c ..\..\src\main.c ..\..\src\router.c ..\..\src\drag.c ..\..\src\document.c ..\..\src\textfile.c ..\..\src\markdown.c ..\..\src\ai.c ..\..\src\api.c || exit /b 1
rc /nologo /I ..\..\src /fo notechad.res ..\..\src\notechad.rc || exit /b 1
link /nologo /subsystem:windows /ltcg /opt:ref /opt:icf /incremental:no /dynamicbase /nxcompat /nodefaultlib:libucrt.lib ucrt.lib /delayload:winhttp.dll delayimp.lib /out:"..\..\bin\%app%.exe" notechad.res main.obj router.obj drag.obj document.obj textfile.obj markdown.obj ai.obj api.obj offset.obj user32.lib gdi32.lib comdlg32.lib shell32.lib winhttp.lib comctl32.lib advapi32.lib || exit /b 1
popd
for %%F in ("bin\%app%.exe") do echo Built %%~fF ^(%%~zF bytes^)
exit /b 0
:notools
echo Install the Visual Studio Desktop development with C++ workload and Windows SDK. 1>&2
exit /b 1
