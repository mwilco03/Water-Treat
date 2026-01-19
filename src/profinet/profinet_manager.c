/**
 * @file profinet_manager.c
 * @brief PROFINET I/O Device manager using p-net stack
 */

#include "profinet_manager.h"
#include "profinet_callbacks.h"
#include "controller_discovery.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "utils/logger.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>      /* errno for error reporting */
#include <dirent.h>     /* opendir, readdir for interface detection */
#include <arpa/inet.h>  /* htonl, ntohl for network byte order per DEVELOPMENT_GUIDELINES.md */
#include <ifaddrs.h>    /* getifaddrs for IP configuration discovery */
#include <net/if.h>     /* IFF_UP, IFF_RUNNING */
#include <sys/ioctl.h>  /* ioctl for gateway discovery */
#include <netinet/in.h> /* sockaddr_in */

#define PROFINET_TICK_INTERVAL_US   1000
#define MAX_PROFINET_SLOTS          64
#define PROFINET_DATA_SIZE          256

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
    uint64_t last_tick_time;
    uint32_t cycle_count;
    
    // Callbacks
    profinet_connect_cb_t on_connect;
    profinet_disconnect_cb_t on_disconnect;
    profinet_data_cb_t on_data_received;
    void *callback_ctx;
} profinet_manager_t;

static profinet_manager_t g_pn = {0};

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

#ifdef HAVE_PNET
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
            slot->input_size = 4;  // Default: 4 bytes (float)
            slot->output_size = 0;
            LOG_DEBUG("Loaded slot %d: module_id=%d, ident=0x%08X", 
                      slot->slot, slot->module_id, slot->module_ident);
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
    
    // Device identity
    g_pn.pnet_cfg.device_id.vendor_id_hi = (config->vendor_id >> 8) & 0xFF;
    g_pn.pnet_cfg.device_id.vendor_id_lo = config->vendor_id & 0xFF;
    g_pn.pnet_cfg.device_id.device_id_hi = (config->device_id >> 8) & 0xFF;
    g_pn.pnet_cfg.device_id.device_id_lo = config->device_id & 0xFF;
    
    // Product name
    strncpy(g_pn.pnet_cfg.product_name, config->product_name, sizeof(g_pn.pnet_cfg.product_name) - 1);
    
    // Timing
    g_pn.pnet_cfg.min_device_interval = config->min_device_interval;
    
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

    g_pn.state = PROFINET_STATE_IDLE;
    g_pn.initialized = true;

    LOG_INFO("PROFINET manager initialized: station=%s, vendor=0x%04X, device=0x%04X",
             config->station_name, config->vendor_id, config->device_id);

    return RESULT_OK;
}

#ifdef HAVE_PNET
// Static buffer for network interface name (p-net v0.2.0 uses const char *)
static char g_netif_name[64] = {0};

/**
 * Read a sysfs attribute for a network interface
 * Returns value or -1 on error
 */
static int read_sysfs_int(const char *iface, const char *attr) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", iface, attr);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int val = -1;
    if (fscanf(f, "%d", &val) != 1) val = -1;
    fclose(f);
    return val;
}

/**
 * Check if interface is virtual (bridge, veth, docker, etc.)
 */
static bool is_virtual_interface(const char *iface) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", iface);

    /* Real hardware interfaces have a 'device' symlink to PCI/USB/etc */
    /* Virtual interfaces (bridges, veth, docker) don't */
    return access(path, F_OK) != 0;
}

/**
 * Score an interface for suitability as PROFINET interface
 * Higher score = better candidate
 */
static int score_interface(const char *iface) {
    int score = 0;

    /* Must not be loopback */
    if (strcmp(iface, "lo") == 0) return -1;

    /* Skip obviously virtual interfaces by name */
    if (strncmp(iface, "docker", 6) == 0) return -1;
    if (strncmp(iface, "veth", 4) == 0) return -1;
    if (strncmp(iface, "br-", 3) == 0) return -1;
    if (strncmp(iface, "virbr", 5) == 0) return -1;

    /* Prefer physical over virtual */
    if (!is_virtual_interface(iface)) {
        score += 100;
    }

    /* Prefer interfaces that are UP */
    int flags = read_sysfs_int(iface, "flags");
    if (flags > 0 && (flags & 0x1)) {  /* IFF_UP */
        score += 50;
    }

    /* Prefer interfaces with carrier (cable connected) */
    int carrier = read_sysfs_int(iface, "carrier");
    if (carrier == 1) {
        score += 30;
    }

    /* Prefer ethernet over wireless */
    int type = read_sysfs_int(iface, "type");
    if (type == 1) {  /* ARPHRD_ETHER */
        score += 20;
    }

    /* Prefer lower interface index (usually primary) */
    int ifindex = read_sysfs_int(iface, "ifindex");
    if (ifindex > 0 && ifindex < 10) {
        score += (10 - ifindex);
    }

    return score;
}

