# nodeChad

A native Windows text and Markdown editor with draggable tabs, a local automation API, and opt-in AI completion. The editor is a single x64 executable. It does not replace Windows Notepad or change your default editor.

## Install or run portable

[Download the Windows x64 installer](https://github.com/modimanz/noteChad/releases/latest/download/nodeChad-Setup-x64.msi). It installs for the current user, adds **nodeChad** to Start, and can be removed from **Settings → Apps → Installed apps**. The MSI is unsigned. For a portable copy, download `nodeChad.exe` from the same [release](https://github.com/modimanz/noteChad/releases/latest). Neither requires Python.

## Try it from source

[Build the editor](#build-and-test), then double-click `bin\nodeChad.exe` or open the supplied Markdown example:

```powershell
.\bin\nodeChad.exe .\examples\field-notes.md
```

| Action | Shortcut |
|---|---|
| New tab / Close tab | Ctrl+T / Ctrl+W |
| Next tab / Previous tab | Ctrl+Tab / Ctrl+Shift+Tab |
| New window / Move current tab to new window | Ctrl+Shift+N / Ctrl+Shift+M |
| New / Open / Save / Save As | Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| Find / Find next | Ctrl+F / F3 |
| Insert local time and date | F5 |
| Bold / inline code | Ctrl+B / Ctrl+backtick |
| Task list / heading outline | Ctrl+Shift+L / Ctrl+Shift+O |
| Reading preview | F6 |
| Request a local completion after enabling it in **AI** | Ctrl+Space |

Drag a tab onto another nodeChad window’s **tab strip** to move it, along its own strip to reorder it, or onto the desktop to give it a window of its own. The tab keeps its unsaved text, selection and undo history. Drop multiple files from Explorer to open them as tabs.

An asterisk marks a tab with unsaved edits. Closing a tab or window asks before discarding those edits. Word wrap is on by default; switching wrapping preserves text and selection but clears the native control’s undo history. Hermes-controlled windows stay single-document: tab gestures never redirect an agent to another document.

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
