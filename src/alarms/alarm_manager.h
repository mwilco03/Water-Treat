#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include "common.h"
#include "db/database.h"
#include "db/db_alarms.h"

typedef void (*alarm_callback_t)(db_alarm_history_t *alarm, void *ctx);

result_t alarm_manager_init(database_t *db);
result_t alarm_manager_start(void);
result_t alarm_manager_stop(void);
void alarm_manager_shutdown(void);

result_t alarm_manager_set_callbacks(alarm_callback_t on_raised, alarm_callback_t on_cleared, void *ctx);
result_t alarm_manager_check_value(int module_id, float value);
result_t alarm_manager_acknowledge(int alarm_id, const char *user);
result_t alarm_manager_acknowledge_all(const char *user);
result_t alarm_manager_get_active_count(int *count);
result_t alarm_manager_get_active_by_severity(alarm_severity_t severity, int *count);

result_t alarm_manager_create_rule(int module_id, const char *name, alarm_condition_t condition,
                                   float threshold_high, float threshold_low,
                                   alarm_severity_t severity, int *rule_id);
result_t alarm_manager_delete_rule(int rule_id);
result_t alarm_manager_enable_rule(int rule_id, bool enabled);

bool alarm_manager_is_running(void);

#endif
