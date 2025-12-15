/**
 * PROFINET Monitor - Main Entry Point
 *
 * Raspberry Pi sensor hub with PROFINET interface.
 * Integrates sensor polling, alarm management, data logging,
 * PROFINET communication, and TUI interface.
 */

#include "common.h"
#include "utils/logger.h"
#include "config/config.h"
#include "db/database.h"
#include "db/db_events.h"
#include "sensors/sensor_manager.h"
#include "alarms/alarm_manager.h"
#include "logging/data_logger.h"
#include "profinet/profinet_manager.h"
#include "tui/tui_main.h"

#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>

/* ============================================================================
 * Global State
 * ========================================================================== */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_config = 0;

static database_t g_db;
static config_manager_t g_config_mgr;
static app_config_t g_app_config;
static sensor_manager_t g_sensor_mgr;

/* ============================================================================
 * Signal Handlers
 * ========================================================================== */

static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGHUP) {
        g_reload_config = 1;
    }
}

static void setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    // Ignore SIGPIPE (can occur with network operations)
    signal(SIGPIPE, SIG_IGN);
}

/* ============================================================================
 * Configuration
 * ========================================================================== */

static const char* find_config_file(void) {
    static const char *paths[] = {
        "/etc/profinet-monitor/profinet-monitor.conf",
        "/etc/profinet-monitor.conf",
        "./profinet-monitor.conf",
        NULL
    };

    for (int i = 0; paths[i] != NULL; i++) {
        if (access(paths[i], R_OK) == 0) {
            return paths[i];
        }
    }
    return NULL;
}

static result_t load_configuration(const char *config_path) {
    config_manager_init(&g_config_mgr);
    config_get_defaults(&g_app_config);

    if (config_path && access(config_path, R_OK) == 0) {
        result_t r = config_load_file(&g_config_mgr, config_path);
        if (r == RESULT_OK) {
            config_load_app_config(&g_config_mgr, &g_app_config);
            LOG_INFO("Loaded configuration from %s", config_path);
        } else {
            LOG_WARNING("Failed to load config file, using defaults");
        }
    } else {
        LOG_INFO("No configuration file found, using defaults");
    }

    return RESULT_OK;
}

/* ============================================================================
 * Subsystem Initialization
 * ========================================================================== */

static result_t init_database(void) {
    // Ensure database directory exists
    char *dir = strdup(g_app_config.database.path);
    if (dir) {
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            mkdir(dir, 0755);
        }
        free(dir);
    }

    result_t r = database_init(&g_db, g_app_config.database.path);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to initialize database at %s", g_app_config.database.path);
        return r;
    }

    LOG_INFO("Database initialized: %s", g_app_config.database.path);
    DB_EVENT_INFO(&g_db, "system", "PROFINET Monitor started");

    return RESULT_OK;
}

static result_t init_profinet(void) {
    if (!g_app_config.profinet.enabled) {
        LOG_INFO("PROFINET is disabled in configuration");
        return RESULT_OK;
    }

    result_t r = profinet_manager_init(&g_db, &g_app_config.profinet);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to initialize PROFINET manager");
        return r;
    }

    r = profinet_manager_start(g_app_config.network.interface);
    if (r != RESULT_OK) {
        LOG_WARNING("Failed to start PROFINET stack (may need p-net library)");
        // Non-fatal - continue without PROFINET
    }

    return RESULT_OK;
}

static result_t init_sensors(void) {
    // Pass NULL for profinet_mgr if PROFINET is disabled
    void *pn_mgr = g_app_config.profinet.enabled ? (void*)1 : NULL;

    result_t r = sensor_manager_init(&g_sensor_mgr, &g_db, (profinet_manager_t*)pn_mgr);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to initialize sensor manager");
        return r;
    }

    r = sensor_manager_start(&g_sensor_mgr);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to start sensor manager");
        return r;
    }

    LOG_INFO("Sensor manager started");
    return RESULT_OK;
}

static result_t init_alarms(void) {
    result_t r = alarm_manager_init(&g_db);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to initialize alarm manager");
        return r;
    }

    r = alarm_manager_start();
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to start alarm manager");
        return r;
    }

    LOG_INFO("Alarm manager started");
    return RESULT_OK;
}

static result_t init_data_logger(void) {
    if (!g_app_config.logging.enabled) {
        LOG_INFO("Data logging is disabled in configuration");
        return RESULT_OK;
    }

    data_logger_config_t log_config = {
        .enabled = g_app_config.logging.enabled,
        .local_enabled = true,
        .remote_enabled = g_app_config.logging.remote_enabled,
        .interval_seconds = g_app_config.logging.interval_seconds,
        .retention_days = g_app_config.logging.retention_days,
        .device_name = "",
        .remote_url = "",
        .api_key = ""
    };

    SAFE_STRNCPY(log_config.device_name, g_app_config.system.device_name, sizeof(log_config.device_name));
    SAFE_STRNCPY(log_config.remote_url, g_app_config.logging.remote_url, sizeof(log_config.remote_url));

    result_t r = data_logger_init(&g_db, &log_config);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to initialize data logger");
        return r;
    }

    r = data_logger_start();
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to start data logger");
        return r;
    }

    LOG_INFO("Data logger started (interval: %ds)", g_app_config.logging.interval_seconds);
    return RESULT_OK;
}

/* ============================================================================
 * Shutdown
 * ========================================================================== */

