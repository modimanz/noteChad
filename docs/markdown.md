# Native Markdown commands

`src/markdown.c` adds Unicode Win32 Markdown commands to the caller's classic multiline `EDIT`. Navigation and preview read a copy of the text; neither rewrites the source, changes its selection, nor marks it modified. A heading jump changes only the selection and scroll position.

## Integration

1. Compile `src/markdown.c` as C with `/utf-8`, `UNICODE`, and `_UNICODE`. MSVC x64 `/O1 /MD` and `/O1 /MT` are exercised by the isolated tests.
2. Link `user32.lib`, `gdi32.lib`, and `comctl32.lib`. Rich Edit is loaded at runtime from the Windows system directory (`Msftedit.dll`); there is no Rich Edit import-library dependency, resource file, browser, renderer process, network access, or script engine.
3. Include `markdown.h` and call the following from the UI thread for the relevant `WM_COMMAND` IDs:

   ```c
   BOOL md_command(HWND owner, HWND edit, UINT id);
   ```

   Pass the owning top-level window and its current Unicode multiline `EDIT`. Keep the source window alive while its views are in use. Views are owned windows and are destroyed with their owner. No source-window subclass or text-ownership transfer occurs.
4. Install menus and accelerators in the host. The module does not create or intercept host accelerators.

| ID | Header constant | Host shortcut | Action |
|---|---|---|---|
| 301 | `MD_BOLD` | Ctrl+B | Wrap selection in `**`; select the original contents. With an empty selection, insert `****` and position the caret inside. |
| 302 | `MD_INLINE_CODE` | Ctrl+` | Wrap in backticks; use a longer delimiter than any selected backtick run and padding at backtick boundaries. Empty selection inserts two backticks with the caret inside. |
| 303 | `MD_TASK` | Ctrl+Shift+L | Operate on the current line or selected physical lines. Add an unchecked task, or toggle an existing `[ ]` / `[x]` / `[X]` task. Preserve indentation and existing bullet/ordered markers. |
| 304 | `MD_HEADING_NAVIGATION` | Ctrl+Shift+O | Open/recreate the native heading list, initially choosing the heading nearest the caret. |
| 305 | `MD_READING_PREVIEW` | F6 | Open/recreate the read-only reading snapshot. |

The return value is `TRUE` for a completed recognized command and `FALSE` for an unknown command or a refusal/failure. The host may display its own nonmodal failure status. Editing commands refuse read-only controls and changes exceeding the source `EM_GETLIMITTEXT` limit. They send `EM_REPLACESEL(TRUE)` so normal edit notifications and the control's native undo mechanism remain in effect.

## Keyboard workflow

- **Outline:** arrow keys, Home/End and native list-box type-ahead choose a heading; Enter or double-click jumps and closes the outline. Esc closes and returns focus to the source. Ctrl+R refreshes.
- **Preview:** native selection, Ctrl+C, scrolling and Page Up/Down work without altering the source. Esc closes and returns focus. F6 or Ctrl+R recreates a snapshot from the current source. There is also a standard close button and Alt+F4.
- Both windows are modeless, resizable, and clearly labeled. One outline and one preview are retained per process; opening another replaces the previous window of that kind. The old snapshot remains intact until a replacement is successfully constructed.
- An outline detects source changes before a jump. It refreshes its entries and requires choosing/jumping again instead of using stale offsets.

## Supported Markdown

- ATX headings `#` through `######`, with zero to three leading spaces, required whitespace/end after the opening marker, and optional whitespace-delimited closing hashes.
- Backtick and tilde fences of at least three markers. Closing fences must match the marker, meet/exceed the opening length, and have only trailing whitespace. Backtick info strings containing backticks do not open a fence. Unclosed fences extend to EOF. Fenced headings never appear in navigation.
- Preview heading sizes, paragraph spacing, hanging-indented bullet/ordered lists, unchecked/checked task glyphs, shaded monospace fenced blocks, simple `**bold**` and matching-run inline code.
- CRLF, LF and CR physical line endings are understood by the parser. Editing preserves the selected source's existing text and line endings.
- Links, image syntax, HTML and URLs are inert text. Rich Edit automatic URL detection is explicitly disabled. Source bytes are inserted as Unicode text, never as RTF or HTML.

## Tests

Run from the project directory on Windows with Visual Studio 2022 Community's C++ tools and the Windows SDK installed:

```console
python tests/test_markdown.py
```

The runner uses the supplied VS 2022 Community `vcvars64.bat` location. It needs only Python's standard library. It creates disposable build directories under the Hermes scratch directory, compiles the production translation unit independently with `/W4 /WX /analyze`, then builds and executes the native tests with both `/MD` and `/MT`. No project build files, application data, network services, or existing editor instances are used.

The test executable includes the implementation to exercise private parsing helpers and creates its own real Unicode `EDIT`, list-box, and Rich Edit windows. Five test groups cover headings/fences, editor commands, styled/source-preserving views, edge cases/stale outlines, and positions beyond 64K plus repeated refresh/owner cleanup. Tests require an interactive Windows desktop. They exercise messages and inspect native control text, styles and selection ranges; they do not qualify high-DPI visual appearance or the host's menu/accelerator routing.

## Limits and troubleshooting

- This is a small Markdown subset, not a CommonMark implementation. Setext headings, nested block parsing, tables, emphasis nesting, escape/entity decoding, images and clickable links are not implemented. Four-space indented ATX headings are excluded; indented code blocks are not otherwise parsed. Fences are recognized at document level, not inside list/blockquote containers.
- Snapshot/command input is capped at 8 Mi UTF-16 code units. Work is synchronous on the UI thread; very large or heavily formatted documents may pause it. This is not incremental or live rendering.
- **Undo limitation:** classic Windows multiline `EDIT` does not provide a reliable transactional replacement undo through `EM_REPLACESEL(TRUE)`. A direct native-control baseline on this system restored deleted text for single-line EDIT, but multiline EDIT undo removed the inserted replacement without restoring the old selection. The module deliberately does not replace the caller's undo implementation. The tests verify undo availability, not full restoration. Hosts needing lossless multi-step undo must provide it separately.
- Task commands are textual line operations, including inside code fences; only navigation/reading interpretation ignores fenced headings. They add/toggle task syntax rather than removing list markers. A selection ending at the next line's start excludes that next line.
- Only outline labels are truncated (500 UTF-16 units); source offsets and preview text are not truncated to that label length.
- If a command returns `FALSE`, check that the source is a live Unicode multiline EDIT, the source is not read-only for editing operations, and both the source text limit and module cap permit the operation. For preview failures, confirm that the Windows system `Msftedit.dll` is available. No download or elevation is attempted.
- If a view seems stale, press F6/Ctrl+R or reopen it. That is expected for a labeled snapshot. A changed outline deliberately refreshes before allowing a jump.
