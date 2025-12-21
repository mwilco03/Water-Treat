/**
 * @file config_validate.c
 * @brief Configuration validation and remote loading implementation
 */

#include "config_validate.h"
#include "utils/logger.h"
#include <string.h>
#include <sys/stat.h>

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

/* ============================================================================
 * Validation
 * ========================================================================== */

static void add_message(config_validation_result_t *result, const char *msg) {
    if (result->message_count < 10) {
        SAFE_STRNCPY(result->messages[result->message_count], msg,
                     sizeof(result->messages[0]));
        result->message_count++;
    }
}

result_t config_validate(const app_config_t *config, config_validation_result_t *result) {
    CHECK_NULL(config);
    CHECK_NULL(result);

    memset(result, 0, sizeof(*result));

    /* Check for auto-generated station names (rtu-XXXX pattern)
     * These are derived from MAC address and are valid, but user may want custom name */
    bool is_auto_station = (strncmp(config->profinet.station_name, "rtu-", 4) == 0 &&
                            strlen(config->profinet.station_name) == 8);
    if (is_auto_station) {
        /* Not an error/warning - auto-generated names are valid for multi-RTU deployments */
        /* Only warn if it's the fallback rtu-0000 (MAC detection failed) */
        if (strcmp(config->profinet.station_name, "rtu-0000") == 0) {
            result->flags |= CONFIG_WARN_DEFAULT_STATION_NAME;
            result->warning_count++;
            add_message(result, "WARNING: MAC-based station ID detection failed, using fallback");
        }
    }

    bool is_auto_device = (strncmp(config->system.device_name, "rtu-", 4) == 0 &&
                           strlen(config->system.device_name) == 8);
    if (is_auto_device && strcmp(config->system.device_name, "rtu-0000") == 0) {
        result->flags |= CONFIG_WARN_DEFAULT_DEVICE_NAME;
        result->warning_count++;
        add_message(result, "WARNING: MAC-based device name detection failed, using fallback");
    }

    /* Check PROFINET status */
    if (!config->profinet.enabled) {
        result->flags |= CONFIG_WARN_PROFINET_DISABLED;
        result->warning_count++;
        add_message(result, "WARNING: PROFINET is disabled - no controller communication");
    }

    /* Check health endpoint */
    if (!config->health.enabled) {
        result->flags |= CONFIG_WARN_HEALTH_DISABLED;
        result->warning_count++;
        add_message(result, "WARNING: Health check endpoint is disabled");
    }

    /* Validate port numbers (http_port is uint16_t, so max is 65535) */
    if (config->health.http_port == 0) {
        result->flags |= CONFIG_ERROR_INVALID_PORT;
        result->error_count++;
        add_message(result, "ERROR: Invalid health HTTP port");
    }

    /* Validate intervals */
    if (config->logging.interval_seconds < 1 || config->logging.interval_seconds > 86400) {
        result->flags |= CONFIG_ERROR_INVALID_INTERVAL;
        result->error_count++;
        add_message(result, "ERROR: Logging interval must be 1-86400 seconds");
    }

    if (config->health.update_interval_seconds < 1 || config->health.update_interval_seconds > 3600) {
        result->flags |= CONFIG_ERROR_INVALID_INTERVAL;
        result->error_count++;
        add_message(result, "ERROR: Health update interval must be 1-3600 seconds");
    }

    /* Validate network interface */
    if (strlen(config->network.interface) == 0) {
        result->flags |= CONFIG_ERROR_MISSING_INTERFACE;
        result->error_count++;
        add_message(result, "ERROR: Network interface not specified");
    }

    /* Validate database path */
    if (strlen(config->database.path) == 0) {
        result->flags |= CONFIG_ERROR_INVALID_DB_PATH;
        result->error_count++;
        add_message(result, "ERROR: Database path not specified");
    }

    return (result->error_count > 0) ? RESULT_ERROR : RESULT_OK;
}

void config_validation_log(const config_validation_result_t *result) {
    if (!result) return;

    if (result->error_count > 0 || result->warning_count > 0) {
        LOG_INFO("Configuration validation: %d warning(s), %d error(s)",
                 result->warning_count, result->error_count);
    }

    for (int i = 0; i < result->message_count; i++) {
        if (strstr(result->messages[i], "ERROR:")) {
            LOG_ERROR("%s", result->messages[i] + 7);  /* Skip "ERROR: " */
        } else if (strstr(result->messages[i], "WARNING:")) {
            LOG_WARNING("%s", result->messages[i] + 9);  /* Skip "WARNING: " */
        } else {
            LOG_INFO("%s", result->messages[i]);
        }
    }
}

bool config_is_first_run(const app_config_t *config) {
    if (!config) return true;

    /* With MAC-based auto-detection, first run is determined differently:
     * - If station name is rtu-0000 (fallback), likely first run with issue
     * - If no config file was loaded, first run
     * - Otherwise, auto-detected names are valid
     */
    bool is_fallback_station = (strcmp(config->profinet.station_name, "rtu-0000") == 0);
    bool is_fallback_device = (strcmp(config->system.device_name, "rtu-0000") == 0);

    /* If both are fallback (MAC detection failed), likely first run or network issue */
    return is_fallback_station && is_fallback_device;
}

/* ============================================================================
 * Remote Configuration Loading
 * ========================================================================== */

#ifdef HAVE_CURL

