/**
 * @file config_defaults.h
 * @brief Central configuration defaults for Water-Treat RTU
 *
 * All configurable defaults are defined here. Override via:
 *   1. Command-line arguments (highest precedence)
 *   2. Environment variables (WT_* prefix)
 *   3. Configuration file
 *   4. Compiled defaults (this file)
 *
 * See: docs/decisions/DR-001-port-allocation.md for port allocation rationale
 */

#ifndef CONFIG_DEFAULTS_H
#define CONFIG_DEFAULTS_H

/* ============================================================================
 * HTTP Server Configuration
 * ============================================================================
 * Port allocation scheme:
 *   8xxx = Controller plane (Water-Controller on SBC #1)
 *   9xxx = RTU plane (Water-Treat on SBC #2)
 *
 * This prevents port conflicts when running both on a single device.
 */
#ifndef WT_HTTP_PORT_DEFAULT
#define WT_HTTP_PORT_DEFAULT    9081
#endif

#define WT_HTTP_PORT_MIN        1
#define WT_HTTP_PORT_MAX        65535

/* Environment variable names - all use WT_ prefix for consistency */
#define WT_HTTP_PORT_ENV        "WT_HTTP_PORT"

/* ============================================================================
 * PROFINET Configuration
 * ============================================================================
 * PROFINET device identity per GSD file:
 *   gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml
 */
#define WT_PROFINET_VENDOR_ID       0x0493  /* Assigned vendor ID for training use */
#define WT_PROFINET_DEVICE_ID       0x0001  /* Device type identifier */
#define WT_PROFINET_MIN_INTERVAL    32      /* Minimum update interval (cycles) */
#define WT_PROFINET_TICK_INTERVAL_US 1000   /* PROFINET stack tick rate (microseconds) */
#define WT_PROFINET_MAX_SLOTS       64      /* Maximum I/O modules supported */
#define WT_PROFINET_DATA_SIZE       256     /* Maximum data payload size per slot */

/* ============================================================================
 * Database Configuration
 * ============================================================================ */
#define WT_DATABASE_PATH            "/var/lib/water-treat/water-treat.db"
#define WT_DATABASE_TIMEOUT_MS      5000    /* SQLite busy timeout */

/* ============================================================================
 * Logging Configuration
 * ============================================================================ */
#define WT_LOG_LEVEL_DEFAULT        "info"
#define WT_LOG_BUFFER_SIZE          4096    /* Maximum log message length */
#define WT_LOG_RETENTION_DAYS       30      /* Days to keep data logs */

/* Data logger queue settings */
#define WT_LOG_QUEUE_SIZE           1000    /* Max pending log entries */
#define WT_LOG_BATCH_SIZE           100     /* Entries per write batch */
#define WT_LOG_REMOTE_BATCH         50      /* Entries per remote upload */
#define WT_LOG_REMOTE_RETRY_MS      60000   /* Remote retry interval (60 sec) */

/* ============================================================================
 * Alarm Configuration
 * ============================================================================ */
#define WT_ALARM_MAX_RULES          256     /* Maximum alarm definitions */
#define WT_ALARM_CHECK_INTERVAL_MS  1000    /* Alarm evaluation frequency */
#define WT_ALARM_HYSTERESIS_PCT     5       /* Default hysteresis percentage */

/* ============================================================================
 * Actuator/Watchdog Configuration
 * ============================================================================
 * Configurable timeouts for actuator watchdog. Adjust these if experiencing
 * false-positive degraded mode alarms due to network latency.
 */
#define WT_WATCHDOG_INTERVAL_MS         1000    /* How often watchdog thread runs */
#define WT_COMMAND_TIMEOUT_MS           5000    /* Max time without command before concern */
#define WT_DEGRADED_ALARM_DELAY_MS      3000    /* Delay before declaring degraded mode */

/* ============================================================================
 * Station Identity
 * ============================================================================ */
#define WT_STATION_NAME_DEFAULT     "water-treat-rtu"

/* ============================================================================
 * Environment Variables
 * ============================================================================
 * All environment overrides use WT_ prefix for namespace isolation.
 */
#define WT_HTTP_PORT_ENV            "WT_HTTP_PORT"
#define WT_LOG_LEVEL_ENV            "WT_LOG_LEVEL"
#define WT_DATABASE_PATH_ENV        "WT_DATABASE_PATH"
#define WT_STATION_NAME_ENV         "WT_STATION_NAME"
#define WT_INTERFACE_ENV            "WT_INTERFACE"

#endif /* CONFIG_DEFAULTS_H */
