/*
 * 面捕组件的版本记录: 安装成功后把版本号写到 <root>/version.txt, 更新检查时读它比较。
 *
 * 单独一个文件是为了守住 facetrack_install.c 的行数策略 (CheckLines.cmake 500 行上限):
 * 下载/校验/选源已经把那一个文件顶到边缘了, 再加版本读写就会超。
 */
#include "facetrack_install.h"

#include "bongo_cat/path.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

bool bongo_cat_facetrack_install_version(const char *root, char *output,
    size_t capacity) {
    if (output && capacity) output[0] = 0;
    if (!root || !root[0]) return false;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), root, "version.txt")) return false;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return false;
    char buffer[64] = {0};
    const size_t read = SDL_ReadIO(io, buffer, sizeof(buffer) - 1);
    SDL_CloseIO(io);
    if (read == 0) return false;
    /* 只留第一行; 顺手去掉行首空白 */
    for (size_t i = 0; i < read; ++i) {
        const char c = buffer[i];
        if (c == '\r' || c == '\n') { buffer[i] = 0; break; }
    }
    char *start = buffer;
    while (*start == ' ' || *start == '\t') ++start;
    if (!start[0]) return false;
    if (output) snprintf(output, capacity, "%s", start);
    return true;
}

void bongo_cat_facetrack_install_write_version(const char *root,
    const char *version) {
    if (!root || !root[0] || !version) return;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), root, "version.txt")) return;
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return;
    SDL_WriteIO(io, version, strlen(version));
    SDL_CloseIO(io);
}
