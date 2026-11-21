/* Map a logical Unicode index (LF line ends) into a classic EDIT's UTF-16 buffer. */
#include <windows.h>
int doc_offset(const WCHAR *text, unsigned long long index) {
    const WCHAR *p = text;
    while (index--) {
        WCHAR ch = *p++;
        if (!ch) return -1;
        if ((ch == '\r' && *p == '\n') ||
            (ch >= 0xd800 && ch <= 0xdbff && *p >= 0xdc00 && *p <= 0xdfff)) ++p;
    }
    return (int)(p - text);
}
