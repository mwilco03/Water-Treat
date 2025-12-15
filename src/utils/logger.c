#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static struct {
    bool initialized;
    logger_config_t config;
    FILE *log_file;
    pthread_mutex_t mutex;
} g_logger = {0};

static const char *level_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "NONE"};
static const char *level_colors[] = {"\033[90m", "\033[36m", "\033[32m", "\033[33m", "\033[31m", "\033[35m", "\033[0m"};

result_t logger_init(const logger_config_t *config) {
    if (g_logger.initialized) return RESULT_OK;
    if (config) memcpy(&g_logger.config, config, sizeof(logger_config_t));
    else { g_logger.config.level = LOG_LEVEL_INFO; g_logger.config.destinations = LOG_DEST_CONSOLE; g_logger.config.include_timestamp = true; }
    pthread_mutex_init(&g_logger.mutex, NULL);
    if ((g_logger.config.destinations & LOG_DEST_FILE) && strlen(g_logger.config.log_file_path) > 0)
        g_logger.log_file = fopen(g_logger.config.log_file_path, "a");
    g_logger.initialized = true;
    return RESULT_OK;
}

void logger_shutdown(void) {
    if (!g_logger.initialized) return;
    pthread_mutex_lock(&g_logger.mutex);
    if (g_logger.log_file) { fclose(g_logger.log_file); g_logger.log_file = NULL; }
    pthread_mutex_unlock(&g_logger.mutex);
    pthread_mutex_destroy(&g_logger.mutex);
    g_logger.initialized = false;
}

void logger_set_level(log_level_t level) { g_logger.config.level = level; }
log_level_t logger_get_level(void) { return g_logger.config.level; }

void logger_log(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...) {
    if (level < g_logger.config.level) return;
    va_list args; va_start(args, fmt);
    pthread_mutex_lock(&g_logger.mutex);
    char ts[32] = "";
    if (g_logger.config.include_timestamp) {
        time_t now = time(NULL);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    }
    char msg[4096]; vsnprintf(msg, sizeof(msg), fmt, args);
    if (g_logger.config.destinations & LOG_DEST_CONSOLE) {
        FILE *out = (level >= LOG_LEVEL_WARNING) ? stderr : stdout;
        if (ts[0]) fprintf(out, "%s ", ts);
        fprintf(out, "%s[%-5s]\033[0m %s\n", level_colors[level], level_names[level], msg);
        fflush(out);
    }
    if ((g_logger.config.destinations & LOG_DEST_FILE) && g_logger.log_file) {
        if (ts[0]) fprintf(g_logger.log_file, "%s ", ts);
        fprintf(g_logger.log_file, "[%-5s] %s\n", level_names[level], msg);
        fflush(g_logger.log_file);
    }
    pthread_mutex_unlock(&g_logger.mutex);
    va_end(args);
}

void logger_flush(void) {
    if (!g_logger.initialized) return;
    pthread_mutex_lock(&g_logger.mutex);
    fflush(stdout); fflush(stderr);
    if (g_logger.log_file) fflush(g_logger.log_file);
    pthread_mutex_unlock(&g_logger.mutex);
}

const char* log_level_to_string(log_level_t level) { return (level >= 0 && level < LOG_LEVEL_NONE) ? level_names[level] : "UNKNOWN"; }
log_level_t log_level_from_string(const char *str) {
    if (!str) return LOG_LEVEL_INFO;
    for (int i = 0; i < LOG_LEVEL_NONE; i++) if (strcasecmp(str, level_names[i]) == 0) return (log_level_t)i;
    return LOG_LEVEL_INFO;
}
