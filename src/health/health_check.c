/**
 * @file health_check.c
 * @brief Health check endpoint implementation
 */

#include "health_check.h"
#include "utils/logger.h"
#include "sensors/sensor_manager.h"
#include "sensors/sensor_instance.h"
#include "actuators/actuator_manager.h"
#include "alarms/alarm_manager.h"
#include "profinet/profinet_manager.h"
#include "logging/data_logger.h"
#include "config/config.h"

#ifdef LED_SUPPORT
#include "hal/led_status.h"
#endif

/* Config export function declaration */
static int config_to_json(char *buffer, size_t buffer_size);

#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sysinfo.h>
#include <fcntl.h>

/* ============================================================================
 * Module State
 * ========================================================================== */

static struct {
    bool initialized;
    bool running;
    health_config_t config;
    database_t *db;

    pthread_t update_thread;
    pthread_t http_thread;
    pthread_mutex_t snapshot_mutex;

    health_snapshot_t current_snapshot;
    uint64_t start_time_ms;

    int http_socket;
} g_health = {0};

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

const char* health_status_to_string(health_status_t status) {
    switch (status) {
        case HEALTH_STATUS_OK: return "ok";
        case HEALTH_STATUS_DEGRADED: return "degraded";
        case HEALTH_STATUS_CRITICAL: return "critical";
        default: return "unknown";
    }
}

static float get_cpu_usage(void) {
    static uint64_t prev_idle = 0, prev_total = 0;
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1.0f;

    unsigned long long user, nice, system, idle, iowait, irq, softirq;
    if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7) {
        fclose(fp);
        return -1.0f;
    }
    fclose(fp);

    uint64_t total = user + nice + system + idle + iowait + irq + softirq;
    uint64_t idle_time = idle + iowait;

    float cpu_percent = 0.0f;
    if (prev_total > 0) {
        uint64_t total_diff = total - prev_total;
        uint64_t idle_diff = idle_time - prev_idle;
        if (total_diff > 0) {
            cpu_percent = 100.0f * (1.0f - (float)idle_diff / (float)total_diff);
        }
    }

    prev_idle = idle_time;
    prev_total = total;

    return cpu_percent;
}

static float get_memory_usage(void) {
    struct sysinfo si;
    if (sysinfo(&si) != 0) return -1.0f;

    unsigned long total = si.totalram;
    unsigned long free_mem = si.freeram + si.bufferram;

    /* Try to get cached memory from /proc/meminfo */
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long cached;
            if (sscanf(line, "Cached: %lu kB", &cached) == 1) {
                free_mem += cached * 1024;
                break;
            }
        }
        fclose(fp);
    }

    if (total == 0) return -1.0f;
    return 100.0f * (1.0f - (float)free_mem / (float)total);
}

static void update_subsystem_health(subsystem_health_t *sub, const char *name,
                                   health_status_t status, const char *message) {
    SAFE_STRNCPY(sub->name, name, sizeof(sub->name));
    sub->status = status;
    SAFE_STRNCPY(sub->message, message, sizeof(sub->message));
    sub->last_check_ms = get_time_ms();
}

/* ============================================================================
 * Health Collection
 * ========================================================================== */

