#ifndef NOTE_CHAD_TEXTFILE_H
#define NOTE_CHAD_TEXTFILE_H
#include <windows.h>
#define TF_EDIT_LIMIT (8u * 1024u * 1024u)
#define TF_BYTE_LIMIT (8u * 1024u * 1024u)
#define TF_PATH_CAP 32768
#define TF_UTF8 1u
#define TF_UTF8_BOM 2u
#define TF_UTF16_LE 3u
#define TF_UTF16_BE 4u
#define TF_CRLF 1u
#define TF_LF 2u
/* Treat fields as opaque; initialize with tf_default, retain after load/save. */
typedef struct {
    DWORD encoding;
    DWORD newline;
    BOOL hasOriginal;
    BY_HANDLE_FILE_INFORMATION original;
    LARGE_INTEGER changeTime;
    WCHAR originalPath[TF_PATH_CAP];
} TextFormat;
void tf_default(TextFormat *fmt);
/* Success returns CRLF-normalized LocalAlloc text; caller must LocalFree.
   Failure sets *text to NULL and preserves fmt. Paths must be absolute. */
BOOL tf_load(const WCHAR *path, WCHAR **text, TextFormat *fmt, WCHAR *reason, DWORD reasonCap);
/* TRUE: optimistic conflict check for same-path Save. FALSE: explicitly
   authorized Save As overwrite. A post-publication refresh failure may return
   FALSE with hasOriginal cleared; always display reason before retrying. */
BOOL tf_save(const WCHAR *path, const WCHAR *text, TextFormat *fmt, BOOL checkOriginal, WCHAR *reason, DWORD reasonCap);
/* Atomically publish ONLY if absent; never replaces an existing target. */
BOOL tf_save_new(const WCHAR *path, const WCHAR *text, TextFormat *fmt, WCHAR *reason, DWORD reasonCap);
const WCHAR *tf_format(const TextFormat *fmt);
#endif
