#include "driver_solenoid.h"
#include "utils/logger.h"

result_t solenoid_init(solenoid_valve_t *valve, int gpio_pin, bool inverted) {
    result_t result = hwif_gpio_export(&valve->gpio, gpio_pin);
    if (result != RESULT_OK) {
        return result;
    }

    hwif_gpio_set_direction(&valve->gpio, GPIO_DIR_OUT);

    valve->inverted = inverted;
    valve->is_open = false;

    // Start closed
    hwif_gpio_write(&valve->gpio, inverted ? true : false);

    LOG_INFO("Initialized solenoid valve on GPIO %d (inverted=%d)", gpio_pin, inverted);
    return RESULT_OK;
}

void solenoid_destroy(solenoid_valve_t *valve) {
    solenoid_close(valve);
    hwif_gpio_unexport(&valve->gpio);
}

result_t solenoid_open(solenoid_valve_t *valve) {
    result_t result = hwif_gpio_write(&valve->gpio, valve->inverted ? false : true);

    if (result == RESULT_OK) {
        valve->is_open = true;
        LOG_DEBUG("Solenoid valve opened");
    }

    return result;
}

result_t solenoid_close(solenoid_valve_t *valve) {
    result_t result = hwif_gpio_write(&valve->gpio, valve->inverted ? true : false);

    if (result == RESULT_OK) {
        valve->is_open = false;
        LOG_DEBUG("Solenoid valve closed");
    }

    return result;
}

result_t solenoid_get_state(solenoid_valve_t *valve, bool *is_open) {
    *is_open = valve->is_open;
    return RESULT_OK;
}
