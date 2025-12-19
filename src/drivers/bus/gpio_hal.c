/**
 * @file gpio_hal.c
 * @brief GPIO Hardware Abstraction Layer - libgpiod implementation
 *
 * Modern GPIO interface using libgpiod (Linux GPIO character device).
 * Falls back to legacy sysfs if libgpiod is not available.
 *
 * Why libgpiod over sysfs?
 * - sysfs GPIO is deprecated since Linux 4.8
 * - libgpiod provides atomic multi-pin operations
 * - Better event handling with proper edge detection
 * - Future-proof (sysfs may be removed in future kernels)
 * - Can query chip/line metadata (names, consumers)
 * - Single ioctl vs multiple file operations = faster
 */

#include "gpio_hal.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <pthread.h>

#ifdef HAVE_GPIOD
#include <gpiod.h>
#endif

/* ============================================================================
 * Configuration
 * ========================================================================== */

#define MAX_GPIO_PINS       64
#define GPIO_CONSUMER_NAME  "profinet-monitor"

typedef struct {
    int pin;
    bool in_use;
    gpio_direction_t direction;
    gpio_edge_t edge;
    gpio_callback_t callback;
    void *callback_ctx;

#ifdef HAVE_GPIOD
    struct gpiod_line *line;
#else
    int value_fd;
    bool exported;
#endif
} gpio_pin_state_t;

static struct {
    bool initialized;
    pthread_mutex_t mutex;

#ifdef HAVE_GPIOD
    struct gpiod_chip *chip;
    char chip_name[64];
#endif

    gpio_pin_state_t pins[MAX_GPIO_PINS];
    int pin_count;
} g_gpio = {0};

/* ============================================================================
 * Common Functions
 * ========================================================================== */

bool gpio_is_available(void) {
    return g_gpio.initialized;
}

bool gpio_chip_exists(const char *chip_name) {
    if (!chip_name) return false;

    char path[128];
    snprintf(path, sizeof(path), "/dev/%s", chip_name);
    if (access(path, F_OK) == 0) return true;

    /* Try without /dev/ prefix for already-prefixed names */
    if (strncmp(chip_name, "/dev/", 5) == 0) {
        if (access(chip_name, F_OK) == 0) return true;
    }

    return false;
}

/* ============================================================================
 * libgpiod Implementation
 * ========================================================================== */

#ifdef HAVE_GPIOD

result_t gpio_init(void) {
    if (g_gpio.initialized) return RESULT_OK;

    pthread_mutex_init(&g_gpio.mutex, NULL);

    /* Try common GPIO chip names */
    const char *chip_names[] = {
        "gpiochip0",    /* Most common on RPi, Orange Pi, etc. */
        "gpiochip1",
        "gpiochip4",    /* RPi 5 uses gpiochip4 */
        NULL
    };

    for (int i = 0; chip_names[i] != NULL; i++) {
        g_gpio.chip = gpiod_chip_open_by_name(chip_names[i]);
        if (g_gpio.chip) {
            SAFE_STRNCPY(g_gpio.chip_name, chip_names[i], sizeof(g_gpio.chip_name));
            LOG_INFO("GPIO: Opened chip %s (%s)",
                     chip_names[i], gpiod_chip_label(g_gpio.chip));
            break;
        }
    }

    if (!g_gpio.chip) {
        /* Try by path */
        g_gpio.chip = gpiod_chip_open("/dev/gpiochip0");
        if (!g_gpio.chip) {
            LOG_ERROR("GPIO: Failed to open any GPIO chip");
            return RESULT_ERROR;
        }
        SAFE_STRNCPY(g_gpio.chip_name, "/dev/gpiochip0", sizeof(g_gpio.chip_name));
    }

    g_gpio.initialized = true;
    LOG_INFO("GPIO HAL initialized (libgpiod backend)");
    return RESULT_OK;
}

