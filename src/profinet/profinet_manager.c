/**
 * @file profinet_manager.c
 * @brief PROFINET I/O Device manager using p-net stack
 */

#define _GNU_SOURCE  /* Required for various GNU extensions */
#include "profinet_manager.h"
#include "profinet_callbacks.h"
#include "controller_discovery.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "utils/logger.h"
#include "gsdml_modules.h"
#include "platform/hw_discover.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>      /* errno, strerror for error reporting */
#include <dirent.h>     /* opendir, readdir for interface detection */
#include <sys/stat.h>   /* mkdir for p-net NV storage directory */
#include <arpa/inet.h>  /* htonl, ntohl for network byte order per DEVELOPMENT_GUIDELINES.md */
#include <net/if.h>     /* IFF_UP, IFF_RUNNING, IFF_PROMISC for interface detection */
#include <ifaddrs.h>    /* getifaddrs, freeifaddrs for IP configuration */
#include <sys/socket.h> /* socket, AF_PACKET for raw socket test */
#include <sys/ioctl.h>  /* ioctl for setting promiscuous mode */
#include <linux/if_packet.h> /* SOCK_RAW for PROFINET */

#define PROFINET_TICK_INTERVAL_US   1000
#define MAX_PROFINET_SLOTS          247
#define PROFINET_DATA_SIZE          256

/* Stuck state detection timeouts (milliseconds) */
#define STATE_TIMEOUT_CONNECTING_MS     30000   /* 30s in CONNECTING before reset */
#define STATE_TIMEOUT_PARAM_END_MS      10000   /* 10s waiting for APPLRDY */
#define RECOVERY_CHECK_INTERVAL_MS      5000    /* Check every 5 seconds */

typedef struct {
    int slot;
    int subslot;
    int module_id;
    uint32_t module_ident;
    uint32_t submodule_ident;
    bool plugged;
    uint8_t input_data[PROFINET_DATA_SIZE];
    uint8_t output_data[PROFINET_DATA_SIZE];
    size_t input_size;
    size_t output_size;
    bool input_valid;
    bool output_valid;
    uint8_t input_iops;
} profinet_slot_t;

typedef struct {
#ifdef HAVE_PNET
    pnet_t *pnet;
    pnet_cfg_t pnet_cfg;
    uint32_t arep;  // Application relationship endpoint (set on connect)
#endif

    database_t *db;
    profinet_config_t config;

    profinet_slot_t slots[MAX_PROFINET_SLOTS];
    int slot_count;

    pthread_t tick_thread;
    pthread_mutex_t mutex;
    volatile bool running;
    bool initialized;
    bool connected;
    
    profinet_state_t state;
    profinet_state_t prev_state;
    uint64_t state_entry_time_ms;    /* When we entered current state */
    uint64_t last_recovery_check_ms; /* Last stuck-state check time */
    uint64_t last_tick_time;
    uint32_t cycle_count;

    /* Connection statistics */
    uint32_t connection_count;       /* Total successful connections */
    uint32_t disconnect_count;       /* Total disconnections */
    uint32_t error_count;            /* Total recoverable errors */
    uint32_t stuck_state_recoveries; /* Times we recovered from stuck state */

    // Callbacks
    profinet_connect_cb_t on_connect;
    profinet_disconnect_cb_t on_disconnect;
    profinet_data_cb_t on_data_received;
    void *callback_ctx;
} profinet_manager_t;

static profinet_manager_t g_pn = {0};

/* State transition with logging and timestamp */
static void set_state(profinet_state_t new_state) {
    if (g_pn.state == new_state) return;

    const char *old_name = profinet_state_to_string(g_pn.state);
    const char *new_name = profinet_state_to_string(new_state);

    LOG_INFO("PROFINET state: %s -> %s", old_name, new_name);

    g_pn.prev_state = g_pn.state;
    g_pn.state = new_state;
    g_pn.state_entry_time_ms = get_time_ms();

    /* Track statistics */
    if (new_state == PROFINET_STATE_CONNECTED) {
        g_pn.connection_count++;
    } else if (new_state == PROFINET_STATE_ERROR) {
        g_pn.error_count++;
    }
}

/* Track initialization failure for health reporting */
static char g_pn_init_error[256] = {0};
static bool g_pn_init_attempted = false;
static bool g_pn_disabled_by_config = false;

/* ============================================================================
 * Internal Functions
 * ========================================================================== */

static profinet_slot_t* find_slot(int slot, int subslot) {
    for (int i = 0; i < g_pn.slot_count; i++) {
        if (g_pn.slots[i].slot == slot && g_pn.slots[i].subslot == subslot) {
            return &g_pn.slots[i];
        }
    }
    return NULL;
}

static profinet_slot_t* add_slot(int slot, int subslot) {
    if (g_pn.slot_count >= MAX_PROFINET_SLOTS) return NULL;

    profinet_slot_t *s = &g_pn.slots[g_pn.slot_count++];
    memset(s, 0, sizeof(*s));
    s->slot = slot;
    s->subslot = subslot;
    return s;
}

/**
 * @brief Determine if a module is an actuator (output) based on GSDML ident
 *
 * Per gsdml_modules.h, module ID pattern is 0x0000XXYY where:
 *   - Sensors:   0x00000010 - 0x00000070 (bit 8 = 0)
 *   - Actuators: 0x00000100 - 0x00000120 (bit 8 = 1)
 *
 * @param module_ident GSDML module identifier
 * @return true if actuator (output module), false if sensor (input module)
 */
static bool is_actuator_module(uint32_t module_ident) {
    /* Actuator modules have bit 8 set (0x00000100 range) */
    return (module_ident & 0x00000100) != 0;
}

#ifdef HAVE_PNET
/**
 * @brief Clear p-net NV storage to ensure config is authoritative
 *
 * CRITICAL: p-net persists station_name to NV storage. If a controller sends
 * DCP Set-Name (e.g., with wrong name "rt-labs-dev"), p-net stores it and
 * ignores our configured station_name on subsequent boots.
 *
 * The old approach searched for "rt-labs-dev" string, but this fails because:
 * 1. Controller can re-contaminate via DCP Set-Name AFTER purge runs
 * 2. Any mismatched name is wrong, not just "rt-labs-dev"
 *
 * New approach: Delete the IP/station_name NV file unconditionally.
 * Our config file is the source of truth, not p-net's NV cache.
 *
 * The file is named "pf_ip_*" or similar (p-net internal naming).
 * We delete any file starting with "pf_" to be safe.
 *
 * Must be called BEFORE pnet_init() to ensure clean state.
 *
 * @param data_dir Path to p-net NV storage directory
 * @param configured_station Expected station name from config
 */
