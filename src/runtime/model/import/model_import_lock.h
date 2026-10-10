/* 模型导入的全局锁接口（实现见 model_import_lock.c）：懒创建，lock 与 unlock 必须成对且不可
 * 重入；..._shutdown 由 lifecycle 关闭阶段调用，那时必须已经没有使用者。 */
#ifndef BONGO_CAT_MODEL_IMPORT_LOCK_H
#define BONGO_CAT_MODEL_IMPORT_LOCK_H

/* Serializes model-directory scans and mutations across import, refresh,
   deletion, rollback, and nearby adapter generation. */
void bongo_cat_import_storage_lock(void);
void bongo_cat_import_storage_unlock(void);
void bongo_cat_import_storage_lock_shutdown(void);

#endif
