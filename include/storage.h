/*
 * storage.h - Durable, crash-safe persistence for the bank.
 *
 * The whole bank lives in one human-readable, pipe-delimited text file.
 * Saves are atomic: data is written to a temp file, the previous database is
 * kept as a .bak backup, and only then is the new file moved into place.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "bank.h"

#include <stdbool.h>
#include <stddef.h>

#define STORAGE_PATH_MAX 512

typedef struct {
    char dir[STORAGE_PATH_MAX];
    char db[STORAGE_PATH_MAX];
    char tmp[STORAGE_PATH_MAX];
    char bak[STORAGE_PATH_MAX];
} Storage;

typedef enum {
    STORAGE_OK = 0,
    STORAGE_NOT_FOUND,   /* no database yet (first run) */
    STORAGE_ERR_IO,
    STORAGE_ERR_CORRUPT,
    STORAGE_ERR_NOMEM,
    STORAGE_ERR_PATH
} StorageStatus;

bool          storage_init(Storage *s, const char *dir);
StorageStatus storage_load(const Storage *s, Bank *bank, int *bad_line);
StorageStatus storage_save(const Storage *s, const Bank *bank);
StorageStatus storage_export_statement(const Storage *s, const Bank *bank,
                                       const Account *acct, char *path_out, size_t path_size);
const char   *storage_strerror(StorageStatus st);

#endif /* STORAGE_H */
