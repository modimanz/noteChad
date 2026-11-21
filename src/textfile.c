#include "textfile.h"
#include <wchar.h>
#include <stdio.h>

static BOOL fail(WCHAR *reason, DWORD cap, const WCHAR *message) {
    if (reason && cap) wcsncpy_s(reason, cap, message, _TRUNCATE);
    return FALSE;
}
static void clear_reason(WCHAR *reason, DWORD cap) { if (reason && cap) reason[0] = 0; }
void tf_default(TextFormat *fmt) {
    if (fmt) { ZeroMemory(fmt, sizeof(*fmt)); fmt->encoding = TF_UTF8; fmt->newline = TF_CRLF; }
}
static BOOL absolute_path(const WCHAR *path, WCHAR *full) {
    DWORD n;
    if (!path || !*path) return FALSE;
    if (!((path[0] && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
          (path[0] == L'\\' && path[1] == L'\\' && path[2] != L'.' && path[2] != L'?'))) return FALSE;
    if (wcschr(path + (path[1] == L':' ? 2 : 0), L':')) return FALSE;
    n = GetFullPathNameW(path, TF_PATH_CAP, full, NULL);
    return n && n < TF_PATH_CAP;
}
static BOOL snapshot(HANDLE h, BY_HANDLE_FILE_INFORMATION *info, LARGE_INTEGER *change) {
    FILE_BASIC_INFO basic;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, info) ||
        !GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic))) return FALSE;
    if (info->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return FALSE;
    *change = basic.ChangeTime; return TRUE;
}
static BOOL same_info(const BY_HANDLE_FILE_INFORMATION *a, LARGE_INTEGER ac,
                      const BY_HANDLE_FILE_INFORMATION *b, LARGE_INTEGER bc) {
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
        a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow &&
        a->nFileSizeHigh == b->nFileSizeHigh && a->nFileSizeLow == b->nFileSizeLow &&
        CompareFileTime(&a->ftLastWriteTime, &b->ftLastWriteTime) == 0 && ac.QuadPart == bc.QuadPart;
}
static HANDLE open_read(const WCHAR *path) {
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}
static BOOL valid_units(const WCHAR *s, SIZE_T n) {
    SIZE_T i;
    for (i = 0; i < n; ++i) {
        unsigned u = s[i];
        if (!u) return FALSE;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (++i == n || s[i] < 0xDC00 || s[i] > 0xDFFF) return FALSE;
        } else if (u >= 0xDC00 && u <= 0xDFFF) return FALSE;
    }
    return TRUE;
}
BOOL tf_load(const WCHAR *path, WCHAR **text, TextFormat *fmt, WCHAR *reason, DWORD cap) {
    TextFormat *next = NULL; HANDLE h = INVALID_HANDLE_VALUE;
    BYTE *raw = NULL; WCHAR *decoded = NULL, *out = NULL;
    BY_HANDLE_FILE_INFORMATION after; LARGE_INTEGER change;
    DWORD got, size, offset = 0, payload; int count; SIZE_T i, j, expanded; BOOL lf = FALSE, crlf = FALSE, ok = FALSE;
    clear_reason(reason, cap); if (text) *text = NULL;
    if (!text || !fmt) return fail(reason, cap, L"Invalid load arguments.");
    next = LocalAlloc(LPTR, sizeof(*next)); if (!next) goto memory;
    tf_default(next);
    if (!absolute_path(path, next->originalPath)) { fail(reason, cap, L"An absolute document path is required."); goto done; }
    h = open_read(next->originalPath);
    if (h == INVALID_HANDLE_VALUE || !snapshot(h, &next->original, &next->changeTime)) {
        fail(reason, cap, L"Cannot open a regular text file (it may be busy or inaccessible)."); goto done;
    }
    if (next->original.nFileSizeHigh || next->original.nFileSizeLow > TF_BYTE_LIMIT) {
        fail(reason, cap, L"File exceeds the 8 MiB byte limit."); goto done;
    }
    size = next->original.nFileSizeLow;
    raw = LocalAlloc(LMEM_FIXED, (SIZE_T)size + 1); if (!raw) goto memory;
    if (!ReadFile(h, raw, size, &got, NULL) || got != size || !snapshot(h, &after, &change) ||
        !same_info(&next->original, next->changeTime, &after, change)) {
        fail(reason, cap, L"File changed or could not be read completely."); goto done;
    }
    if (size >= 4 && raw[0] == '+' && raw[1] == '/' && raw[2] == 'v' &&
        (raw[3] == '8' || raw[3] == '9' || raw[3] == '+' || raw[3] == '/')) {
        fail(reason, cap, L"UTF-7 is not supported."); goto done;
    }
    if (size >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) {
        next->encoding = TF_UTF8_BOM; offset = 3;
    } else if (size >= 2 && ((raw[0] == 0xff && raw[1] == 0xfe) || (raw[0] == 0xfe && raw[1] == 0xff))) {
        next->encoding = raw[0] == 0xff ? TF_UTF16_LE : TF_UTF16_BE; offset = 2;
    }
    payload = size - offset;
    if (next->encoding == TF_UTF16_LE || next->encoding == TF_UTF16_BE) {
        if (payload & 1) { fail(reason, cap, L"Truncated UTF-16 code unit."); goto done; }
        count = (int)(payload / 2);
    } else {
        count = payload ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)raw + offset, (int)payload, NULL, 0) : 0;
        if (payload && !count) { fail(reason, cap, L"Unsupported or invalid encoding; expected UTF-8 or BOM UTF-16."); goto done; }
    }
    decoded = LocalAlloc(LMEM_FIXED, ((SIZE_T)count + 1) * sizeof(WCHAR)); if (!decoded) goto memory;
    if (next->encoding == TF_UTF16_LE || next->encoding == TF_UTF16_BE) {
        for (i = 0; i < (SIZE_T)count; ++i) {
            BYTE a = raw[offset + i*2], b = raw[offset + i*2+1];
            decoded[i] = (WCHAR)(next->encoding == TF_UTF16_LE ? a | ((unsigned)b << 8) : ((unsigned)a << 8) | b);
        }
    } else if (count && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)raw + offset, (int)payload, decoded, count)) goto memory;
    decoded[count] = 0;
    if (!valid_units(decoded, (SIZE_T)count)) { fail(reason, cap, L"Embedded NUL or malformed Unicode is not supported."); goto done; }
    expanded = (SIZE_T)count;
    for (i = 0; i < (SIZE_T)count; ++i) {
        if (decoded[i] == L'\r') {
            if (i + 1 == (SIZE_T)count || decoded[i+1] != L'\n') { fail(reason, cap, L"Bare CR line endings are not supported."); goto done; }
            crlf = TRUE; ++i;
        } else if (decoded[i] == L'\n') { lf = TRUE; ++expanded; }
    }
    if (lf && crlf) { fail(reason, cap, L"Mixed LF and CRLF line endings would be lossy; file not opened."); goto done; }
    if (expanded > TF_EDIT_LIMIT) { fail(reason, cap, L"Normalized text exceeds the editor limit."); goto done; }
    out = LocalAlloc(LMEM_FIXED, (expanded + 1) * sizeof(WCHAR)); if (!out) goto memory;
    for (i = j = 0; i < (SIZE_T)count; ++i) {
        if (decoded[i] == L'\n' && (i == 0 || decoded[i-1] != L'\r')) out[j++] = L'\r';
        out[j++] = decoded[i];
    }
    out[j] = 0; next->newline = lf ? TF_LF : TF_CRLF; next->hasOriginal = TRUE;
    *fmt = *next; *text = out; out = NULL; ok = TRUE; goto done;
