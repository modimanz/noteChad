# Windows x64 verification

`bin/noteChad.exe` and the optional `bin/noteChad-asm.exe` both measured **99,328 bytes (97 KiB)** for v0.1.2 in the tested MSVC x64 builds. Replacing the C offset routine with the MASM version did not reduce the final PE size. The comparison is for this routine, not a claim about a hypothetical all-assembly editor.

`dumpbin /dependents` showed Windows OS and Universal CRT imports, with `WINHTTP.dll` delay-loaded. No model, browser runtime or Python interpreter is part of the editor executable.

| Check | Entry point | What was exercised |
|---|---|---|
| Tabs and windows | `python tests/test_tabs_core.py`, `python tests/test_router.py`, `python tests/test_window_drag.py` | Two separate launches share the normal window workspace. A dragged dirty tab keeps its native editor, selection, undo and original path. Cancelling a window close preserves unsaved tabs. |
| Split Markdown workspace | `python tests/test_split_workspace.py` | Live unsaved rendering without touching disk; switching/moving tabs; divider and keyboard controls; Cancel/Save on a background tab; a canceled Save As; oversize preview recovery; isolated API writes refused during GUI Open. |
| Close button | `python tests/test_tabclose_build.py`, `python tests/test_tabs_core_native.py`, `python tests/test_drag_build.py` | Native glyph pixels and hit rectangles, press/release, capture loss, stale tab identity and overflow; click a dirty background tab’s × and cancel without losing either tab. |
| Installer | `python tests/test_installer.py`, `python installer/verify.py` | Read back per-user MSI tables, extract the embedded cabinet and compare its executable with the build and SHA-256 receipt. |
| Native editor | `python tests/test_editor.py` | Launch, edit, save and reopen a Markdown file; unsaved GUI changes visible to the API; reject stale changes, unrelated overwrite, changed-on-disk save and dirty load; Undo of Markdown/API selection replacement. |
| File I/O | `python tests/test_textfile_build.py` | 307 native checks on real temporary files, including both common Unicode encodings, round trips, write conflicts and concurrent exclusive creation. |
| Markdown | `python tests/test_markdown.py` | Source-preserving outline/preview, fenced blocks, keyboard navigation, tasks, and selection offsets beyond 64K, against actual Windows controls. |
| Local AI | `python tests/test_ai.py` | Real WinHTTP with an isolated loopback fixture, exact request contents, accept/reject, stale results and failure handling; **no inference model invoked**. |
| Stdio | `python -m unittest discover -s tests -p "test_api_transport*.py"` | Production protocol with a native message-loop fixture, Python client, Unicode, framing/limits and rejection. |
| Hermes MCP | See command below | SDK initialization and discovery; all five tools against the compiled app, two simultaneous mutations, bounded read pages, allowed paths, and persistence to disk. |
| C vs. MASM | `tests\offsets.cmd` | Both implementations agree on CRLFs, Unicode surrogate pairs, insertion at EOF and out-of-range indices. |

Run the MCP check in PowerShell from the project root with the optional `mcp` package installed:

```powershell
$env:NOTECHAD_MCP_EXE = (Resolve-Path .\bin\noteChad.exe).Path
python -m unittest discover -s tests -p 'test_mcp*.py' -v
```

The v0.1.0 MSI was also installed for a Windows user. Windows Installer reported the product installed at version `0.1.0`; the installed executable matched the release SHA-256, and the Start shortcut pointed to that executable. The five tab tests and four editor/API tests passed against the installed copy. The subsequent upgrade to v0.1.1 was exercised on the same account: the previous MSI registration, shortcut, installed executable and Open With command were removed. The new shortcut pointed to `noteChad.exe`, its SHA-256 matched the build receipt, and its live window caption ended in **noteChad**. Standalone uninstall and Explorer’s app picker were not exercised live.

**Scope:** Windows x64, local filesystem. Markdown preview renders a subset, not all of CommonMark. This is a note editor, not a large-file editor: files are capped at 8 MiB and very long lines can be slow. AI model output quality, other CPU architectures, power-failure durability and hostile concurrent writers were not tested.
