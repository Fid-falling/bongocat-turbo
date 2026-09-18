#ifndef BONGO_CAT_MODEL_IMPORT_LOCK_H
#define BONGO_CAT_MODEL_IMPORT_LOCK_H

/* Serializes model-directory scans and mutations across import, refresh,
   deletion, rollback, and nearby adapter generation. */
void bongo_cat_import_storage_lock(void);
void bongo_cat_import_storage_unlock(void);
void bongo_cat_import_storage_lock_shutdown(void);

#endif