static void collect_health_snapshot(health_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));

    snapshot->timestamp = (uint64_t)time(NULL);
    snapshot->uptime_seconds = (get_time_ms() - g_health.start_time_ms) / 1000;

    /* PROFINET status */
    if (profinet_manager_is_running()) {
        if (profinet_manager_is_connected()) {
            update_subsystem_health(&snapshot->profinet, "profinet",
                                   HEALTH_STATUS_OK, "Connected to controller");
        } else {
            update_subsystem_health(&snapshot->profinet, "profinet",
                                   HEALTH_STATUS_DEGRADED, "Disconnected from controller");
        }
    } else {
        update_subsystem_health(&snapshot->profinet, "profinet",
                               HEALTH_STATUS_UNKNOWN, "PROFINET not enabled");
    }

    /* Sensor status */
    extern sensor_manager_t g_sensor_mgr;  /* From main.c */
    if (g_sensor_mgr.running) {
        int total = 0, failed = 0;
        for (int i = 0; i < g_sensor_mgr.instance_count; i++) {
            if (!g_sensor_mgr.instances[i]) continue;
            total++;
            if (!g_sensor_mgr.instances[i]->connected) {
                failed++;
            }
        }
        snapshot->active_sensors = total - failed;
        snapshot->failed_sensors = failed;

        if (failed == 0) {
            update_subsystem_health(&snapshot->sensors, "sensors",
                                   HEALTH_STATUS_OK, "All sensors operational");
        } else if (failed < total) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%d of %d sensors failed", failed, total);
            update_subsystem_health(&snapshot->sensors, "sensors",
                                   HEALTH_STATUS_DEGRADED, msg);
        } else {
            update_subsystem_health(&snapshot->sensors, "sensors",
                                   HEALTH_STATUS_CRITICAL, "All sensors failed");
        }
    } else {
        update_subsystem_health(&snapshot->sensors, "sensors",
                               HEALTH_STATUS_UNKNOWN, "Sensor manager not running");
    }

    /* Actuator status */
    extern actuator_manager_t g_actuator_mgr;  /* From main.c */
    if (g_actuator_mgr.initialized) {
        snapshot->active_actuators = g_actuator_mgr.actuator_count;
        if (g_actuator_mgr.degraded_mode) {
            update_subsystem_health(&snapshot->actuators, "actuators",
                                   HEALTH_STATUS_DEGRADED, "Operating in degraded mode");
        } else {
            update_subsystem_health(&snapshot->actuators, "actuators",
                                   HEALTH_STATUS_OK, "All actuators operational");
        }
    } else {
        update_subsystem_health(&snapshot->actuators, "actuators",
                               HEALTH_STATUS_UNKNOWN, "Actuator manager not initialized");
    }

    /* Database status */
    if (database_is_connected(g_health.db)) {
        update_subsystem_health(&snapshot->database, "database",
                               HEALTH_STATUS_OK, "Database connected");
    } else {
        update_subsystem_health(&snapshot->database, "database",
                               HEALTH_STATUS_CRITICAL, "Database disconnected");
    }

    /* Alarm status */
    if (alarm_manager_is_running()) {
        int active = 0;
        alarm_manager_get_active_count(&active);
        snapshot->active_alarms = active;
        if (active == 0) {
            update_subsystem_health(&snapshot->alarms, "alarms",
                                   HEALTH_STATUS_OK, "No active alarms");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "%d active alarm(s)", active);
            update_subsystem_health(&snapshot->alarms, "alarms",
                                   HEALTH_STATUS_DEGRADED, msg);
        }
    } else {
        update_subsystem_health(&snapshot->alarms, "alarms",
                               HEALTH_STATUS_UNKNOWN, "Alarm manager not running");
    }

    /* System metrics */
    snapshot->cpu_usage_percent = get_cpu_usage();
    snapshot->memory_usage_percent = get_memory_usage();

    /* Calculate overall status */
    snapshot->overall_status = HEALTH_STATUS_OK;

    if (snapshot->database.status == HEALTH_STATUS_CRITICAL ||
        snapshot->sensors.status == HEALTH_STATUS_CRITICAL) {
        snapshot->overall_status = HEALTH_STATUS_CRITICAL;
    } else if (snapshot->profinet.status == HEALTH_STATUS_DEGRADED ||
               snapshot->sensors.status == HEALTH_STATUS_DEGRADED ||
               snapshot->actuators.status == HEALTH_STATUS_DEGRADED ||
               snapshot->alarms.status == HEALTH_STATUS_DEGRADED) {
        snapshot->overall_status = HEALTH_STATUS_DEGRADED;
    }
}

/* ============================================================================
 * Output Formatters
 * ========================================================================== */

