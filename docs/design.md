# Architecture choices

We considered two existing open-source implementations rather than assuming Notepad needed a rewrite:

- **Notepad3** builds a Notepad-like Windows editor on Scintilla. Its README identifies the project as BSD-3-Clause.[1] This is the fork to consider if language lexers and code folding become more important than footprint.
- **wesmar/notepad** implements a Notepad clone in assembly against the Win32 API and bypasses the C runtime.[2] This is a useful reference for what an assembly-first build entails; the editor is not just a text-scanning loop.

**Decision:** use original C with Win32 controls. Windows supplies the text editor, clipboard, menus, file dialogs and preview control. No upstream editor code, widget library, web engine or model is bundled. A standard-input/output control session uses the same in-memory document as the window. Markdown and AI aren't separate file editors that could silently disagree about unsaved text.

## Assembly experiment

`src/offset.c` and `src/offset.asm` implement the same operation: translate a logical character offset from the API to the Windows editor's UTF-16 offset, accounting for CRLF and surrogate pairs. The default and MASM builds differ only in that implementation. Compare their **final executable sizes** rather than using source-line counts or extrapolating this one routine to an entire editor. The size-optimized C build uses link-time optimization and Windows' built-in Universal CRT, with static compiler support code; the target is x64 Windows 10/11, without a separate Visual C++ Redistributable installation. WinHTTP is delay-loaded so the editor does not load its AI transport just to edit a file.

```powershell
.\build.cmd
.\build.cmd asm
Get-Item .\bin\noteChad*.exe | Select-Object Name,Length
```

## Sources

[1] https://github.com/rizonesoft/Notepad3 — rizonesoft/Notepad3
[2] https://github.com/wesmar/notepad — wesmar/notepad
