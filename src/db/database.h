#ifndef DATABASE_H
#define DATABASE_H

#include "common.h"
#include <sqlite3.h>

typedef struct { sqlite3 *db; char db_path[MAX_PATH_LEN]; bool initialized; } database_t;

result_t database_init(database_t *db, const char *path);
void database_close(database_t *db);
bool database_is_connected(database_t *db);
result_t database_execute(database_t *db, const char *sql);
result_t database_begin_transaction(database_t *db);
result_t database_commit(database_t *db);
result_t database_rollback(database_t *db);
int64_t database_last_insert_id(database_t *db);
int database_changes(database_t *db);
const char* database_error_message(database_t *db);

#endif
