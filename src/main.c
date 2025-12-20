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
#include "config/config_validate.h"
#include "db/database.h"
#include "db/db_events.h"
#include "sensors/sensor_manager.h"
#include "actuators/actuator_manager.h"
#include "alarms/alarm_manager.h"
#include "logging/data_logger.h"
#include "profinet/profinet_manager.h"
#include "health/health_check.h"
#include "hal/led_status.h"
#include "tui/tui_main.h"
#include "tui/pages/page_wizard.h"

#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include "auth/auth.h"

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

/* ============================================================================
 * Global State
 * ========================================================================== */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_config = 0;

static database_t g_db;

/* Non-static globals - accessed by TUI and health modules */
config_manager_t g_config_mgr;
app_config_t g_app_config;
sensor_manager_t g_sensor_mgr;
actuator_manager_t g_actuator_mgr;

#ifdef LED_SUPPORT
led_status_manager_t g_led_mgr;  /* Non-static for health check access */
#endif

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

    /* Validate configuration and log warnings/errors */
    config_validation_result_t validation;
    if (config_validate(&g_app_config, &validation) != RESULT_OK) {
        LOG_ERROR("Configuration validation failed with %d error(s)", validation.error_count);
        config_validation_log(&validation);
    } else if (validation.warning_count > 0) {
        config_validation_log(&validation);
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

    r = auth_init(&g_db);
    if (r != RESULT_OK) {
        LOG_ERROR("Auth init failed");
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

/* Callback when actuator manager enters/exits degraded mode */
static void on_degraded_mode(bool degraded, void *ctx) {
    UNUSED(ctx);

    // Notify data logger of connection state change
    data_logger_notify_connection(!degraded);

#ifdef LED_SUPPORT
    // Update LED status for PROFINET connection
    if (g_led_mgr.initialized) {
        led_set_profinet_status(&g_led_mgr, !degraded, !degraded);
        led_set_system_status(&g_led_mgr, degraded ? LED_STATUS_WARNING : LED_STATUS_OK);
    }
#endif

    if (degraded) {
        LOG_WARNING("DEGRADED MODE: Controller disconnected - actuators maintaining last state");
        DB_EVENT_WARNING(&g_db, "system", "Entered DEGRADED MODE - controller disconnected");
    } else {
        LOG_INFO("NORMAL MODE: Controller reconnected");
        DB_EVENT_INFO(&g_db, "system", "Exited DEGRADED MODE - controller reconnected");
    }
}

static result_t init_actuators(void) {
    /* Initialize actuator manager - works in standalone mode (no PROFINET) */
    result_t r = actuator_manager_init(&g_actuator_mgr, &g_db);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to initialize actuator manager");
        return r;
    }

    // Set degraded mode callback to notify data logger
    actuator_manager_set_callback(&g_actuator_mgr, on_degraded_mode, NULL);

    // Load actuators from database
    r = actuator_manager_reload(&g_actuator_mgr);
    if (r != RESULT_OK) {
        LOG_WARNING("No actuators loaded from database (add via TUI)");
    }

    r = actuator_manager_start(&g_actuator_mgr);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to start actuator manager");
        return r;
    }

    if (g_app_config.profinet.enabled) {
        LOG_INFO("Actuator manager started (PROFINET mode)");
    } else {
        LOG_INFO("Actuator manager started (standalone mode - manual control only)");
    }
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

static result_t init_health_check(void) {
    if (!g_app_config.health.enabled) {
        LOG_INFO("Health check is disabled in configuration");
        return RESULT_OK;
    }

    health_config_t health_config = {
        .enabled = g_app_config.health.enabled,
        .http_enabled = g_app_config.health.http_enabled,
        .http_port = g_app_config.health.http_port,
        .update_interval_seconds = g_app_config.health.update_interval_seconds
    };
    SAFE_STRNCPY(health_config.file_path, g_app_config.health.file_path, sizeof(health_config.file_path));

    result_t r = health_check_init(&g_db, &health_config);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to initialize health check");
        return r;
    }

    r = health_check_start();
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to start health check");
        return r;
    }

    LOG_INFO("Health check started (port=%d, file=%s)",
             g_app_config.health.http_port, g_app_config.health.file_path);
    return RESULT_OK;
}

#ifdef LED_SUPPORT
static result_t init_led_status(void) {
    if (!g_app_config.led.enabled) {
        LOG_INFO("LED status indicator is disabled in configuration");
        return RESULT_OK;
    }

    /* Convert app config to LED config */
    led_config_t led_cfg;
    led_config_defaults(&led_cfg);
    led_cfg.led_count = g_app_config.led.led_count;
    led_cfg.brightness = g_app_config.led.brightness;
    SAFE_STRNCPY(led_cfg.spi_device, g_app_config.led.spi_device, sizeof(led_cfg.spi_device));
    led_cfg.spi_speed_hz = g_app_config.led.spi_speed_hz;
    led_cfg.gpio_pin = g_app_config.led.gpio_pin;
    led_cfg.dma_channel = g_app_config.led.dma_channel;

    /* Determine backend */
    if (strcmp(g_app_config.led.backend, "spi") == 0) {
        led_cfg.backend = LED_BACKEND_SPI;
    } else if (strcmp(g_app_config.led.backend, "rpi") == 0 ||
               strcmp(g_app_config.led.backend, "rpi_ws281x") == 0) {
        led_cfg.backend = LED_BACKEND_RPI_WS281X;
    } else {
        led_cfg.backend = LED_BACKEND_AUTO;
    }

    result_t r = led_status_init(&g_led_mgr, &led_cfg);
    if (r != RESULT_OK) {
        LOG_WARNING("Failed to initialize LED status indicator");
        return r;
    }

    /* Set initial system status */
    led_set_system_status(&g_led_mgr, LED_STATUS_INITIALIZING);
    led_status_update(&g_led_mgr);

    LOG_INFO("LED status indicator initialized (%d LEDs, backend=%s)",
             led_cfg.led_count, led_backend_name(led_cfg.backend));
    return RESULT_OK;
}
#endif /* LED_SUPPORT */