/**
 * Discover and select best network interface for PROFINET
 * Scans all interfaces, scores them, returns highest scoring
 */
static bool detect_network_interface(char *buf, size_t buf_size) {
    DIR *dir = opendir("/sys/class/net");
    if (!dir) {
        LOG_ERROR("Cannot open /sys/class/net for interface discovery");
        return false;
    }

    char best_iface[64] = {0};
    int best_score = -1;
    int iface_count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        iface_count++;
        int score = score_interface(entry->d_name);

        LOG_DEBUG("Interface %s: score=%d", entry->d_name, score);

        if (score > best_score) {
            best_score = score;
            strncpy(best_iface, entry->d_name, sizeof(best_iface) - 1);
        }
    }
    closedir(dir);

    LOG_INFO("Discovered %d network interfaces, best candidate: %s (score=%d)",
             iface_count, best_iface[0] ? best_iface : "none", best_score);

    if (best_iface[0] != '\0' && best_score >= 0) {
        strncpy(buf, best_iface, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return true;
    }

    return false;
}
#endif

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

#ifdef HAVE_PNET
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
    if (interface && interface[0] != '\0') {
        strncpy(g_netif_name, interface, sizeof(g_netif_name) - 1);
        g_netif_name[sizeof(g_netif_name) - 1] = '\0';
        LOG_INFO("PROFINET using configured interface: %s", g_netif_name);
    } else {
        // Auto-detect network interface
        if (detect_network_interface(g_netif_name, sizeof(g_netif_name))) {
            LOG_INFO("PROFINET auto-detected interface: %s", g_netif_name);
        } else {
            // Last resort fallback
            strncpy(g_netif_name, "eth0", sizeof(g_netif_name) - 1);
            LOG_WARNING("Could not auto-detect network interface, falling back to 'eth0'. "
                        "Set [network] interface in config file if this is incorrect.");
        }
    }
    g_pn.pnet_cfg.if_cfg.main_netif_name = g_netif_name;

    /*
     * Configure IP settings from the interface
     * p-net requires: ip_addr, ip_mask, ip_gateway for DCP responses
     * Without these, DCP Identify won't return proper device information
     */
    if (!configure_pnet_ip(g_netif_name, &g_pn.pnet_cfg)) {
        LOG_ERROR("Failed to configure IP settings for PROFINET. %s", g_pn_init_error);
        return RESULT_ERROR;
    }

    // Initialize p-net
    g_pn.pnet = pnet_init(&g_pn.pnet_cfg);
    if (!g_pn.pnet) {
        snprintf(g_pn_init_error, sizeof(g_pn_init_error),
                 "pnet_init() failed on interface '%s'. "
                 "Verify: 1) Interface exists (ip link show), "
                 "2) IP is configured (ip addr show %s), "
                 "3) User has CAP_NET_RAW capability",
                 g_netif_name, g_netif_name);
        LOG_ERROR("%s", g_pn_init_error);
        return RESULT_ERROR;
    }
    
    // Plug modules
    for (int i = 0; i < g_pn.slot_count; i++) {
        profinet_slot_t *slot = &g_pn.slots[i];
        
        int ret = pnet_plug_module(g_pn.pnet, 0, slot->slot, slot->module_ident);
        if (ret != 0) {
            LOG_WARNING("Failed to plug module at slot %d", slot->slot);
            continue;
        }
        
        ret = pnet_plug_submodule(g_pn.pnet, 0, slot->slot, slot->subslot,
                                  slot->module_ident, slot->submodule_ident,
                                  PNET_DIR_INPUT,
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
    
    g_pn.state = PROFINET_STATE_READY;
    LOG_INFO("PROFINET stack started on interface %s", g_netif_name);
#else
    UNUSED(interface);
    LOG_WARNING("PROFINET support not compiled in (HAVE_PNET not defined)");
    g_pn.running = true;
    g_pn.state = PROFINET_STATE_READY;
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

    g_pn.state = PROFINET_STATE_IDLE;
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
    return g_pn.state;
}

bool profinet_manager_is_connected(void) {
    return g_pn.connected;
}

bool profinet_manager_is_running(void) {
    return g_pn.running;
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

/* Called from callbacks */
void profinet_manager_set_connected(bool connected, uint32_t arep) {
    g_pn.connected = connected;
    g_pn.state = connected ? PROFINET_STATE_CONNECTED : PROFINET_STATE_READY;

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

    if (g_pn.slot_count >= MAX_PROFINET_SLOTS) {
        LOG_ERROR("Maximum slots exceeded");
        return RESULT_ERROR;
    }

    profinet_slot_t *s = &g_pn.slots[g_pn.slot_count++];
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