int health_check_to_json(char *buffer, size_t buffer_size) {
    health_snapshot_t snap;
    pthread_mutex_lock(&g_health.snapshot_mutex);
    snap = g_health.current_snapshot;
    pthread_mutex_unlock(&g_health.snapshot_mutex);

    return snprintf(buffer, buffer_size,
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"timestamp\": %lu,\n"
        "  \"uptime_seconds\": %lu,\n"
        "  \"subsystems\": {\n"
        "    \"profinet\": {\"status\": \"%s\", \"message\": \"%s\"},\n"
        "    \"sensors\": {\"status\": \"%s\", \"message\": \"%s\"},\n"
        "    \"actuators\": {\"status\": \"%s\", \"message\": \"%s\"},\n"
        "    \"database\": {\"status\": \"%s\", \"message\": \"%s\"},\n"
        "    \"alarms\": {\"status\": \"%s\", \"message\": \"%s\"}\n"
        "  },\n"
        "  \"metrics\": {\n"
        "    \"active_sensors\": %d,\n"
        "    \"failed_sensors\": %d,\n"
        "    \"active_alarms\": %d,\n"
        "    \"active_actuators\": %d,\n"
        "    \"cpu_usage_percent\": %.1f,\n"
        "    \"memory_usage_percent\": %.1f\n"
        "  }\n"
        "}\n",
        health_status_to_string(snap.overall_status),
        (unsigned long)snap.timestamp,
        (unsigned long)snap.uptime_seconds,
        health_status_to_string(snap.profinet.status), snap.profinet.message,
        health_status_to_string(snap.sensors.status), snap.sensors.message,
        health_status_to_string(snap.actuators.status), snap.actuators.message,
        health_status_to_string(snap.database.status), snap.database.message,
        health_status_to_string(snap.alarms.status), snap.alarms.message,
        snap.active_sensors, snap.failed_sensors, snap.active_alarms,
        snap.active_actuators, snap.cpu_usage_percent, snap.memory_usage_percent);
}