memory: fail(reason, cap, L"Not enough memory or Unicode conversion failed.");
done:
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    LocalFree(raw); LocalFree(decoded); LocalFree(out); LocalFree(next); return ok;
}
static LONG temp_counter;
static BOOL save_impl(const WCHAR *path, const WCHAR *text, TextFormat *fmt, BOOL check, BOOL newOnly,
                       WCHAR *reason, DWORD cap) {
    WCHAR *full = NULL, *temp = NULL, *serialized = NULL; BYTE *raw = NULL;
    HANDLE old = INVALID_HANDLE_VALUE, h = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info = {0}, latest, candidate; LARGE_INTEGER change = {0}, latestChange;
    SIZE_T n, i, j, parent; DWORD size, wrote, error; int bytes; BOOL exists = FALSE, published = FALSE, ok = FALSE;
    clear_reason(reason, cap);
    if (!text || !fmt || fmt->encoding < TF_UTF8 || fmt->encoding > TF_UTF16_BE || (fmt->newline != TF_LF && fmt->newline != TF_CRLF))
        return fail(reason, cap, L"Invalid save arguments or unsupported format.");
    for (n = 0; n <= TF_EDIT_LIMIT && text[n]; ++n) {}
    if (n > TF_EDIT_LIMIT || !valid_units(text, n)) return fail(reason, cap, L"Text is too long or contains malformed Unicode.");
    full = LocalAlloc(LPTR, TF_PATH_CAP * sizeof(WCHAR)); temp = LocalAlloc(LPTR, TF_PATH_CAP * sizeof(WCHAR));
    if (!full || !temp) goto memory;
    if (!absolute_path(path, full)) { fail(reason, cap, L"An absolute document path is required."); goto done; }
    if (check && (!fmt->hasOriginal || CompareStringOrdinal(full, -1, fmt->originalPath, -1, TRUE) != CSTR_EQUAL)) {
        fail(reason, cap, L"Conflict: no original snapshot for this path; use Save As."); goto done;
    }
    old = newOnly ? INVALID_HANDLE_VALUE : open_read(full);
    if (newOnly) SetLastError(ERROR_FILE_NOT_FOUND);
    if (old == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        if (check || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
            fail(reason, cap, L"Conflict or inaccessible target; disk file was not replaced."); goto done;
        }
    } else {
        exists = TRUE;
        if (!snapshot(old, &info, &change) || (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) {
            fail(reason, cap, L"Target is not a writable regular file."); goto done;
        }
        if (check && !same_info(&fmt->original, fmt->changeTime, &info, change)) {
            fail(reason, cap, L"Conflict: file changed on disk; reload or Save As."); goto done;
        }
    }
    serialized = LocalAlloc(LMEM_FIXED, (n + 1) * sizeof(WCHAR)); if (!serialized) goto memory;
    for (i = j = 0; i < n; ++i) {
        if (text[i] == L'\r') {
            if (i + 1 == n || text[i+1] != L'\n') { fail(reason, cap, L"Save requires EDIT-normalized CRLF text (no bare CR)."); goto done; }
            if (fmt->newline == TF_CRLF) serialized[j++] = L'\r';
        } else if (text[i] == L'\n' && (i == 0 || text[i-1] != L'\r')) {
            fail(reason, cap, L"Save requires EDIT-normalized CRLF text (no bare LF)."); goto done;
        } else serialized[j++] = text[i];
    }
    if (fmt->encoding == TF_UTF16_LE || fmt->encoding == TF_UTF16_BE) {
        if (j > (TF_BYTE_LIMIT - 2) / 2) { fail(reason, cap, L"Serialized UTF-16 exceeds 8 MiB."); goto done; }
        size = (DWORD)(j * 2 + 2); raw = LocalAlloc(LMEM_FIXED, size); if (!raw) goto memory;
        raw[0] = fmt->encoding == TF_UTF16_LE ? 0xff : 0xfe; raw[1] = fmt->encoding == TF_UTF16_LE ? 0xfe : 0xff;
        for (i = 0; i < j; ++i) {
            BYTE lo = (BYTE)(serialized[i] & 255), hi = (BYTE)(serialized[i] >> 8);
            raw[2+i*2] = fmt->encoding == TF_UTF16_LE ? lo : hi;
            raw[3+i*2] = fmt->encoding == TF_UTF16_LE ? hi : lo;
        }
    } else {
        DWORD bom = fmt->encoding == TF_UTF8_BOM ? 3 : 0;
        bytes = j ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, serialized, (int)j, NULL, 0, NULL, NULL) : 0;
        if ((j && !bytes) || (DWORD)bytes > TF_BYTE_LIMIT - bom) { fail(reason, cap, L"Serialized file exceeds 8 MiB or cannot be encoded."); goto done; }
        size = (DWORD)bytes + bom; raw = LocalAlloc(LMEM_FIXED, (SIZE_T)size + 1); if (!raw) goto memory;
        if (bom) { raw[0] = 0xef; raw[1] = 0xbb; raw[2] = 0xbf; }
        if (bytes && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, serialized, (int)j, (char *)raw + bom, bytes, NULL, NULL)) goto memory;
    }
    parent = (SIZE_T)(wcsrchr(full, L'\\') - full) + 1;
    if (parent + 90 >= TF_PATH_CAP) { fail(reason, cap, L"Path is too long for a sibling temporary file."); goto done; }
    wcsncpy_s(temp, TF_PATH_CAP, full, parent);
    for (i = 0; i < 100; ++i) {
        swprintf_s(temp + parent, TF_PATH_CAP - parent, L".note-chad-%lu-%llu-%lu.tmp", GetCurrentProcessId(),
                   GetTickCount64(), (DWORD)InterlockedIncrement(&temp_counter));
        h = CreateFileW(temp, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                         NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) { temp[0] = 0; fail(reason, cap, L"Cannot create a sibling temporary file."); goto done; }
    if (!WriteFile(h, raw, size, &wrote, NULL) || wrote != size || !FlushFileBuffers(h) || !GetFileInformationByHandle(h, &candidate)) {
        fail(reason, cap, L"Cannot write/flush temporary file; original was not replaced."); goto done;
    }
    CloseHandle(h); h = INVALID_HANDLE_VALUE;
    if (exists) {
        HANDLE probe = open_read(full);
        BOOL unchanged = probe != INVALID_HANDLE_VALUE && snapshot(probe, &latest, &latestChange) && same_info(&info, change, &latest, latestChange);
        if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
        if (!unchanged) { fail(reason, cap, L"Conflict: target changed during save."); goto done; }
        CloseHandle(old); old = INVALID_HANDLE_VALUE;
        if (!ReplaceFileW(full, temp, NULL, 0, NULL, NULL)) { fail(reason, cap, L"Atomic replacement failed; inspect target before retrying."); goto done; }
    } else if (!MoveFileExW(temp, full, MOVEFILE_WRITE_THROUGH)) {
        fail(reason, cap, L"Cannot publish new file; target may already exist."); goto done;
    }
    published = TRUE;
    h = open_read(full);
    if (h == INVALID_HANDLE_VALUE || !snapshot(h, &latest, &latestChange) ||
        latest.dwVolumeSerialNumber != candidate.dwVolumeSerialNumber ||
        latest.nFileIndexHigh != candidate.nFileIndexHigh || latest.nFileIndexLow != candidate.nFileIndexLow ||
        latest.nFileSizeHigh != 0 || latest.nFileSizeLow != size) {
        fmt->hasOriginal = FALSE; fail(reason, cap, L"File published, but target changed or identity refresh failed; reload before saving again."); goto done;
    }
    fmt->original = latest; fmt->changeTime = latestChange;
    wcscpy_s(fmt->originalPath, TF_PATH_CAP, full); fmt->hasOriginal = TRUE; ok = TRUE; goto done;
memory: fail(reason, cap, L"Not enough memory or Unicode conversion failed.");
done:
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    if (temp && *temp && !published) DeleteFileW(temp);
    LocalFree(full); LocalFree(temp); LocalFree(serialized); LocalFree(raw); return ok;
}
BOOL tf_save(const WCHAR *path, const WCHAR *text, TextFormat *fmt, BOOL check, WCHAR *reason, DWORD cap) {
    return save_impl(path, text, fmt, check, FALSE, reason, cap);
}
BOOL tf_save_new(const WCHAR *path, const WCHAR *text, TextFormat *fmt, WCHAR *reason, DWORD cap) {
    return save_impl(path, text, fmt, FALSE, TRUE, reason, cap);
}
const WCHAR *tf_format(const TextFormat *fmt) {
    static const WCHAR *labels[4][2] = {
        {L"UTF-8 (CRLF)", L"UTF-8 (LF)"},
        {L"UTF-8 BOM (CRLF)", L"UTF-8 BOM (LF)"},
        {L"UTF-16 LE (CRLF)", L"UTF-16 LE (LF)"},
        {L"UTF-16 BE (CRLF)", L"UTF-16 BE (LF)"}
    };
    if (!fmt || fmt->encoding < TF_UTF8 || fmt->encoding > TF_UTF16_BE ||
        (fmt->newline != TF_CRLF && fmt->newline != TF_LF)) return L"Unknown format";
    return labels[fmt->encoding - 1][fmt->newline == TF_LF ? 1 : 0];
}
