#ifndef DB_EVENTS_H
#define DB_EVENTS_H

#include "common.h"
#include "database.h"
#include <stdarg.h>

typedef struct {
    int id;
    time_t timestamp;
    char source[32];
    char level[16];
    char message[512];
} db_event_t;

// Event logging operations
result_t db_event_insert(database_t *db, const char *source, const char *level, const char *message);
result_t db_event_insert_formatted(database_t *db, const char *source, const char *level, const char *fmt, ...);

// Event retrieval operations
result_t db_event_list(database_t *db, int limit, db_event_t **events, int *count);
result_t db_event_list_by_source(database_t *db, const char *source, int limit, db_event_t **events, int *count);
result_t db_event_list_by_level(database_t *db, const char *level, int limit, db_event_t **events, int *count);
result_t db_event_list_recent(database_t *db, int minutes, db_event_t **events, int *count);

// Event cleanup operations
result_t db_event_cleanup(database_t *db, int retention_days);
result_t db_event_count(database_t *db, int *count);

// Utility
void db_event_free_list(db_event_t *events);

// Helper macros for common event types
#define DB_EVENT_INFO(db, src, msg) db_event_insert(db, src, "info", msg)
#define DB_EVENT_WARNING(db, src, msg) db_event_insert(db, src, "warning", msg)
#define DB_EVENT_ERROR(db, src, msg) db_event_insert(db, src, "error", msg)

#endif