static void clear_pnet_nv_station(const char *data_dir, const char *configured_station) {
    if (!data_dir || data_dir[0] == '\0') {
        LOG_WARNING("p-net data_dir not set, NV files may be in CWD");
        return;
    }

    DIR *dir = opendir(data_dir);
    if (!dir) {
        /* Directory doesn't exist yet - nothing to clear */
        LOG_DEBUG("p-net NV directory doesn't exist yet: %s", data_dir);
        return;
    }

    LOG_INFO("Clearing p-net NV storage to enforce station_name='%s'", configured_station);

    struct dirent *entry;
    int cleared_count = 0;
    char filepath[512];

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (entry->d_name[0] == '.') {
            continue;
        }

        /*
         * p-net NV files use "pf_" prefix (pf_ip, pf_im, pf_pdport, etc.)
         * Delete all of them to ensure clean state.
         * This loses I&M data too, but station_name correctness is critical.
         */
        if (strncmp(entry->d_name, "pf_", 3) != 0) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "%s/%s", data_dir, entry->d_name);

        /* Check if it's a regular file */
        struct stat st;
        if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        LOG_INFO("Deleting p-net NV file: %s", filepath);
        if (unlink(filepath) == 0) {
            cleared_count++;
        } else {
            LOG_ERROR("Failed to delete %s: %s", filepath, strerror(errno));
        }
    }

    closedir(dir);

    if (cleared_count > 0) {
        LOG_INFO("Cleared %d p-net NV file(s). Station name forced to '%s'",
                 cleared_count, configured_station);
    } else {
        LOG_DEBUG("No p-net NV files found in %s", data_dir);
    }
}

static void poll_output_slots(void) {
    /* Poll all output slots for new data from controller */
    for (int i = 0; i < g_pn.slot_count; i++) {
        profinet_slot_t *slot = &g_pn.slots[i];

        /* Skip slots that don't have output data */
        if (!slot->plugged || slot->output_size == 0) {
            continue;
        }

        uint8_t data[PROFINET_DATA_SIZE];
        uint8_t iops;
        bool new_data;
        uint16_t len = slot->output_size;

        int ret = pnet_output_get_data_and_iops(g_pn.pnet, 0, slot->slot, slot->subslot,
                                                 &new_data, data, &len, &iops);
        if (ret == 0 && new_data && iops == PNET_IOXS_GOOD) {
            /* Check if data actually changed to avoid redundant callbacks */
            if (len > 0 && memcmp(data, slot->output_data, len) != 0) {
                /* New data received - cache and dispatch to listeners */
                memcpy(slot->output_data, data, len);
                slot->output_valid = true;

                /* Call the data callback (actuator manager handler) */
                if (g_pn.on_data_received) {
                    g_pn.on_data_received(slot->slot, slot->subslot, data, len, g_pn.callback_ctx);
                }
            }
        }
    }
}

/**
 * @brief Check for stuck states and trigger recovery
 *
 * Detects when the state machine is stuck:
 * - CONNECTING for > 30 seconds without progress
 * - Waiting for APPLRDY > 10 seconds after PRMEND
 *
 * Recovery clears p-net AR state and returns to IDLE.
 */
static void check_stuck_state(void) {
    uint64_t now = get_time_ms();

    /* Only check periodically to reduce overhead */
    if (now - g_pn.last_recovery_check_ms < RECOVERY_CHECK_INTERVAL_MS) {
        return;
    }
    g_pn.last_recovery_check_ms = now;

    uint64_t state_duration = now - g_pn.state_entry_time_ms;

    switch (g_pn.state) {
        case PROFINET_STATE_CONNECTING:
            if (state_duration > STATE_TIMEOUT_CONNECTING_MS) {
                LOG_WARNING("Stuck in CONNECTING state for %llu ms, resetting",
                            (unsigned long long)state_duration);
                g_pn.stuck_state_recoveries++;
                profinet_manager_clear_ar_state();
                set_state(PROFINET_STATE_READY);
            }
            break;

        case PROFINET_STATE_READY:
            /*
             * After clearing stale state, controller should retry within ~30s.
             * If we've been in READY for > 60s, log but don't reset
             * (controller may be offline).
             */
            if (state_duration > 60000 && (state_duration % 60000) < RECOVERY_CHECK_INTERVAL_MS) {
                LOG_DEBUG("Waiting for controller connection (%llu s)...",
                          (unsigned long long)(state_duration / 1000));
            }
            break;

        case PROFINET_STATE_ERROR:
            /*
             * Auto-recover from ERROR state after clearing NV files.
             * Give it 5 seconds then try to go back to READY.
             */
            if (state_duration > 5000) {
                LOG_INFO("Recovering from ERROR state, clearing AR and retrying");
                profinet_manager_clear_ar_state();
                set_state(PROFINET_STATE_READY);
            }
            break;

        default:
            /* IDLE, CONNECTED states don't need stuck detection */
            break;
    }
}

static void* profinet_tick_thread(void *arg) {
    UNUSED(arg);

    while (g_pn.running) {
        pthread_mutex_lock(&g_pn.mutex);

        if (g_pn.pnet) {
            pnet_handle_periodic(g_pn.pnet);
            g_pn.cycle_count++;

            /* Poll output slots when connected to controller */
            if (g_pn.connected) {
                poll_output_slots();
            }

            /* Check for stuck states and trigger recovery */
            check_stuck_state();
        }

        pthread_mutex_unlock(&g_pn.mutex);

        usleep(PROFINET_TICK_INTERVAL_US);
    }

    return NULL;
}
#endif

