#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>

static struct {
    bool initialized;
    logger_config_t config;
    FILE *log_file;
    pthread_mutex_t mutex;
    bool syslog_opened;
} g_logger = {0};

static const char *level_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "NONE"};
static const char *level_colors[] = {"\033[90m", "\033[36m", "\033[32m", "\033[33m", "\033[31m", "\033[35m", "\033[0m"};

/**
 * ensure_parent_dir - Create parent directory for a file path
 *
 * What: Creates all parent directories needed for a file path
 * Why: fopen() fails silently if parent directory doesn't exist
 */
static int ensure_parent_dir(const char *file_path) {
    char path_copy[512];
    strncpy(path_copy, file_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    /* Find last slash to get directory portion */
    char *last_slash = strrchr(path_copy, '/');
    if (!last_slash || last_slash == path_copy) {
        return 0;  /* No parent directory or root */
    }
    *last_slash = '\0';

    /* Check if directory exists */
    struct stat st;
    if (stat(path_copy, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;  /* Already exists */
    }

    /* Create directories recursively */
    char *p = path_copy + 1;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path_copy, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
        p++;
    }
    if (mkdir(path_copy, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Map our log levels to syslog priorities */
static int log_level_to_syslog_priority(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_TRACE:   return LOG_DEBUG;
        case LOG_LEVEL_DEBUG:   return LOG_DEBUG;
        case LOG_LEVEL_INFO:    return LOG_INFO;
        case LOG_LEVEL_WARNING: return LOG_WARNING;
        case LOG_LEVEL_ERROR:   return LOG_ERR;
        case LOG_LEVEL_FATAL:   return LOG_CRIT;
        default:                return LOG_INFO;
    }
}

/* Map our facility enum to syslog facility */
static int log_facility_to_syslog(log_facility_t facility) {
    switch (facility) {
        case LOG_FACILITY_DAEMON: return LOG_DAEMON;
        case LOG_FACILITY_LOCAL0: return LOG_LOCAL0;
        case LOG_FACILITY_LOCAL1: return LOG_LOCAL1;
        case LOG_FACILITY_LOCAL2: return LOG_LOCAL2;
        case LOG_FACILITY_LOCAL3: return LOG_LOCAL3;
        case LOG_FACILITY_LOCAL4: return LOG_LOCAL4;
        case LOG_FACILITY_LOCAL5: return LOG_LOCAL5;
        case LOG_FACILITY_LOCAL6: return LOG_LOCAL6;
        case LOG_FACILITY_LOCAL7: return LOG_LOCAL7;
        case LOG_FACILITY_USER:   return LOG_USER;
        default:                  return LOG_DAEMON;
    }
}

result_t logger_init(const logger_config_t *config) {
    if (g_logger.initialized) return RESULT_OK;
    if (config) {
        memcpy(&g_logger.config, config, sizeof(logger_config_t));
    } else {
        g_logger.config.level = LOG_LEVEL_INFO;
        g_logger.config.destinations = LOG_DEST_CONSOLE;
        g_logger.config.include_timestamp = true;
        g_logger.config.syslog_facility = LOG_FACILITY_DAEMON;
        SAFE_STRNCPY(g_logger.config.syslog_ident, "water-treat", sizeof(g_logger.config.syslog_ident));
    }
    pthread_mutex_init(&g_logger.mutex, NULL);

    /* Open file log if requested */
    if ((g_logger.config.destinations & LOG_DEST_FILE) && strlen(g_logger.config.log_file_path) > 0) {
        /* Ensure parent directory exists before opening file */
        if (ensure_parent_dir(g_logger.config.log_file_path) != 0) {
            /* Directory creation failed - log to stderr since we can't log to file */
            fprintf(stderr, "[WARN] Cannot create log directory for %s\n",
                    g_logger.config.log_file_path);
        }
        g_logger.log_file = fopen(g_logger.config.log_file_path, "a");
        if (!g_logger.log_file) {
            fprintf(stderr, "[WARN] Cannot open log file %s: %s\n",
                    g_logger.config.log_file_path, strerror(errno));
        }
    }

    /* Open syslog if requested - enables centralized logging via rsyslog */
    if (g_logger.config.destinations & LOG_DEST_SYSLOG) {
        const char *ident = strlen(g_logger.config.syslog_ident) > 0
                          ? g_logger.config.syslog_ident
                          : "water-treat";
        int facility = log_facility_to_syslog(g_logger.config.syslog_facility);
        openlog(ident, LOG_PID | LOG_NDELAY, facility);
        g_logger.syslog_opened = true;
    }

    g_logger.initialized = true;
    return RESULT_OK;
}

void logger_shutdown(void) {
    if (!g_logger.initialized) return;
    pthread_mutex_lock(&g_logger.mutex);
    if (g_logger.log_file) {
        fclose(g_logger.log_file);
        g_logger.log_file = NULL;
    }
    if (g_logger.syslog_opened) {
        closelog();
        g_logger.syslog_opened = false;
    }
    pthread_mutex_unlock(&g_logger.mutex);
    pthread_mutex_destroy(&g_logger.mutex);
    g_logger.initialized = false;
}

void logger_set_level(log_level_t level) { g_logger.config.level = level; }
log_level_t logger_get_level(void) { return g_logger.config.level; }

void logger_log(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...) {
    if (level < g_logger.config.level) return;
    UNUSED(file); UNUSED(line); UNUSED(func);

    va_list args;
    va_start(args, fmt);
    pthread_mutex_lock(&g_logger.mutex);

    char ts[32] = "";
    if (g_logger.config.include_timestamp) {
        time_t now = time(NULL);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    }

    char msg[4096];
    vsnprintf(msg, sizeof(msg), fmt, args);

    /* Console output with colors */
    if (g_logger.config.destinations & LOG_DEST_CONSOLE) {
        FILE *out = (level >= LOG_LEVEL_WARNING) ? stderr : stdout;
        if (ts[0]) fprintf(out, "%s ", ts);
        fprintf(out, "%s[%-5s]\033[0m %s\n", level_colors[level], level_names[level], msg);
        fflush(out);
    }

    /* File output */
    if ((g_logger.config.destinations & LOG_DEST_FILE) && g_logger.log_file) {
        if (ts[0]) fprintf(g_logger.log_file, "%s ", ts);
        fprintf(g_logger.log_file, "[%-5s] %s\n", level_names[level], msg);
        fflush(g_logger.log_file);
    }

    /* Syslog output - enables centralized logging via rsyslog/journald
     * Messages go to:
     *   - /var/log/syslog (or /var/log/messages)
     *   - journalctl (if using systemd)
     *   - Remote syslog server (if rsyslog is configured with forwarding)
     */
    if ((g_logger.config.destinations & LOG_DEST_SYSLOG) && g_logger.syslog_opened) {
        int priority = log_level_to_syslog_priority(level);
        syslog(priority, "[%s] %s", level_names[level], msg);
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