static void shutdown_subsystems(void) {
    LOG_INFO("Shutting down subsystems...");

    // Stop in reverse order of initialization
    if (data_logger_is_running()) {
        data_logger_stop();
        data_logger_shutdown();
        LOG_INFO("Data logger stopped");
    }

    if (alarm_manager_is_running()) {
        alarm_manager_stop();
        alarm_manager_shutdown();
        LOG_INFO("Alarm manager stopped");
    }

    if (g_sensor_mgr.running) {
        sensor_manager_stop(&g_sensor_mgr);
        sensor_manager_destroy(&g_sensor_mgr);
        LOG_INFO("Sensor manager stopped");
    }

    if (profinet_manager_is_running()) {
        profinet_manager_stop();
        profinet_manager_shutdown();
        LOG_INFO("PROFINET manager stopped");
    }

    if (database_is_connected(&g_db)) {
        DB_EVENT_INFO(&g_db, "system", "PROFINET Monitor stopped");
        database_close(&g_db);
        LOG_INFO("Database closed");
    }

    config_manager_destroy(&g_config_mgr);
}

/* ============================================================================
 * Command Line Arguments
 * ========================================================================== */

static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  -c, --config FILE   Configuration file path\n");
    printf("  -d, --daemon        Run as daemon\n");
    printf("  -v, --verbose       Increase verbosity\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -V, --version       Show version information\n");
    printf("\n");
}

static void print_version(void) {
    printf("PROFINET Monitor v%s\n", VERSION_STRING);
    printf("Raspberry Pi Sensor Hub with PROFINET Interface\n");
    printf("Build: %s %s\n", __DATE__, __TIME__);
}

/* ============================================================================
 * Main Entry Point
 * ========================================================================== */

int main(int argc, char *argv[]) {
    const char *config_path = NULL;
    bool daemon_mode = false;
    int verbose_level = 0;

    // Parse command line arguments
    static struct option long_options[] = {
        {"config",  required_argument, 0, 'c'},
        {"daemon",  no_argument,       0, 'd'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {"version", no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:dvhV", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'd':
                daemon_mode = true;
                break;
            case 'v':
                verbose_level++;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'V':
                print_version();
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Print banner
    print_version();
    printf("\n");

    // Initialize logger
    log_level_t log_level = LOG_LEVEL_INFO;
    if (verbose_level >= 2) log_level = LOG_LEVEL_TRACE;
    else if (verbose_level >= 1) log_level = LOG_LEVEL_DEBUG;

    logger_config_t log_cfg = {
        .level = log_level,
        .destinations = daemon_mode ? LOG_DEST_FILE : (LOG_DEST_CONSOLE | LOG_DEST_FILE),
        .include_timestamp = true,
        .include_source = true
    };
    logger_init(&log_cfg);

    LOG_INFO("Starting PROFINET Monitor v%s", VERSION_STRING);

    // Setup signal handlers
    setup_signal_handlers();

    // Find and load configuration
    if (!config_path) {
        config_path = find_config_file();
    }

    if (load_configuration(config_path) != RESULT_OK) {
        LOG_ERROR("Failed to load configuration");
        return 1;
    }

    // Override daemon mode from config if specified on command line
    if (daemon_mode) {
        g_app_config.system.daemon_mode = true;
    }

    // Initialize database
    if (init_database() != RESULT_OK) {
        LOG_ERROR("Database initialization failed");
        logger_shutdown();
        return 1;
    }

    // Initialize PROFINET
    if (init_profinet() != RESULT_OK) {
        LOG_WARNING("PROFINET initialization failed, continuing without it");
    }

    // Initialize sensors
    if (init_sensors() != RESULT_OK) {
        LOG_ERROR("Sensor manager initialization failed");
        shutdown_subsystems();
        logger_shutdown();
        return 1;
    }

    // Initialize alarm manager
    if (init_alarms() != RESULT_OK) {
        LOG_ERROR("Alarm manager initialization failed");
        shutdown_subsystems();
        logger_shutdown();
        return 1;
    }

    // Initialize data logger
    if (init_data_logger() != RESULT_OK) {
        LOG_WARNING("Data logger initialization failed, continuing without it");
    }

    LOG_INFO("All subsystems initialized successfully");
    LOG_INFO("Device: %s", g_app_config.system.device_name);
    LOG_INFO("PROFINET Station: %s", g_app_config.profinet.station_name);

    // Run TUI or daemon mode
    if (g_app_config.system.daemon_mode) {
        LOG_INFO("Running in daemon mode");

        // Main daemon loop
        while (g_running) {
            // Check for config reload signal
            if (g_reload_config) {
                LOG_INFO("Reloading configuration...");
                load_configuration(config_path);
                sensor_manager_reload_sensors(&g_sensor_mgr);
                g_reload_config = 0;
            }

            // Sleep for a bit
            sleep(1);
        }
    } else {
        // Initialize and run TUI
        result_t r = tui_init(&g_db, &g_config_mgr, &g_app_config);
        if (r != RESULT_OK) {
            LOG_ERROR("Failed to initialize TUI");
            shutdown_subsystems();
            logger_shutdown();
            return 1;
        }

        LOG_INFO("Starting TUI interface");
        tui_run();
        tui_shutdown();
    }

    // Shutdown all subsystems
    LOG_INFO("Initiating shutdown...");
    shutdown_subsystems();

    logger_shutdown();

    printf("\nPROFINET Monitor stopped.\n");
    return 0;
}
