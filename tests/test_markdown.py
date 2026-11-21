"""Build/run isolated Unicode Win32 C tests; no project build changes."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VCVARS = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")

def main():
    source = ROOT / "src" / "markdown.c"
    if not source.exists():
        raise AssertionError("Markdown heading implementation missing (RED)")
    scratch = Path.home() / "AppData/Local/hermes/cache/scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="test-markdown-", dir=scratch) as temp:
        work = Path(temp)
        batch = work / "build.cmd"
        batch.write_text(
            '@echo off\ncall "' + str(VCVARS) + '" >nul\n'
            'if errorlevel 1 exit /b 1\n'
            'cl /nologo /W4 /WX /std:c11 /utf-8 /O1 /MD /DUNICODE /D_UNICODE /analyze /c '
            '"' + str(source) + '"\n'
            'if errorlevel 1 exit /b 1\n'
            'cl /nologo /W4 /WX /std:c11 /utf-8 /O1 /MD /DUNICODE /D_UNICODE '
            '"' + str(ROOT / 'tests/test_markdown.c') + '" '
            '/Fe:"' + str(work / 'test_markdown.exe') + '" '
            'user32.lib gdi32.lib comctl32.lib\n'
            'if errorlevel 1 exit /b 1\n'
            '"' + str(work / 'test_markdown.exe') + '"\n'
            'if errorlevel 1 exit /b 1\n'
            'cl /nologo /W4 /WX /std:c11 /utf-8 /O1 /MT /DUNICODE /D_UNICODE '
            '"' + str(ROOT / 'tests/test_markdown.c') + '" '
            '/Fe:"' + str(work / 'test_markdown_mt.exe') + '" '
            'user32.lib gdi32.lib comctl32.lib\n'
            'if errorlevel 1 exit /b 1\n'
            '"' + str(work / 'test_markdown_mt.exe') + '"\n', encoding="utf-8")
        subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=work, check=True, timeout=90)

if __name__ == "__main__":
    main()
