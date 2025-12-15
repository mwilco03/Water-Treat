#ifndef DIALOG_SENSOR_H
#define DIALOG_SENSOR_H

#include "common.h"

/* Sensor dialog functions */
int dialog_sensor_add(void);
bool dialog_sensor_edit(int sensor_id);
bool dialog_sensor_delete(int sensor_id);

#endif