static result_t load_modules_from_db(void) {
    if (!g_pn.db) return RESULT_NOT_INITIALIZED;
    
    db_module_t *modules = NULL;
    int count = 0;
    
    result_t r = db_module_list(g_pn.db, &modules, &count);
    if (r != RESULT_OK) return r;
    
    for (int i = 0; i < count && i < MAX_PROFINET_SLOTS; i++) {
        profinet_slot_t *slot = add_slot(modules[i].slot, modules[i].subslot);
        if (slot) {
            slot->module_id = modules[i].id;
            slot->module_ident = modules[i].module_ident;
            slot->submodule_ident = modules[i].submodule_ident;

            /*
             * Set correct I/O sizes based on module type per GSDML specification.
             * Sensors are INPUT-only (device → controller).
             * Actuators are OUTPUT-only (controller → device).
             */
            if (is_actuator_module(slot->module_ident)) {
                slot->input_size = 0;
                slot->output_size = GSDML_ACTUATOR_OUTPUT_SIZE;  /* 4 bytes */
            } else {
                slot->input_size = GSDML_SENSOR_INPUT_SIZE;      /* 5 bytes */
                slot->output_size = 0;
            }

            LOG_DEBUG("Loaded slot %d: module_id=%d, ident=0x%08X, %s",
                      slot->slot, slot->module_id, slot->module_ident,
                      is_actuator_module(slot->module_ident) ? "OUTPUT" : "INPUT");
        }
    }
    
    if (modules) free(modules);
    
    LOG_INFO("Loaded %d modules from database", g_pn.slot_count);
    return RESULT_OK;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t profinet_manager_init(database_t *db, const profinet_config_t *config) {
    CHECK_NULL(db); CHECK_NULL(config);
    
    if (g_pn.initialized) return RESULT_OK;
    
    memset(&g_pn, 0, sizeof(g_pn));
    g_pn.db = db;
    memcpy(&g_pn.config, config, sizeof(profinet_config_t));
    
    pthread_mutex_init(&g_pn.mutex, NULL);

#ifdef HAVE_PNET
    // Configure p-net
    memset(&g_pn.pnet_cfg, 0, sizeof(g_pn.pnet_cfg));

    // Station name
    strncpy(g_pn.pnet_cfg.station_name, config->station_name, sizeof(g_pn.pnet_cfg.station_name) - 1);

    // Non-volatile storage directory for p-net
    // This controls where station_name, IP settings, and I&M data persist.
    // Without this, p-net uses CWD and may fall back to defaults like "rt-labs-dev".
    // Configurable via [profinet] data_dir in config file
    if (config->data_dir[0] != '\0') {
        strncpy(g_pn.pnet_cfg.file_directory, config->data_dir, sizeof(g_pn.pnet_cfg.file_directory) - 1);
    }

    // Device identity
    g_pn.pnet_cfg.device_id.vendor_id_hi = (config->vendor_id >> 8) & 0xFF;
    g_pn.pnet_cfg.device_id.vendor_id_lo = config->vendor_id & 0xFF;
    g_pn.pnet_cfg.device_id.device_id_hi = (config->device_id >> 8) & 0xFF;
    g_pn.pnet_cfg.device_id.device_id_lo = config->device_id & 0xFF;

    // Product name
    strncpy(g_pn.pnet_cfg.product_name, config->product_name, sizeof(g_pn.pnet_cfg.product_name) - 1);

    // Timing - tick_us MUST match our tick thread interval
    g_pn.pnet_cfg.tick_us = PROFINET_TICK_INTERVAL_US;
    g_pn.pnet_cfg.min_device_interval = config->min_device_interval;

    // Physical ports - REQUIRED by p-net
    g_pn.pnet_cfg.num_physical_ports = 1;
    // Note: physical_ports[0].netif_name is set in profinet_manager_start()
    // after we know the interface name

    // Callbacks
    g_pn.pnet_cfg.state_cb = profinet_state_callback;
    g_pn.pnet_cfg.connect_cb = profinet_connect_callback;
    g_pn.pnet_cfg.release_cb = profinet_release_callback;
    g_pn.pnet_cfg.dcontrol_cb = profinet_dcontrol_callback;
    g_pn.pnet_cfg.ccontrol_cb = profinet_ccontrol_callback;
    g_pn.pnet_cfg.read_cb = profinet_read_callback;
    g_pn.pnet_cfg.write_cb = profinet_write_callback;
    g_pn.pnet_cfg.exp_module_cb = profinet_exp_module_callback;
    g_pn.pnet_cfg.exp_submodule_cb = profinet_exp_submodule_callback;
    g_pn.pnet_cfg.new_data_status_cb = profinet_new_data_status_callback;
    g_pn.pnet_cfg.alarm_ind_cb = profinet_alarm_ind_callback;
    g_pn.pnet_cfg.alarm_cnf_cb = profinet_alarm_cnf_callback;
    g_pn.pnet_cfg.alarm_ack_cnf_cb = profinet_alarm_ack_cnf_callback;
    g_pn.pnet_cfg.reset_cb = profinet_reset_callback;
    g_pn.pnet_cfg.signal_led_cb = profinet_signal_led_callback;
    
    g_pn.pnet_cfg.cb_arg = &g_pn;
#endif
    
    // Load modules from database
    load_modules_from_db();

    // Initialize controller discovery
    controller_discovery_init(config);

    /* Initialize state machine with timestamp */
    g_pn.state = PROFINET_STATE_IDLE;
    g_pn.prev_state = PROFINET_STATE_IDLE;
    g_pn.state_entry_time_ms = get_time_ms();
    g_pn.last_recovery_check_ms = g_pn.state_entry_time_ms;
    g_pn.initialized = true;

    LOG_INFO("PROFINET manager initialized: station=%s, vendor=0x%04X, device=0x%04X",
             config->station_name, config->vendor_id, config->device_id);

    return RESULT_OK;
}

#ifdef HAVE_PNET
// Static buffer for network interface name (p-net v0.2.0 uses const char *)
static char g_netif_name[64] = {0};

/**
 * @brief Enable promiscuous mode on network interface
 *
 * PROFINET requires promiscuous mode for raw Ethernet frame handling.
 * DCP discovery uses multicast frames that need promisc mode to be received.
 *
 * @param iface Network interface name
 * @return true on success, false on failure
 */
static bool enable_promiscuous_mode(const char *iface) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_WARNING("Cannot create socket for promisc mode: %s", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    /* Get current flags */
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        LOG_WARNING("Cannot get interface flags for %s: %s", iface, strerror(errno));
        close(sock);
        return false;
    }

    /* Check if already in promisc mode */
    if (ifr.ifr_flags & IFF_PROMISC) {
        LOG_DEBUG("Interface %s already in promiscuous mode", iface);
        close(sock);
        return true;
    }

    /* Enable promisc mode */
    ifr.ifr_flags |= IFF_PROMISC;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        LOG_WARNING("Cannot enable promiscuous mode on %s: %s", iface, strerror(errno));
        close(sock);
        return false;
    }

    close(sock);
    LOG_INFO("Enabled promiscuous mode on %s", iface);
    return true;
}

/**
 * Get the default gateway from /proc/net/route
 * Returns: gateway IP in network byte order, or 0 on failure
 */
static uint32_t get_default_gateway(const char *iface) {
    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) {
        LOG_DEBUG("Cannot open /proc/net/route for gateway discovery");
        return 0;
    }

    char line[256];
    char route_iface[64];
    uint32_t dest, gateway;

    /* Skip header line */
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%63s %x %x", route_iface, &dest, &gateway) == 3) {
            /* Default route has destination 0.0.0.0 */
            if (dest == 0 && (!iface || strcmp(route_iface, iface) == 0)) {
                fclose(fp);
                LOG_DEBUG("Found default gateway: %u.%u.%u.%u",
                         gateway & 0xFF, (gateway >> 8) & 0xFF,
                         (gateway >> 16) & 0xFF, (gateway >> 24) & 0xFF);
                return gateway;  /* Already in network byte order from /proc */
            }
        }
    }

    fclose(fp);
    return 0;
}

/**
 * Helper to convert in_addr to p-net's ip_addr format (a.b.c.d octets)
 */
static void in_addr_to_pnet_ip(struct in_addr addr, pnet_cfg_ip_addr_t *pnet_ip) {
    uint32_t ip = ntohl(addr.s_addr);
    pnet_ip->a = (ip >> 24) & 0xFF;
    pnet_ip->b = (ip >> 16) & 0xFF;
    pnet_ip->c = (ip >> 8) & 0xFF;
    pnet_ip->d = ip & 0xFF;
}

/**
 * Configure p-net IP settings from the network interface
 *
 * What: Reads IP address, netmask, and gateway from the interface
 * Why: p-net requires complete IP configuration for DCP responses
 *      Without this, pnet_init() may fail or DCP won't work
 *
 * Returns: true if configuration was successful, false otherwise
 */
