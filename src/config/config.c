#include "config.h"
#include "config_defaults.h"
#include "platform/board_detect.h"
#include "platform/hw_discover.h"
#include "utils/logger.h"
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* ============================================================================
 * Table-Driven Config Loading
 * ============================================================================
 * What: Replace 37 repetitive if-blocks with a single data-driven loop
 * Why: Reduces cyclomatic complexity from 37 to ~5, eliminates copy-paste errors
 * How: Define field metadata (section, key, type, offset) and iterate
 */

typedef enum {
    CFG_TYPE_STRING,
    CFG_TYPE_INT,
    CFG_TYPE_BOOL,
    CFG_TYPE_UINT16,
    CFG_TYPE_UINT32
} config_field_type_t;

typedef struct {
    const char *section;
    const char *key;
    config_field_type_t type;
    size_t offset;
    size_t size;  /* For strings: buffer size. For others: 0 */
} config_field_t;

/* Field descriptor table - all 37 config entries */
static const config_field_t config_fields[] = {
    /* System section */
    { "system", "device_name", CFG_TYPE_STRING,
      offsetof(app_config_t, system.device_name),
      sizeof(((app_config_t*)0)->system.device_name) },
    { "system", "log_level", CFG_TYPE_STRING,
      offsetof(app_config_t, system.log_level),
      sizeof(((app_config_t*)0)->system.log_level) },
    { "system", "log_file", CFG_TYPE_STRING,
      offsetof(app_config_t, system.log_file),
      sizeof(((app_config_t*)0)->system.log_file) },
    { "system", "daemon_mode", CFG_TYPE_BOOL,
      offsetof(app_config_t, system.daemon_mode), 0 },

    /* Network section */
    { "network", "interface", CFG_TYPE_STRING,
      offsetof(app_config_t, network.interface),
      sizeof(((app_config_t*)0)->network.interface) },
    { "network", "dhcp_enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, network.dhcp_enabled), 0 },
    { "network", "ip_address", CFG_TYPE_STRING,
      offsetof(app_config_t, network.ip_address),
      sizeof(((app_config_t*)0)->network.ip_address) },
    { "network", "netmask", CFG_TYPE_STRING,
      offsetof(app_config_t, network.netmask),
      sizeof(((app_config_t*)0)->network.netmask) },
    { "network", "gateway", CFG_TYPE_STRING,
      offsetof(app_config_t, network.gateway),
      sizeof(((app_config_t*)0)->network.gateway) },

    /* PROFINET section */
    { "profinet", "station_name", CFG_TYPE_STRING,
      offsetof(app_config_t, profinet.station_name),
      sizeof(((app_config_t*)0)->profinet.station_name) },
    { "profinet", "vendor_id", CFG_TYPE_UINT16,
      offsetof(app_config_t, profinet.vendor_id), 0 },
    { "profinet", "device_id", CFG_TYPE_UINT16,
      offsetof(app_config_t, profinet.device_id), 0 },
    { "profinet", "product_name", CFG_TYPE_STRING,
      offsetof(app_config_t, profinet.product_name),
      sizeof(((app_config_t*)0)->profinet.product_name) },
    { "profinet", "min_device_interval", CFG_TYPE_UINT32,
      offsetof(app_config_t, profinet.min_device_interval), 0 },
    { "profinet", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, profinet.enabled), 0 },
    { "profinet", "controller_ip", CFG_TYPE_STRING,
      offsetof(app_config_t, profinet.controller_ip),
      sizeof(((app_config_t*)0)->profinet.controller_ip) },
    { "profinet", "controller_name", CFG_TYPE_STRING,
      offsetof(app_config_t, profinet.controller_name),
      sizeof(((app_config_t*)0)->profinet.controller_name) },
    { "profinet", "data_dir", CFG_TYPE_STRING,
      offsetof(app_config_t, profinet.data_dir),
      sizeof(((app_config_t*)0)->profinet.data_dir) },

    /* Database section */
    { "database", "path", CFG_TYPE_STRING,
      offsetof(app_config_t, database.path),
      sizeof(((app_config_t*)0)->database.path) },
    { "database", "create_if_missing", CFG_TYPE_BOOL,
      offsetof(app_config_t, database.create_if_missing), 0 },
    { "database", "busy_timeout_ms", CFG_TYPE_INT,
      offsetof(app_config_t, database.busy_timeout_ms), 0 },

    /* Logging section */
    { "logging", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, logging.enabled), 0 },
    { "logging", "interval_seconds", CFG_TYPE_INT,
      offsetof(app_config_t, logging.interval_seconds), 0 },
    { "logging", "retention_days", CFG_TYPE_INT,
      offsetof(app_config_t, logging.retention_days), 0 },
    { "logging", "destination", CFG_TYPE_INT,
      offsetof(app_config_t, logging.destination), 0 },
    { "logging", "remote_url", CFG_TYPE_STRING,
      offsetof(app_config_t, logging.remote_url),
      sizeof(((app_config_t*)0)->logging.remote_url) },
    { "logging", "remote_enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, logging.remote_enabled), 0 },

    /* Health section */
    { "health", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, health.enabled), 0 },
    { "health", "http_enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, health.http_enabled), 0 },
    { "health", "http_port", CFG_TYPE_UINT16,
      offsetof(app_config_t, health.http_port), 0 },
    { "health", "file_path", CFG_TYPE_STRING,
      offsetof(app_config_t, health.file_path),
      sizeof(((app_config_t*)0)->health.file_path) },
    { "health", "update_interval_seconds", CFG_TYPE_INT,
      offsetof(app_config_t, health.update_interval_seconds), 0 },

    /* LED section */
    { "led", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, led.enabled), 0 },
    { "led", "led_count", CFG_TYPE_INT,
      offsetof(app_config_t, led.led_count), 0 },
    { "led", "brightness", CFG_TYPE_INT,
      offsetof(app_config_t, led.brightness), 0 },
    { "led", "backend", CFG_TYPE_STRING,
      offsetof(app_config_t, led.backend),
      sizeof(((app_config_t*)0)->led.backend) },
    { "led", "spi_device", CFG_TYPE_STRING,
      offsetof(app_config_t, led.spi_device),
      sizeof(((app_config_t*)0)->led.spi_device) },
    { "led", "spi_speed_hz", CFG_TYPE_UINT32,
      offsetof(app_config_t, led.spi_speed_hz), 0 },
    { "led", "gpio_pin", CFG_TYPE_INT,
      offsetof(app_config_t, led.gpio_pin), 0 },
    { "led", "dma_channel", CFG_TYPE_INT,
      offsetof(app_config_t, led.dma_channel), 0 },

    /* Watchdog section - actuator timeout configuration */
    { "watchdog", "interval_ms", CFG_TYPE_INT,
      offsetof(app_config_t, watchdog.watchdog_interval_ms), 0 },
    { "watchdog", "command_timeout_ms", CFG_TYPE_INT,
      offsetof(app_config_t, watchdog.command_timeout_ms), 0 },
    { "watchdog", "degraded_alarm_delay_ms", CFG_TYPE_INT,
      offsetof(app_config_t, watchdog.degraded_alarm_delay_ms), 0 },
};