/* CURL write callback */
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} curl_buffer_t;

static size_t config_curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    curl_buffer_t *buf = (curl_buffer_t *)userp;

    /* Grow buffer if needed */
    if (buf->size + total + 1 > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->size + total + 1) {
            new_cap = buf->size + total + 1024;
        }
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';

    return total;
}

result_t config_load_from_url(const char *url, config_manager_t *mgr) {
    CHECK_NULL(url);
    CHECK_NULL(mgr);

    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize CURL");
        return RESULT_ERROR;
    }

    curl_buffer_t buffer = {
        .data = malloc(4096),
        .size = 0,
        .capacity = 4096
    };

    if (!buffer.data) {
        curl_easy_cleanup(curl);
        return RESULT_NO_MEMORY;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, config_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "WaterTreat-RTU/1.0");

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        LOG_WARNING("Failed to fetch remote config: %s", curl_easy_strerror(res));
        free(buffer.data);
        curl_easy_cleanup(curl);
        return RESULT_IO_ERROR;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
        LOG_WARNING("Remote config returned HTTP %ld", http_code);
        free(buffer.data);
        return RESULT_ERROR;
    }

    LOG_INFO("Fetched %zu bytes from remote config URL", buffer.size);

    /* Parse the fetched INI content */
    /* Reset config manager */
    mgr->entry_count = 0;
    SAFE_STRNCPY(mgr->config_path, url, sizeof(mgr->config_path));

    char *line = strtok(buffer.data, "\n");
    char section[MAX_NAME_LEN] = "default";

    while (line) {
        /* Trim whitespace */
        while (*line == ' ' || *line == '\t') line++;
        char *end = line + strlen(line) - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\r')) {
            *end-- = '\0';
        }

        /* Skip empty lines and comments */
        if (*line == '\0' || *line == '#' || *line == ';') {
            line = strtok(NULL, "\n");
            continue;
        }

        /* Section header */
        if (*line == '[') {
            char *close = strchr(line, ']');
            if (close) {
                *close = '\0';
                SAFE_STRNCPY(section, line + 1, sizeof(section));
            }
            line = strtok(NULL, "\n");
            continue;
        }

        /* Key = Value */
        char *eq = strchr(line, '=');
        if (eq && mgr->entry_count < MAX_CONFIG_ENTRIES) {
            *eq = '\0';
            char *key = line;
            char *value = eq + 1;

            /* Trim key */
            while (*key == ' ' || *key == '\t') key++;
            char *key_end = eq - 1;
            while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
                *key_end-- = '\0';
            }

            /* Trim value */
            while (*value == ' ' || *value == '\t') value++;

            /* Remove quotes */
            size_t vlen = strlen(value);
            if (vlen >= 2 && ((value[0] == '"' && value[vlen-1] == '"') ||
                             (value[0] == '\'' && value[vlen-1] == '\''))) {
                value[vlen-1] = '\0';
                value++;
            }

            config_entry_t *e = &mgr->entries[mgr->entry_count++];
            SAFE_STRNCPY(e->section, section, sizeof(e->section));
            SAFE_STRNCPY(e->key, key, sizeof(e->key));
            SAFE_STRNCPY(e->value, value, sizeof(e->value));
        }

        line = strtok(NULL, "\n");
    }

    free(buffer.data);
    LOG_INFO("Parsed %d config entries from remote URL", mgr->entry_count);

    return RESULT_OK;
}

#else /* !HAVE_CURL */

result_t config_load_from_url(const char *url, config_manager_t *mgr) {
    UNUSED(url);
    UNUSED(mgr);
    LOG_WARNING("Remote configuration requires libcurl (HAVE_CURL)");
    return RESULT_NOT_SUPPORTED;
}

#endif /* HAVE_CURL */

/* ============================================================================
 * Bootstrap Configuration
 * ========================================================================== */

result_t config_bootstrap(config_manager_t *mgr, app_config_t *config,
                          const char *bootstrap_url, const char *local_path) {
    CHECK_NULL(mgr);
    CHECK_NULL(config);

    config_manager_init(mgr);
    bool config_loaded = false;

    /* Try remote URL first if provided */
    if (bootstrap_url && strlen(bootstrap_url) > 0) {
        LOG_INFO("Attempting to load config from URL: %s", bootstrap_url);
        if (config_load_from_url(bootstrap_url, mgr) == RESULT_OK) {
            config_loaded = true;
            LOG_INFO("Loaded configuration from remote URL");
        } else {
            LOG_WARNING("Remote config unavailable, falling back to local");
        }
    }

    /* Try local file */
    if (!config_loaded && local_path && strlen(local_path) > 0) {
        struct stat st;
        if (stat(local_path, &st) == 0) {
            if (config_load_file(mgr, local_path) == RESULT_OK) {
                config_loaded = true;
                LOG_INFO("Loaded configuration from %s", local_path);
            }
        }
    }

    /* Apply to app config (defaults first, then override) */
    config_get_defaults(config);

    if (config_loaded) {
        config_load_app_config(mgr, config);
    } else {
        LOG_INFO("Using default configuration");
    }

    /* Validate */
    config_validation_result_t validation;
    config_validate(config, &validation);
    config_validation_log(&validation);

    /* Check for first run */
    if (config_is_first_run(config)) {
        LOG_INFO("First-run detected - setup wizard recommended");
    }

    return RESULT_OK;
}