void gpio_shutdown(void) {
    if (!g_gpio.initialized) return;

    pthread_mutex_lock(&g_gpio.mutex);

    /* Release all pins */
    for (int i = 0; i < g_gpio.pin_count; i++) {
        if (g_gpio.pins[i].in_use && g_gpio.pins[i].line) {
            gpiod_line_release(g_gpio.pins[i].line);
            g_gpio.pins[i].line = NULL;
            g_gpio.pins[i].in_use = false;
        }
    }

    if (g_gpio.chip) {
        gpiod_chip_close(g_gpio.chip);
        g_gpio.chip = NULL;
    }

    pthread_mutex_unlock(&g_gpio.mutex);
    pthread_mutex_destroy(&g_gpio.mutex);

    g_gpio.initialized = false;
    LOG_INFO("GPIO HAL shutdown");
}

static gpio_pin_state_t* find_or_create_pin(int pin) {
    for (int i = 0; i < g_gpio.pin_count; i++) {
        if (g_gpio.pins[i].pin == pin) {
            return &g_gpio.pins[i];
        }
    }

    if (g_gpio.pin_count >= MAX_GPIO_PINS) {
        return NULL;
    }

    gpio_pin_state_t *state = &g_gpio.pins[g_gpio.pin_count++];
    memset(state, 0, sizeof(*state));
    state->pin = pin;
    return state;
}

result_t gpio_configure(int pin, gpio_direction_t dir, gpio_pull_t pull) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state) {
        pthread_mutex_unlock(&g_gpio.mutex);
        LOG_ERROR("GPIO: Too many pins configured");
        return RESULT_NO_MEMORY;
    }

    /* Release existing line if reconfiguring */
    if (state->line) {
        gpiod_line_release(state->line);
        state->line = NULL;
    }

    /* Get line from chip */
    state->line = gpiod_chip_get_line(g_gpio.chip, pin);
    if (!state->line) {
        pthread_mutex_unlock(&g_gpio.mutex);
        LOG_ERROR("GPIO: Failed to get line %d", pin);
        return RESULT_ERROR;
    }

    /* Configure line direction and bias */
    int flags = 0;
    switch (pull) {
        case GPIO_PULL_UP:   flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP; break;
        case GPIO_PULL_DOWN: flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN; break;
        default:             flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE; break;
    }

    int ret;
    if (dir == GPIO_DIR_OUTPUT) {
        ret = gpiod_line_request_output_flags(state->line, GPIO_CONSUMER_NAME, flags, 0);
    } else {
        ret = gpiod_line_request_input_flags(state->line, GPIO_CONSUMER_NAME, flags);
    }

    if (ret < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        LOG_ERROR("GPIO: Failed to configure line %d", pin);
        return RESULT_ERROR;
    }

    state->in_use = true;
    state->direction = dir;

    pthread_mutex_unlock(&g_gpio.mutex);
    LOG_DEBUG("GPIO: Configured pin %d as %s", pin, dir == GPIO_DIR_OUTPUT ? "output" : "input");
    return RESULT_OK;
}

