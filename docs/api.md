# nodeChad stdio API

Control one isolated nodeChad editor over its inherited stdin/stdout pipes. The API executes only `read`, `write`, `edit`, `save`, and `load`; it has no shell, arbitrary-code command, listener, network dependency, or MCP runtime.

## Requirements and quick start

- Build nodeChad for Windows x64 with MSVC, including `src/api.c` and `user32.lib`. Static CRT (`/MT`) is supported.
- Use Python 3.8+ for the dependency-free client in `tools/nodechad_client.py`.
- Launch the executable directly, with binary stdin/stdout pipes; never through a shell. Reserve stdout exclusively for the protocol. Send diagnostics to stderr.

Place `tools` on Python's import path, then run:

```python
from pathlib import Path
from nodechad_client import NodeChad, ApiError

exe = Path('bin/nodeChad.exe').resolve()
target = Path('new-note.txt').resolve()  # Use a new path, not another existing file.

with NodeChad(exe, visible=False) as nc:
    snapshot = nc.request('read')
    changed = nc.request('write', rev=snapshot.revision, text='Hello\nworld 😀\n')
    # Offsets count Unicode code points in the LF-normalized document.
    changed = nc.request('edit', rev=changed.revision, start=6, end=11, text='reader')
    saved = nc.request('save', rev=changed.revision, text=str(target))
    assert saved.text == ''
    assert nc.request('read').text == 'Hello\nreader 😀\n'
```

Read first to obtain the live revision; every non-read request must supply an exact revision. GUI edits can invalidate it before the next request. On `ApiError`, inspect `.revision`, `.dirty`, and `.text`; read again and decide whether to retry. Do not blindly retry a mutation after `ProtocolError`: the pipe may have failed after the effect occurred.

```python
with NodeChad(exe, visible=True) as nc:
    current = nc.request('read')
    try:
        nc.request('load', rev=current.revision, text=str(target))
    except ApiError as error:
        print(error.revision, error.dirty, error.text)
# EOF detaches the API; the visible editor remains open.
```

`request(verb, rev=0, start=0, end=0, text='')` returns `Response` with `.revision`, `.dirty`, and `.text`. Successful mutations have empty text. Coordinator errors raise `ApiError` with the same fields and `.response`; framing, decoding, and pipe failures raise `ProtocolError`. Invalid local arguments raise `ValueError` before writing anything. Calls are synchronous and serialized; there is no request deadline or background polling.

## Instance lifecycle

| Launch option | Window | On stdin EOF |
|---|---|---|
| `--api` | Hidden, isolated editor | Exit and discard unsaved changes without prompting |
| `--api-window` | Visible, isolated editor | Leave the editor open; detach API |

The client closes stdin on context exit and never implicitly saves. For hidden instances it waits up to five seconds, then terminates an unresponsive owned child (two-second terminate grace before kill). For visible instances it closes its pipe endpoints without terminating or waiting for the window; `.process` remains available to its caller. The host implements these window/EOF policies; the transport only posts `WM_NODECHAD_EOF` when its worker exits. Clean EOF between packets emits no response.

## Wire protocol

All headers are printable ASCII terminated by one LF byte, with exactly one space between fields. Numeric fields contain 1–20 decimal digits, no sign, and fit unsigned 64-bit. Leading zeros are accepted. Headers including LF are at most 128 bytes. Header CRLF is **not** accepted; CRLF in text payloads may be normalized by the document coordinator.

Request:

```text
verb rev start end bytes\n
<exactly bytes UTF-8 bytes>
```

Response:

```text
OK rev dirty bytes\n
<exactly bytes UTF-8 bytes>
```

or:

```text
ERR rev dirty bytes\n
<exactly bytes UTF-8 error-reason bytes>
```

There is no delimiter after a payload. A subsequent header may immediately follow it. `dirty` is exactly `0` or `1`. Each payload is limited to 33,554,432 bytes (32 MiB); this is a byte limit, not a character limit. Embedded NUL, malformed UTF-8, UTF-8-encoded surrogates, overlong encodings, and code points above U+10FFFF are rejected. Empty payloads are valid except for `load`.

| Verb | Revision | Offsets | Payload | Success body |
|---|---|---|---|---|
| `read` | Must be `0` | Both `0` | Empty | Entire LF-normalized document |
| `write` | Exact live revision | Both `0` | Replacement document | Empty |
| `edit` | Exact live revision | `start <= end` | Replacement text | Empty |
| `save` | Exact live revision | Both `0` | Destination path; empty means current path | Empty |
| `load` | Exact live revision | Both `0` | Nonempty source path | Empty |

`edit` uses Unicode code-point offsets in LF-normalized text, with an exclusive end. These are neither UTF-8 byte offsets nor UTF-16 code-unit offsets; an astral emoji counts as one code point. Range validation, text normalization, exact revision matching, and changes to revision/dirty/path state belong to the coordinator. Paths are literal Unicode paths, not commands or shell expansions. Use an absolute local drive path; the application rejects relative paths, device names, alternate data streams and UNC paths on the API.

