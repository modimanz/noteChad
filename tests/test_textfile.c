#include "../src/textfile.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static int failures, checks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s (winerr %lu)\n", __LINE__, #x, GetLastError()); } } while (0)
static WCHAR path[TF_PATH_CAP], root[TF_PATH_CAP], why[256];
static TextFormat fmt;
static void put(const void *data, DWORD size) {
    DWORD n = 0; HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE); if (h == INVALID_HANDLE_VALUE) return;
    CHECK(WriteFile(h, data, size, &n, NULL) && n == size); CHECK(CloseHandle(h));
}
static void bytes(const void *data, DWORD size) {
    BYTE buf[1024]; DWORD n = 0; HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE); if (h == INVALID_HANDLE_VALUE) return;
    CHECK(ReadFile(h, buf, sizeof(buf), &n, NULL)); CHECK(n == size && !memcmp(buf, data, size)); CloseHandle(h);
}
static void roundtrip(void) {
    WCHAR *text = NULL;
    const char input[] = "one\n\xE2\x98\x83\n";
    put(input, sizeof(input)-1);
    CHECK(tf_load(path, &text, &fmt, why, 256));
    CHECK(text && !wcscmp(text, L"one\r\n\x2603\r\n"));
    if (text) { CHECK(tf_save(path, text, &fmt, TRUE, why, 256)); LocalFree(text); }
    bytes(input, sizeof(input)-1);
}
static void fixture(const void *data, DWORD size, const WCHAR *expected, DWORD encoding, DWORD newline) {
    WCHAR *text = NULL; put(data, size);
    CHECK(tf_load(path, &text, &fmt, why, 256));
    CHECK(text && !wcscmp(text, expected)); CHECK(fmt.encoding == encoding && fmt.newline == newline);
    { const WCHAR *labels[] = {L"", L"UTF-8", L"UTF-8 BOM", L"UTF-16 LE", L"UTF-16 BE"}; CHECK(wcsstr(tf_format(&fmt), labels[encoding]) != NULL); }
    if (text) { CHECK(tf_save(path, text, &fmt, TRUE, why, 256)); CHECK(tf_save(path, text, &fmt, TRUE, why, 256)); LocalFree(text); }
    bytes(data, size);
}
static void encodings(void) {
    static const BYTE le[] = {0xff,0xfe, 'a',0, 10,0, 0x3d,0xd8,0,0xde};
    static const BYTE be[] = {0xfe,0xff, 0,'a', 0,13,0,10, 0xd8,0x3d,0xde,0};
    static const BYTE leEmpty[] = {0xff,0xfe};
    fixture("", 0, L"", TF_UTF8, TF_CRLF);
    fixture("abc", 3, L"abc", TF_UTF8, TF_CRLF);
    fixture("a\r\n\r\n", 5, L"a\r\n\r\n", TF_UTF8, TF_CRLF);
    fixture("\xef\xbb\xbf" "a\n", 5, L"a\r\n", TF_UTF8_BOM, TF_LF);
    fixture("\xef\xbb\xbf", 3, L"", TF_UTF8_BOM, TF_CRLF);
    fixture(le, sizeof(le), L"a\r\n\xd83d\xde00", TF_UTF16_LE, TF_LF);
    fixture(be, sizeof(be), L"a\r\n\xd83d\xde00", TF_UTF16_BE, TF_CRLF);
    fixture(leEmpty, sizeof(leEmpty), L"", TF_UTF16_LE, TF_CRLF);
}
static void rejects(const void *data, DWORD size) {
    WCHAR *text = (WCHAR *)1; TextFormat *before = LocalAlloc(LMEM_FIXED, sizeof(fmt));
    *before = fmt; put(data, size); why[0] = 0;
    CHECK(!tf_load(path, &text, &fmt, why, 256)); CHECK(text == NULL); CHECK(why[0] != 0);
    CHECK(!memcmp(before, &fmt, sizeof(fmt))); LocalFree(before); bytes(data, size);
}
static void invalid_encodings(void) {
    static const BYTE nul[] = {'a',0,'b'}, overlong[] = {0xc0,0xaf}, ansi[] = {0xe9},
        utf32[] = {0xff,0xfe,0,0,65,0,0,0}, odd[] = {0xff,0xfe,65},
        surrogate[] = {0xff,0xfe,0,0xd8};
    rejects(nul, sizeof(nul)); rejects(overlong, sizeof(overlong)); rejects(ansi, sizeof(ansi));
    rejects(utf32, sizeof(utf32)); rejects(odd, sizeof(odd)); rejects(surrogate, sizeof(surrogate));
    rejects("a\r\nb\n", 6); rejects("a\rb", 3);
    rejects("+/v8-text", 9); /* UTF-7 signature must not masquerade as UTF-8. */
}
static void conflicts_and_failures(void) {
    BYTE securityBefore[4096], securityAfter[4096]; DWORD securityBeforeSize = 0, securityAfterSize = 0;
    WCHAR *text = NULL; HANDLE lock; FILETIME creation, write; BY_HANDLE_FILE_INFORMATION before, after;
    put("first", 5); CHECK(tf_load(path, &text, &fmt, why, 256)); LocalFree(text);
    put("other", 5); CHECK(!tf_save(path, L"edited", &fmt, TRUE, why, 256)); CHECK(wcsstr(why, L"Conflict") != NULL); bytes("other", 5);
    CHECK(tf_load(path, &text, &fmt, why, 256)); LocalFree(text);
    CHECK(DeleteFileW(path)); CHECK(!tf_save(path, L"edited", &fmt, TRUE, why, 256));
    CHECK(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
    put("first", 5); CHECK(tf_save(path, L"explicit", &fmt, FALSE, why, 256)); bytes("explicit", 8);
    CHECK(tf_load(path, &text, &fmt, why, 256)); LocalFree(text);
    CHECK(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY));
    CHECK(!tf_save(path, L"edited", &fmt, TRUE, why, 256)); bytes("explicit", 8);
    CHECK(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
    CHECK(tf_load(path, &text, &fmt, why, 256)); LocalFree(text);
    lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(lock != INVALID_HANDLE_VALUE); CHECK(!tf_save(path, L"edited", &fmt, TRUE, why, 256)); bytes("explicit", 8); CloseHandle(lock);
    CHECK(tf_load(path, &text, &fmt, why, 256)); LocalFree(text);
    lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(lock != INVALID_HANDLE_VALUE); CHECK(GetFileTime(lock, &creation, NULL, &write));
    { DWORD n; CHECK(WriteFile(lock, "external", 8, &n, NULL) && n == 8); CHECK(SetFileTime(lock, &creation, NULL, &write)); }
    CloseHandle(lock); CHECK(!tf_save(path, L"edited", &fmt, TRUE, why, 256)); bytes("external", 8);
    CHECK(SetFileAttributesW(path, FILE_ATTRIBUTE_HIDDEN));
    lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(GetFileInformationByHandle(lock, &before)); CloseHandle(lock);
    CHECK(GetFileSecurityW(path, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, securityBefore, sizeof(securityBefore), &securityBeforeSize));
    CHECK(tf_load(path, &text, &fmt, why, 256)); LocalFree(text);
    CHECK(tf_save(path, L"saved", &fmt, TRUE, why, 256)); bytes("saved", 5);
    lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(GetFileInformationByHandle(lock, &after)); CloseHandle(lock);
    CHECK(CompareFileTime(&before.ftCreationTime, &after.ftCreationTime) == 0);
    CHECK((after.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK(GetFileSecurityW(path, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, securityAfter, sizeof(securityAfter), &securityAfterSize));
    CHECK(securityBeforeSize == securityAfterSize && !memcmp(securityBefore, securityAfter, securityBeforeSize));
    CHECK(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
}
static void limits_and_arguments(void) {
    WCHAR *text = NULL, *large = LocalAlloc(LPTR, ((SIZE_T)TF_EDIT_LIMIT + 2) * sizeof(WCHAR));
    BYTE *manyLF = LocalAlloc(LMEM_FIXED, TF_BYTE_LIMIT); HANDLE h; LARGE_INTEGER size; SIZE_T i;
    tf_default(&fmt); CHECK(fmt.encoding == TF_UTF8 && fmt.newline == TF_CRLF && !fmt.hasOriginal);
    CHECK(!tf_save(path, L"x", &fmt, TRUE, why, 256));
    CHECK(!tf_load(L"relative.txt", &text, &fmt, why, 256)); CHECK(text == NULL);
    CHECK(!tf_save(L"C:relative.txt", L"x", &fmt, FALSE, why, 256));
    CHECK(!tf_save(path, L"bad\n", &fmt, FALSE, why, 256));
    CHECK(!tf_save(path, L"bad\r", &fmt, FALSE, why, 256));
    CHECK(!tf_save(path, L"\xd800", &fmt, FALSE, why, 256));
    CHECK(!tf_load(path, NULL, &fmt, why, 256));
    CHECK(!tf_save(path, L"x", NULL, FALSE, why, 256));
    CHECK(!tf_load(path, &text, &fmt, NULL, 0) || text != NULL); LocalFree(text); text = NULL;
    CHECK(large && manyLF); if (!large || !manyLF) { LocalFree(large); LocalFree(manyLF); return; }
    for (i = 0; i <= TF_EDIT_LIMIT; ++i) large[i] = L'x';
    CHECK(!tf_save(path, large, &fmt, FALSE, why, 256));
    large[TF_EDIT_LIMIT] = 0; large[0] = 0x2603;
    CHECK(!tf_save(path, large, &fmt, FALSE, why, 256));
    large[0] = L'x'; CHECK(tf_save(path, large, &fmt, FALSE, why, 256));
    CHECK(tf_load(path, &text, &fmt, why, 256)); CHECK(text && wcslen(text) == TF_EDIT_LIMIT); LocalFree(text); text = NULL;
    large[0] = L'x'; fmt.encoding = TF_UTF16_LE;
    CHECK(!tf_save(path, large, &fmt, FALSE, why, 256)); tf_default(&fmt);
    memset(manyLF, '\n', TF_BYTE_LIMIT); put(manyLF, TF_BYTE_LIMIT);
    CHECK(!tf_load(path, &text, &fmt, why, 256)); CHECK(text == NULL);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); size.QuadPart = (LONGLONG)TF_BYTE_LIMIT + 1;
    CHECK(h != INVALID_HANDLE_VALUE); CHECK(SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h)); CloseHandle(h);
    CHECK(!tf_load(path, &text, &fmt, why, 256)); CHECK(text == NULL);
    LocalFree(large); LocalFree(manyLF);
}
static void no_temps(void) {
    WCHAR pattern[TF_PATH_CAP]; WIN32_FIND_DATAW data; HANDLE h;
    swprintf_s(pattern, TF_PATH_CAP, L"%s\\.note-chad-*.tmp", root);
    h = FindFirstFileW(pattern, &data); CHECK(h == INVALID_HANDLE_VALUE); if (h != INVALID_HANDLE_VALUE) FindClose(h);
}
static HANDLE start;
static LONG winners;
static DWORD WINAPI writer(void *arg) {
    TextFormat *f = LocalAlloc(LPTR, sizeof(*f)); WCHAR reason[256];
    (void)arg; if (!f) return 1; tf_default(f);
    WaitForSingleObject(start, INFINITE);
    if (tf_save_new(path, L"winner\r\n", f, reason, 256)) InterlockedIncrement(&winners);
    LocalFree(f); return 0;
}
static void new_only(void) {
    HANDLE threads[12]; DWORD i;
    DeleteFileW(path); tf_default(&fmt);
    CHECK(tf_save_new(path, L"new", &fmt, why, 256)); bytes("new", 3);
    CHECK(!tf_save_new(path, L"replacement", &fmt, why, 256)); bytes("new", 3);
    DeleteFileW(path); winners = 0; start = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < 12; ++i) { threads[i] = CreateThread(NULL, 0, writer, NULL, 0, NULL); CHECK(threads[i] != NULL); }
    SetEvent(start); CHECK(WaitForMultipleObjects(12, threads, TRUE, 30000) == WAIT_OBJECT_0);
    CHECK(winners == 1); bytes("winner\r\n", 8);
    for (i = 0; i < 12; ++i) CloseHandle(threads[i]); CloseHandle(start);
}
int wmain(int argc, WCHAR **argv) {
    if (argc != 2) return 2;
    wcscpy_s(root, TF_PATH_CAP, argv[1]);
    swprintf_s(path, TF_PATH_CAP, L"%s\\text.txt", root);
    tf_default(&fmt); roundtrip(); new_only(); encodings(); invalid_encodings(); conflicts_and_failures(); limits_and_arguments(); no_temps();
    DeleteFileW(path);
    printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
