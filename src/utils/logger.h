#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"
#include <stdarg.h>

/* Log destination flags (can be combined) */
#define LOG_DEST_CONSOLE  0x01
#define LOG_DEST_FILE     0x02
#define LOG_DEST_SYSLOG   0x04  /* Forward to syslog for centralized logging */
#define LOG_DEST_JOURNAL  0x08  /* Direct systemd journal integration */

/* Syslog facility selection */
typedef enum {
    LOG_FACILITY_DAEMON = 0,   /* LOG_DAEMON - system daemons */
    LOG_FACILITY_LOCAL0,       /* LOG_LOCAL0 - custom use */
    LOG_FACILITY_LOCAL1,
    LOG_FACILITY_LOCAL2,
    LOG_FACILITY_LOCAL3,
    LOG_FACILITY_LOCAL4,
    LOG_FACILITY_LOCAL5,
    LOG_FACILITY_LOCAL6,
    LOG_FACILITY_LOCAL7,
    LOG_FACILITY_USER          /* LOG_USER - user-level messages */
} log_facility_t;

typedef struct {
    log_level_t level;
    int destinations;
    char log_file_path[MAX_PATH_LEN];
    bool include_timestamp;
    bool include_source;
    /* Syslog options */
    log_facility_t syslog_facility;
    char syslog_ident[64];     /* Application identifier for syslog */
} logger_config_t;

result_t logger_init(const logger_config_t *config);
void logger_shutdown(void);
void logger_set_level(log_level_t level);
log_level_t logger_get_level(void);
void logger_log(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...);
void logger_flush(void);

#define LOG_TRACE(fmt, ...) logger_log(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) logger_log(LOG_LEVEL_WARNING, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) logger_log(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

const char* log_level_to_string(log_level_t level);
log_level_t log_level_from_string(const char *str);

#endif
