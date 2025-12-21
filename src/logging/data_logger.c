/**
 * @file data_logger.c
 * @brief Data logging system with local and remote storage
 */

#include "data_logger.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "db/db_events.h"
#include "utils/logger.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_CURL
#include <curl/curl.h>
#else
typedef void CURL;
typedef int CURLcode;
#define CURLE_OK 0
#define curl_easy_init() NULL
#define curl_easy_cleanup(x) (void)(x)
#define curl_easy_setopt(...) (void)0
#define curl_easy_perform(x) CURLE_OK
#define curl_easy_getinfo(...) (void)0
#define curl_easy_strerror(x) "CURL not available"
#define curl_slist_append(x, y) NULL
#define curl_slist_free_all(x) (void)(x)
#define curl_global_init(x) (void)(x)
#define curl_global_cleanup() (void)0
#define CURLOPT_WRITEFUNCTION 0
#define CURLOPT_TIMEOUT 0
#define CURLOPT_CONNECTTIMEOUT 0
#define CURLOPT_URL 0
#define CURLOPT_HTTPHEADER 0
#define CURLOPT_POSTFIELDS 0
#define CURLOPT_POST 0
#define CURLINFO_RESPONSE_CODE 0
#define CURL_GLOBAL_DEFAULT 0
struct curl_slist { void *next; };
#endif

#ifdef HAVE_CJSON
#include <cjson/cJSON.h>
#endif

#define MAX_LOG_BATCH_SIZE      100
#define LOG_QUEUE_SIZE          1000
#define REMOTE_RETRY_INTERVAL   60000  // 60 seconds
#define REMOTE_BATCH_SIZE       50

typedef struct {
    int module_id;
    float value;
    char status[16];
    time_t timestamp;
} log_entry_t;

typedef struct {
    database_t *db;
    data_logger_config_t config;

    // Log queue
    log_entry_t queue[LOG_QUEUE_SIZE];
    int queue_head;
    int queue_tail;
    int queue_count;

    // Remote logging state
    CURL *curl;
    char *remote_url;
    char *api_key;
    bool remote_available;
    uint64_t last_remote_attempt;
    int remote_failures;

    // Store & Forward state
    bool network_connected;         // External connection state (from PROFINET)
    bool queue_when_offline;        // Queue entries when remote unavailable
    bool flush_on_reconnect;        // Flush queue when connection restored
    bool flush_pending;             // Flag to trigger immediate flush
    int max_queue_age_seconds;

    // Threading
    pthread_t log_thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool running;
    bool initialized;

    // Statistics
    uint64_t total_logged;
    uint64_t total_remote_sent;
    uint64_t total_remote_failed;
    uint64_t total_dropped_age;     // Entries dropped due to age
} data_logger_t;

static data_logger_t g_logger = {0};

/* ============================================================================
 * Internal Functions
 * ========================================================================== */

#ifdef HAVE_CURL
static size_t logger_curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    UNUSED(contents); UNUSED(userp);
    return size * nmemb;
}
#endif

