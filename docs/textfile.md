# Native text-file I/O

## Integrate

1. Compile `src/textfile.c` as C with the Windows SDK, Unicode defines, and `_WIN32_WINNT >= 0x0601`. Production uses Win32/CRT only; no third-party libraries or elevation.
2. Initialize one `TextFormat` per document with `tf_default`. Set the Unicode EDIT control's `EM_SETLIMITTEXT` to `TF_EDIT_LIMIT` (8,388,608 UTF-16 code units). The terminating NUL is additional.
3. Pass absolute drive or UNC paths to `tf_load`. On success, pass its CRLF-normalized, NUL-terminated buffer to EDIT and release it with `LocalFree`. On failure, `*text` is NULL and the supplied format is unchanged. Keep the current document until load succeeds.
4. Retrieve NUL-terminated EDIT text and call `tf_save(..., TRUE, ...)` for ordinary Save after successful load/save. Keep the same `TextFormat`; the snapshot binds to its canonical absolute path.
5. For Save As, use `tf_save_new` to **never overwrite an existing target**, including one created concurrently. Only after explicit overwrite confirmation use `tf_save(..., FALSE, ...)` for that other target. FALSE permits overwrite but does not bypass malformed text, read-only-file, or busy-file checks.
6. On success, the format contains the new target snapshot; subsequent ordinary Saves use TRUE. Display `tf_format` as a static, non-owned label. Display the supplied `reason` on failure; do not silently retry with checks disabled.

The APIs are synchronous. Do not share a mutable `TextFormat` between simultaneous operations. Independent documents may be saved concurrently. Format fields are public for C ABI convenience, but treat snapshot/path fields as opaque. `TextFormat` contains a large fixed path buffer; avoid putting many copies on a small thread stack.

## Supported representations

- Strict UTF-8, with or without a preserved BOM.
- BOM-tagged UTF-16 little-endian and big-endian, with validated surrogate pairs.
- Uniform CRLF or LF disk endings; loading expands LF to CRLF for EDIT. Saving requires CRLF-normalized EDIT input and restores the detected disk style. No newline is added or removed, including the final newline and trailing blank lines. Files without any line ending default to CRLF for future edits.
- Empty files, BOM-only files, and supplementary Unicode characters.

Reject malformed Unicode, embedded NUL on disk, bare CR, mixed LF/CRLF, UTF-32, recognized UTF-7 signatures, and non-UTF-8 legacy bytes. No fallback code page or encoding guess is used. An ASCII-compatible encoding whose bytes happen to be valid UTF-8 is inherently indistinguishable; it is interpreted as UTF-8. A literal UTF-8 document beginning with a recognized UTF-7 signature is conservatively refused. BOMless UTF-16 is not detected.

A save receives a C string: its first NUL is the terminator, not a representable document character. Callers must not supply a buffer containing hidden text after that terminator.

## Limits and file safety

Both input and serialized output are capped at **8 MiB**, including BOM bytes. Normalized editor text is independently capped at `TF_EDIT_LIMIT` UTF-16 units. A disk file below the byte limit can still exceed the text limit after LF expansion; conversely, editable text can exceed the serialized byte limit in UTF-8 or UTF-16. Such operations fail before replacing the target.

Save creates an exclusively named sibling temporary file, writes the complete encoded content, and calls `FlushFileBuffers` before publication. Existing targets use `ReplaceFileW` without ignore-ACL/merge flags. This retains Windows-supported destination metadata, including its DACL and creation time; tests also check hidden attributes. File contents intentionally get a new last-write time. New targets use same-directory `MoveFileExW` **without `MOVEFILE_REPLACE_EXISTING` or cross-volume copy**, so publication fails atomically if the name is already occupied. `tf_save_new` never enters the replacement branch. New files inherit directory security.

Known conflicts are rejected using volume/file identity, byte size, last-write time, NTFS-style change time, and canonical path. The target is checked on initial save admission and again just before replacement. An open read handle denies concurrent content writers while the temporary file is prepared; it must be closed for Windows replacement. Published file identity is read back and checked against the temporary file before claiming success. Read-only files, final-component reparse points, directories, device namespaces, alternate streams, and relative paths are refused. Parent directories are not created.

### Boundaries

- Existing-target checks are **optimistic, not a filesystem compare-and-swap**. Windows `ReplaceFileW` does not accept an expected file ID. Another process can exchange or modify the target between the final check and replacement; there is also a short post-publication observation interval. This is not protection against an adversarial writer. By contrast, new-only publication cannot replace an existing name even in a race.
- Parent-directory junctions and hard-link aliases are not resolved/rejected. Atomic replacement replaces one directory entry, not every hard link to the old content. Device-prefix (`\\?\\` / `\\.\\`) paths are intentionally not accepted. Long paths require normal Windows/application long-path support; there is no manifest change here.
- Atomicity/durability follow Windows and the underlying filesystem. Temporary-file flush is exercised, but power-loss recovery, network filesystem behavior, disk-full/physical-I/O fault injection, EFS/ADS merge behavior, and rare partial `ReplaceFileW` failure states are not qualified. A replacement failure explicitly asks the caller to inspect the target rather than asserting it is unchanged. No operation opens the original with truncate/create-always.
- After successful publication, a failed/conflicting identity refresh returns FALSE with a reason stating that publication occurred and clears `hasOriginal`. Reload before retrying. An operation returning FALSE is therefore not an unconditional guarantee that publication never happened.
- Normal failure cleanup removes owned temporary files. Process crashes can leave sibling `.note-chad-*.tmp` files. Automatic crash-recovery deletion is not implemented.

## Build and tests

Requirements: Windows, Python 3 with standard library, MSVC x64 C compiler, and Windows SDK. The runner calls the installed Visual Studio 2022 Community `VC/Auxiliary/Build/vcvars64.bat` under Program Files. Adjust that runner path for another installation. Production needs no extra link library; the metadata tests additionally link Windows `advapi32.lib`.

From the project root:

```console
python tests/test_textfile_build.py
python tests/test_textfile_build.py --repeat 10
python tests/test_textfile_build.py --static
```

The runner builds using `/W4 /WX /O1`, Unicode defines, `_WIN32_WINNT=0x0601`, and `/MD` (or `/MT` with `--static`). Artifacts and real-file fixtures are isolated under `%LOCALAPPDATA%/hermes/cache/scratch` and removed after normal completion. No GUI or elevated privileges are needed.

Verified: **307 checks per execution, zero failures**, ten repeated `/MD` executions and one `/MT` execution. Coverage includes encoding/BOM byte-for-byte round trips, empty and unterminated files, CRLF/LF and final newline, surrogate validation, invalid encodings and mixed endings, exact/over-limit sizes, conflict/deletion/read-only/locked-target failures, same-size external changes with restored last-write time, explicit overwrite, creation-time/DACL preservation, temporary-file cleanup, and twelve simultaneous new-only writers with exactly one successful publisher.

## Troubleshooting

- **Conflict:** reload the disk version or choose a new Save As path. Never automatically disable the original-file check.
- **File busy / replacement failed:** close other applications' non-delete-sharing handles, inspect/reload the target, and retry deliberately.
- **Unsupported encoding / mixed endings:** convert a copy with an encoding-aware editor first. This module deliberately does not guess or normalize lossily.
- **Size limit:** split the document or use a larger-file editor; raising only the EDIT limit does not raise the serialization budget.
- **Compiler not found:** install the MSVC C++ x64 tools and Windows SDK or point the test runner's `vcvars` variable at the installed toolchain.