static bool configure_pnet_ip(const char *iface, pnet_cfg_t *cfg) {
    struct ifaddrs *ifaddr, *ifa;
    bool found = false;

    if (getifaddrs(&ifaddr) != 0) {
        LOG_ERROR("getifaddrs() failed: %s", strerror(errno));
        snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                 "Cannot enumerate network interfaces: %s", strerror(errno));
        return false;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (strcmp(ifa->ifa_name, iface) != 0) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        struct sockaddr_in *mask = (struct sockaddr_in *)ifa->ifa_netmask;

        /* Set IP address using p-net's octet format */
        in_addr_to_pnet_ip(addr->sin_addr, &cfg->if_cfg.ip_cfg.ip_addr);
        LOG_INFO("PROFINET IP address: %s", inet_ntoa(addr->sin_addr));

        /* Set netmask */
        if (mask) {
            in_addr_to_pnet_ip(mask->sin_addr, &cfg->if_cfg.ip_cfg.ip_mask);
            LOG_INFO("PROFINET netmask: %s", inet_ntoa(mask->sin_addr));
        }

        /* Get gateway from routing table */
        uint32_t gw = get_default_gateway(iface);
        if (gw != 0) {
            struct in_addr gw_addr;
            gw_addr.s_addr = gw;
            in_addr_to_pnet_ip(gw_addr, &cfg->if_cfg.ip_cfg.ip_gateway);
            LOG_INFO("PROFINET gateway: %s", inet_ntoa(gw_addr));
        } else {
            /* No default gateway - use zeros (common for direct-connect networks) */
            memset(&cfg->if_cfg.ip_cfg.ip_gateway, 0, sizeof(cfg->if_cfg.ip_cfg.ip_gateway));
            LOG_DEBUG("No default gateway found for %s", iface);
        }

        found = true;
        break;
    }

    freeifaddrs(ifaddr);

    if (!found) {
        LOG_ERROR("Interface '%s' has no IPv4 address configured", iface);
        snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                 "Interface '%s' has no IPv4 address. Check 'ip addr show %s'",
                 iface, iface);
        return false;
    }

    return true;
}
#endif