#define CONFIG_FIELD_COUNT (sizeof(config_fields) / sizeof(config_fields[0]))

/* Generic field loader - handles all types via the descriptor */
static void config_load_field(
    config_manager_t *m,
    app_config_t *c,
    const config_field_t *field
) {
    void *target = (char*)c + field->offset;
    char str_buf[MAX_CONFIG_VALUE_LEN];
    int int_val;
    bool bool_val;

    switch (field->type) {
        case CFG_TYPE_STRING:
            if (config_get_string(m, field->section, field->key,
                                  str_buf, sizeof(str_buf)) == RESULT_OK) {
                SAFE_STRNCPY((char*)target, str_buf, field->size);
            }
            break;

        case CFG_TYPE_INT:
            if (config_get_int(m, field->section, field->key,
                               &int_val) == RESULT_OK) {
                *(int*)target = int_val;
            }
            break;

        case CFG_TYPE_BOOL:
            if (config_get_bool(m, field->section, field->key,
                                &bool_val) == RESULT_OK) {
                *(bool*)target = bool_val;
            }
            break;

        case CFG_TYPE_UINT16:
            if (config_get_int(m, field->section, field->key,
                               &int_val) == RESULT_OK) {
                *(uint16_t*)target = (uint16_t)int_val;
            }
            break;

        case CFG_TYPE_UINT32:
            if (config_get_int(m, field->section, field->key,
                               &int_val) == RESULT_OK) {
                *(uint32_t*)target = (uint32_t)int_val;
            }
            break;
    }
}