int health_check_to_prometheus(char *buffer, size_t buffer_size) {
    health_snapshot_t snap;
    pthread_mutex_lock(&g_health.snapshot_mutex);
    snap = g_health.current_snapshot;
    pthread_mutex_unlock(&g_health.snapshot_mutex);

    /* Get data logger stats for observability */
    data_logger_stats_t logger_stats = {0};
    data_logger_get_stats(&logger_stats);

    int len = snprintf(buffer, buffer_size,
        "# HELP water_treat_health Overall health status (0=ok, 1=degraded, 2=critical)\n"
        "# TYPE water_treat_health gauge\n"
        "water_treat_health %d\n"
        "# HELP water_treat_uptime_seconds Uptime in seconds\n"
        "# TYPE water_treat_uptime_seconds counter\n"
        "water_treat_uptime_seconds %lu\n"
        "# HELP water_treat_subsystem_health Subsystem health (0=ok, 1=degraded, 2=critical, 3=unknown)\n"
        "# TYPE water_treat_subsystem_health gauge\n"
        "water_treat_subsystem_health{subsystem=\"profinet\"} %d\n"
        "water_treat_subsystem_health{subsystem=\"sensors\"} %d\n"
        "water_treat_subsystem_health{subsystem=\"actuators\"} %d\n"
        "water_treat_subsystem_health{subsystem=\"database\"} %d\n"
        "water_treat_subsystem_health{subsystem=\"alarms\"} %d\n"
        "# HELP water_treat_sensors_active Number of active sensors\n"
        "# TYPE water_treat_sensors_active gauge\n"
        "water_treat_sensors_active %d\n"
        "# HELP water_treat_sensors_failed Number of failed sensors\n"
        "# TYPE water_treat_sensors_failed gauge\n"
        "water_treat_sensors_failed %d\n"
        "# HELP water_treat_alarms_active Number of active alarms\n"
        "# TYPE water_treat_alarms_active gauge\n"
        "water_treat_alarms_active %d\n"
        "# HELP water_treat_actuators_active Number of active actuators\n"
        "# TYPE water_treat_actuators_active gauge\n"
        "water_treat_actuators_active %d\n"
        "# HELP water_treat_cpu_usage_percent CPU usage percentage\n"
        "# TYPE water_treat_cpu_usage_percent gauge\n"
        "water_treat_cpu_usage_percent %.1f\n"
        "# HELP water_treat_memory_usage_percent Memory usage percentage\n"
        "# TYPE water_treat_memory_usage_percent gauge\n"
        "water_treat_memory_usage_percent %.1f\n",
        (int)snap.overall_status,
        (unsigned long)snap.uptime_seconds,
        (int)snap.profinet.status,
        (int)snap.sensors.status,
        (int)snap.actuators.status,
        (int)snap.database.status,
        (int)snap.alarms.status,
        snap.active_sensors,
        snap.failed_sensors,
        snap.active_alarms,
        snap.active_actuators,
        snap.cpu_usage_percent,
        snap.memory_usage_percent);

    /* Add data logger observability metrics (P2 operator request) */
    len += snprintf(buffer + len, buffer_size - len,
        "# HELP water_treat_logger_total_logged Total entries logged locally\n"
        "# TYPE water_treat_logger_total_logged counter\n"
        "water_treat_logger_total_logged %lu\n"
        "# HELP water_treat_logger_remote_sent Total entries sent to remote successfully\n"
        "# TYPE water_treat_logger_remote_sent counter\n"
        "water_treat_logger_remote_sent %lu\n"
        "# HELP water_treat_logger_remote_failed Total entries that failed to send to remote\n"
        "# TYPE water_treat_logger_remote_failed counter\n"
        "water_treat_logger_remote_failed %lu\n"
        "# HELP water_treat_logger_queue_depth Current entries queued for remote transmission\n"
        "# TYPE water_treat_logger_queue_depth gauge\n"
        "water_treat_logger_queue_depth %d\n"
        "# HELP water_treat_logger_queue_capacity Maximum queue capacity\n"
        "# TYPE water_treat_logger_queue_capacity gauge\n"
        "water_treat_logger_queue_capacity %d\n"
        "# HELP water_treat_logger_remote_available Remote logging available (1=yes, 0=no)\n"
        "# TYPE water_treat_logger_remote_available gauge\n"
        "water_treat_logger_remote_available %d\n"
        "# HELP water_treat_logger_consecutive_failures Consecutive remote send failures\n"
        "# TYPE water_treat_logger_consecutive_failures gauge\n"
        "water_treat_logger_consecutive_failures %d\n",
        (unsigned long)logger_stats.total_logged,
        (unsigned long)logger_stats.total_remote_sent,
        (unsigned long)logger_stats.total_remote_failed,
        logger_stats.queue_count,
        logger_stats.queue_capacity,
        logger_stats.remote_available ? 1 : 0,
        logger_stats.remote_failures);

    /* Add alarm rule capacity metrics (P2 operator request for capacity planning) */
    alarm_manager_stats_t alarm_stats = {0};
    if (alarm_manager_get_stats(&alarm_stats) == RESULT_OK) {
        len += snprintf(buffer + len, buffer_size - len,
            "# HELP water_treat_alarm_rules_configured Number of configured alarm rules\n"
            "# TYPE water_treat_alarm_rules_configured gauge\n"
            "water_treat_alarm_rules_configured %d\n"
            "# HELP water_treat_alarm_rules_max Maximum alarm rules allowed\n"
            "# TYPE water_treat_alarm_rules_max gauge\n"
            "water_treat_alarm_rules_max %d\n"
            "# HELP water_treat_alarm_cache_hits Alarm rule cache hits\n"
            "# TYPE water_treat_alarm_cache_hits counter\n"
            "water_treat_alarm_cache_hits %lu\n"
            "# HELP water_treat_alarm_total_checks Total alarm rule checks performed\n"
            "# TYPE water_treat_alarm_total_checks counter\n"
            "water_treat_alarm_total_checks %lu\n",
            alarm_stats.cached_rule_count,
            256,  /* MAX_ALARM_RULES from alarm_manager.c */
            (unsigned long)alarm_stats.cache_hits,
            (unsigned long)alarm_stats.total_checks);
    }

    /* Add per-sensor health metrics (P2 operator request for predictive maintenance) */
    extern sensor_manager_t g_sensor_mgr;
    if (g_sensor_mgr.running && g_sensor_mgr.instance_count > 0) {
        len += snprintf(buffer + len, buffer_size - len,
            "# HELP water_treat_sensor_total_reads Total read attempts per sensor\n"
            "# TYPE water_treat_sensor_total_reads counter\n");
        for (int i = 0; i < g_sensor_mgr.instance_count && (size_t)len < buffer_size - 256; i++) {
            sensor_instance_t *s = g_sensor_mgr.instances[i];
            if (!s) continue;
            len += snprintf(buffer + len, buffer_size - len,
                "water_treat_sensor_total_reads{sensor=\"%s\",slot=\"%d\"} %lu\n",
                s->name, s->slot, (unsigned long)s->total_reads);
        }

        len += snprintf(buffer + len, buffer_size - len,
            "# HELP water_treat_sensor_total_failures Total failed reads per sensor\n"
            "# TYPE water_treat_sensor_total_failures counter\n");
        for (int i = 0; i < g_sensor_mgr.instance_count && (size_t)len < buffer_size - 256; i++) {
            sensor_instance_t *s = g_sensor_mgr.instances[i];
            if (!s) continue;
            len += snprintf(buffer + len, buffer_size - len,
                "water_treat_sensor_total_failures{sensor=\"%s\",slot=\"%d\"} %lu\n",
                s->name, s->slot, (unsigned long)s->total_failures);
        }

        len += snprintf(buffer + len, buffer_size - len,
            "# HELP water_treat_sensor_consecutive_failures Current consecutive failures per sensor\n"
            "# TYPE water_treat_sensor_consecutive_failures gauge\n");
        for (int i = 0; i < g_sensor_mgr.instance_count && (size_t)len < buffer_size - 256; i++) {
            sensor_instance_t *s = g_sensor_mgr.instances[i];
            if (!s) continue;
            len += snprintf(buffer + len, buffer_size - len,
                "water_treat_sensor_consecutive_failures{sensor=\"%s\",slot=\"%d\"} %d\n",
                s->name, s->slot, s->consecutive_failures);
        }
    }

    return len;
}