A malformed or oversized packet causes an `ERR` and terminal transport shutdown, not stream resynchronization. Before reporting such an error, the worker sends a separate valid `read` to obtain current revision/dirty state and frees its document reply. The malformed operation itself is never dispatched. If the editor cannot claim even that snapshot request, the error uses the last observed status (initially revision zero/clean). Coordinator `ERR` responses do not close the channel.

## Host integration contract

Call `api_start(owner)` once from the editor's GUI thread after the editor is ready. It rejects non-pipe stdin/stdout and foreign-thread/foreign-process windows. It starts one blocking worker and assumes ownership of stdin/stdout handles; it closes them when the channel ends. No main-thread polling is required.

Handle messages on the **same live editor**:

```c
case WM_NODECHAD_API: {
    ApiRequest *request;
    if (!api_owns(lParam)) return 0; /* Check BEFORE dereference, exactly once. */
    request = (ApiRequest *)lParam;
    /* Apply request synchronously on this GUI thread.
       Set request->ok, ->current, ->dirty and optionally ->reply.
       reply must be NULL or a NUL-terminated LocalAlloc allocation. */
    return 0;
}
case WM_NODECHAD_EOF:
    /* Hidden: destroy window without save prompt.
       Visible: keep window/editor open. No pointer is carried by this message. */
    return 0;
```

`api_owns` checks the GUI-thread identity and atomically claims only the exact currently published worker pointer. It rejects NULL, arbitrary pointers and repeated claims without dereferencing them. Never use a raw `WM_NODECHAD_API` lParam before this check, and never stash a request for asynchronous processing.

The worker owns the request and UTF-16 payload; both remain valid only until the synchronous `SendMessageW` returns. Treat payload as immutable. The GUI fills `BOOL ok`, `BOOL dirty`, `unsigned long long current`, and optional UTF-16 `reply`. Allocate reply with `LocalAlloc`; the worker frees it with `LocalFree` even for mutation replies whose successful body is forced empty. Do not free or retain the request/payload, or retain reply after returning. Invalid/oversized UTF-16 replies fail the channel.

The coordinator must:

1. Match every non-read revision against the current live document before changing anything. Set revision/dirty in both successful and error responses.
2. Normalize text to LF for read and offset calculations; safely translate code-point ranges to Win32 EDIT UTF-16/CRLF representation.
3. Implement `write`/`edit`/`load`/`save` against the existing editor, not a parallel buffer or temporary second document.
4. Refuse a save-as destination that already belongs to another existing file. Do not silently overwrite it; enforce this in the filesystem operation rather than a racy pre-check. Saving the document's current file is permitted under the coordinator's normal policy.
5. Apply its documented load/unsaved-change policy without silently losing changes or blocking hidden automation behind a modal prompt.
6. Return a reason on failure; keep successful mutation responses empty and reads LF-normalized. The transport does not execute filesystem operations itself.
7. Keep pumping GUI messages while the worker may be inside `SendMessageW`. Do not join the worker from the GUI thread.

## Tests

Run from the checkout on a Windows host with MSVC x64 Build Tools installed:

```text
python -m unittest discover -s tests -p "test_api_transport*.py" -v
```

The runner discovers MSVC via `vswhere`, builds with `/W4 /WX /MT`, and uses the Hermes scratch directory for temporary build products. The native fixture uses a real Win32 message loop and the production transport; it intentionally implements only read/write document behavior, returning coordinator errors for edit/load/save. Tests cover same-document Unicode round-trips, one-shot pointer claims, fragmented input, revision conflicts, all allowed verb routing, malformed framing/UTF-8/NUL, bounds, EOF, Python response validation, structured errors and hidden-client cleanup. Full document-operation/visible-window acceptance belongs to the assembled application tests, not this transport fixture.

## Troubleshooting

- **Launch/start fails:** Supply an executable path, not a shell command. Check that stdin/stdout are binary pipes, the GUI calls `api_start`, and no diagnostics contaminate stdout.
- **Revision conflict:** Read the current document and revision; recompute the intended edit instead of resubmitting stale offsets.
- **Bad edit location:** Count code points after LF normalization, not Windows CRLF units or UTF-16 units.
- **Malformed packet closes the channel:** Start a new isolated instance. Terminal framing errors are not recoverable in place.
- **Save-as rejected:** Choose a new destination or explicitly open the intended existing document first. The API has no force-overwrite option.
- **Request hangs:** The client deliberately blocks; verify the GUI is pumping messages and not displaying an automation-blocking modal dialog. An external supervisor can terminate its own hidden child.
- **Build test cannot locate MSVC:** Install the x64 C++ Build Tools component and the Visual Studio Installer (`vswhere`); these tests require a real Windows toolchain.