/* ============================================================================
 * Station ID Detection
 * ============================================================================
 * What: Generate unique station ID from hardware MAC address
 * Why: Multiple RTUs need unique identifiers tied to physical hardware
 * Format: rtu-XXXX where XXXX is last 4 hex chars of primary MAC
 * Note: "rtu" aligns with control station / RTU architecture naming
 */
static void detect_station_id(char *station_name, size_t size) {
    char best_iface[64] = {0};
    char mac_path[256];
    char mac_addr[18] = {0};
    FILE *f;

    /* Default fallback */
    SAFE_STRNCPY(station_name, "rtu-0000", size);

    if (!hw_detect_network_interface(best_iface, sizeof(best_iface))) return;

    /* Read MAC address */
    snprintf(mac_path, sizeof(mac_path), "/sys/class/net/%s/address", best_iface);

    f = fopen(mac_path, "r");
    if (!f) return;

    if (fgets(mac_addr, sizeof(mac_addr), f)) {
        /* MAC format: aa:bb:cc:dd:ee:ff - extract last 4 hex chars */
        size_t len = strlen(mac_addr);
        if (len >= 17) {
            /* Remove newline if present */
            if (mac_addr[len-1] == '\n') mac_addr[len-1] = '\0';

            /* Skip all-zeros MAC */
            if (strcmp(mac_addr, "00:00:00:00:00:00") != 0) {
                /* Extract last 4 hex digits (positions 12,13 and 15,16) */
                char suffix[5];
                suffix[0] = mac_addr[12];
                suffix[1] = mac_addr[13];
                suffix[2] = mac_addr[15];
                suffix[3] = mac_addr[16];
                suffix[4] = '\0';

                /* Convert to lowercase */
                for (int i = 0; i < 4; i++) {
                    if (suffix[i] >= 'A' && suffix[i] <= 'F') {
                        suffix[i] = suffix[i] - 'A' + 'a';
                    }
                }

                snprintf(station_name, size, "rtu-%s", suffix);
            }
        }
    }
    fclose(f);
}

static char *trim(char *str) {
    char *end;

    /* Trim leading whitespace */
    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == 0) {
        return str;
    }

    /* Trim trailing whitespace */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }

    *(end + 1) = '\0';
    return str;
}

static config_entry_t *find_entry(config_manager_t *mgr, const char *section, const char *key) {
    for (int i = 0; i < mgr->entry_count; i++) {
        if (strcasecmp(mgr->entries[i].section, section) == 0 &&
            strcasecmp(mgr->entries[i].key, key) == 0) {
            return &mgr->entries[i];
        }
    }
    return NULL;
}

result_t config_manager_init(config_manager_t *mgr) {
    CHECK_NULL(mgr);
    memset(mgr, 0, sizeof(*mgr));
    return RESULT_OK;
}

void config_manager_destroy(config_manager_t *mgr) {
    if (mgr) {
        memset(mgr, 0, sizeof(*mgr));
    }
}

result_t config_load_file(config_manager_t *mgr, const char *path) {
    CHECK_NULL(mgr);
    CHECK_NULL(path);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Provide detailed error context for operators troubleshooting config issues */
        LOG_WARNING("Cannot open config file '%s': %s (errno=%d)",
                    path, strerror(errno), errno);
        return RESULT_IO_ERROR;
    }

    SAFE_STRNCPY(mgr->config_path, path, sizeof(mgr->config_path));
    mgr->entry_count = 0;

    char line[1024];
    char current_section[MAX_NAME_LEN] = "default";

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);

        /* Skip empty lines and comments */
        if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') {
            continue;
        }

        /* Section header */
        if (*trimmed == '[') {
            char *end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';
                SAFE_STRNCPY(current_section, trimmed + 1, sizeof(current_section));
            }
            continue;
        }

        /* Key=value pair */
        char *eq = strchr(trimmed, '=');
        if (eq && mgr->entry_count < MAX_CONFIG_ENTRIES) {
            *eq = '\0';
            char *key = trim(trimmed);
            char *value = trim(eq + 1);

            /* Strip surrounding quotes */
            size_t value_len = strlen(value);
            if (value_len >= 2) {
                if ((value[0] == '"' && value[value_len - 1] == '"') ||
                    (value[0] == '\'' && value[value_len - 1] == '\'')) {
                    value[value_len - 1] = '\0';
                    value++;
                }
            }

            config_entry_t *entry = &mgr->entries[mgr->entry_count++];
            SAFE_STRNCPY(entry->section, current_section, sizeof(entry->section));
            SAFE_STRNCPY(entry->key, key, sizeof(entry->key));
            SAFE_STRNCPY(entry->value, value, sizeof(entry->value));
        }
    }

    fclose(f);
    LOG_INFO("Loaded %d entries from %s", mgr->entry_count, path);
    return RESULT_OK;
}

