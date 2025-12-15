#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"
#include <stdarg.h>

#define LOG_DEST_CONSOLE  0x01
#define LOG_DEST_FILE     0x02

typedef struct {
    log_level_t level;
    int destinations;
    char log_file_path[MAX_PATH_LEN];
    bool include_timestamp;
    bool include_source;
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