result_t gpio_read(int pin, bool *value) {
    CHECK_NULL(value);
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state || !state->line) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    int val = gpiod_line_get_value(state->line);
    if (val < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        LOG_ERROR("GPIO: Failed to read pin %d", pin);
        return RESULT_IO_ERROR;
    }

    *value = (val != 0);
    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_write(int pin, bool value) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state || !state->line) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    int ret = gpiod_line_set_value(state->line, value ? 1 : 0);
    if (ret < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        LOG_ERROR("GPIO: Failed to write pin %d", pin);
        return RESULT_IO_ERROR;
    }

    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_set_edge(int pin, gpio_edge_t edge) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    /* Release and reconfigure for edge detection */
    if (state->line) {
        gpiod_line_release(state->line);
    }

    state->line = gpiod_chip_get_line(g_gpio.chip, pin);
    if (!state->line) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }

    int request_type;
    switch (edge) {
        case GPIO_EDGE_RISING:  request_type = GPIOD_LINE_REQUEST_EVENT_RISING_EDGE; break;
        case GPIO_EDGE_FALLING: request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE; break;
        case GPIO_EDGE_BOTH:    request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES; break;
        default:
            pthread_mutex_unlock(&g_gpio.mutex);
            return RESULT_INVALID_PARAM;
    }

    struct gpiod_line_request_config config = {
        .consumer = GPIO_CONSUMER_NAME,
        .request_type = request_type,
        .flags = 0
    };

    if (gpiod_line_request(state->line, &config, 0) < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        LOG_ERROR("GPIO: Failed to configure edge detection on pin %d", pin);
        return RESULT_ERROR;
    }

    state->edge = edge;
    state->in_use = true;

    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_wait_edge(int pin, int timeout_ms, bool *value) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state || !state->line) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    struct timespec ts = {
        .tv_sec = timeout_ms / 1000,
        .tv_nsec = (timeout_ms % 1000) * 1000000
    };

    int ret = gpiod_line_event_wait(state->line, &ts);
    if (ret < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }
    if (ret == 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_TIMEOUT;
    }

    struct gpiod_line_event event;
    if (gpiod_line_event_read(state->line, &event) < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_IO_ERROR;
    }

    if (value) {
        *value = (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE);
    }

    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_add_callback(int pin, gpio_callback_t callback, void *ctx) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    state->callback = callback;
    state->callback_ctx = ctx;

    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_remove_callback(int pin) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (state) {
        state->callback = NULL;
        state->callback_ctx = NULL;
    }

    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

#else /* Legacy sysfs implementation */

/* ============================================================================
 * Legacy sysfs Implementation (fallback)
 * ========================================================================== */

result_t gpio_init(void) {
    if (g_gpio.initialized) return RESULT_OK;

    pthread_mutex_init(&g_gpio.mutex, NULL);
    g_gpio.initialized = true;

    LOG_WARNING("GPIO HAL initialized (legacy sysfs backend - deprecated)");
    return RESULT_OK;
}

void gpio_shutdown(void) {
    if (!g_gpio.initialized) return;

    pthread_mutex_lock(&g_gpio.mutex);

    for (int i = 0; i < g_gpio.pin_count; i++) {
        gpio_pin_state_t *state = &g_gpio.pins[i];
        if (state->in_use) {
            if (state->value_fd >= 0) {
                close(state->value_fd);
            }
            if (state->exported) {
                int fd = open("/sys/class/gpio/unexport", O_WRONLY);
                if (fd >= 0) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d", state->pin);
                    (void)write(fd, buf, strlen(buf));
                    close(fd);
                }
            }
        }
    }

    pthread_mutex_unlock(&g_gpio.mutex);
    pthread_mutex_destroy(&g_gpio.mutex);
    g_gpio.initialized = false;
}

static gpio_pin_state_t* find_or_create_pin(int pin) {
    for (int i = 0; i < g_gpio.pin_count; i++) {
        if (g_gpio.pins[i].pin == pin) return &g_gpio.pins[i];
    }
    if (g_gpio.pin_count >= MAX_GPIO_PINS) return NULL;
    gpio_pin_state_t *state = &g_gpio.pins[g_gpio.pin_count++];
    memset(state, 0, sizeof(*state));
    state->pin = pin;
    state->value_fd = -1;
    return state;
}

static result_t sysfs_export(gpio_pin_state_t *state) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", state->pin);

    if (access(path, F_OK) == 0) {
        state->exported = true;
        return RESULT_OK;
    }

    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return RESULT_ERROR;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", state->pin);
    if (write(fd, buf, strlen(buf)) < 0) {
        close(fd);
        return RESULT_ERROR;
    }
    close(fd);

    usleep(100000); /* Wait for sysfs */
    state->exported = true;
    return RESULT_OK;
}

