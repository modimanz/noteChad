#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static int box_count;
static int box_reply = IDNO;
static HWND preview_move_edit;
static WCHAR last_box[8192];
static UINT last_flags;
static int WINAPI test_message_box(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags)
{
    (void)owner; (void)title;
    lstrcpynW(last_box, text, 8192);
    last_flags = flags;
    ++box_count;
    if ((flags & MB_TYPEMASK) == MB_YESNO && preview_move_edit) {
        SendMessageW(preview_move_edit, EM_SETSEL, 0, 0);
        preview_move_edit = NULL;
    }
    return box_reply;
}
#define MessageBoxW test_message_box
#include "../src/ai.c"
#undef MessageBoxW
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static HWND owner, edit;
static LPARAM wait_result(void)
{
    ULONGLONG start = GetTickCount64();
    MSG msg;
    while (GetTickCount64() - start < 10000) {
        if (PeekMessageW(&msg, owner, WM_NOTECHAD_AI, WM_NOTECHAD_AI, PM_REMOVE))
            return msg.lParam;
        Sleep(5);
    }
    CHECK(0); return 0;
}
static void reset_doc(void)
{
    CHECK(SetWindowTextW(edit, L"Hello "));
    SendMessageW(edit, EM_SETSEL, 6, 6);
    box_reply = IDNO;
}
static void unchanged(void)
{
    WCHAR text[64];
    GetWindowTextW(edit, text, 64);
    CHECK(!lstrcmpW(text, L"Hello "));
}
static LPARAM request(const WCHAR *model)
{
    ULONGLONG start;
    SetEnvironmentVariableW(L"NOTECHAD_MODEL", model);
    start = GetTickCount64();
    ai_request(owner, edit, 7);
    CHECK(GetTickCount64() - start < 250);
    return wait_result();
}
static void parser_tests(void)
{
    WCHAR out[AI_OUTPUT + 1];
    unsigned i, cut;
    const char *valid = "{\"model\":\"fixture\",\"response\":\"\\uD83D\\uDE00\\nquote\\\"\\\\\",\"done\":true,\"context\":[1,-2.5e+2,null,false,{\"a\":true}]}";
    const char *invalid[] = {
        "{}", "[]", "{\"response\":3,\"done\":true}",
        "{\"response\":\"ok\",\"done\":true,\"done\":true}",
        "{\"response\":\"ok\",\"done\":true} garbage",
        "{\"response\":\"\\uD800\",\"done\":true}",
        "{\"response\":\"\\uDC00\",\"done\":true}",
        "{\"response\":\"ok\",\"done\":true,\"x\":[01]}",
        "{\"response\":\"ok\",\"done\":true,\"x\":[1,]}",
        "{\"response\":\"ok\",\"done\":true,\"error\":\"bad\"}",
        "{\"nested\":{\"response\":\"bad\"},\"done\":true}",
        "{\"response\":\"\",\"done\":true}"
    };
    CHECK(ai_parse(valid, (int)strlen(valid), out));
    CHECK(!lstrcmpW(out, L"\xD83D\xDE00\r\nquote\"\\"));
    for (cut = 0; cut < (unsigned)strlen(valid); ++cut) CHECK(!ai_parse(valid, (int)cut, out));
    for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) CHECK(!ai_parse(invalid[i], (int)strlen(invalid[i]), out));
    puts("PASS: JSON escapes, surrogate pair, unknown nested metadata, numbers; all truncated prefixes and 12 malformed documents rejected");
}
int main(void)
{
    LPARAM result;
    int before;
    const WCHAR *bad[] = { L"fixture-http", L"fixture-redirect", L"fixture-json", L"fixture-large", L"fixture-utf8", L"fixture-nul", L"fixture-duplicate", L"fixture-incomplete" };
    unsigned i;
    parser_tests();
    owner = CreateWindowExW(0, L"STATIC", L"test", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE, 0, 0, 100, 100, owner, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(owner && edit);
    CHECK(!ai_enabled());
    ai_request(owner, edit, 1);
    ai_result(owner, edit, 7, (LPARAM)1); /* forged message must never be dereferenced */
    ai_toggle(owner); CHECK(ai_enabled());
    reset_doc();
    result = request(L"fixture-good");
    ai_result(owner, edit, 7, (LPARAM)1); /* cannot consume a real pending completion */
    unchanged();
    box_reply = IDYES;
    ai_result(owner, edit, 7, result);
    { WCHAR text[64]; GetWindowTextW(edit, text, 64); CHECK(!lstrcmpW(text, L"Hello world\r\n\x03a9")); }
    CHECK((last_flags & MB_DEFMASK) == MB_DEFBUTTON2);
    CHECK(SendMessageW(edit, EM_UNDO, 0, 0)); unchanged();
    ai_result(owner, edit, 7, result); /* replay must be inert */
    unchanged();
    reset_doc(); result = request(L"fixture-good"); ai_result(owner, edit, 7, result); unchanged();
    reset_doc(); result = request(L"fixture-good"); box_reply = IDYES; ai_result(owner, edit, 8, result); unchanged();
    reset_doc(); result = request(L"fixture-good"); SendMessageW(edit, EM_SETSEL, 2, 2); box_reply = IDYES; ai_result(owner, edit, 7, result); unchanged();
    reset_doc(); result = request(L"fixture-good"); SendMessageW(edit, EM_SETSEL, 0, 3); box_reply = IDYES; ai_result(owner, edit, 7, result); unchanged();
    reset_doc(); result = request(L"fixture-good"); ai_toggle(owner); ai_toggle(owner); box_reply = IDYES; ai_result(owner, edit, 7, result); unchanged();
    reset_doc(); result = request(L"fixture-good"); preview_move_edit = edit; box_reply = IDYES; ai_result(owner, edit, 7, result); unchanged();
    reset_doc(); result = request(L"fixture-good"); EnableWindow(owner, FALSE); box_reply = IDYES; ai_result(owner, edit, 7, result); EnableWindow(owner, TRUE); unchanged();
    reset_doc();
    SetEnvironmentVariableW(L"NOTECHAD_MODEL", NULL);
    before = box_count; ai_request(owner, edit, 7); CHECK(box_count > before);
    SetEnvironmentVariableW(L"NOTECHAD_MODEL", L"fixture-good");
    { WCHAR port[32]; GetEnvironmentVariableW(L"NOTECHAD_OLLAMA_PORT", port, 32);
      SetEnvironmentVariableW(L"NOTECHAD_OLLAMA_PORT", L"11434.evil");
      before = box_count; ai_request(owner, edit, 7); CHECK(box_count > before);
      SetEnvironmentVariableW(L"NOTECHAD_OLLAMA_PORT", port); }
    SendMessageW(edit, EM_SETSEL, 0, 2);
    before = box_count; ai_request(owner, edit, 7); CHECK(box_count > before); unchanged();
    for (i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        reset_doc(); result = request(bad[i]); before = box_count;
        box_reply = IDYES; ai_result(owner, edit, 7, result);
        CHECK(box_count > before); CHECK((last_flags & MB_TYPEMASK) == MB_OK); CHECK(last_box[0]); unchanged();
    }
    { WCHAR text[3002]; for (i = 0; i < 3000; ++i) text[i] = L'x';
      text[3000] = 0; CHECK(SetWindowTextW(edit, text)); SendMessageW(edit, EM_SETSEL, 3000, 3000);
      result = request(L"fixture-context"); box_reply = IDNO; ai_result(owner, edit, 7, result); }
    ai_toggle(owner); CHECK(!ai_enabled());
    DestroyWindow(owner);
    (void)last_box;
    puts("PASS: toggle, approval, rejection, revision/caret/selection/session guards, forged/replayed messages, missing configuration, strict port, HTTP/JSON/size/UTF-8 errors, bounded context");
    return 0;
}
