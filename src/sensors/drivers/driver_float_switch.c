#include "driver_float_switch.h"
#include "utils/logger.h"

result_t float_switch_init(float_switch_t *sw, int gpio_pin, bool inverted) {
    result_t result = hwif_gpio_export(&sw->gpio, gpio_pin);
    if (result != RESULT_OK) {
        return result;
    }

    hwif_gpio_set_direction(&sw->gpio, GPIO_DIR_IN);
    hwif_gpio_set_edge(&sw->gpio, GPIO_EDGE_BOTH);

    sw->inverted = inverted;
    hwif_gpio_read(&sw->gpio, &sw->last_state);

    LOG_INFO("Initialized float switch on GPIO %d (inverted=%d)", gpio_pin, inverted);
    return RESULT_OK;
}

void float_switch_destroy(float_switch_t *sw) {
    hwif_gpio_unexport(&sw->gpio);
}

result_t float_switch_read(float_switch_t *sw, bool *water_detected) {
    bool state;
    result_t result = hwif_gpio_read(&sw->gpio, &state);

    if (result == RESULT_OK) {
        *water_detected = sw->inverted ? !state : state;
        sw->last_state = state;
    }

    return result;
}

result_t float_switch_wait_for_change(float_switch_t *sw, int timeout_ms) {
    return hwif_gpio_wait_for_edge(&sw->gpio, timeout_ms);
}
