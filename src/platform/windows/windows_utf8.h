#ifndef BONGO_CAT_WINDOWS_UTF8_H
#define BONGO_CAT_WINDOWS_UTF8_H

#ifdef _WIN32
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

wchar_t *bongo_cat_windows_wide(const char *text);
bool bongo_cat_windows_utf8(const wchar_t *text, char *output, size_t capacity);
#endif

#endif