result_t gpio_configure(int pin, gpio_direction_t dir, gpio_pull_t pull) {
    UNUSED(pull); /* sysfs doesn't support pull configuration */
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NO_MEMORY;
    }

    if (sysfs_export(state) != RESULT_OK) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }

    const char *dir_str = (dir == GPIO_DIR_OUTPUT) ? "out" : "in";
    if (write(fd, dir_str, strlen(dir_str)) < 0) {
        close(fd);
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }
    close(fd);

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    state->value_fd = open(path, O_RDWR);
    if (state->value_fd < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }

    state->in_use = true;
    state->direction = dir;

    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_read(int pin, bool *value) {
    CHECK_NULL(value);
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state || state->value_fd < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    char buf[4];
    lseek(state->value_fd, 0, SEEK_SET);
    if (read(state->value_fd, buf, sizeof(buf)) < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_IO_ERROR;
    }

    *value = (buf[0] == '1');
    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_write(int pin, bool value) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state || state->value_fd < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    lseek(state->value_fd, 0, SEEK_SET);
    if (write(state->value_fd, value ? "1" : "0", 1) < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_IO_ERROR;
    }

    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_set_edge(int pin, gpio_edge_t edge) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state || !state->exported) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/edge", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }

    const char *edge_str;
    switch (edge) {
        case GPIO_EDGE_RISING:  edge_str = "rising"; break;
        case GPIO_EDGE_FALLING: edge_str = "falling"; break;
        case GPIO_EDGE_BOTH:    edge_str = "both"; break;
        default:                edge_str = "none"; break;
    }

    if (write(fd, edge_str, strlen(edge_str)) < 0) {
        close(fd);
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_ERROR;
    }
    close(fd);

    state->edge = edge;
    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

result_t gpio_wait_edge(int pin, int timeout_ms, bool *value) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);

    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (!state || state->value_fd < 0) {
        pthread_mutex_unlock(&g_gpio.mutex);
        return RESULT_NOT_FOUND;
    }

    /* Clear pending interrupt */
    char buf[4];
    lseek(state->value_fd, 0, SEEK_SET);
    (void)read(state->value_fd, buf, sizeof(buf));

    struct pollfd pfd = {
        .fd = state->value_fd,
        .events = POLLPRI | POLLERR
    };

    pthread_mutex_unlock(&g_gpio.mutex);

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) return RESULT_ERROR;
    if (ret == 0) return RESULT_TIMEOUT;

    if (value) {
        gpio_read(pin, value);
    }

    return RESULT_OK;
}

result_t gpio_add_callback(int pin, gpio_callback_t callback, void *ctx) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);
    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (state) {
        state->callback = callback;
        state->callback_ctx = ctx;
    }
    pthread_mutex_unlock(&g_gpio.mutex);
    return state ? RESULT_OK : RESULT_NOT_FOUND;
}

result_t gpio_remove_callback(int pin) {
    if (!g_gpio.initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&g_gpio.mutex);
    gpio_pin_state_t *state = find_or_create_pin(pin);
    if (state) {
        state->callback = NULL;
        state->callback_ctx = NULL;
    }
    pthread_mutex_unlock(&g_gpio.mutex);
    return RESULT_OK;
}

#endif /* HAVE_GPIOD */

/* ============================================================================
 * PWM Functions (shared implementation)
 * ========================================================================== */

bool gpio_has_pwm(int pin) {
    /* Check for hardware PWM - board specific */
    UNUSED(pin);
    /* TODO: Check /sys/class/pwm/pwmchipN for hardware PWM support */
    return false;
}

result_t gpio_pwm_start(int pin, int frequency_hz, float duty_cycle) {
    UNUSED(pin); UNUSED(frequency_hz); UNUSED(duty_cycle);
    LOG_WARNING("GPIO: Hardware PWM not yet implemented");
    return RESULT_NOT_SUPPORTED;
}

result_t gpio_pwm_set_duty(int pin, float duty_cycle) {
    UNUSED(pin); UNUSED(duty_cycle);
    return RESULT_NOT_SUPPORTED;
}

result_t gpio_pwm_stop(int pin) {
    UNUSED(pin);
    return RESULT_NOT_SUPPORTED;
}