result_t profinet_manager_start(const char *interface) {
    if (!g_pn.initialized) return RESULT_NOT_INITIALIZED;
    if (g_pn.running) return RESULT_OK;

    g_pn_init_attempted = true;

#ifdef HAVE_PNET
    // Set network interface (use static buffer since if_cfg expects const char *)
    bool use_configured = false;
    if (interface && interface[0] != '\0') {
        // Check if configured interface exists before using it
        char sysfs_path[128];
        snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/net/%s", interface);
        if (access(sysfs_path, F_OK) == 0) {
            strncpy(g_netif_name, interface, sizeof(g_netif_name) - 1);
            g_netif_name[sizeof(g_netif_name) - 1] = '\0';
            LOG_INFO("PROFINET using configured interface: %s", g_netif_name);
            use_configured = true;
        } else {
            LOG_WARNING("Configured interface '%s' does not exist, trying auto-detection", interface);
        }
    }

    if (!use_configured) {
        // Auto-detect network interface
        if (hw_detect_network_interface(g_netif_name, sizeof(g_netif_name))) {
            LOG_INFO("PROFINET auto-detected interface: %s", g_netif_name);
        } else {
            // No interface found - fail with clear error
            snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                     "No network interface found. Set [network] interface in config file.");
            LOG_ERROR("Could not auto-detect network interface and none configured. "
                      "Set [network] interface in /etc/water-treat/water-treat.conf");
            return RESULT_ERROR;
        }
    }
    g_pn.pnet_cfg.if_cfg.main_netif_name = g_netif_name;

    // Enable promiscuous mode for PROFINET raw Ethernet frames
    if (!enable_promiscuous_mode(g_netif_name)) {
        LOG_WARNING("Could not enable promiscuous mode - PROFINET may not work correctly");
        // Continue anyway - p-net might still work if already in promisc mode
    }

    // Configure physical port (single port device - same as main interface)
    g_pn.pnet_cfg.if_cfg.physical_ports[0].netif_name = g_netif_name;
    g_pn.pnet_cfg.if_cfg.physical_ports[0].default_mau_type = 0x0010; // 100Mbit copper full-duplex

    // Configure IP settings from interface
    if (!configure_pnet_ip(g_netif_name, &g_pn.pnet_cfg)) {
        LOG_ERROR("Failed to configure IP settings for PROFINET");
        return RESULT_ERROR;
    }

    // =========================================================================
    // PRE-VALIDATION: Check all conditions BEFORE calling pnet_init()
    // This avoids guessing why pnet_init() failed - we know the exact cause.
    // =========================================================================

    // 1. Verify interface exists
    char sysfs_path[128];
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/net/%s", g_netif_name);
    if (access(sysfs_path, F_OK) != 0) {
        snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                 "Interface '%s' does not exist", g_netif_name);
        LOG_ERROR("PROFINET: Interface '%s' not found in /sys/class/net/", g_netif_name);
        LOG_ERROR("Available interfaces: run 'ls /sys/class/net/'");
        return RESULT_ERROR;
    }

    // 2. Verify interface is UP
    char operstate_path[160];
    snprintf(operstate_path, sizeof(operstate_path), "/sys/class/net/%s/operstate", g_netif_name);
    FILE *opf = fopen(operstate_path, "r");
    if (opf) {
        char state[32] = {0};
        if (fgets(state, sizeof(state), opf)) {
            state[strcspn(state, "\n")] = '\0';
            if (strcmp(state, "up") != 0 && strcmp(state, "unknown") != 0) {
                LOG_WARNING("PROFINET: Interface '%s' operstate is '%s' (expected 'up')",
                            g_netif_name, state);
            }
        }
        fclose(opf);
    }

    // 3. Verify interface has valid MAC address
    char mac_path[160];
    snprintf(mac_path, sizeof(mac_path), "/sys/class/net/%s/address", g_netif_name);
    FILE *macf = fopen(mac_path, "r");
    if (macf) {
        char mac[32] = {0};
        if (fgets(mac, sizeof(mac), macf)) {
            mac[strcspn(mac, "\n")] = '\0';
            if (strcmp(mac, "00:00:00:00:00:00") == 0) {
                snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                         "Interface '%s' has null MAC address", g_netif_name);
                LOG_ERROR("PROFINET: Interface '%s' has all-zero MAC address", g_netif_name);
                fclose(macf);
                return RESULT_ERROR;
            }
            LOG_DEBUG("PROFINET: Interface '%s' MAC: %s", g_netif_name, mac);
        }
        fclose(macf);
    }

    // 4. Verify raw socket can be created (tests permissions)
    int test_sock = socket(AF_PACKET, SOCK_RAW, htons(0x8892));
    if (test_sock < 0) {
        snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                 "Cannot create raw socket: %s (need root or CAP_NET_RAW)", strerror(errno));
        LOG_ERROR("PROFINET: Cannot create raw socket: %s", strerror(errno));
        LOG_ERROR("Fix: run as root OR: sudo setcap cap_net_raw+ep ./water-treat");
        return RESULT_ERROR;
    }
    close(test_sock);
    LOG_DEBUG("PROFINET: Raw socket test passed");

    // 5. Verify p-net config fields are set
    if (g_pn.pnet_cfg.tick_us == 0) {
        snprintf(g_pn_init_error, sizeof(g_pn_init_error), "pnet_cfg.tick_us is 0 (internal error)");
        LOG_ERROR("PROFINET: tick_us not configured (internal error)");
        return RESULT_ERROR;
    }
    if (g_pn.pnet_cfg.num_physical_ports == 0) {
        snprintf(g_pn_init_error, sizeof(g_pn_init_error), "pnet_cfg.num_physical_ports is 0 (internal error)");
        LOG_ERROR("PROFINET: num_physical_ports not configured (internal error)");
        return RESULT_ERROR;
    }
    if (!g_pn.pnet_cfg.if_cfg.physical_ports[0].netif_name ||
        g_pn.pnet_cfg.if_cfg.physical_ports[0].netif_name[0] == '\0') {
        snprintf(g_pn_init_error, sizeof(g_pn_init_error), "physical_ports[0].netif_name not set (internal error)");
        LOG_ERROR("PROFINET: physical_ports[0].netif_name not configured (internal error)");
        return RESULT_ERROR;
    }

    // 6. Ensure p-net NV storage directory exists
    // This prevents p-net from falling back to defaults (e.g., "rt-labs-dev")
    if (g_pn.pnet_cfg.file_directory[0] != '\0') {
        struct stat st = {0};
        if (stat(g_pn.pnet_cfg.file_directory, &st) == -1) {
            // Directory doesn't exist - create it with parent directories
            // Use mkdir -p equivalent: create each path component
            char path_copy[256];
            strncpy(path_copy, g_pn.pnet_cfg.file_directory, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';

            for (char *p = path_copy + 1; *p; p++) {
                if (*p == '/') {
                    *p = '\0';
                    if (mkdir(path_copy, 0755) == -1 && errno != EEXIST) {
                        LOG_WARNING("Could not create directory '%s': %s", path_copy, strerror(errno));
                    }
                    *p = '/';
                }
            }
            // Create final directory
            if (mkdir(g_pn.pnet_cfg.file_directory, 0755) == -1 && errno != EEXIST) {
                LOG_WARNING("Could not create p-net data directory '%s': %s",
                            g_pn.pnet_cfg.file_directory, strerror(errno));
                LOG_WARNING("p-net NV storage may not persist across reboots");
            } else {
                LOG_INFO("Created p-net NV storage directory: %s", g_pn.pnet_cfg.file_directory);
            }
        } else {
            LOG_DEBUG("p-net NV storage directory exists: %s", g_pn.pnet_cfg.file_directory);
        }

        // 7. CRITICAL: Clear p-net NV files to enforce our configured station_name
        // p-net persists station name to disk via DCP Set-Name from controller.
        // If controller has wrong name (e.g., "rt-labs-dev"), it contaminates RTU.
        // Delete NV files BEFORE pnet_init() so our config is authoritative.
        clear_pnet_nv_station(g_pn.pnet_cfg.file_directory, g_pn.pnet_cfg.station_name);
    }

    // Log validated configuration
    LOG_DEBUG("pnet_init() config: interface=%s, station=%s, vendor=0x%02X%02X, device=0x%02X%02X",
              g_pn.pnet_cfg.if_cfg.main_netif_name,
              g_pn.pnet_cfg.station_name,
              g_pn.pnet_cfg.device_id.vendor_id_hi, g_pn.pnet_cfg.device_id.vendor_id_lo,
              g_pn.pnet_cfg.device_id.device_id_hi, g_pn.pnet_cfg.device_id.device_id_lo);
    LOG_DEBUG("pnet_init() IP config: %d.%d.%d.%d / %d.%d.%d.%d gw %d.%d.%d.%d",
              g_pn.pnet_cfg.if_cfg.ip_cfg.ip_addr.a, g_pn.pnet_cfg.if_cfg.ip_cfg.ip_addr.b,
              g_pn.pnet_cfg.if_cfg.ip_cfg.ip_addr.c, g_pn.pnet_cfg.if_cfg.ip_cfg.ip_addr.d,
              g_pn.pnet_cfg.if_cfg.ip_cfg.ip_mask.a, g_pn.pnet_cfg.if_cfg.ip_cfg.ip_mask.b,
              g_pn.pnet_cfg.if_cfg.ip_cfg.ip_mask.c, g_pn.pnet_cfg.if_cfg.ip_cfg.ip_mask.d,
              g_pn.pnet_cfg.if_cfg.ip_cfg.ip_gateway.a, g_pn.pnet_cfg.if_cfg.ip_cfg.ip_gateway.b,
              g_pn.pnet_cfg.if_cfg.ip_cfg.ip_gateway.c, g_pn.pnet_cfg.if_cfg.ip_cfg.ip_gateway.d);
    LOG_DEBUG("pnet_init() timing: tick_us=%u, num_ports=%u, port[0]=%s",
              g_pn.pnet_cfg.tick_us, g_pn.pnet_cfg.num_physical_ports,
              g_pn.pnet_cfg.if_cfg.physical_ports[0].netif_name);
    LOG_DEBUG("pnet_init() NV storage: %s",
              g_pn.pnet_cfg.file_directory[0] ? g_pn.pnet_cfg.file_directory : "(current directory)");

    // Initialize p-net (all pre-conditions verified)
    g_pn.pnet = pnet_init(&g_pn.pnet_cfg);
    if (!g_pn.pnet) {
        // If we get here, all our pre-checks passed but p-net still failed.
        // This indicates a p-net internal issue we couldn't pre-detect.
        snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                 "pnet_init() failed after all pre-checks passed - check p-net library version/config");
        LOG_ERROR("pnet_init() failed on interface '%s'", g_netif_name);
        LOG_ERROR("All pre-validation checks passed. Possible causes:");
        LOG_ERROR("  1. p-net library compiled with incompatible options");
        LOG_ERROR("  2. Missing p-net configuration field not checked above");
        LOG_ERROR("  3. Run 'strace water-treat' to see syscall failures");
        return RESULT_ERROR;
    }

    // =========================================================================
    // Plug DAP (Device Access Point) at slot 0 - REQUIRED by PROFINET spec
    // The DAP must be plugged before any application modules.
    // =========================================================================
    int dap_ret;

    // Plug DAP module at slot 0
    // Uses GSDML-defined idents from gsdml_modules.h
    dap_ret = pnet_plug_module(g_pn.pnet, 0, 0, GSDML_MOD_DAP);
    if (dap_ret != 0) {
        LOG_ERROR("Failed to plug DAP module at slot 0 (required by PROFINET)");
        g_pn.pnet = NULL;
        return RESULT_ERROR;
    }
    LOG_DEBUG("Plugged DAP module at slot 0 (ident=0x%08X)", GSDML_MOD_DAP);

    // Plug DAP submodule at slot 0, subslot 1
    dap_ret = pnet_plug_submodule(g_pn.pnet, 0, 0, 1,
                                   GSDML_MOD_DAP, GSDML_SUBMOD_DAP,
                                   PNET_DIR_NO_IO, 0, 0);
    if (dap_ret != 0) {
        LOG_ERROR("Failed to plug DAP submodule at slot 0.1");
        g_pn.pnet = NULL;
        return RESULT_ERROR;
    }
    LOG_DEBUG("Plugged DAP submodule at slot 0.1 (ident=0x%08X)", GSDML_SUBMOD_DAP);

    // Plug DAP interface submodule at slot 0, subslot 0x8000
    // Note: subslot 0x8000 is standard, but submodule ident is per GSDML
    dap_ret = pnet_plug_submodule(g_pn.pnet, 0, 0, 0x8000,
                                   GSDML_MOD_DAP, GSDML_SUBMOD_DAP_INTERFACE,
                                   PNET_DIR_NO_IO, 0, 0);
    if (dap_ret != 0) {
        LOG_WARNING("Failed to plug DAP interface submodule at slot 0.0x8000");
        // Non-fatal - some controllers don't require this
    } else {
        LOG_DEBUG("Plugged DAP interface submodule at slot 0.0x8000 (ident=0x%08X)", GSDML_SUBMOD_DAP_INTERFACE);
    }

    // Plug DAP port submodule at slot 0, subslot 0x8001
    dap_ret = pnet_plug_submodule(g_pn.pnet, 0, 0, 0x8001,
                                   GSDML_MOD_DAP, GSDML_SUBMOD_DAP_PORT,
                                   PNET_DIR_NO_IO, 0, 0);
    if (dap_ret != 0) {
        LOG_WARNING("Failed to plug DAP port submodule at slot 0.0x8001");
        // Non-fatal - some controllers don't require this
    } else {
        LOG_DEBUG("Plugged DAP port submodule at slot 0.0x8001 (ident=0x%08X)", GSDML_SUBMOD_DAP_PORT);
    }

    // =========================================================================
    // Plug application modules from database
    // =========================================================================
    for (int i = 0; i < g_pn.slot_count; i++) {
        profinet_slot_t *slot = &g_pn.slots[i];
        
        int ret = pnet_plug_module(g_pn.pnet, 0, slot->slot, slot->module_ident);
        if (ret != 0) {
            LOG_WARNING("Failed to plug module at slot %d", slot->slot);
            continue;
        }
        
        /*
         * Set PROFINET data direction based on module type:
         * - Sensors (input modules):   PNET_DIR_INPUT  (device → controller)
         * - Actuators (output modules): PNET_DIR_OUTPUT (controller → device)
         *
         * This MUST match the GSDML or the controller will reject the connection.
         */
        pnet_submodule_dir_t direction = is_actuator_module(slot->module_ident)
                                             ? PNET_DIR_OUTPUT
                                             : PNET_DIR_INPUT;

        ret = pnet_plug_submodule(g_pn.pnet, 0, slot->slot, slot->subslot,
                                  slot->module_ident, slot->submodule_ident,
                                  direction,
                                  slot->input_size, slot->output_size);
        if (ret != 0) {
            LOG_WARNING("Failed to plug submodule at slot %d.%d", slot->slot, slot->subslot);
            continue;
        }
        
        slot->plugged = true;
        LOG_DEBUG("Plugged slot %d.%d", slot->slot, slot->subslot);
    }

    // p-net v0.2.0: Device state machine is handled internally by the stack
    // The device becomes ready for connections after modules are plugged

    // Start tick thread
    g_pn.running = true;
    if (pthread_create(&g_pn.tick_thread, NULL, profinet_tick_thread, NULL) != 0) {
        LOG_ERROR("Failed to create PROFINET tick thread");
        // p-net v0.2.0: No explicit close function, just clean up handle
        g_pn.pnet = NULL;
        g_pn.running = false;
        return RESULT_ERROR;
    }
    
    set_state(PROFINET_STATE_READY);
    LOG_INFO("PROFINET stack started on interface %s", g_netif_name);