static result_t init_curl(void) {
    if (g_logger.curl) return RESULT_OK;
    
    g_logger.curl = curl_easy_init();
    if (!g_logger.curl) {
        LOG_ERROR("Failed to initialize CURL");
        return RESULT_ERROR;
    }
    
    curl_easy_setopt(g_logger.curl, CURLOPT_WRITEFUNCTION, logger_curl_write_cb);
    curl_easy_setopt(g_logger.curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(g_logger.curl, CURLOPT_CONNECTTIMEOUT, 5L);
    
    return RESULT_OK;
}

static result_t send_to_remote(log_entry_t *entries, int count) {
    if (!g_logger.config.remote_enabled || !g_logger.remote_url) {
        return RESULT_NOT_SUPPORTED;
    }
    
    if (init_curl() != RESULT_OK) return RESULT_ERROR;
    
#ifdef HAVE_CJSON
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    
    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "module_id", entries[i].module_id);
        cJSON_AddNumberToObject(entry, "value", entries[i].value);
        cJSON_AddStringToObject(entry, "status", entries[i].status);
        cJSON_AddNumberToObject(entry, "timestamp", entries[i].timestamp);
        cJSON_AddItemToArray(data, entry);
    }
    
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(root, "device", g_logger.config.device_name);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) return RESULT_NO_MEMORY;
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    if (g_logger.api_key && strlen(g_logger.api_key) > 0) {
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", g_logger.api_key);
        headers = curl_slist_append(headers, auth_header);
    }
    
    curl_easy_setopt(g_logger.curl, CURLOPT_URL, g_logger.remote_url);
    curl_easy_setopt(g_logger.curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_logger.curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(g_logger.curl, CURLOPT_POST, 1L);
    
    CURLcode res = curl_easy_perform(g_logger.curl);
    
    curl_slist_free_all(headers);
    free(json_str);
    
    if (res != CURLE_OK) {
        LOG_WARNING("Remote log failed: %s", curl_easy_strerror(res));
        g_logger.remote_failures++;
        g_logger.total_remote_failed += count;
        return RESULT_IO_ERROR;
    }
    
    long http_code = 0;
    curl_easy_getinfo(g_logger.curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    if (http_code >= 200 && http_code < 300) {
        g_logger.remote_failures = 0;
        g_logger.total_remote_sent += count;
        LOG_DEBUG("Sent %d entries to remote", count);
        return RESULT_OK;
    }
    
    LOG_WARNING("Remote log HTTP error: %ld", http_code);
    g_logger.remote_failures++;
    g_logger.total_remote_failed += count;
    return RESULT_ERROR;
    
#else
    UNUSED(entries); UNUSED(count);
    LOG_WARNING("Remote logging requires cJSON library");
    return RESULT_NOT_SUPPORTED;
#endif
}

static void drop_old_entries(void) {
    if (g_logger.max_queue_age_seconds <= 0) return;

    time_t now = time(NULL);
    time_t cutoff = now - g_logger.max_queue_age_seconds;
    int dropped = 0;

    while (g_logger.queue_count > 0) {
        log_entry_t *entry = &g_logger.queue[g_logger.queue_tail];
        if (entry->timestamp >= cutoff) break;  // Entry is fresh enough

        // Drop old entry
        g_logger.queue_tail = (g_logger.queue_tail + 1) % LOG_QUEUE_SIZE;
        g_logger.queue_count--;
        dropped++;
    }

    if (dropped > 0) {
        g_logger.total_dropped_age += dropped;
        LOG_WARNING("Dropped %d entries older than %d seconds (total dropped: %lu)",
                    dropped, g_logger.max_queue_age_seconds, g_logger.total_dropped_age);
    }
}

static void process_queue(void) {
    if (g_logger.queue_count == 0) return;

    // Drop entries that are too old
    drop_old_entries();

    if (g_logger.queue_count == 0) return;

    log_entry_t batch[MAX_LOG_BATCH_SIZE];
    int batch_count = 0;

    // Extract batch from queue
    while (g_logger.queue_count > 0 && batch_count < MAX_LOG_BATCH_SIZE) {
        batch[batch_count++] = g_logger.queue[g_logger.queue_tail];
        g_logger.queue_tail = (g_logger.queue_tail + 1) % LOG_QUEUE_SIZE;
        g_logger.queue_count--;
    }

    // Log to local database
    if (g_logger.config.local_enabled && g_logger.db) {
        for (int i = 0; i < batch_count; i++) {
            db_sensor_log_insert(g_logger.db, batch[i].module_id,
                                 batch[i].value, batch[i].status);
        }
        g_logger.total_logged += batch_count;
    }

    // Send to remote if enabled and network is connected
    bool should_send_remote = g_logger.config.remote_enabled &&
                              g_logger.remote_available &&
                              g_logger.network_connected;

    // If offline and queue_when_offline is false, we already processed locally
    // If offline and queue_when_offline is true, entries stay in queue (but we already extracted them)
    // So we need to handle this differently...

    if (should_send_remote || g_logger.flush_pending) {
        uint64_t now = get_time_ms();

        // Check if we should retry after failures (unless flush is pending)
        if (!g_logger.flush_pending && g_logger.remote_failures > 0) {
            if (now - g_logger.last_remote_attempt < REMOTE_RETRY_INTERVAL) {
                return;
            }
        }

        g_logger.last_remote_attempt = now;
        g_logger.flush_pending = false;

        // Send in smaller batches for remote
        int sent_ok = 0;
        for (int i = 0; i < batch_count; i += REMOTE_BATCH_SIZE) {
            int send_count = MIN(REMOTE_BATCH_SIZE, batch_count - i);
            if (send_to_remote(&batch[i], send_count) == RESULT_OK) {
                sent_ok += send_count;
            }
        }

        if (sent_ok > 0) {
            LOG_DEBUG("Flushed %d entries to remote", sent_ok);
        }
    }
}

static void* logger_thread(void *arg) {
    UNUSED(arg);
    
    while (g_logger.running) {
        pthread_mutex_lock(&g_logger.mutex);
        
        // Wait for data or timeout
        if (g_logger.queue_count == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += g_logger.config.interval_seconds;
            pthread_cond_timedwait(&g_logger.cond, &g_logger.mutex, &ts);
        }
        
        process_queue();
        
        pthread_mutex_unlock(&g_logger.mutex);
    }
    
    // Final flush
    pthread_mutex_lock(&g_logger.mutex);
    process_queue();
    pthread_mutex_unlock(&g_logger.mutex);
    
    return NULL;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t data_logger_init(database_t *db, const data_logger_config_t *config) {
    CHECK_NULL(db); CHECK_NULL(config);
    
    if (g_logger.initialized) return RESULT_OK;
    
    memset(&g_logger, 0, sizeof(g_logger));
    g_logger.db = db;
    memcpy(&g_logger.config, config, sizeof(data_logger_config_t));
    
    pthread_mutex_init(&g_logger.mutex, NULL);
    pthread_cond_init(&g_logger.cond, NULL);
    
    // Set up remote logging
    if (config->remote_enabled && strlen(config->remote_url) > 0) {
        g_logger.remote_url = strdup(config->remote_url);
        if (strlen(config->api_key) > 0) {
            g_logger.api_key = strdup(config->api_key);
        }
        g_logger.remote_available = true;
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    // Store & Forward defaults (can be overridden by config)
    g_logger.queue_when_offline = config->queue_when_offline ? config->queue_when_offline : true;
    g_logger.flush_on_reconnect = config->flush_on_reconnect ? config->flush_on_reconnect : true;
    g_logger.max_queue_age_seconds = config->max_queue_age_seconds > 0 ? config->max_queue_age_seconds : 3600;
    g_logger.network_connected = true;  // Assume connected initially

    g_logger.initialized = true;
    LOG_INFO("Data logger initialized (local=%d, remote=%d, interval=%ds, queue_offline=%d, flush_reconnect=%d)",
             config->local_enabled, config->remote_enabled, config->interval_seconds,
             g_logger.queue_when_offline, g_logger.flush_on_reconnect);
    
    return RESULT_OK;
}

result_t data_logger_start(void) {
    if (!g_logger.initialized) return RESULT_NOT_INITIALIZED;
    if (g_logger.running) return RESULT_OK;
    
    g_logger.running = true;
    
    if (pthread_create(&g_logger.log_thread, NULL, logger_thread, NULL) != 0) {
        LOG_ERROR("Failed to create logger thread");
        g_logger.running = false;
        return RESULT_ERROR;
    }
    
    LOG_INFO("Data logger started");
    return RESULT_OK;
}

result_t data_logger_stop(void) {
    if (!g_logger.running) return RESULT_OK;
    
    g_logger.running = false;
    
    pthread_mutex_lock(&g_logger.mutex);
    pthread_cond_signal(&g_logger.cond);
    pthread_mutex_unlock(&g_logger.mutex);
    
    pthread_join(g_logger.log_thread, NULL);
    
    LOG_INFO("Data logger stopped (total logged: %lu, remote sent: %lu)",
             g_logger.total_logged, g_logger.total_remote_sent);
    
    return RESULT_OK;
}

void data_logger_shutdown(void) {
    data_logger_stop();
    
    if (g_logger.curl) {
        curl_easy_cleanup(g_logger.curl);
        g_logger.curl = NULL;
    }
    
    if (g_logger.remote_url) {
        free(g_logger.remote_url);
        g_logger.remote_url = NULL;
    }
    
    if (g_logger.api_key) {
        free(g_logger.api_key);
        g_logger.api_key = NULL;
    }
    
    if (g_logger.remote_available) {
        curl_global_cleanup();
    }
    
    pthread_mutex_destroy(&g_logger.mutex);
    pthread_cond_destroy(&g_logger.cond);
    
    g_logger.initialized = false;
    LOG_INFO("Data logger shutdown");
}

result_t data_logger_log(int module_id, float value, const char *status) {
    if (!g_logger.initialized) return RESULT_NOT_INITIALIZED;
    if (!g_logger.config.enabled) return RESULT_OK;
    
    pthread_mutex_lock(&g_logger.mutex);
    
    if (g_logger.queue_count >= LOG_QUEUE_SIZE) {
        // Queue full, drop oldest
        g_logger.queue_tail = (g_logger.queue_tail + 1) % LOG_QUEUE_SIZE;
        g_logger.queue_count--;
        LOG_WARNING("Log queue full, dropping oldest entry");
    }
    
    log_entry_t *entry = &g_logger.queue[g_logger.queue_head];
    entry->module_id = module_id;
    entry->value = value;
    SAFE_STRNCPY(entry->status, status ? status : "ok", sizeof(entry->status));
    entry->timestamp = time(NULL);
    
    g_logger.queue_head = (g_logger.queue_head + 1) % LOG_QUEUE_SIZE;
    g_logger.queue_count++;
    
    // Signal if queue is getting full
    if (g_logger.queue_count >= LOG_QUEUE_SIZE / 2) {
        pthread_cond_signal(&g_logger.cond);
    }
    
    pthread_mutex_unlock(&g_logger.mutex);
    return RESULT_OK;
}

result_t data_logger_log_batch(int *module_ids, float *values, const char **statuses, int count) {
    if (!g_logger.initialized) return RESULT_NOT_INITIALIZED;
    if (!g_logger.config.enabled) return RESULT_OK;
    CHECK_NULL(module_ids); CHECK_NULL(values);
    
    for (int i = 0; i < count; i++) {
        data_logger_log(module_ids[i], values[i], statuses ? statuses[i] : "ok");
    }
    
    return RESULT_OK;
}

result_t data_logger_flush(void) {
    if (!g_logger.initialized) return RESULT_NOT_INITIALIZED;
    
    pthread_mutex_lock(&g_logger.mutex);
    pthread_cond_signal(&g_logger.cond);
    pthread_mutex_unlock(&g_logger.mutex);
    
    // Wait a bit for flush
    usleep(100000);
    
    return RESULT_OK;
}

result_t data_logger_cleanup(int retention_days) {
    if (!g_logger.initialized || !g_logger.db) return RESULT_NOT_INITIALIZED;
    if (retention_days <= 0) retention_days = g_logger.config.retention_days;
    if (retention_days <= 0) return RESULT_OK;
    
    return db_sensor_log_cleanup(g_logger.db, retention_days);
}

result_t data_logger_get_stats(data_logger_stats_t *stats) {
    CHECK_NULL(stats);
    if (!g_logger.initialized) return RESULT_NOT_INITIALIZED;
    
    pthread_mutex_lock(&g_logger.mutex);
    
    stats->total_logged = g_logger.total_logged;
    stats->total_remote_sent = g_logger.total_remote_sent;
    stats->total_remote_failed = g_logger.total_remote_failed;
    stats->queue_count = g_logger.queue_count;
    stats->queue_capacity = LOG_QUEUE_SIZE;
    stats->remote_available = g_logger.remote_available;
    stats->remote_failures = g_logger.remote_failures;
    
    pthread_mutex_unlock(&g_logger.mutex);
    return RESULT_OK;
}

result_t data_logger_set_remote(const char *url, const char *api_key) {
    pthread_mutex_lock(&g_logger.mutex);
    
    if (g_logger.remote_url) free(g_logger.remote_url);
    if (g_logger.api_key) free(g_logger.api_key);
    
    g_logger.remote_url = url ? strdup(url) : NULL;
    g_logger.api_key = api_key ? strdup(api_key) : NULL;
    g_logger.remote_available = (url && strlen(url) > 0);
    g_logger.remote_failures = 0;
    
    pthread_mutex_unlock(&g_logger.mutex);
    
    LOG_INFO("Remote logging %s: %s", 
             g_logger.remote_available ? "enabled" : "disabled",
             url ? url : "(none)");
    
    return RESULT_OK;
}

result_t data_logger_enable(bool enabled) {
    g_logger.config.enabled = enabled;
    LOG_INFO("Data logging %s", enabled ? "enabled" : "disabled");
    return RESULT_OK;
}

result_t data_logger_set_interval(int seconds) {
    if (seconds < 1) seconds = 1;
    if (seconds > 3600) seconds = 3600;
    g_logger.config.interval_seconds = seconds;
    LOG_INFO("Logging interval set to %d seconds", seconds);
    return RESULT_OK;
}

bool data_logger_is_running(void) {
    return g_logger.running;
}

/* ============================================================================
 * Store & Forward Control
 * ========================================================================== */

result_t data_logger_set_queue_mode(bool queue_when_offline, bool flush_on_reconnect) {
    pthread_mutex_lock(&g_logger.mutex);

    g_logger.queue_when_offline = queue_when_offline;
    g_logger.flush_on_reconnect = flush_on_reconnect;

    pthread_mutex_unlock(&g_logger.mutex);

    LOG_INFO("Store & forward mode: queue_offline=%d, flush_reconnect=%d",
             queue_when_offline, flush_on_reconnect);

    return RESULT_OK;
}

result_t data_logger_force_flush(void) {
    if (!g_logger.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_logger.mutex);

    g_logger.flush_pending = true;
    g_logger.remote_failures = 0;  // Reset failures to allow immediate retry
    pthread_cond_signal(&g_logger.cond);

    pthread_mutex_unlock(&g_logger.mutex);

    LOG_INFO("Forced flush requested (%d entries in queue)", g_logger.queue_count);

    return RESULT_OK;
}

result_t data_logger_notify_connection(bool connected) {
    if (!g_logger.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_logger.mutex);

    bool was_connected = g_logger.network_connected;
    g_logger.network_connected = connected;

    // If we just reconnected and flush_on_reconnect is enabled, trigger flush
    if (connected && !was_connected && g_logger.flush_on_reconnect) {
        g_logger.flush_pending = true;
        g_logger.remote_failures = 0;  // Reset failures to allow immediate retry
        pthread_cond_signal(&g_logger.cond);

        LOG_INFO("Network reconnected - triggering queue flush (%d entries)",
                 g_logger.queue_count);
    } else if (!connected && was_connected) {
        LOG_WARNING("Network disconnected - entries will be queued locally");
    }

    pthread_mutex_unlock(&g_logger.mutex);

    return RESULT_OK;
}
