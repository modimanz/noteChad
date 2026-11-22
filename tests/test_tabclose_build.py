"""Build/run only test-owned native tab-close windows; artifacts stay in scratch."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
scratch = Path(os.environ['LOCALAPPDATA']) / 'hermes/cache/scratch'
scratch.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                              'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                              '-property', 'installationPath'], text=True).strip()
if not vs:
    raise SystemExit('MSVC x64 tools not installed')
vcvars = Path(vs) / 'VC/Auxiliary/Build/vcvars64.bat'
with tempfile.TemporaryDirectory(prefix='tabclose-', dir=scratch) as directory:
    tmp = Path(directory)
    exe = tmp / 'test tabclose.exe'
    command = (f'call "{vcvars}" >nul && cl /nologo /std:c11 /W4 /WX /O1 /MT '
               f'/DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"{root / "src"}" '
               f'/Fe:"{exe}" /Fo:"{tmp.as_posix()}/" '
               f'"{root / "tests/test_tabclose.c"}" "{root / "src/drag.c"}" '
               '/link comctl32.lib user32.lib gdi32.lib')
    runner = tmp / 'run.cmd'
    runner.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(runner)], cwd=tmp, check=True)
    subprocess.run([str(exe)], cwd=tmp, check=True, timeout=30)