result_t config_save_file(config_manager_t *mgr, const char *path) {
    CHECK_NULL(mgr);

    const char *save_path = path ? path : mgr->config_path;
    if (!save_path || strlen(save_path) == 0) {
        return RESULT_INVALID_PARAM;
    }

    FILE *f = fopen(save_path, "w");
    if (!f) {
        return RESULT_IO_ERROR;
    }

    fprintf(f, "# Water-Treat RTU Configuration\n\n");

    char current_section[MAX_NAME_LEN] = "";

    for (int i = 0; i < mgr->entry_count; i++) {
        /* Start new section if needed */
        if (strcmp(current_section, mgr->entries[i].section) != 0) {
            if (current_section[0] != '\0') {
                fprintf(f, "\n");
            }
            fprintf(f, "[%s]\n", mgr->entries[i].section);
            SAFE_STRNCPY(current_section, mgr->entries[i].section, sizeof(current_section));
        }

        fprintf(f, "%s = %s\n", mgr->entries[i].key, mgr->entries[i].value);
    }

    fclose(f);
    mgr->modified = false;
    return RESULT_OK;
}

result_t config_get_string(config_manager_t *mgr, const char *section,
                           const char *key, char *value, size_t size) {
    CHECK_NULL(mgr);
    CHECK_NULL(section);
    CHECK_NULL(key);
    CHECK_NULL(value);

    config_entry_t *entry = find_entry(mgr, section, key);
    if (!entry) {
        return RESULT_NOT_FOUND;
    }

    SAFE_STRNCPY(value, entry->value, size);
    return RESULT_OK;
}

result_t config_set_string(config_manager_t *mgr, const char *section,
                           const char *key, const char *value) {
    CHECK_NULL(mgr);
    CHECK_NULL(section);
    CHECK_NULL(key);
    CHECK_NULL(value);

    config_entry_t *entry = find_entry(mgr, section, key);

    if (entry) {
        /* Update existing entry */
        SAFE_STRNCPY(entry->value, value, sizeof(entry->value));
    } else {
        /* Create new entry */
        if (mgr->entry_count >= MAX_CONFIG_ENTRIES) {
            return RESULT_NO_MEMORY;
        }

        entry = &mgr->entries[mgr->entry_count++];
        SAFE_STRNCPY(entry->section, section, sizeof(entry->section));
        SAFE_STRNCPY(entry->key, key, sizeof(entry->key));
        SAFE_STRNCPY(entry->value, value, sizeof(entry->value));
    }

    mgr->modified = true;
    return RESULT_OK;
}

result_t config_get_int(config_manager_t *mgr, const char *section,
                        const char *key, int *value) {
    char str[MAX_CONFIG_VALUE_LEN];

    result_t r = config_get_string(mgr, section, key, str, sizeof(str));
    if (r != RESULT_OK) {
        return r;
    }

    *value = (int)strtol(str, NULL, 0);
    return RESULT_OK;
}

result_t config_get_bool(config_manager_t *mgr, const char *section,
                         const char *key, bool *value) {
    char str[MAX_CONFIG_VALUE_LEN];

    result_t r = config_get_string(mgr, section, key, str, sizeof(str));
    if (r != RESULT_OK) {
        return r;
    }

    *value = (strcasecmp(str, "true") == 0 ||
              strcasecmp(str, "yes") == 0 ||
              strcasecmp(str, "1") == 0);
    return RESULT_OK;
}

