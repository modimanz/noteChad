# nodeChad local AI completion

Optional, off-by-default completion using an already-running local Ollama server. The module is C/Win32 and has no third-party runtime libraries or model-management code.

## Setup and use

1. Start Ollama yourself with an **installed local model**. List the available models:

   ```cmd
   ollama list
   ```

   If no model is installed, choose and provision one yourself using Ollama's documentation. nodeChad does not install Ollama, download models, run shell commands, or start model-server processes. An explicit completion request asks the existing server to load/run the chosen model.

2. Set the exact installed model name in the shell used to start nodeChad:

   ```cmd
   set "NODECHAD_MODEL=your-installed-model-name"
   nodeChad.exe
   ```

   Replace the placeholder with a name from the list. Use only a local model, not an Ollama cloud model or a cloud-backed alias. For an entirely offline Ollama deployment, also disable Ollama's cloud features in its own configuration. nodeChad itself only connects to the numeric IPv4 loopback address below; it cannot govern a separately configured server's outbound behavior.

3. Enable AI completion for the current session (command ID **401**), place the caret with **no selection**, then request completion (ID **402**). Turning the toggle on does not connect, probe, list, or start models.
4. Review the suggestion. **No is the default. Only Yes inserts text.** Esc, closing the preview, rejection, or a stale document leaves the document unchanged. Accepted text uses the EDIT control's undoable insertion path.

| Setting | Recommended value | Notes |
| --- | --- | --- |
| `NODECHAD_MODEL` | Exact installed local model name | Required; 1–128 UTF-16 units. Read when requesting. Missing/oversized/invalid names produce setup guidance without connecting. |
| `NODECHAD_OLLAMA_PORT` | Unset (`11434`) | Optional fixture/custom local port. Digits only, range 1–65535. Not a host or URL. |

## Privacy and bounds

- Only an explicit enabled request creates a background worker and opens WinHTTP handles.
- The destination is fixed: `http://127.0.0.1:<port>/api/generate`. No DNS hostname, operator address, credential, configurable origin, proxy, redirects, cookies, or automatic authentication is used.
- The prompt is at most **2048 UTF-16 units immediately preceding the insertion caret**. Nothing after the caret is sent; the full document is not copied. Classic Unicode multiline EDIT storage is borrowed with `EM_GETHANDLE`/`LocalLock`, and only bounded context is copied. Selection is never moved to collect context.
- A context boundary drops a leading low surrogate rather than splitting its pair. A caret splitting a character is refused. Control characters other than tab/CR/LF are refused.
- Non-streaming JSON is requested with a 128-token prediction limit. Responses are capped at **64 KiB** and accepted suggestions at **4096 UTF-16 units**, including LF-to-CRLF normalization. Invalid UTF-8/JSON, duplicate response/done fields, embedded NUL, incomplete responses, and non-success HTTP status are errors, not suggestions.
- One request is outstanding at a time. Disabling AI invalidates it; it does not abruptly terminate an already-sent server request. Network work has finite connect/send/receive timeouts (2/5/15 seconds) and a 30-second elapsed check between body reads; this is not a strict 30-second wall-clock cancellation deadline.
- Prompt/result heap buffers are cleared when released. No prompt or completion logging is implemented. Ollama's own retention/logging policy is separate.

## Parent integration contract

Build `src/ai.c` with Unicode defines and link **`winhttp.lib`** in addition to the application's existing Win32 libraries. MSVC x64 `/O1 /MD` and `/O1 /MT` are tested. No C++, exceptions, elevation, manifest change, or shipped SDK/dependency is needed.

Use the exact declarations in `src/ai.h`:

```c
case 401: ai_toggle(owner); break;
case 402: ai_request(owner, edit, revision); break;
/* Window message handler: */
case WM_NODECHAD_AI:
    ai_result(owner, edit, revision, lParam);
    return 0;
```

`WM_NODECHAD_AI` is `WM_APP + 10`. Call the API on the owner UI thread, with a classic Unicode multiline EDIT (not RichEdit). Supply the **current** document revision at both calls. Increment a monotonic revision for every mutation, undo/redo, load/new/replacement, and API edit, even if text length is unchanged. Never reset/reuse revisions for a different document.

The worker posts an opaque result. The parent must pass it through without inspecting, freeing, or synthesizing it. `ai_result` atomically compares and consumes only a worker-published pointer before dereferencing it; arbitrary and immediately replayed LPARAMs are ignored. The module additionally binds results to the edit window property, original owner/edit handles, session generation, document revision, text length, empty selection, caret offset, and bounded context.

The preview is a modal MessageBox owned by `owner`. **All parent editing commands and API-window document operations must refuse changes while `IsWindowEnabled(owner)` is false.** This is required to keep the revision argument authoritative throughout the nested modal loop. The module rechecks caret/context/selection/session after approval; it cannot query a parent's revision variable again after that call. Do not mutate the document reentrantly during preview. The request/result path also refuses read-only or disabled controls.

Keep the normal owner message pump running while the application is alive, including for error/discard results, so posted allocations are consumed. On process exit Windows releases process memory; there is no public cancel/join API. Do not unload this module dynamically while its worker is running. If the owner is destroyed after a successful post but before message dispatch, one bounded pending result may remain until process exit.

## Tests

Run from the checkout using Python 3 and an installed MSVC x64 toolchain:

```cmd
python tests\test_ai.py
```

The runner uses the standard Community 2022 `vcvars64.bat`; override its path using `NODECHAD_VCVARS` if needed. Both `/MD` and `/MT` are compiled and executed as two test cases. Each case separately compiles the production translation unit and builds the native harness in a unique temporary object/output directory, with `/TC /O1 /W4 /WX /DUNICODE /D_UNICODE`, then removes those outputs. Python uses only its standard library and binds an ephemeral **127.0.0.1** HTTP fixture. Each executable sends exactly 17 synthetic requests, with the fixture checking their path and bounded context. The runner overrides the application's port for the child test executable. **No installed Ollama server, live model, model download, or internet connection is used by the tests.**

The native harness uses real classic EDIT controls and real WinHTTP transport; only MessageBox is substituted to select Yes/No deterministically. The fixture delays replies to check that request dispatch stays asynchronous. Coverage includes explicit approval/rejection, stale revisions/carets/selections, session invalidation, preview-time caret mutation, modal-owner refusal, missing model/invalid port, forged/replayed messages, redirects, HTTP errors, malformed/oversized/non-UTF-8/embedded-NUL/incomplete JSON, metadata skipping, surrogate pairs, all truncated prefixes of a valid response, and exact bounded prompt contents. Interactive visual layout and a live model's quality are not tested.

## Troubleshooting

- **Model setup notice:** Run the list command above, choose an installed local model, set the variable, and restart from that shell. Enabling the toggle does not validate the server.
- **Windows error / connection failure:** Ensure the local Ollama server is already running and the port is correct. The notice includes the Windows error code. There is no proxy or remote-host fallback.
- **HTTP 404:** Check the exact installed model name. nodeChad will not pull a missing model.
- **HTTP redirect:** Configure the local server to respond directly. Redirects are deliberately not followed.
- **Completion discarded:** Request again after finishing edits and leaving the caret at the intended empty selection. Even toggling off and back on invalidates the old suggestion.
- **Already pending:** Wait for the existing result. Disabling AI prevents its insertion but does not cancel the server operation.
- **Invalid/oversized completion:** Choose a model/server that implements Ollama's non-streaming generate JSON contract. Large responses or unsupported control characters are refused rather than silently truncated.