/* ============================================================================
 * Shutdown
 * ========================================================================== */

static void shutdown_subsystems(void) {
    LOG_INFO("Shutting down subsystems...");

    // Stop in reverse order of initialization
#ifdef LED_SUPPORT
    if (g_led_mgr.initialized) {
        led_status_cleanup(&g_led_mgr);
        LOG_INFO("LED status indicator stopped");
    }
#endif

    if (health_check_is_running()) {
        health_check_stop();
        health_check_shutdown();
        LOG_INFO("Health check stopped");
    }

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

    if (g_actuator_mgr.initialized) {
        actuator_manager_stop(&g_actuator_mgr);
        actuator_manager_destroy(&g_actuator_mgr);
        LOG_INFO("Actuator manager stopped");
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

    // Initialize actuators (PROFINET output -> pumps/valves)
    if (init_actuators() != RESULT_OK) {
        LOG_WARNING("Actuator manager initialization failed, continuing without it");
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

    // Initialize health check endpoint
    if (init_health_check() != RESULT_OK) {
        LOG_WARNING("Health check initialization failed, continuing without it");
    }

#ifdef LED_SUPPORT
    // Initialize LED status indicator
    if (init_led_status() != RESULT_OK) {
        LOG_WARNING("LED status initialization failed, continuing without it");
    }
#endif

    LOG_INFO("All subsystems initialized successfully");
    LOG_INFO("Device: %s", g_app_config.system.device_name);
    LOG_INFO("PROFINET Station: %s", g_app_config.profinet.station_name);

#ifdef LED_SUPPORT
    // Set LED status to OK now that all subsystems are initialized
    if (g_led_mgr.initialized) {
        led_set_system_status(&g_led_mgr, LED_STATUS_OK);
        led_set_profinet_status(&g_led_mgr, g_app_config.profinet.enabled, false);
        led_status_update(&g_led_mgr);
    }
#endif

#ifdef HAVE_SYSTEMD
    /* Notify systemd that we're ready - this unblocks "systemctl start"
     * Without this, Type=notify services would hang indefinitely */
    sd_notify(0, "READY=1\n"
                 "STATUS=All subsystems initialized");
    LOG_INFO("Notified systemd: service ready");

    /* Check if watchdog is enabled */
    uint64_t watchdog_usec = 0;
    int wd_enabled = sd_watchdog_enabled(0, &watchdog_usec);
    if (wd_enabled > 0) {
        LOG_INFO("Systemd watchdog enabled (interval: %lu ms)", watchdog_usec / 1000);
    }
#endif

    // Run TUI or daemon mode
    if (g_app_config.system.daemon_mode) {
        LOG_INFO("Running in daemon mode");

#ifdef HAVE_SYSTEMD
        int watchdog_interval_sec = (wd_enabled > 0) ? (watchdog_usec / 2000000) : 0;
        if (watchdog_interval_sec < 1) watchdog_interval_sec = 1;
        int loop_counter = 0;
#endif

        // Main daemon loop
        while (g_running) {
            // Check for config reload signal
            if (g_reload_config) {
                LOG_INFO("Reloading configuration...");
#ifdef HAVE_SYSTEMD
                sd_notify(0, "RELOADING=1");
#endif
                load_configuration(config_path);
                sensor_manager_reload_sensors(&g_sensor_mgr);
                g_reload_config = 0;
#ifdef HAVE_SYSTEMD
                sd_notify(0, "READY=1\n"
                             "STATUS=Configuration reloaded");
#endif
            }

#ifdef HAVE_SYSTEMD
            /* Pet the watchdog at half the configured interval
             * If we don't call this, systemd will restart the service */
            if (wd_enabled > 0) {
                loop_counter++;
                if (loop_counter >= watchdog_interval_sec) {
                    sd_notify(0, "WATCHDOG=1");
                    loop_counter = 0;
                }
            }
#endif

#ifdef LED_SUPPORT
            // Update LED animations (limited rate in daemon mode)
            if (g_led_mgr.initialized) {
                led_status_update(&g_led_mgr);
            }
#endif

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

        // Give TUI access to sensor manager for live updates
        tui_set_sensor_manager(&g_sensor_mgr);
#ifdef LED_SUPPORT
        if (g_led_mgr.initialized) {
            tui_set_led_manager(&g_led_mgr);
        }
#endif

        // Check for first run and offer setup wizard
        if (config_is_first_run(&g_app_config)) {
            LOG_INFO("First-run detected - launching setup wizard");
            page_wizard_init();
            if (wizard_run() == RESULT_OK) {
                LOG_INFO("Setup wizard completed successfully");
                // Reload config after wizard
                load_configuration(config_path);
            } else {
                LOG_INFO("Setup wizard skipped or cancelled");
            }
        }

        LOG_INFO("Starting TUI interface");
        tui_run();
        tui_shutdown();
    }

    // Shutdown all subsystems
    LOG_INFO("Initiating shutdown...");
#ifdef HAVE_SYSTEMD
    sd_notify(0, "STOPPING=1\n"
                 "STATUS=Shutting down subsystems");
#endif
    shutdown_subsystems();

    logger_shutdown();

    printf("\nPROFINET Monitor stopped.\n");
    return 0;
}
