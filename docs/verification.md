# Windows x64 verification

`bin/nodeChad.exe` and the optional `bin/nodeChad-asm.exe` both measured **76,288 bytes (74.5 KiB)** in the tested MSVC x64 builds. Replacing the C offset routine with the MASM version did not reduce the final PE size. The comparison is for this routine, not a claim about a hypothetical all-assembly editor.

`dumpbin /dependents` showed Windows OS and Universal CRT imports, with `WINHTTP.dll` delay-loaded. No model, browser runtime or Python interpreter is part of the editor executable.

| Check | Entry point | What was exercised |
|---|---|---|
| Native editor | `python tests/test_editor.py` | Launch, edit, save and reopen a Markdown file; unsaved GUI changes visible to the API; reject stale changes, unrelated overwrite, changed-on-disk save and dirty load; Undo of Markdown/API selection replacement. |
| File I/O | `python tests/test_textfile_build.py` | 307 native checks on real temporary files, including both common Unicode encodings, round trips, write conflicts and concurrent exclusive creation. |
| Markdown | `python tests/test_markdown.py` | Source-preserving outline/preview, fenced blocks, keyboard navigation, tasks, and selection offsets beyond 64K, against actual Windows controls. |
| Local AI | `python tests/test_ai.py` | Real WinHTTP with an isolated loopback fixture, exact request contents, accept/reject, stale results and failure handling; **no inference model invoked**. |
| Stdio | `python -m unittest discover -s tests -p "test_api_transport*.py"` | Production protocol with a native message-loop fixture, Python client, Unicode, framing/limits and rejection. |
| Hermes MCP | See command below | SDK initialization and discovery; all five tools against the compiled app, two simultaneous mutations, bounded read pages, allowed paths, and persistence to disk. |
| C vs. MASM | `tests\offsets.cmd` | Both implementations agree on CRLFs, Unicode surrogate pairs, insertion at EOF and out-of-range indices. |

Run the MCP check in PowerShell from the project root with the optional `mcp` package installed:

```powershell
$env:NODECHAD_MCP_EXE = (Resolve-Path .\bin\nodeChad.exe).Path
python -m unittest discover -s tests -p 'test_mcp*.py' -v
```

**Scope:** Windows x64, local filesystem. Markdown preview renders a subset, not all of CommonMark. This is a note editor, not a large-file editor: files are capped at 8 MiB and very long lines can be slow. AI model output quality, other CPU architectures, power-failure durability and hostile concurrent writers were not tested.
