#ifndef PROFINET_MANAGER_H
#define PROFINET_MANAGER_H

#include "common.h"
#include "db/database.h"
#include "config/config.h"

#ifdef HAVE_PNET
#include <pnet_api.h>
#endif

typedef enum {
    PROFINET_STATE_IDLE = 0,
    PROFINET_STATE_READY,
    PROFINET_STATE_CONNECTING,
    PROFINET_STATE_CONNECTED,
    PROFINET_STATE_ERROR
} profinet_state_t;

typedef struct {
    profinet_state_t state;
    bool connected;
    uint32_t cycle_count;
    int slot_count;
    int plugged_modules;
} profinet_stats_t;

// PROFINET IOXS values (only define when p-net is not available)
// When HAVE_PNET is defined, these come from pnet_api.h as enum values
#ifndef HAVE_PNET
#define PNET_IOXS_BAD  0x00
#define PNET_IOXS_GOOD 0x80
#endif

typedef void (*profinet_connect_cb_t)(void *ctx);
typedef void (*profinet_disconnect_cb_t)(void *ctx);
typedef void (*profinet_data_cb_t)(int slot, int subslot, const uint8_t *data, size_t len, void *ctx);

result_t profinet_manager_init(database_t *db, const profinet_config_t *config);
result_t profinet_manager_start(const char *interface);
result_t profinet_manager_stop(void);
void profinet_manager_shutdown(void);

result_t profinet_manager_update_input(int slot, int subslot, const void *data, size_t size);
result_t profinet_manager_update_input_float(int slot, int subslot, float value);
result_t profinet_manager_get_output(int slot, int subslot, void *data, size_t *size);

result_t profinet_manager_write_input_data(void *mgr, int slot, int subslot, const uint8_t *data, size_t len);
result_t profinet_manager_set_input_iops(void *mgr, int slot, int subslot, uint8_t iops);
result_t profinet_manager_add_module(void *mgr, int slot, uint32_t module_ident, int subslot,
                                      uint32_t submodule_ident, size_t input_len, size_t output_len);

result_t profinet_manager_set_callbacks(profinet_connect_cb_t on_connect,
                                        profinet_disconnect_cb_t on_disconnect,
                                        profinet_data_cb_t on_data, void *ctx);

profinet_state_t profinet_manager_get_state(void);
bool profinet_manager_is_connected(void);
bool profinet_manager_is_running(void);
result_t profinet_manager_get_stats(profinet_stats_t *stats);
result_t profinet_manager_send_alarm(int slot, int subslot, uint16_t alarm_type,
                                     const uint8_t *data, size_t data_len);

const char* profinet_state_to_string(profinet_state_t state);

// Internal callbacks used by profinet_callbacks.c
void profinet_manager_set_connected(bool connected, uint32_t arep);
void profinet_manager_handle_output_data(int slot, int subslot, const uint8_t *data, size_t len);

#endif
