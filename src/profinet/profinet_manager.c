/**
 * @file profinet_manager.c
 * @brief PROFINET I/O Device manager using p-net stack
 */

#include "profinet_manager.h"
#include "profinet_callbacks.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "utils/logger.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_PNET
#include <pnet_api.h>
#endif

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
    
    g_pn.state = PROFINET_STATE_IDLE;
    g_pn.initialized = true;
    
    LOG_INFO("PROFINET manager initialized: station=%s, vendor=0x%04X, device=0x%04X",
             config->station_name, config->vendor_id, config->device_id);
    
    return RESULT_OK;
}

// Static buffer for network interface name (p-net v0.2.0 uses const char *)
static char g_netif_name[64] = "eth0";

result_t profinet_manager_start(const char *interface) {
    if (!g_pn.initialized) return RESULT_NOT_INITIALIZED;
    if (g_pn.running) return RESULT_OK;

#ifdef HAVE_PNET
    // Set network interface (use static buffer since if_cfg expects const char *)
    if (interface) {
        strncpy(g_netif_name, interface, sizeof(g_netif_name) - 1);
        g_netif_name[sizeof(g_netif_name) - 1] = '\0';
    }
    g_pn.pnet_cfg.if_cfg.main_netif_name = g_netif_name;
    
    // Initialize p-net
    g_pn.pnet = pnet_init(&g_pn.pnet_cfg);
    if (!g_pn.pnet) {
        LOG_ERROR("Failed to initialize p-net stack");
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

    g_pn.running = false;

#ifdef HAVE_PNET
    pthread_join(g_pn.tick_thread, NULL);

    // p-net v0.2.0: No explicit close/destroy function
    // Just nullify the handle; resources freed on process exit
    if (g_pn.pnet) {
        g_pn.pnet = NULL;
    }
#endif
    
    g_pn.state = PROFINET_STATE_IDLE;
    g_pn.connected = false;
    
    LOG_INFO("PROFINET stack stopped");
    return RESULT_OK;
}

void profinet_manager_shutdown(void) {
    profinet_manager_stop();
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
    // Big-endian float for PROFINET
    uint32_t *p = (uint32_t *)&value;
    data[0] = (*p >> 24) & 0xFF;
    data[1] = (*p >> 16) & 0xFF;
    data[2] = (*p >> 8) & 0xFF;
    data[3] = *p & 0xFF;
    return profinet_manager_update_input(slot, subslot, data, 4);
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
#else
    UNUSED(arep);
#endif

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
