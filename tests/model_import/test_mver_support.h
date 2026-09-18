#ifndef BONGO_CAT_TEST_MVER_SUPPORT_H
#define BONGO_CAT_TEST_MVER_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>

bool write_text(const char *path, const char *text);
bool child(char *output, size_t capacity, const char *root,
    const char *name, bool directory);
bool mver_fixture(const char *root);

#endif