#else
    UNUSED(interface);
    LOG_WARNING("PROFINET support not compiled in (HAVE_PNET not defined)");
    g_pn.running = true;
    set_state(PROFINET_STATE_READY);
#endif

    return RESULT_OK;
}

result_t profinet_manager_stop(void) {
    if (!g_pn.running) return RESULT_OK;

    LOG_INFO("Stopping PROFINET stack...");
    g_pn.running = false;

#ifdef HAVE_PNET
    /* Wait for tick thread to exit cleanly */
    LOG_DEBUG("Waiting for PROFINET tick thread to terminate...");
    pthread_join(g_pn.tick_thread, NULL);
    LOG_DEBUG("PROFINET tick thread terminated");

    /*
     * p-net cleanup: The p-net library (v0.2.0+) may not have explicit
     * pnet_close() depending on version. We attempt to call it if available,
     * otherwise log that resources will be freed on process exit.
     *
     * NOTE: If you upgrade p-net and pnet_close() becomes available,
     * enable this block to ensure clean shutdown without requiring reboot.
     */
    if (g_pn.pnet) {
#ifdef pnet_close
        /* p-net newer versions may have explicit close */
        LOG_DEBUG("Calling pnet_close() for clean resource release");
        pnet_close(g_pn.pnet);
#else
        /* Older p-net: Resources freed on process exit.
         * This is acceptable for service restart but not ideal.
         * If you experience issues after service restart (duplicate
         * registrations, port binding failures), a full reboot
         * may be required. */
        LOG_DEBUG("pnet_close() not available - resources freed on process exit");
#endif
        g_pn.pnet = NULL;
    }
#endif

    set_state(PROFINET_STATE_IDLE);
    g_pn.connected = false;

    LOG_INFO("PROFINET stack stopped cleanly");
    return RESULT_OK;
}

void profinet_manager_shutdown(void) {
    profinet_manager_stop();
    controller_discovery_shutdown();
    pthread_mutex_destroy(&g_pn.mutex);
    g_pn.initialized = false;
    LOG_INFO("PROFINET manager shutdown");
}

result_t profinet_manager_update_input(int slot, int subslot, const void *data, size_t size) {
    if (!g_pn.initialized) return RESULT_NOT_INITIALIZED;
    
    pthread_mutex_lock(&g_pn.mutex);
    
    profinet_slot_t *s = find_slot(slot, subslot);
    if (!s || !s->plugged) {
        pthread_mutex_unlock(&g_pn.mutex);
        return RESULT_NOT_FOUND;
    }
    
    if (size > sizeof(s->input_data)) size = sizeof(s->input_data);
    
    memcpy(s->input_data, data, size);
    s->input_size = size;
    s->input_valid = true;
    
#ifdef HAVE_PNET
    if (g_pn.pnet && g_pn.connected) {
        uint8_t iops = PNET_IOXS_GOOD;
        pnet_input_set_data_and_iops(g_pn.pnet, 0, slot, subslot, 
                                      s->input_data, s->input_size, iops);
    }
#endif
    
    pthread_mutex_unlock(&g_pn.mutex);
    return RESULT_OK;
}

result_t profinet_manager_update_input_float(int slot, int subslot, float value) {
    uint8_t data[4];
    /*
     * PROFINET uses big-endian (network byte order) per DEVELOPMENT_GUIDELINES.md
     * Use htonl() for proper byte order conversion
     */
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));
    uint32_t be = htonl(raw);
    memcpy(data, &be, sizeof(be));
    return profinet_manager_update_input(slot, subslot, data, 4);
}

/**
 * @brief Update PROFINET input with value and quality (5-byte format)
 *
 * Per DEVELOPMENT_GUIDELINES.md Part 1.2:
 *   RTU -> CONTROLLER (Input Data):
 *   Bytes 0-3: Sensor Value (Float32, BE)
 *   Byte 4:    Sensor Quality (OPC UA compatible)
 *
 * This is the preferred function for sending sensor data as it includes
 * the quality indicator required by the Water-Controller.
 */
result_t profinet_manager_update_input_with_quality(int slot, int subslot,
                                                     float value, data_quality_t quality) {
    uint8_t data[5];

    /* Convert float to big-endian using htonl per guidelines */
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));
    uint32_t be = htonl(raw);
    memcpy(data, &be, sizeof(be));

    /* Quality byte (OPC UA compatible values) */
    data[4] = (uint8_t)quality;

    return profinet_manager_update_input(slot, subslot, data, 5);
}

