#include "windows_utf8.h"

#ifdef _WIN32
#include <stdlib.h>
#include <string.h>
#include <windows.h>

wchar_t *bongo_cat_windows_wide(const char *text) {
    int length = text ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text, -1, NULL, 0) : 0;
    wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof(*wide)) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text, -1, wide, length)) {
        free(wide); return NULL;
    }
    if (!wide) return NULL;
    for (wchar_t *value = wide; *value; ++value)
        if (*value == L'/') *value = L'\\';
    bool drive = length > 3 && wide[1] == L':' && wide[2] == L'\\';
    bool unc = length > 3 && wide[0] == L'\\' && wide[1] == L'\\' &&
        wide[2] != L'?';
    if (length <= 240 || (!drive && !unc)) return wide;
    size_t prefix = unc ? 8 : 4, skip = unc ? 2 : 0;
    wchar_t *extended = malloc(((size_t)length + prefix - skip) * sizeof(*extended));
    if (!extended) { free(wide); return NULL; }
    memcpy(extended, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\", prefix * sizeof(*extended));
    memcpy(extended + prefix, wide + skip, ((size_t)length - skip) * sizeof(*extended));
    free(wide); return extended;
}

bool bongo_cat_windows_utf8(const wchar_t *text, char *output, size_t capacity) {
    if (!text || !output || !capacity) return false;
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, -1, NULL, 0, NULL, NULL);
    return required > 0 && (size_t)required <= capacity &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text, -1, output, (int)capacity, NULL, NULL) > 0;
}
#endif