result_t health_check_write_file(const char *path) {
    if (!path || strlen(path) == 0) {
        return RESULT_INVALID_PARAM;
    }

    /* Write to temp file first, then rename (atomic) */
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        LOG_ERROR("Failed to open health file: %s", tmp_path);
        return RESULT_IO_ERROR;
    }

    char buffer[4096];
    int len = health_check_to_prometheus(buffer, sizeof(buffer));
    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        fclose(fp);
        return RESULT_ERROR;
    }

    if (fwrite(buffer, 1, len, fp) != (size_t)len) {
        fclose(fp);
        return RESULT_IO_ERROR;
    }

    fclose(fp);

    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        LOG_ERROR("Failed to rename health file: %s -> %s", tmp_path, path);
        return RESULT_IO_ERROR;
    }

    return RESULT_OK;
}

/* ============================================================================
 * Config Export
 * ========================================================================== */

static int config_to_json(char *buffer, size_t buffer_size) {
    extern app_config_t g_app_config;

    return snprintf(buffer, buffer_size,
        "{\n"
        "  \"system\": {\n"
        "    \"device_name\": \"%s\",\n"
        "    \"log_level\": \"%s\",\n"
        "    \"log_file\": \"%s\",\n"
        "    \"daemon_mode\": %s\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"interface\": \"%s\",\n"
        "    \"ip_address\": \"%s\",\n"
        "    \"netmask\": \"%s\",\n"
        "    \"gateway\": \"%s\",\n"
        "    \"dhcp_enabled\": %s\n"
        "  },\n"
        "  \"profinet\": {\n"
        "    \"station_name\": \"%s\",\n"
        "    \"vendor_id\": %d,\n"
        "    \"device_id\": %d,\n"
        "    \"product_name\": \"%s\",\n"
        "    \"enabled\": %s\n"
        "  },\n"
        "  \"database\": {\n"
        "    \"path\": \"%s\"\n"
        "  },\n"
        "  \"logging\": {\n"
        "    \"enabled\": %s,\n"
        "    \"interval_seconds\": %d,\n"
        "    \"retention_days\": %d,\n"
        "    \"remote_enabled\": %s,\n"
        "    \"remote_url\": \"%s\"\n"
        "  },\n"
        "  \"health\": {\n"
        "    \"enabled\": %s,\n"
        "    \"http_enabled\": %s,\n"
        "    \"http_port\": %d,\n"
        "    \"file_path\": \"%s\",\n"
        "    \"update_interval_seconds\": %d\n"
        "  },\n"
        "  \"led\": {\n"
        "    \"enabled\": %s,\n"
        "    \"led_count\": %d,\n"
        "    \"brightness\": %d,\n"
        "    \"backend\": \"%s\"\n"
        "  }\n"
        "}\n",
        g_app_config.system.device_name,
        g_app_config.system.log_level,
        g_app_config.system.log_file,
        g_app_config.system.daemon_mode ? "true" : "false",
        g_app_config.network.interface,
        g_app_config.network.ip_address,
        g_app_config.network.netmask,
        g_app_config.network.gateway,
        g_app_config.network.dhcp_enabled ? "true" : "false",
        g_app_config.profinet.station_name,
        g_app_config.profinet.vendor_id,
        g_app_config.profinet.device_id,
        g_app_config.profinet.product_name,
        g_app_config.profinet.enabled ? "true" : "false",
        g_app_config.database.path,
        g_app_config.logging.enabled ? "true" : "false",
        g_app_config.logging.interval_seconds,
        g_app_config.logging.retention_days,
        g_app_config.logging.remote_enabled ? "true" : "false",
        g_app_config.logging.remote_url,
        g_app_config.health.enabled ? "true" : "false",
        g_app_config.health.http_enabled ? "true" : "false",
        g_app_config.health.http_port,
        g_app_config.health.file_path,
        g_app_config.health.update_interval_seconds,
        g_app_config.led.enabled ? "true" : "false",
        g_app_config.led.led_count,
        g_app_config.led.brightness,
        g_app_config.led.backend);
}

