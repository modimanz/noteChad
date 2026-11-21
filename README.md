# nodeChad

A classic, single-document text editor for Windows with Markdown navigation and a local automation API. The portable editor is one native x64 executable. It does not replace Windows Notepad, register file associations, start an AI model or run a background server.

## Run

Double-click `bin\nodeChad.exe`, drag a text file onto its window, or open the supplied Markdown example:

```powershell
.\bin\nodeChad.exe .\examples\field-notes.md
```

| Action | Shortcut |
|---|---|
| New / Open / Save / Save As | Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| Find / Find next | Ctrl+F / F3 |
| Insert local time and date | F5 |
| Bold / inline code | Ctrl+B / Ctrl+backtick |
| Task list / heading outline | Ctrl+Shift+L / Ctrl+Shift+O |
| Reading preview | F6 |
| Request a local completion after enabling it in **AI** | Ctrl+Space |

The asterisk in the title marks unsaved edits. **New**, **Open** and **Close** ask before discarding them. Word wrap is on by default; switching it preserves text and selection but clears the native control's undo history.

## Control it from Hermes

The editor exposes **read, write, edit, save and load**. Start `--api` for an invisible scratch buffer or `--api-window` for a shared, visible editor. A normal double-click does not expose an API. The API only controls the instance its caller starts; it does not attach to other open documents. No listener, password, model or network connection is required.

- [File formats and save safety](docs/textfile.md) — documents up to 8 MiB; UTF-8 with or without BOM, and BOM-tagged UTF-16 LE/BE. Consistent LF/CRLF and final-newline presence are preserved.
- [Hermes MCP setup and tools](docs/hermes.md) — optional first-class tools for the agent.
- [Wire protocol and Python client](docs/api.md) — direct integration without MCP.

Read the current revision before changing anything. A stale edit is rejected, including when a human typed into the window between the read and the edit. **Write** changes the buffer; **save** is the separate disk operation. For an existing file, load it before saving to it. The API won't replace an unrelated file via Save As.

## Optional local AI

Use **AI → Enable local completion for this session**, then **Ctrl+Space** at the insertion point. Suggestions are reviewed before insertion. This uses a separately installed Ollama service; you don't need Ollama, Python or Hermes to edit or preview Markdown. See [AI setup and privacy](docs/ai.md) and [Markdown support](docs/markdown.md).

## Build and test

1. Install Visual Studio 2022 Build Tools (or Community), the **Desktop development with C++** workload, and a Windows SDK. A developer shell is not required.
2. Build from PowerShell in the project directory:

   ```powershell
   .\build.cmd
   ```

3. With Python 3 installed, exercise the finished executable and the live API/GUI interaction:

   ```powershell
   python tests/test_editor.py
   ```

The [design note](docs/design.md) compares existing clones and explains the optional MASM build. See [verification](docs/verification.md) for measured sizes and test coverage.

## Troubleshooting

- **File will not open or save:** keep the original. Use a supported Unicode encoding and consistent line endings. A changed-on-disk warning means another writer changed the source; use **Save As** to preserve both versions.
- **Revision conflict in Hermes:** read again, inspect the new text, and reapply only the intended change. Do not blindly resend old offsets.
- **AI unavailable:** check [AI setup](docs/ai.md). The editor and its automation API work without a model.
- **Build tools not found:** install the x64 MSVC toolchain and a Windows SDK, not just the Visual Studio IDE.
