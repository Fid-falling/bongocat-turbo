/* ZIP 导入：后缀识别（is_archive）、解压到临时目录（archive_extract）与临时树清理
 * （archive_cleanup）。extract 成功时 directory 是 temporary 的子目录，失败时两者都清空。 */
#ifndef BONGO_CAT_MODEL_IMPORT_ARCHIVE_H
#define BONGO_CAT_MODEL_IMPORT_ARCHIVE_H

#include "model_import.h"

bool bongo_cat_import_is_archive(const char *source);
BongoCatResult bongo_cat_import_archive_extract(const char *source,
    char directory[BONGO_CAT_PATH_CAP], char temporary[BONGO_CAT_PATH_CAP],
    BongoCatError *error);
void bongo_cat_import_archive_cleanup(const char *temporary);

#endif