/* ============================================================================
 * HTTP Server
 * ========================================================================== */

static void handle_http_request(int client_fd) {
    char request[1024];
    ssize_t n = recv(client_fd, request, sizeof(request) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    request[n] = '\0';

    /* Parse request path */
    char method[16], path[256];
    if (sscanf(request, "%15s %255s", method, path) != 2) {
        close(client_fd);
        return;
    }

    char response_body[8192];
    char response[16384];
    const char *content_type;
    int status_code = 200;

    if (strcmp(path, "/health") == 0 || strcmp(path, "/") == 0) {
        /* JSON health endpoint */
        health_check_to_json(response_body, sizeof(response_body));
        content_type = "application/json";

        /* Return 503 if system is critical */
        health_snapshot_t snap;
        health_check_get_snapshot(&snap);
        if (snap.overall_status == HEALTH_STATUS_CRITICAL) {
            status_code = 503;
        }
    } else if (strcmp(path, "/metrics") == 0) {
        /* Prometheus metrics endpoint */
        health_check_to_prometheus(response_body, sizeof(response_body));
        content_type = "text/plain; version=0.0.4; charset=utf-8";
    } else if (strcmp(path, "/ready") == 0 || strcmp(path, "/healthz") == 0) {
        /* Kubernetes-style readiness probe */
        health_snapshot_t snap;
        health_check_get_snapshot(&snap);
        if (snap.overall_status == HEALTH_STATUS_CRITICAL) {
            snprintf(response_body, sizeof(response_body), "{\"ready\": false}");
            status_code = 503;
        } else {
            snprintf(response_body, sizeof(response_body), "{\"ready\": true}");
        }
        content_type = "application/json";
    } else if (strcmp(path, "/live") == 0 || strcmp(path, "/livez") == 0) {
        /* Kubernetes-style liveness probe (always true if server is running) */
        snprintf(response_body, sizeof(response_body), "{\"alive\": true}");
        content_type = "application/json";
#ifdef LED_SUPPORT
    } else if (strcmp(path, "/led/test") == 0) {
        /* LED test endpoint for commissioning */
        extern led_status_manager_t g_led_mgr;
        if (g_led_mgr.initialized) {
            led_status_test(&g_led_mgr);
            snprintf(response_body, sizeof(response_body),
                    "{\"success\": true, \"message\": \"LED test pattern running\", \"led_count\": %d}",
                    g_led_mgr.led_count);
            LOG_INFO("LED test triggered via HTTP endpoint");
        } else {
            snprintf(response_body, sizeof(response_body),
                    "{\"success\": false, \"error\": \"LED manager not initialized\"}");
            status_code = 503;
        }
        content_type = "application/json";
    } else if (strcmp(path, "/led/status") == 0) {
        /* LED status endpoint */
        extern led_status_manager_t g_led_mgr;
        if (g_led_mgr.initialized) {
            int pos = snprintf(response_body, sizeof(response_body),
                    "{\"enabled\": %s, \"led_count\": %d, \"leds\": [",
                    g_led_mgr.enabled ? "true" : "false",
                    g_led_mgr.led_count);
            for (int i = 0; i < g_led_mgr.led_count && i < LED_FUNC_MAX; i++) {
                if (i > 0) pos += snprintf(response_body + pos, sizeof(response_body) - pos, ",");
                pos += snprintf(response_body + pos, sizeof(response_body) - pos,
                        "{\"index\": %d, \"status\": \"%s\"}",
                        i, led_status_name(g_led_mgr.leds[i].status));
            }
            snprintf(response_body + pos, sizeof(response_body) - pos, "]}");
        } else {
            snprintf(response_body, sizeof(response_body),
                    "{\"enabled\": false, \"error\": \"LED manager not initialized\"}");
        }
        content_type = "application/json";
#endif
    } else if (strcmp(path, "/config") == 0 || strcmp(path, "/config/export") == 0) {
        /* Configuration export endpoint */
        config_to_json(response_body, sizeof(response_body));
        content_type = "application/json";
        LOG_DEBUG("Config export requested via HTTP");
    } else {
        /* 404 Not Found */
        snprintf(response_body, sizeof(response_body),
                "{\"error\": \"Not Found\", \"endpoints\": [\"/health\", \"/metrics\", \"/ready\", \"/live\", \"/config\""
#ifdef LED_SUPPORT
                ", \"/led/test\", \"/led/status\""
#endif
                "]}");
        content_type = "application/json";
        status_code = 404;
    }

    const char *status_text = (status_code == 200) ? "OK" :
                              (status_code == 404) ? "Not Found" :
                              (status_code == 503) ? "Service Unavailable" : "Unknown";

    int response_len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n%s",
        status_code, status_text, content_type, strlen(response_body), response_body);

    send(client_fd, response, response_len, 0);
    close(client_fd);
}