void config_get_defaults(app_config_t *c) {
    memset(c,0,sizeof(*c));

    /* System defaults */
    detect_station_id(c->system.device_name, sizeof(c->system.device_name));
    SAFE_STRNCPY(c->system.log_level,"info",sizeof(c->system.log_level));
    SAFE_STRNCPY(c->system.log_file,"/var/log/water-treat/monitor.log",sizeof(c->system.log_file));
    c->system.daemon_mode=false;

    /* Network defaults - auto-detect interface */
    hw_detect_network_interface(c->network.interface, sizeof(c->network.interface));
    c->network.dhcp_enabled=true;
    /* ip_address, netmask, gateway left empty (DHCP) */

    /* PROFINET defaults - station name derived from MAC */
    detect_station_id(c->profinet.station_name, sizeof(c->profinet.station_name));
    SAFE_STRNCPY(c->profinet.product_name,"Water Treatment RTU",sizeof(c->profinet.product_name));
    c->profinet.vendor_id=WT_PROFINET_VENDOR_ID;
    c->profinet.device_id=WT_PROFINET_DEVICE_ID;
    c->profinet.min_device_interval=WT_PROFINET_MIN_INTERVAL;
    c->profinet.enabled=true;
    /* Controller fields - empty until configured or auto-discovered */
    c->profinet.controller_ip[0]='\0';
    c->profinet.controller_name[0]='\0';
    /* p-net NV storage - derived from data directory, overridable via config */
    SAFE_STRNCPY(c->profinet.data_dir,"/var/lib/water-treat/pnet",sizeof(c->profinet.data_dir));

    /* Database defaults */
    SAFE_STRNCPY(c->database.path,"/var/lib/water-treat/data.db",sizeof(c->database.path));
    c->database.create_if_missing=true;
    c->database.busy_timeout_ms=5000;

    /* Logging defaults */
    c->logging.enabled=true;
    c->logging.interval_seconds=60;
    c->logging.retention_days=30;
    c->logging.destination=1; /* Local */
    c->logging.remote_enabled=false;

    /* Health check defaults - see docs/decisions/DR-001-port-allocation.md */
    c->health.enabled=true;
    c->health.http_enabled=true;
    c->health.http_port=WT_HTTP_PORT_DEFAULT;
    SAFE_STRNCPY(c->health.file_path,"/var/lib/water-treat/health.prom",sizeof(c->health.file_path));
    c->health.update_interval_seconds=10;

    /* LED indicator defaults (disabled by default)
     * Use board detection for SPI device and GPIO pin - board agnostic */
    c->led.enabled=false;
    c->led.led_count=8;
    c->led.brightness=64;  /* 25% - safe default */
    SAFE_STRNCPY(c->led.backend,"auto",sizeof(c->led.backend));
    c->led.spi_speed_hz=2400000;
    c->led.dma_channel=10;

    /* Get board-specific SPI and GPIO pin defaults */
    board_info_t board_info;
    if (board_detect(&board_info) == RESULT_OK) {
        /* Construct SPI device path from detected board */
        SAFE_SNPRINTF(c->led.spi_device, sizeof(c->led.spi_device),
                      "/dev/spidev%d.0", board_info.pins.spi_bus);
        c->led.gpio_pin = board_info.pins.pwm_channel_0;
        LOG_DEBUG("Config: Using detected board SPI bus %d, PWM pin %d",
                  board_info.pins.spi_bus, board_info.pins.pwm_channel_0);
    } else {
        /* Fallback to common defaults if board detection fails */
        SAFE_STRNCPY(c->led.spi_device, "/dev/spidev0.0", sizeof(c->led.spi_device));
        c->led.gpio_pin = 18;
        LOG_WARNING("Board detection failed — using fallback SPI=/dev/spidev0.0, GPIO=18. "
                    "Verify these are correct for your hardware.");
    }

    /* Watchdog defaults - see config_defaults.h for rationale */
    c->watchdog.watchdog_interval_ms = WT_WATCHDOG_INTERVAL_MS;
    c->watchdog.command_timeout_ms = WT_COMMAND_TIMEOUT_MS;
    c->watchdog.degraded_alarm_delay_ms = WT_DEGRADED_ALARM_DELAY_MS;
}

result_t config_load_app_config(config_manager_t *m, app_config_t *c) {
    CHECK_NULL(m);
    CHECK_NULL(c);

    /* Set defaults first - missing config entries use these */
    config_get_defaults(c);

    /* Load all fields from table - errors are non-fatal (keeps default) */
    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++) {
        config_load_field(m, c, &config_fields[i]);
    }

    return RESULT_OK;
}