result_t profinet_manager_get_output(int slot, int subslot, void *data, size_t *size) {
    if (!g_pn.initialized) return RESULT_NOT_INITIALIZED;
    CHECK_NULL(data); CHECK_NULL(size);
    
    pthread_mutex_lock(&g_pn.mutex);
    
    profinet_slot_t *s = find_slot(slot, subslot);
    if (!s || !s->plugged) {
        pthread_mutex_unlock(&g_pn.mutex);
        return RESULT_NOT_FOUND;
    }
    
#ifdef HAVE_PNET
    if (g_pn.pnet && g_pn.connected) {
        uint8_t iops;
        bool new_data;
        uint16_t len = *size;
        
        int ret = pnet_output_get_data_and_iops(g_pn.pnet, 0, slot, subslot,
                                                 &new_data, data, &len, &iops);
        if (ret == 0) {
            *size = len;
            s->output_valid = (iops == PNET_IOXS_GOOD);
            pthread_mutex_unlock(&g_pn.mutex);
            return RESULT_OK;
        }
    }
#endif
    
    // Return cached data
    size_t copy_size = MIN(*size, s->output_size);
    memcpy(data, s->output_data, copy_size);
    *size = copy_size;
    
    pthread_mutex_unlock(&g_pn.mutex);
    return s->output_valid ? RESULT_OK : RESULT_ERROR;
}

result_t profinet_manager_set_callbacks(profinet_connect_cb_t on_connect,
                                        profinet_disconnect_cb_t on_disconnect,
                                        profinet_data_cb_t on_data,
                                        void *ctx) {
    pthread_mutex_lock(&g_pn.mutex);
    g_pn.on_connect = on_connect;
    g_pn.on_disconnect = on_disconnect;
    g_pn.on_data_received = on_data;
    g_pn.callback_ctx = ctx;
    pthread_mutex_unlock(&g_pn.mutex);
    return RESULT_OK;
}

profinet_state_t profinet_manager_get_state(void) {
    pthread_mutex_lock(&g_pn.mutex);
    profinet_state_t state = g_pn.state;
    pthread_mutex_unlock(&g_pn.mutex);
    return state;
}

bool profinet_manager_is_connected(void) {
    pthread_mutex_lock(&g_pn.mutex);
    bool connected = g_pn.connected;
    pthread_mutex_unlock(&g_pn.mutex);
    return connected;
}

bool profinet_manager_is_running(void) {
    pthread_mutex_lock(&g_pn.mutex);
    bool running = g_pn.running;
    pthread_mutex_unlock(&g_pn.mutex);
    return running;
}

result_t profinet_manager_get_stats(profinet_stats_t *stats) {
    CHECK_NULL(stats);

    pthread_mutex_lock(&g_pn.mutex);

    stats->state = g_pn.state;
    stats->connected = g_pn.connected;
    stats->cycle_count = g_pn.cycle_count;
    stats->slot_count = g_pn.slot_count;

    int plugged = 0;
    for (int i = 0; i < g_pn.slot_count; i++) {
        if (g_pn.slots[i].plugged) plugged++;
    }
    stats->plugged_modules = plugged;

    /* Connection resilience stats */
    stats->connection_count = g_pn.connection_count;
    stats->disconnect_count = g_pn.disconnect_count;
    stats->error_count = g_pn.error_count;
    stats->stuck_state_recoveries = g_pn.stuck_state_recoveries;
    stats->state_duration_ms = get_time_ms() - g_pn.state_entry_time_ms;

    pthread_mutex_unlock(&g_pn.mutex);
    return RESULT_OK;
}

result_t profinet_manager_send_alarm(int slot, int subslot, uint16_t alarm_type,
                                     const uint8_t *data, size_t data_len) {
#ifdef HAVE_PNET
    if (!g_pn.pnet || !g_pn.connected) return RESULT_NOT_INITIALIZED;

    // p-net v0.2.0 API: positional arguments instead of struct
    // pnet_alarm_send_process_alarm(pnet, arep, api, slot, subslot, usi, len, data)
    int ret = pnet_alarm_send_process_alarm(
        g_pn.pnet,
        g_pn.arep,           // arep from connection
        0,                   // api (always 0 for standard PROFINET)
        (uint16_t)slot,
        (uint16_t)subslot,
        alarm_type,          // User Structure Identifier (USI)
        (uint16_t)data_len,
        data
    );
    if (ret != 0) {
        LOG_ERROR("Failed to send PROFINET alarm");
        return RESULT_ERROR;
    }

    LOG_INFO("Sent PROFINET alarm: slot=%d, type=0x%04X", slot, alarm_type);
    return RESULT_OK;
#else
    UNUSED(slot); UNUSED(subslot); UNUSED(alarm_type); UNUSED(data); UNUSED(data_len);
    return RESULT_NOT_SUPPORTED;
#endif
}

/**
 * @brief Initialize all input subslots with default data and GOOD IOPS
 *
 * This function is CRITICAL for successful connection establishment.
 * The p-net library requires all plugged input subslots to have:
 *   1. Valid data set via pnet_input_set_data_and_iops()
 *   2. IOPS set to GOOD (or BAD if sensor not ready)
 *
 * Without this, pnet_application_ready() will return -1 and the
 * controller connection will timeout waiting for CControl response.
 *
 * Called from profinet_state_callback() when PNET_EVENT_PRMEND is received,
 * BEFORE calling pnet_application_ready().
 */
int profinet_manager_init_all_inputs(void) {
    int initialized = 0;

#ifdef HAVE_PNET
    if (!g_pn.pnet) {
        LOG_ERROR("Cannot init inputs: pnet handle is NULL");
        return 0;
    }

    pthread_mutex_lock(&g_pn.mutex);

    for (int i = 0; i < g_pn.slot_count; i++) {
        profinet_slot_t *slot = &g_pn.slots[i];

        /* Only initialize plugged INPUT subslots (sensors) */
        if (!slot->plugged || slot->input_size == 0) {
            continue;
        }

        /*
         * Initialize with default data:
         * - Float32 value: 0.0 (4 bytes, big-endian)
         * - Quality: DATA_QUALITY_BAD (0x00) - will update when real data arrives
         *
         * Using BAD quality initially is correct - we haven't read the sensor yet.
         * The sensor manager will update with GOOD quality once real data is available.
         */
        uint8_t init_data[PROFINET_DATA_SIZE] = {0};

        /* Set IOPS to GOOD so p-net accepts our data
         * Note: IOPS indicates the provider status (are we providing valid structure),
         * not the data quality (which is in byte 4 of the payload) */
        uint8_t iops = PNET_IOXS_GOOD;

        int ret = pnet_input_set_data_and_iops(g_pn.pnet, 0,
                                                slot->slot, slot->subslot,
                                                init_data, slot->input_size, iops);
        if (ret == 0) {
            slot->input_iops = iops;
            initialized++;
            LOG_DEBUG("Initialized input slot %d.%d with %zu bytes, IOPS=GOOD",
                      slot->slot, slot->subslot, slot->input_size);
        } else {
            LOG_WARNING("Failed to initialize input slot %d.%d: ret=%d",
                        slot->slot, slot->subslot, ret);
        }
    }

    pthread_mutex_unlock(&g_pn.mutex);

    LOG_INFO("Initialized %d input subslots for PROFINET connection", initialized);
#endif

    return initialized;
}

/* Called from connect callback when controller initiates connection */
void profinet_manager_set_connecting(void) {
    if (g_pn.state != PROFINET_STATE_CONNECTING) {
        set_state(PROFINET_STATE_CONNECTING);
    }
}

