"""Run real MSVC x64 C tests. Artifacts/fixtures stay in Hermes scratch."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--repeat', type=int, default=1)
parser.add_argument('--static', action='store_true', help='Use /MT rather than /MD')
args = parser.parse_args()
if args.repeat < 1:
    parser.error('--repeat must be positive')
runtime = '/MT' if args.static else '/MD'
root = Path(__file__).resolve().parents[1]
scratch = Path(os.environ['LOCALAPPDATA']) / 'hermes/cache/scratch'
scratch.mkdir(parents=True, exist_ok=True)
vcvars = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat')
with tempfile.TemporaryDirectory(prefix='textfile-', dir=scratch) as tmp:
    tmp = Path(tmp)
    exe = tmp / 'test_textfile.exe'
    command = (f'call "{vcvars}" >nul && cl /nologo /W4 /WX /O1 {runtime} /DUNICODE /D_UNICODE '
               f'/D_WIN32_WINNT=0x0601 /Fe:"{exe}" /Fo:"{tmp.as_posix()}/" '
               f'"{root / "tests/test_textfile.c"}" "{root / "src/textfile.c"}" advapi32.lib')
    runner = tmp / 'run.cmd'
    runner.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    result = subprocess.run(['cmd.exe', '/d', '/c', str(runner)], cwd=tmp)
    result.check_returncode()
    for _ in range(args.repeat):
        subprocess.run([str(exe), str(tmp)], cwd=root, check=True)
