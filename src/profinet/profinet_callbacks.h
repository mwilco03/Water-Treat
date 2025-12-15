#ifndef PROFINET_CALLBACKS_H
#define PROFINET_CALLBACKS_H

#include "common.h"

#ifdef HAVE_PNET
#include <pnet_api.h>

int profinet_state_callback(pnet_t *net, void *arg, uint32_t arep, pnet_event_values_t event);
int profinet_connect_callback(pnet_t *net, void *arg, uint32_t arep, pnet_result_t *result);
int profinet_release_callback(pnet_t *net, void *arg, uint32_t arep, pnet_result_t *result);
int profinet_dcontrol_callback(pnet_t *net, void *arg, uint32_t arep, pnet_control_command_t cmd, pnet_result_t *result);
int profinet_ccontrol_callback(pnet_t *net, void *arg, uint32_t arep, pnet_result_t *result);
int profinet_read_callback(pnet_t *net, void *arg, uint32_t arep, uint32_t api, uint16_t slot, uint16_t subslot,
                           uint16_t idx, uint16_t seq, uint8_t **data, uint16_t *len, pnet_result_t *result);
int profinet_write_callback(pnet_t *net, void *arg, uint32_t arep, uint32_t api, uint16_t slot, uint16_t subslot,
                            uint16_t idx, uint16_t seq, uint16_t len, const uint8_t *data, pnet_result_t *result);
int profinet_exp_module_callback(pnet_t *net, void *arg, uint32_t api, uint16_t slot, uint32_t ident);
int profinet_exp_submodule_callback(pnet_t *net, void *arg, uint32_t api, uint16_t slot, uint16_t subslot, uint32_t ident);
int profinet_new_data_status_callback(pnet_t *net, void *arg, uint32_t arep, uint32_t crep, uint8_t changes, uint8_t data_status);
int profinet_alarm_ind_callback(pnet_t *net, void *arg, uint32_t arep, const pnet_alarm_argument_t *p_alarm, uint16_t data_len, uint16_t usi, const uint8_t *data);
int profinet_alarm_cnf_callback(pnet_t *net, void *arg, uint32_t arep, const pnet_pnio_status_t *status);
int profinet_alarm_ack_cnf_callback(pnet_t *net, void *arg, uint32_t arep, int result);
int profinet_reset_callback(pnet_t *net, void *arg, bool factory_reset, uint16_t reset_mode);
int profinet_signal_led_callback(pnet_t *net, void *arg, bool on);

#endif /* HAVE_PNET */

#endif