static void* http_thread_func(void *arg) {
    UNUSED(arg);

    g_health.http_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_health.http_socket < 0) {
        LOG_ERROR("Failed to create HTTP socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(g_health.http_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_health.config.http_port);

    if (bind(g_health.http_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind HTTP socket to port %d", g_health.config.http_port);
        close(g_health.http_socket);
        g_health.http_socket = -1;
        return NULL;
    }

    if (listen(g_health.http_socket, 5) < 0) {
        LOG_ERROR("Failed to listen on HTTP socket");
        close(g_health.http_socket);
        g_health.http_socket = -1;
        return NULL;
    }

    LOG_INFO("Health check HTTP server listening on port %d", g_health.config.http_port);

    /* Set non-blocking for clean shutdown */
    fcntl(g_health.http_socket, F_SETFL, O_NONBLOCK);

    while (g_health.running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_health.http_socket, &readfds);

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(g_health.http_socket + 1, &readfds, NULL, NULL, &tv);

        if (ready > 0 && FD_ISSET(g_health.http_socket, &readfds)) {
            int client_fd = accept(g_health.http_socket, NULL, NULL);
            if (client_fd >= 0) {
                handle_http_request(client_fd);
            }
        }
    }

    close(g_health.http_socket);
    g_health.http_socket = -1;

    return NULL;
}

/* ============================================================================
 * Update Thread
 * ========================================================================== */

static void* update_thread_func(void *arg) {
    UNUSED(arg);

    while (g_health.running) {
        /* Collect health data */
        health_snapshot_t snap;
        collect_health_snapshot(&snap);

        /* Update shared snapshot */
        pthread_mutex_lock(&g_health.snapshot_mutex);
        g_health.current_snapshot = snap;
        pthread_mutex_unlock(&g_health.snapshot_mutex);

        /* Write to file if configured */
        if (strlen(g_health.config.file_path) > 0) {
            health_check_write_file(g_health.config.file_path);
        }

        /* Sleep until next update */
        for (int i = 0; i < g_health.config.update_interval_seconds && g_health.running; i++) {
            sleep(1);
        }
    }

    return NULL;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t health_check_init(database_t *db, const health_config_t *config) {
    if (g_health.initialized) {
        return RESULT_ALREADY_EXISTS;
    }

    memset(&g_health, 0, sizeof(g_health));
    g_health.db = db;
    g_health.config = *config;
    g_health.start_time_ms = get_time_ms();
    g_health.http_socket = -1;

    pthread_mutex_init(&g_health.snapshot_mutex, NULL);

    /* Initial snapshot */
    collect_health_snapshot(&g_health.current_snapshot);

    g_health.initialized = true;
    LOG_INFO("Health check module initialized (port=%d, file=%s)",
             config->http_port, config->file_path);

    return RESULT_OK;
}

result_t health_check_start(void) {
    if (!g_health.initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (g_health.running) {
        return RESULT_ALREADY_EXISTS;
    }

    g_health.running = true;

    /* Start update thread */
    if (pthread_create(&g_health.update_thread, NULL, update_thread_func, NULL) != 0) {
        g_health.running = false;
        LOG_ERROR("Failed to start health update thread");
        return RESULT_ERROR;
    }

    /* Start HTTP thread if enabled */
    if (g_health.config.http_enabled) {
        if (pthread_create(&g_health.http_thread, NULL, http_thread_func, NULL) != 0) {
            LOG_WARNING("Failed to start health HTTP thread");
            /* Continue without HTTP - file output still works */
        }
    }

    LOG_INFO("Health check module started");
    return RESULT_OK;
}

void health_check_stop(void) {
    if (!g_health.running) {
        return;
    }

    g_health.running = false;

    pthread_join(g_health.update_thread, NULL);
    if (g_health.config.http_enabled) {
        pthread_join(g_health.http_thread, NULL);
    }

    LOG_INFO("Health check module stopped");
}

void health_check_shutdown(void) {
    health_check_stop();

    pthread_mutex_destroy(&g_health.snapshot_mutex);
    g_health.initialized = false;
}

bool health_check_is_running(void) {
    return g_health.running;
}

result_t health_check_get_snapshot(health_snapshot_t *snapshot) {
    if (!snapshot) {
        return RESULT_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_health.snapshot_mutex);
    *snapshot = g_health.current_snapshot;
    pthread_mutex_unlock(&g_health.snapshot_mutex);

    return RESULT_OK;
}

void health_check_trigger_update(void) {
    health_snapshot_t snap;
    collect_health_snapshot(&snap);

    pthread_mutex_lock(&g_health.snapshot_mutex);
    g_health.current_snapshot = snap;
    pthread_mutex_unlock(&g_health.snapshot_mutex);
}