/* Called from callbacks */
void profinet_manager_set_connected(bool connected, uint32_t arep) {
    bool was_connected = g_pn.connected;
    g_pn.connected = connected;

    /* Use state machine helper for proper tracking */
    if (connected) {
        set_state(PROFINET_STATE_CONNECTED);
    } else {
        if (was_connected) {
            g_pn.disconnect_count++;
        }
        set_state(PROFINET_STATE_READY);
    }

#ifdef HAVE_PNET
    if (connected) {
        g_pn.arep = arep;
    } else {
        g_pn.arep = 0;
    }
#endif

    /* Notify controller discovery module */
    if (connected) {
        controller_discovery_on_connect(arep);
    } else {
        controller_discovery_on_disconnect(arep);
    }

    if (connected && g_pn.on_connect) {
        g_pn.on_connect(g_pn.callback_ctx);
    } else if (!connected && g_pn.on_disconnect) {
        g_pn.on_disconnect(g_pn.callback_ctx);
    }
}

/**
 * @brief Clear AR state to recover from stale connection errors
 *
 * Called when PNIO errors indicate stale AR state:
 *   - 0x03: AR already exists
 *   - 0x04: Session key mismatch
 *
 * Clears p-net NV files that store AR/session state so next
 * connection attempt starts fresh. Controller will automatically
 * retry after a brief timeout.
 */
void profinet_manager_clear_ar_state(void) {
#ifdef HAVE_PNET
    g_pn.arep = 0;

    /* Clear p-net AR state files if data directory is configured */
    if (g_pn.pnet_cfg.file_directory[0] != '\0') {
        DIR *dir = opendir(g_pn.pnet_cfg.file_directory);
        if (dir) {
            struct dirent *entry;
            char filepath[512];

            while ((entry = readdir(dir)) != NULL) {
                /* Clear AR and session-related NV files */
                if (strncmp(entry->d_name, "pf_", 3) == 0) {
                    snprintf(filepath, sizeof(filepath), "%s/%s",
                             g_pn.pnet_cfg.file_directory, entry->d_name);
                    unlink(filepath);
                }
            }
            closedir(dir);
            LOG_DEBUG("AR state cleared, ready for reconnect");
        }
    }
#endif
    /* Note: Caller is responsible for setting appropriate state after clearing */
}

void profinet_manager_handle_output_data(int slot, int subslot, const uint8_t *data, size_t len) {
    profinet_slot_t *s = find_slot(slot, subslot);
    if (s && len <= sizeof(s->output_data)) {
        memcpy(s->output_data, data, len);
        s->output_size = len;
        s->output_valid = true;
        
        if (g_pn.on_data_received) {
            g_pn.on_data_received(slot, subslot, data, len, g_pn.callback_ctx);
        }
    }
}

const char* profinet_state_to_string(profinet_state_t state) {
    switch (state) {
        case PROFINET_STATE_IDLE: return "Idle";
        case PROFINET_STATE_READY: return "Ready";
        case PROFINET_STATE_CONNECTING: return "Connecting";
        case PROFINET_STATE_CONNECTED: return "Connected";
        case PROFINET_STATE_ERROR: return "Error";
        default: return "Unknown";
    }
}

const char* profinet_manager_get_init_error(void) {
    return g_pn_init_error[0] ? g_pn_init_error : NULL;
}

/**
 * @brief Dump all plugged slots for debugging
 *
 * Logs all slots registered with p-net, including:
 * - Slot/subslot numbers
 * - Module/submodule identifiers
 * - I/O direction and sizes
 * - Current IOPS status
 *
 * Call this after profinet_manager_start() to verify configuration.
 */
void profinet_manager_dump_slots(void) {
    LOG_INFO("=== PROFINET Slot Configuration ===");
    LOG_INFO("Total application slots: %d", g_pn.slot_count);

    /* DAP (always slot 0) */
    LOG_INFO("Slot 0 (DAP):");
    LOG_INFO("  0.1     : module=0x%08X submod=0x%08X (DAP)", GSDML_MOD_DAP, GSDML_SUBMOD_DAP);
    LOG_INFO("  0.0x8000: module=0x%08X submod=0x%08X (Interface)", GSDML_MOD_DAP, GSDML_SUBMOD_DAP_INTERFACE);
    LOG_INFO("  0.0x8001: module=0x%08X submod=0x%08X (Port)", GSDML_MOD_DAP, GSDML_SUBMOD_DAP_PORT);

    /* Application modules */
    for (int i = 0; i < g_pn.slot_count; i++) {
        profinet_slot_t *slot = &g_pn.slots[i];
        const char *dir = is_actuator_module(slot->module_ident) ? "OUTPUT" : "INPUT";
        const char *plugged = slot->plugged ? "PLUGGED" : "NOT_PLUGGED";

        LOG_INFO("Slot %d.%d: module=0x%08X submod=0x%08X dir=%s in=%zu out=%zu iops=0x%02X [%s]",
                 slot->slot, slot->subslot,
                 slot->module_ident, slot->submodule_ident,
                 dir, slot->input_size, slot->output_size,
                 slot->input_iops, plugged);
    }

    LOG_INFO("=== End Slot Configuration ===");
}

bool profinet_manager_is_disabled_by_config(void) {
    return g_pn_disabled_by_config;
}

bool profinet_manager_init_attempted(void) {
    return g_pn_init_attempted;
}

void profinet_manager_mark_disabled(void) {
    g_pn_disabled_by_config = true;
}

/* Wrapper functions for sensor_manager integration */
result_t profinet_manager_write_input_data(void *mgr, int slot, int subslot,
                                           const uint8_t *data, size_t len) {
    UNUSED(mgr);
    return profinet_manager_update_input(slot, subslot, data, len);
}

result_t profinet_manager_set_input_iops(void *mgr, int slot, int subslot, uint8_t iops) {
    UNUSED(mgr);
    profinet_slot_t *s = find_slot(slot, subslot);
    if (!s) return RESULT_NOT_FOUND;

    s->input_iops = iops;
    return RESULT_OK;
}

result_t profinet_manager_add_module(void *mgr, int slot, uint32_t module_ident,
                                     int subslot, uint32_t submodule_ident,
                                     size_t input_len, size_t output_len) {
    UNUSED(mgr);

    /* Check if slot already exists (may be loaded from database) */
    profinet_slot_t *s = find_slot(slot, subslot);
    if (s) {
        /* Update existing slot with correct GSDML identifiers and sizes */
        s->module_ident = module_ident;
        s->submodule_ident = submodule_ident;
        s->input_size = input_len;
        s->output_size = output_len;
        LOG_DEBUG("Updated PROFINET module: slot=%d, subslot=%d, ident=0x%08X",
                  slot, subslot, module_ident);
        return RESULT_OK;
    }

    /* Add new slot */
    if (g_pn.slot_count >= MAX_PROFINET_SLOTS) {
        LOG_ERROR("Maximum slots exceeded");
        return RESULT_ERROR;
    }

    s = &g_pn.slots[g_pn.slot_count++];
    memset(s, 0, sizeof(*s));

    s->slot = slot;
    s->subslot = subslot;
    s->module_ident = module_ident;
    s->submodule_ident = submodule_ident;
    s->input_size = input_len;
    s->output_size = output_len;
    s->input_iops = PNET_IOXS_BAD;

    LOG_INFO("Added PROFINET module: slot=%d, subslot=%d, ident=0x%08X",
             slot, subslot, module_ident);

    return RESULT_OK;
}
