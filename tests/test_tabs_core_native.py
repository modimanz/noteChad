"""Build and run real frame-state boundary tests in an isolated process."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def main():
    vswhere=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
    vs=subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip()
    vcvars=Path(vs)/'VC/Auxiliary/Build/vcvars64.bat'
    with tempfile.TemporaryDirectory(prefix='tabs-core-',dir=os.environ['TMPDIR']) as tmp:
        work=Path(tmp); exe=work/'tabs core.exe'
        sources=[ROOT/'tests/test_tabs_core_native.c']+[ROOT/'src'/f'{name}.c' for name in ('router','drag','document','textfile','markdown','ai','api','offset')]
        batch=work/'build.cmd'
        batch.write_text('@echo off\ncall "'+str(vcvars)+'" >nul || exit /b 1\ncl /nologo /std:c11 /utf-8 /W4 /WX /O1 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS '+ ' '.join('"'+str(p)+'"' for p in sources)+' /Fe:"'+str(exe)+'" /link user32.lib gdi32.lib comdlg32.lib shell32.lib winhttp.lib comctl32.lib advapi32.lib\nif errorlevel 1 exit /b 1\n"'+str(exe)+'"\n',encoding='utf-8')
        subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=work,check=True,timeout=90)
if __name__=='__main__':main()
