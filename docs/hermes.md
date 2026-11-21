# nodeChad tools for Hermes

Run the optional Python stdio MCP bridge to expose `read`, `write`, `edit`, `save`, and `load` as actual Hermes tools. Python and the MCP SDK belong to the bridge process only; they are not linked, embedded, or required by the standalone GUI.

## Requirements

- A built `bin/nodeChad.exe` supporting `--api` and `--api-window`.
- `tools/nodechad_mcp.py` and its sibling `tools/nodechad_client.py` from the same checkout.
- Python with the optional official `mcp` package. Both SDK 1.30.0 and 2.0.0 have been exercised with the synthetic-app tests below.
- Hermes with the official `hermes mcp add` command.

The bridge starts an isolated, blank editor **on the first valid tool call**, not during initialization or tool discovery. It does not attach to an existing editor or inspect any user document automatically. One bridge process owns one editor buffer shared by callers of that bridge. It is not a multi-document service.

## Setup and registration

The commands below use Bash, including Git Bash on Windows. Replace the example paths with absolute paths. Registration is an explicit operator action: tests and setup probes do not register a server or change Hermes configuration.

1. Select a Python interpreter and verify the optional SDK. Hermes' own Python may already include it; discover its executable from an activated Hermes environment rather than assuming a location.

   ```bash
   python -c "import sys, importlib.metadata; print(sys.executable); print(importlib.metadata.version('mcp'))"
   ```

   If needed, create a separate bridge environment in the checkout; do not add dependencies to the GUI build:

   ```bash
   uv venv .venv-mcp --python 3.11
   uv pip install --python .venv-mcp/Scripts/python.exe 'mcp>=1.30,<3'
   ```

   On non-Windows hosts, the virtualenv Python is `.venv-mcp/bin/python`; the native GUI itself is Windows-only.

2. Set the paths and run the tests **before registration**. The default suite launches a synthetic inert Python application through real stdio MCP, so it does not require a GUI build or touch real user files.

   ```bash
   REPO='C:/path/to/note_chad'
   BRIDGE_PY='C:/path/to/python.exe'
   cd "$REPO"
   "$BRIDGE_PY" tools/nodechad_mcp.py --help
   "$BRIDGE_PY" -m unittest discover -s tests -p 'test_mcp*.py' -v
   ```

3. Inspect the official Hermes registration syntax. `--args` consumes the remaining arguments and **must be last among Hermes options**.

   ```bash
   hermes mcp add --help
   ```

4. Register an in-memory-only server. This command changes the active Hermes profile, so run it only when you want that registration:

   ```bash
   hermes mcp add nodechad --command "$BRIDGE_PY" --connect-timeout 30 --args "$REPO/tools/nodechad_mcp.py" --exe "$REPO/bin/nodeChad.exe"
   hermes mcp test nodechad
   ```

   To enable file operations instead, create a dedicated directory and add `--allow-root` to the bridge arguments when registering. For example, use the following **instead of** the add command above:

   ```bash
   mkdir -p "$REPO/sandbox"
   hermes mcp add nodechad --command "$BRIDGE_PY" --connect-timeout 30 --args "$REPO/tools/nodechad_mcp.py" --exe "$REPO/bin/nodeChad.exe" --allow-root "$REPO/sandbox"
   ```

   Repeat `--allow-root` to permit multiple existing directories. Do not allow a broad home directory merely for convenience. No environment-variable inheritance or working-directory assumptions are needed: the interpreter, server script, executable, and allowed roots are explicit absolute paths.

5. Start a new Hermes conversation or use `/reload-mcp` to refresh discovery. Hermes prefixes these tools as `mcp_nodechad_read`, `mcp_nodechad_write`, and so forth. A connection/discovery test is not a provider inference test.

   Remove the registration when it is no longer needed:

   ```bash
   hermes mcp remove nodechad
   ```

See the [official Hermes MCP documentation](https://hermes-agent.nousresearch.com/docs/user-guide/features/mcp). CLI instructions above were checked against `hermes mcp add --help`, `hermes mcp test --help`, and `hermes mcp remove --help`. The test suite does not change your registration.

## Tool contract

All calls return one MCP text-content block containing JSON. Failures set MCP `isError: true` and include `error`, `retryable: false`, and an actionable `action`. Structured native errors also include `revision` and `dirty`. A successful mutation returns `{ "revision": ..., "dirty": ..., "text": "" }`; use `read` to verify buffer contents after a change and inspect the saved file when disk persistence matters.

| Tool | Arguments | Behavior |
| --- | --- | --- |
| `read` | Optional `start=0`, `max_chars=16000` | Return LF-normalized text, `revision`, `dirty`, `start`, `total_chars`, `truncated`, and `next_start`. Offset/limit count Unicode code points. |
| `write` | Required `expected_revision`, `text` | Replace the entire in-memory buffer. Does not save. |
| `edit` | Required `expected_revision`, `start`, `end`, `text` | Replace `[start,end)`. Count Unicode code points, **not** UTF-8 bytes, UTF-16 units, or visual graphemes. Equal offsets insert text. |
| `save` | Required `expected_revision`; optional `path=""` | Save to an absolute allowed path, potentially overwriting that file. Empty/omitted path reuses the last successful **bridge-approved** load/save path. No approved path means an error. |
| `load` | Required `expected_revision`, absolute `path` | Replace the buffer from disk. Dirty buffers must be saved first; there is no implicit discard. |

Read first, inspect the relevant text, and supply that exact revision to every mutation, including `save` and `load`. Example tool arguments, using an illustrative revision of 1:

```json
{"name":"read","arguments":{}}
{"name":"write","arguments":{"expected_revision":1,"text":"Hello 😀\n"}}
```

Do not assume 1 is the actual revision. A stale error is not permission to substitute the returned revision and replay the request. Read and reassess the current contents and the user's intent first. The bridge serializes access with a lock and forwards the caller's exact revision; it never retries a mutation. Native revision checks still protect a visible editor from a UI change between the bridge's inspection and its request.

### Limits and lifecycle

- Read bodies contain at most 16,000 Unicode code points per call. Follow `next_start` to page, checking that all page revisions match. Discard a mixed-revision snapshot and read again.
- Write/edit inputs are capped at 1 MiB of UTF-8 per call. NUL, unpaired surrogates, boolean revisions, invalid offsets, and unknown arguments are rejected. Error messages are capped at 1,000 code points.
- The native client bounds wire payloads at 32 MiB. Paging limits the MCP result, not the size of the native read: each page retrieves a full native snapshot before slicing. JSON escaping can expand the serialized response but remains bounded by the page limit.
- Disk access is disabled unless allowed roots are configured. Paths must be absolute, and resolved paths must remain under an allowed root. Empty save always sends the approved absolute path explicitly, so a visible editor's manually changed current file is not silently adopted.
- Root checks prevent ordinary traversal and symlink escapes, **not** hostile concurrent filesystem replacement, hard-link aliasing, or other same-user attacks. Use a dedicated trusted directory and ordinary OS permissions; this is not an OS sandbox.
- Hidden mode is the default. EOF/normal shutdown closes that owned editor and discards any unsaved buffer; there is no automatic save. Add `--visible` explicitly to use `--api-window`; its editor remains open after bridge EOF and must be closed by the operator.
- A broken/uncertain transport latches the session unavailable instead of silently respawning an empty editor with recycled revisions. Stop and manually reconcile outcomes. Restarting or reconnecting creates a new blank session; revision tokens are not durable across bridge processes.
- Calls to the native client are synchronous inside a worker thread. An unresponsive native application can stall the session; a caller timeout does not undo a possibly completed write. Stop the bridge/owned hidden editor and reconcile manually rather than replaying the operation.

## Repeatable native integration test

After building the native executable, run the same real MCP initialization, discovery, pagination, concurrent mutation, stale-write, Unicode-edit, save/load, and refusal cases against the actual app/client pair:

```bash
cd "$REPO"
NODECHAD_MCP_EXE="$REPO/bin/nodeChad.exe" "$BRIDGE_PY" -m unittest discover -s tests -p 'test_mcp.py' -v
```

The opt-in path must exist or the suite fails. Native integration uses hidden mode and only test-created temporary documents and allowed roots. Without `NODECHAD_MCP_EXE`, `test_mcp.py` injects a test-only agreed-interface adapter and launches `tests/test_mcp_surrogate.py --app`; this exercises MCP and the bridge over real subprocess pipes but **does not qualify the production native client or GUI**. `test_mcp_bridge.py` checks bridge lifecycle and policy without requiring the SDK.

For an independent optional SDK 1.x compatibility run:

```bash
uv run --no-project --with 'mcp>=1.30,<2' python -m unittest discover -s tests -p 'test_mcp*.py' -v
```

These tests make no model/provider requests, do not claim an end-to-end inference, and do not modify Hermes configuration.

### Qualification

The final suite passed 10 tests with the synthetic app under SDK 2.0.0 and 10 under SDK 1.30.0. The explicit native integration command also passed all 5 MCP transport cases under each SDK against the available `bin/nodeChad.exe` and production `nodechad_client.py`. With site packages disabled, all 5 dependency-free policy tests passed and all 5 SDK transport cases were explicitly skipped. These receipts qualify initialization/discovery and the exercised tool paths, not Hermes registration or provider inference.

An exploratory 16,010-emoji single-line write exceeded the test's 15-second call deadline. The bounded-pagination test uses the same code-point count split over short lines and passes natively. Very long single-line rendering remains a native performance concern; the bridge does not retry a timed-out mutation. Saved files may use native CRLF formatting even though API reads normalize to LF.

## Troubleshooting

- **Optional MCP SDK missing:** choose an interpreter where `import mcp` succeeds, or install into the separate bridge venv. `--help` and bridge policy tests do not require the SDK.
- **Tools discover but first read fails:** initialization is deliberately lazy. Check the executable exists, supports `--api`, and has its runtime files available. Do not mistake successful discovery for successful editor launch.
- **Tools do not appear in Hermes:** run `hermes mcp test nodechad`, check the active profile and absolute command paths, then use `/reload-mcp` or start a new conversation. Never hand-edit a live config to guess at registration syntax.
- **Disk access disabled/outside allowed roots:** configure the intended dedicated directory using `--allow-root`; supply an absolute path inside it. Do not weaken the default merely to suppress an error.
- **Dirty-buffer load refused:** save the buffer first, using another allowed file if necessary to preserve the file you intend to load.
- **Stale revision or unknown outcome:** read/reconcile; never blindly retry. For a latched transport error, restart the bridge only after accounting for possible disk effects and unsaved text.
- **SDK tests skipped:** the selected Python lacks the optional SDK; a skipped suite is not transport qualification. Run with the SDK-enabled interpreter.
