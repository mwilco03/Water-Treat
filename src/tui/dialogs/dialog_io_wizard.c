/**
 * @file dialog_io_wizard.c
 * @brief Progressive Disclosure I/O Configuration Wizard Implementation
 *
 * Core Architecture Principles Applied:
 * 1. Dynamic Discovery - Scan I2C/1-Wire before asking questions
 * 2. Loose Coupling - Wizard doesn't know about specific drivers
 * 3. Graceful Degradation - Show conflicts, don't block
 * 4. Single Source of Truth - User points, system derives
 * 5. Hardware Agnostic - Works on any supported board
 * 6. Informational Output - Show what's happening
 *
 * Navigation Contract: ESC always goes back exactly one step.
 */

#include "dialog_io_wizard.h"
#include "dialog_helpers.h"
#include "../tui_common.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "db/db_actuators.h"
#include "platform/board_detect.h"
#include "platform/hw_discover.h"
#include "sensors/sensor_api.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ============================================================================
 * Constants
 * ========================================================================== */

#define WIZARD_WIDTH      70
#define WIZARD_HEIGHT     20
#define MAX_MENU_ITEMS    32
#define SCAN_DELAY_MS     50

/* PROFINET slot ranges (from CONTROLLER_SPEC.md) */
#define SENSOR_SLOT_MIN   1
#define SENSOR_SLOT_MAX   8
#define ACTUATOR_SLOT_MIN 9
#define ACTUATOR_SLOT_MAX 16

/* ============================================================================
 * Wizard State Machine
 * ========================================================================== */

typedef enum {
    WIZ_STATE_IO_TYPE = 0,       /* Screen 1: INPUT or OUTPUT? */

    /* Sensor (INPUT) path */
    WIZ_STATE_SENSOR_CONNECTION, /* Screen 2A: How connected? */
    WIZ_STATE_SENSOR_SCAN,       /* Screen 3A: Scanning... */
    WIZ_STATE_SENSOR_SCAN_PICK,  /* Screen 3A-Results: Pick device */
    WIZ_STATE_SENSOR_GPIO_PIN,   /* Screen 3B: Pick GPIO pin */
    WIZ_STATE_SENSOR_GPIO_TYPE,  /* Screen 3B-Type: What GPIO input? */
    WIZ_STATE_SENSOR_ADC_SCAN,   /* Screen 3C: Find ADCs */
    WIZ_STATE_SENSOR_ADC_PICK,   /* Screen 3C: Pick ADC channel */
    WIZ_STATE_SENSOR_ADC_TYPE,   /* Screen 3C-Type: What analog sensor? */

    /* Actuator (OUTPUT) path */
    WIZ_STATE_ACTUATOR_TYPE,     /* Screen 2B: Relay or PWM? */
    WIZ_STATE_ACTUATOR_PIN,      /* Screen 3D: Pick GPIO pin */
    WIZ_STATE_ACTUATOR_DEVICE,   /* Screen 3D-Type: What device? */

    /* Common final screens */
    WIZ_STATE_NAME,              /* Screen 4: Name it */
    WIZ_STATE_CONFIRM,           /* Screen 5: Confirm */

    WIZ_STATE_DONE,              /* Finished successfully */
    WIZ_STATE_CANCELLED,         /* User cancelled */
} wizard_state_t;

/* ============================================================================
 * Discovered Device Types (unified for selection)
 * ========================================================================== */

typedef enum {
    DISCOVERED_NONE = 0,
    DISCOVERED_I2C_ADC,          /* ADS1115, MCP3008, etc. */
    DISCOVERED_I2C_SENSOR,       /* BME280, SHT31, etc. */
    DISCOVERED_ONEWIRE_TEMP,     /* DS18B20 */
    DISCOVERED_GPIO_INPUT,       /* Float switch, flow meter */
    DISCOVERED_ADC_CHANNEL,      /* Specific ADC channel */
} discovered_type_t;

typedef struct {
    discovered_type_t type;
    char display_name[64];       /* What user sees */
    char description[64];        /* Additional info */
    bool in_use;                 /* Already configured? */
    char used_by[32];            /* Name of existing config */

    /* Hardware details (filled based on type) */
    union {
        struct {
            int bus;
            uint8_t address;
            i2c_device_type_t device_type;
        } i2c;
        struct {
            char id[24];
            float current_value;
        } onewire;
        struct {
            int pin;
            const char *label;
        } gpio;
        struct {
            int bus;
            uint8_t adc_address;
            int channel;
        } adc_channel;
    } hw;
} discovered_device_t;

/* ============================================================================
 * Sensor Type Definitions (Plain English)
 * ========================================================================== */

typedef struct {
    const char *name;            /* Display name */
    const char *description;     /* What it does */
    sensor_channel_t channel;    /* Sensor channel type */
    const char *unit;            /* Default unit */
    float range_min;             /* Default min value */
    float range_max;             /* Default max value */
} sensor_type_def_t;

/* GPIO input sensor types */
static const sensor_type_def_t gpio_sensor_types[] = {
    {"Flow Meter / Pulse Counter", "Counts pulses per second",
     SENSOR_CHAN_BINARY, "pulses/s", 0, 10000},
    {"Float Switch / Level Sensor", "Simple on/off state",
     SENSOR_CHAN_BINARY, "", 0, 1},
    {"DHT22 Temperature/Humidity", "Single-wire digital sensor",
     SENSOR_CHAN_TEMPERATURE, "°C", -40, 80},
    {"Generic Digital Input", "Any on/off signal",
     SENSOR_CHAN_BINARY, "", 0, 1},
};
#define GPIO_SENSOR_TYPE_COUNT 4

/* ADC-based sensor types */
static const sensor_type_def_t adc_sensor_types[] = {
    {"pH Probe", "Measures acidity/alkalinity",
     SENSOR_CHAN_PH, "pH", 0, 14},
    {"Pressure Transducer", "4-20mA or 0-5V pressure",
     SENSOR_CHAN_PRESSURE, "bar", 0, 10},
    {"TDS Sensor", "Total Dissolved Solids",
     SENSOR_CHAN_TDS, "ppm", 0, 5000},
    {"Turbidity Sensor", "Measures water clarity",
     SENSOR_CHAN_TURBIDITY, "NTU", 0, 1000},
    {"ORP Sensor", "Oxidation-Reduction Potential",
     SENSOR_CHAN_ORP, "mV", -2000, 2000},
    {"Generic 0-5V Analog", "Custom voltage input",
     SENSOR_CHAN_VOLTAGE, "V", 0, 5},
};
#define ADC_SENSOR_TYPE_COUNT 6

/* Actuator types */
typedef struct {
    const char *name;
    const char *description;
    int actuator_type;           /* actuator_type_t enum value */
} actuator_type_def_t;

static const actuator_type_def_t actuator_types[] = {
    {"Pump", "Chemical dosing, transfer, circulation", 0},
    {"Valve / Solenoid", "Flow control, isolation", 5},
    {"Generic Relay", "Any on/off controlled device", 0},
};
#define ACTUATOR_TYPE_COUNT 3

/* ============================================================================
 * Wizard Context
 * ========================================================================== */

typedef struct {
    wizard_state_t state;
    wizard_state_t prev_states[16];  /* Stack for ESC navigation */
    int state_depth;

    WINDOW *win;
    board_info_t board;
    hw_discovery_result_t discovery;

    /* What we're building */
    bool is_sensor;              /* true=sensor, false=actuator */

    /* Discovered devices list */
    discovered_device_t devices[MAX_MENU_ITEMS];
    int device_count;
    int selected_device;

    /* Selected configuration */
    int selected_gpio_pin;
    int selected_adc_bus;
    uint8_t selected_adc_address;
    int selected_adc_channel;
    int selected_sensor_type;    /* Index into type arrays */
    int selected_actuator_type;
    bool is_pwm;

    /* 1-Wire specific */
    char selected_onewire_id[24];
    float onewire_current_value;

    /* I2C sensor specific */
    i2c_device_type_t selected_i2c_type;
    int selected_i2c_bus;
    uint8_t selected_i2c_address;

    /* Final configuration */
    char name[64];
    int assigned_slot;

    /* Result */
    io_wizard_result_t result;

} wizard_context_t;

static wizard_context_t g_wiz = {0};

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

/**
 * Push current state onto history stack (for ESC navigation)
 */
static void push_state(wizard_state_t new_state) {
    if (g_wiz.state_depth < 15) {
        g_wiz.prev_states[g_wiz.state_depth++] = g_wiz.state;
    }
    g_wiz.state = new_state;
}

/**
 * Pop state from history stack (ESC pressed)
 */
static bool pop_state(void) {
    if (g_wiz.state_depth > 0) {
        g_wiz.state = g_wiz.prev_states[--g_wiz.state_depth];
        return true;
    }
    return false;  /* No more history - exit wizard */
}

/**
 * Draw wizard header with north star explanation
 */
static void draw_header(const char *title, const char *explanation) {
    werase(g_wiz.win);
    box(g_wiz.win, 0, 0);

    /* Title */
    wattron(g_wiz.win, A_BOLD);
    mvwprintw(g_wiz.win, 0, (WIZARD_WIDTH - strlen(title) - 2) / 2, " %s ", title);
    wattroff(g_wiz.win, A_BOLD);

    /* North star explanation - what we're doing and why */
    if (explanation) {
        wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_NORMAL) | A_DIM);
        mvwprintw(g_wiz.win, 2, 3, "%s", explanation);
        wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_NORMAL) | A_DIM);
    }
}

/**
 * Draw navigation hints at bottom
 */
static void draw_nav_hints(const char *hints) {
    wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_NORMAL) | A_DIM);
    mvwhline(g_wiz.win, WIZARD_HEIGHT - 3, 1, ACS_HLINE, WIZARD_WIDTH - 2);
    mvwprintw(g_wiz.win, WIZARD_HEIGHT - 2, 3, "%s", hints);
    wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_NORMAL) | A_DIM);
}

/**
 * Draw a menu item
 */
static void draw_menu_item(int row, int selected_idx, int item_idx,
                           const char *label, const char *desc, bool disabled) {
    int col = 5;

    if (disabled) {
        wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR) | A_DIM);
    } else if (item_idx == selected_idx) {
        wattron(g_wiz.win, A_REVERSE);
    }

    /* Selection indicator */
    mvwprintw(g_wiz.win, row, col - 2, item_idx == selected_idx ? ">" : " ");

    /* Label */
    mvwprintw(g_wiz.win, row, col, "%-40s", label);

    if (item_idx == selected_idx) {
        wattroff(g_wiz.win, A_REVERSE);
    }

    /* Description on next line */
    if (desc && strlen(desc) > 0) {
        if (disabled) {
            mvwprintw(g_wiz.win, row + 1, col + 2, "%s", desc);
            wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR) | A_DIM);
        } else {
            wattron(g_wiz.win, A_DIM);
            mvwprintw(g_wiz.win, row + 1, col + 2, "%s", desc);
            wattroff(g_wiz.win, A_DIM);
        }
    }
}

/**
 * Find next available PROFINET slot
 */
static int find_next_slot(bool for_sensor) {
    database_t *db = tui_get_database();
    if (!db) return for_sensor ? SENSOR_SLOT_MIN : ACTUATOR_SLOT_MIN;

    int min_slot = for_sensor ? SENSOR_SLOT_MIN : ACTUATOR_SLOT_MIN;
    int max_slot = for_sensor ? SENSOR_SLOT_MAX : ACTUATOR_SLOT_MAX;

    /* Get list of used slots */
    bool used[17] = {false};

    if (for_sensor) {
        db_module_t *modules = NULL;
        int count = 0;
        if (db_module_list(db, &modules, &count) == RESULT_OK && modules) {
            for (int i = 0; i < count; i++) {
                if (modules[i].slot >= min_slot && modules[i].slot <= max_slot) {
                    used[modules[i].slot] = true;
                }
            }
            free(modules);
        }
    } else {
        db_actuator_t *actuators = NULL;
        int count = 0;
        if (db_actuator_list(db, &actuators, &count) == RESULT_OK && actuators) {
            for (int i = 0; i < count; i++) {
                if (actuators[i].slot >= min_slot && actuators[i].slot <= max_slot) {
                    used[actuators[i].slot] = true;
                }
            }
            free(actuators);
        }
    }

    /* Find first unused slot */
    for (int slot = min_slot; slot <= max_slot; slot++) {
        if (!used[slot]) return slot;
    }

    return min_slot;  /* Fallback - will show conflict */
}

/**
 * Check if GPIO pin is already in use
 */
static bool check_gpio_conflict(int gpio_pin, char *used_by, size_t used_by_size) {
    database_t *db = tui_get_database();
    if (!db) return false;

    gpio_conflict_t conflict;
    if (db_actuator_gpio_conflict_check(db, gpio_pin, g_wiz.board.pins.gpio_chip,
                                        0, &conflict) == RESULT_OK) {
        if (conflict.has_conflict) {
            if (used_by) {
                snprintf(used_by, used_by_size, "%s", conflict.conflicting_name);
            }
            return true;
        }
    }
    return false;
}

/**
 * Validate name (lowercase with underscores)
 */
static bool validate_name(const char *name, char *error, size_t error_size) {
    if (strlen(name) == 0) {
        snprintf(error, error_size, "Name cannot be empty");
        return false;
    }

    if (strlen(name) < 3) {
        snprintf(error, error_size, "Name too short (min 3 chars)");
        return false;
    }

    for (size_t i = 0; i < strlen(name); i++) {
        char c = name[i];
        if (!islower(c) && !isdigit(c) && c != '_') {
            snprintf(error, error_size, "Use lowercase letters, numbers, underscores only");
            return false;
        }
    }

    /* Check for duplicate names */
    database_t *db = tui_get_database();
    if (db) {
        db_module_t *modules = NULL;
        int count = 0;
        if (db_module_list(db, &modules, &count) == RESULT_OK && modules) {
            for (int i = 0; i < count; i++) {
                if (strcmp(modules[i].name, name) == 0) {
                    snprintf(error, error_size, "Name already in use by sensor");
                    free(modules);
                    return false;
                }
            }
            free(modules);
        }

        db_actuator_t *actuators = NULL;
        if (db_actuator_list(db, &actuators, &count) == RESULT_OK && actuators) {
            for (int i = 0; i < count; i++) {
                if (strcmp(actuators[i].name, name) == 0) {
                    snprintf(error, error_size, "Name already in use by actuator");
                    free(actuators);
                    return false;
                }
            }
            free(actuators);
        }
    }

    return true;
}

/* ============================================================================
 * Screen 1: INPUT or OUTPUT?
 * ========================================================================== */

static void screen_io_type(void) {
    static int selected = 0;

    draw_header("Configure New I/O Point",
                "What does this connection do?");

    int row = 5;

    /* INPUT option */
    if (selected == 0) wattron(g_wiz.win, A_REVERSE);
    mvwprintw(g_wiz.win, row, 5, " [1] INPUT  - Reads the physical world              ");
    if (selected == 0) wattroff(g_wiz.win, A_REVERSE);
    wattron(g_wiz.win, A_DIM);
    mvwprintw(g_wiz.win, row + 1, 10, "Temperature, pressure, flow, level");
    mvwprintw(g_wiz.win, row + 2, 10, "(sensors, probes, meters)");
    wattroff(g_wiz.win, A_DIM);

    row += 5;

    /* OUTPUT option */
    if (selected == 1) wattron(g_wiz.win, A_REVERSE);
    mvwprintw(g_wiz.win, row, 5, " [2] OUTPUT - Changes the physical world            ");
    if (selected == 1) wattroff(g_wiz.win, A_REVERSE);
    wattron(g_wiz.win, A_DIM);
    mvwprintw(g_wiz.win, row + 1, 10, "Pumps, valves, relays, solenoids");
    mvwprintw(g_wiz.win, row + 2, 10, "(actuators, switches)");
    wattroff(g_wiz.win, A_DIM);

    draw_nav_hints("[1/2] or [Up/Down] Select  |  [Enter] Continue  |  [ESC] Cancel");

    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case '1':
        case KEY_UP:
            selected = 0;
            break;
        case '2':
        case KEY_DOWN:
            selected = 1;
            break;
        case '\n':
        case KEY_ENTER:
            g_wiz.is_sensor = (selected == 0);
            if (g_wiz.is_sensor) {
                push_state(WIZ_STATE_SENSOR_CONNECTION);
            } else {
                push_state(WIZ_STATE_ACTUATOR_TYPE);
            }
            break;
        case 27:  /* ESC */
            g_wiz.state = WIZ_STATE_CANCELLED;
            break;
    }
}

/* ============================================================================
 * Screen 2A: Sensor Connection Type
 * ========================================================================== */

static void screen_sensor_connection(void) {
    static int selected = 0;

    draw_header("INPUT: Connection Type",
                "How is your sensor connected?");

    int row = 5;

    const struct {
        char key;
        const char *label;
        const char *desc1;
        const char *desc2;
    } options[] = {
        {'S', "SCAN - Let me look for connected devices",
         "I'll check I2C buses and 1-Wire for you", NULL},
        {'G', "GPIO PIN - Direct wire to a specific pin",
         "Flow meters, float switches, pulse counters", NULL},
        {'A', "ADC CHANNEL - Analog voltage via ADC",
         "pH probes, pressure transducers, 4-20mA",
         "(requires I2C ADC like ADS1115)"},
    };

    for (int i = 0; i < 3; i++) {
        if (selected == i) wattron(g_wiz.win, A_REVERSE);
        mvwprintw(g_wiz.win, row, 5, " [%c] %s ", options[i].key, options[i].label);
        if (selected == i) wattroff(g_wiz.win, A_REVERSE);

        wattron(g_wiz.win, A_DIM);
        mvwprintw(g_wiz.win, row + 1, 10, "%s", options[i].desc1);
        if (options[i].desc2) {
            mvwprintw(g_wiz.win, row + 2, 10, "%s", options[i].desc2);
            row++;
        }
        wattroff(g_wiz.win, A_DIM);

        row += 3;
    }

    draw_nav_hints("[S/G/A] or [Up/Down] Select  |  [Enter] Continue  |  [ESC] Back");

    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case 's': case 'S':
            selected = 0;
            push_state(WIZ_STATE_SENSOR_SCAN);
            break;
        case 'g': case 'G':
            selected = 1;
            push_state(WIZ_STATE_SENSOR_GPIO_PIN);
            break;
        case 'a': case 'A':
            selected = 2;
            push_state(WIZ_STATE_SENSOR_ADC_SCAN);
            break;
        case KEY_UP:
            if (selected > 0) selected--;
            break;
        case KEY_DOWN:
            if (selected < 2) selected++;
            break;
        case '\n':
        case KEY_ENTER:
            if (selected == 0) push_state(WIZ_STATE_SENSOR_SCAN);
            else if (selected == 1) push_state(WIZ_STATE_SENSOR_GPIO_PIN);
            else push_state(WIZ_STATE_SENSOR_ADC_SCAN);
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 3A: Hardware Discovery Scan
 * ========================================================================== */

static void screen_sensor_scan(void) {
    draw_header("Scanning for Devices...",
                "Checking I2C buses and 1-Wire interfaces");

    /* Show progress */
    int row = 5;
    mvwprintw(g_wiz.win, row++, 5, "Checking I2C buses...");
    wrefresh(g_wiz.win);

    /* Do the actual scan */
    memset(&g_wiz.discovery, 0, sizeof(g_wiz.discovery));
    hw_discover_all(&g_wiz.discovery);

    /* Build device list from discovery results */
    g_wiz.device_count = 0;

    /* Add I2C devices */
    for (int i = 0; i < g_wiz.discovery.i2c_count && g_wiz.device_count < MAX_MENU_ITEMS; i++) {
        i2c_device_t *dev = &g_wiz.discovery.i2c_devices[i];
        discovered_device_t *d = &g_wiz.devices[g_wiz.device_count];

        /* Skip non-sensor I2C devices */
        if (dev->type == I2C_DEVICE_AT24C ||
            dev->type == I2C_DEVICE_OLED_SSD1306 ||
            dev->type == I2C_DEVICE_DS3231) {
            continue;
        }

        /* Is it an ADC? Expand to channels */
        if (dev->type == I2C_DEVICE_ADS1115 || dev->type == I2C_DEVICE_ADS1015) {
            for (int ch = 0; ch < 4 && g_wiz.device_count < MAX_MENU_ITEMS; ch++) {
                d = &g_wiz.devices[g_wiz.device_count];
                d->type = DISCOVERED_ADC_CHANNEL;
                snprintf(d->display_name, sizeof(d->display_name),
                         "I2C 0x%02X: %s Channel %d", dev->address, dev->name, ch);
                snprintf(d->description, sizeof(d->description),
                         "Analog input for pH, pressure, etc.");
                d->hw.adc_channel.bus = dev->bus;
                d->hw.adc_channel.adc_address = dev->address;
                d->hw.adc_channel.channel = ch;
                d->in_use = false;  /* TODO: check database */
                g_wiz.device_count++;
            }
            continue;
        }

        /* Regular I2C sensor */
        d->type = DISCOVERED_I2C_SENSOR;
        snprintf(d->display_name, sizeof(d->display_name),
                 "I2C 0x%02X: %s", dev->address, dev->name);
        snprintf(d->description, sizeof(d->description), "%s", dev->description);
        d->hw.i2c.bus = dev->bus;
        d->hw.i2c.address = dev->address;
        d->hw.i2c.device_type = dev->type;
        d->in_use = false;
        g_wiz.device_count++;
    }

    /* Add 1-Wire devices */
    for (int i = 0; i < g_wiz.discovery.onewire_count && g_wiz.device_count < MAX_MENU_ITEMS; i++) {
        onewire_device_t *dev = &g_wiz.discovery.onewire_devices[i];
        discovered_device_t *d = &g_wiz.devices[g_wiz.device_count];

        d->type = DISCOVERED_ONEWIRE_TEMP;
        if (dev->last_value > -100) {
            snprintf(d->display_name, sizeof(d->display_name),
                     "1-Wire: %s (%s) %.1f°C", dev->name, dev->id, dev->last_value);
        } else {
            snprintf(d->display_name, sizeof(d->display_name),
                     "1-Wire: %s (%s)", dev->name, dev->id);
        }
        snprintf(d->description, sizeof(d->description), "Temperature sensor");
        strncpy(d->hw.onewire.id, dev->id, sizeof(d->hw.onewire.id) - 1);
        d->hw.onewire.current_value = dev->last_value;
        d->in_use = false;
        g_wiz.device_count++;
    }

    g_wiz.selected_device = 0;

    /* Transition to pick screen */
    g_wiz.state = WIZ_STATE_SENSOR_SCAN_PICK;
}

/* ============================================================================
 * Screen 3A-Results: Pick Discovered Device
 * ========================================================================== */

static void screen_sensor_scan_pick(void) {
    char title[64];
    snprintf(title, sizeof(title), "Found %d Device%s",
             g_wiz.device_count, g_wiz.device_count == 1 ? "" : "s");

    draw_header(title,
                g_wiz.device_count > 0 ?
                "Select the device you want to configure:" :
                "No devices found. Check connections and try again.");

    if (g_wiz.device_count == 0) {
        wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(g_wiz.win, 8, 5, "No I2C or 1-Wire devices detected.");
        mvwprintw(g_wiz.win, 10, 5, "Tips:");
        mvwprintw(g_wiz.win, 11, 7, "- Check wiring connections");
        mvwprintw(g_wiz.win, 12, 7, "- Enable I2C in raspi-config");
        mvwprintw(g_wiz.win, 13, 7, "- Load 1-Wire kernel module");
        wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_WARNING));

        draw_nav_hints("[R] Rescan  |  [ESC] Back");
        wrefresh(g_wiz.win);

        int ch = wgetch(g_wiz.win);
        if (ch == 'r' || ch == 'R') {
            g_wiz.state = WIZ_STATE_SENSOR_SCAN;
        } else if (ch == 27) {
            pop_state();
        }
        return;
    }

    /* Draw device list */
    int row = 5;
    int visible = MIN(10, g_wiz.device_count);
    int scroll = 0;
    if (g_wiz.selected_device >= visible) {
        scroll = g_wiz.selected_device - visible + 1;
    }

    for (int i = 0; i < visible; i++) {
        int idx = scroll + i;
        if (idx >= g_wiz.device_count) break;

        discovered_device_t *d = &g_wiz.devices[idx];

        if (idx == g_wiz.selected_device) {
            wattron(g_wiz.win, A_REVERSE);
            mvwprintw(g_wiz.win, row, 3, ">");
        } else {
            mvwprintw(g_wiz.win, row, 3, " ");
        }

        if (d->in_use) {
            wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
        }

        mvwprintw(g_wiz.win, row, 5, "%-50s", d->display_name);

        if (d->in_use) {
            wprintw(g_wiz.win, " [in use: %s]", d->used_by);
            wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
        } else {
            wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_STATUS));
            wprintw(g_wiz.win, " [available]");
            wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_STATUS));
        }

        if (idx == g_wiz.selected_device) {
            wattroff(g_wiz.win, A_REVERSE);
        }

        row++;
    }

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [R] Rescan  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            if (g_wiz.selected_device > 0) g_wiz.selected_device--;
            break;
        case KEY_DOWN:
            if (g_wiz.selected_device < g_wiz.device_count - 1) g_wiz.selected_device++;
            break;
        case 'r': case 'R':
            g_wiz.state = WIZ_STATE_SENSOR_SCAN;
            break;
        case '\n':
        case KEY_ENTER:
            {
                discovered_device_t *d = &g_wiz.devices[g_wiz.selected_device];
                if (d->in_use) {
                    /* Show conflict dialog */
                    dialog_warning("Device already in use. Choose another.");
                    break;
                }

                /* Store selected device info */
                switch (d->type) {
                    case DISCOVERED_ADC_CHANNEL:
                        g_wiz.selected_adc_bus = d->hw.adc_channel.bus;
                        g_wiz.selected_adc_address = d->hw.adc_channel.adc_address;
                        g_wiz.selected_adc_channel = d->hw.adc_channel.channel;
                        push_state(WIZ_STATE_SENSOR_ADC_TYPE);
                        break;
                    case DISCOVERED_ONEWIRE_TEMP:
                        strncpy(g_wiz.selected_onewire_id, d->hw.onewire.id,
                                sizeof(g_wiz.selected_onewire_id) - 1);
                        g_wiz.onewire_current_value = d->hw.onewire.current_value;
                        push_state(WIZ_STATE_NAME);  /* DS18B20 needs no type selection */
                        break;
                    case DISCOVERED_I2C_SENSOR:
                        g_wiz.selected_i2c_bus = d->hw.i2c.bus;
                        g_wiz.selected_i2c_address = d->hw.i2c.address;
                        g_wiz.selected_i2c_type = d->hw.i2c.device_type;
                        push_state(WIZ_STATE_NAME);  /* Known sensor, go to naming */
                        break;
                    default:
                        break;
                }
            }
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 3B: GPIO Pin Picker (for sensors)
 * ========================================================================== */

static void screen_sensor_gpio_pin(void) {
    draw_header("GPIO Input Pin Selection",
                "Which GPIO pin is your sensor connected to?");

    /* Build pin list from board detection */
    struct {
        int pin;
        const char *label;
        bool in_use;
        char used_by[32];
    } pins[8];
    int pin_count = 0;

    /* Add suggested input pins from board config */
    if (g_wiz.board.pins.gpio_input_1 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_input_1;
        pins[pin_count].label = "Input 1";
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }
    if (g_wiz.board.pins.gpio_input_2 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_input_2;
        pins[pin_count].label = "Input 2";
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }
    if (g_wiz.board.pins.gpio_input_3 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_input_3;
        pins[pin_count].label = "Input 3";
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }
    if (g_wiz.board.pins.gpio_input_4 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_input_4;
        pins[pin_count].label = "Input 4";
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }

    /* Fallback if no pins defined */
    if (pin_count == 0) {
        int fallback_pins[] = {17, 27, 22, 5, 6};
        for (int i = 0; i < 5 && pin_count < 8; i++) {
            pins[pin_count].pin = fallback_pins[i];
            pins[pin_count].label = "GPIO";
            pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                          pins[pin_count].used_by, 32);
            pin_count++;
        }
    }

    static int selected = 0;
    if (selected >= pin_count) selected = 0;

    int row = 5;
    mvwprintw(g_wiz.win, row++, 5, "Board: %s", g_wiz.board.name);
    row++;

    for (int i = 0; i < pin_count; i++) {
        if (i == selected) wattron(g_wiz.win, A_REVERSE);

        if (pins[i].in_use) {
            wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
            mvwprintw(g_wiz.win, row, 5, " GPIO %2d - %-12s [in use: %s]",
                      pins[i].pin, pins[i].label, pins[i].used_by);
            wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
        } else {
            mvwprintw(g_wiz.win, row, 5, " GPIO %2d - %-12s [available]      ",
                      pins[i].pin, pins[i].label);
        }

        if (i == selected) wattroff(g_wiz.win, A_REVERSE);
        row++;
    }

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [M] Manual entry  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            if (selected > 0) selected--;
            break;
        case KEY_DOWN:
            if (selected < pin_count - 1) selected++;
            break;
        case 'm': case 'M':
            {
                int manual_pin = 17;
                if (dialog_input_int("Manual GPIO", "Enter GPIO pin number",
                                     &manual_pin, 0, 40)) {
                    char used_by[32];
                    if (check_gpio_conflict(manual_pin, used_by, sizeof(used_by))) {
                        char msg[96];
                        snprintf(msg, sizeof(msg), "GPIO %d in use by '%s'", manual_pin, used_by);
                        dialog_warning(msg);
                    } else {
                        g_wiz.selected_gpio_pin = manual_pin;
                        push_state(WIZ_STATE_SENSOR_GPIO_TYPE);
                    }
                }
            }
            break;
        case '\n':
        case KEY_ENTER:
            if (pins[selected].in_use) {
                char msg[96];
                snprintf(msg, sizeof(msg), "GPIO %d in use by '%s'. Choose another.",
                         pins[selected].pin, pins[selected].used_by);
                dialog_warning(msg);
            } else {
                g_wiz.selected_gpio_pin = pins[selected].pin;
                push_state(WIZ_STATE_SENSOR_GPIO_TYPE);
            }
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 3B-Type: GPIO Sensor Type
 * ========================================================================== */

static void screen_sensor_gpio_type(void) {
    char title[64];
    snprintf(title, sizeof(title), "GPIO %d: Input Type", g_wiz.selected_gpio_pin);

    draw_header(title, "What's connected to this GPIO pin?");

    static int selected = 0;

    int row = 5;
    for (int i = 0; i < GPIO_SENSOR_TYPE_COUNT; i++) {
        draw_menu_item(row, selected, i,
                       gpio_sensor_types[i].name,
                       gpio_sensor_types[i].description,
                       false);
        row += 3;
    }

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            if (selected > 0) selected--;
            break;
        case KEY_DOWN:
            if (selected < GPIO_SENSOR_TYPE_COUNT - 1) selected++;
            break;
        case '\n':
        case KEY_ENTER:
            g_wiz.selected_sensor_type = selected;
            push_state(WIZ_STATE_NAME);
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 3C: ADC Scan
 * ========================================================================== */

static void screen_sensor_adc_scan(void) {
    draw_header("Scanning for ADC Devices...",
                "Looking for I2C ADC chips (ADS1115, etc.)");

    wrefresh(g_wiz.win);

    /* Scan for ADCs */
    memset(&g_wiz.discovery, 0, sizeof(g_wiz.discovery));
    hw_discover_i2c_all(&g_wiz.discovery);

    /* Build ADC channel list */
    g_wiz.device_count = 0;

    for (int i = 0; i < g_wiz.discovery.i2c_count; i++) {
        i2c_device_t *dev = &g_wiz.discovery.i2c_devices[i];

        if (dev->type == I2C_DEVICE_ADS1115 || dev->type == I2C_DEVICE_ADS1015) {
            for (int ch = 0; ch < 4 && g_wiz.device_count < MAX_MENU_ITEMS; ch++) {
                discovered_device_t *d = &g_wiz.devices[g_wiz.device_count];
                d->type = DISCOVERED_ADC_CHANNEL;
                snprintf(d->display_name, sizeof(d->display_name),
                         "Channel %d (A%d)", ch, ch);
                snprintf(d->description, sizeof(d->description),
                         "%s at 0x%02X", dev->name, dev->address);
                d->hw.adc_channel.bus = dev->bus;
                d->hw.adc_channel.adc_address = dev->address;
                d->hw.adc_channel.channel = ch;
                d->in_use = false;
                g_wiz.device_count++;
            }
        }
    }

    g_wiz.selected_device = 0;
    g_wiz.state = WIZ_STATE_SENSOR_ADC_PICK;
}

/* ============================================================================
 * Screen 3C: ADC Channel Pick
 * ========================================================================== */

static void screen_sensor_adc_pick(void) {
    if (g_wiz.device_count == 0) {
        draw_header("No ADC Devices Found",
                    "ADC chips convert analog voltages to digital readings.");

        wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(g_wiz.win, 6, 5, "No ADC devices found on I2C bus.");
        mvwprintw(g_wiz.win, 8, 5, "ADC chips like ADS1115 convert analog voltages");
        mvwprintw(g_wiz.win, 9, 5, "to digital readings for pH probes, pressure sensors.");
        mvwprintw(g_wiz.win, 11, 5, "Connect an ADC to I2C (SDA/SCL pins) and try again.");
        wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_WARNING));

        draw_nav_hints("[R] Rescan  |  [ESC] Back");
        wrefresh(g_wiz.win);

        int ch = wgetch(g_wiz.win);
        if (ch == 'r' || ch == 'R') {
            g_wiz.state = WIZ_STATE_SENSOR_ADC_SCAN;
        } else if (ch == 27) {
            pop_state();
        }
        return;
    }

    /* Found ADCs - show channel picker */
    char title[64];
    snprintf(title, sizeof(title), "Select ADC Channel");
    draw_header(title, "Choose which ADC channel your sensor is connected to:");

    /* Show ADC info */
    discovered_device_t *first = &g_wiz.devices[0];
    mvwprintw(g_wiz.win, 4, 5, "Found: %s at I2C address 0x%02X",
              first->description, first->hw.adc_channel.adc_address);

    int row = 7;
    for (int i = 0; i < g_wiz.device_count && i < 8; i++) {
        discovered_device_t *d = &g_wiz.devices[i];

        if (i == g_wiz.selected_device) wattron(g_wiz.win, A_REVERSE);

        if (d->in_use) {
            wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
            mvwprintw(g_wiz.win, row, 5, " %s  [in use: %s]",
                      d->display_name, d->used_by);
            wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
        } else {
            mvwprintw(g_wiz.win, row, 5, " %s  [available]       ", d->display_name);
        }

        if (i == g_wiz.selected_device) wattroff(g_wiz.win, A_REVERSE);
        row++;
    }

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            if (g_wiz.selected_device > 0) g_wiz.selected_device--;
            break;
        case KEY_DOWN:
            if (g_wiz.selected_device < g_wiz.device_count - 1) g_wiz.selected_device++;
            break;
        case '\n':
        case KEY_ENTER:
            {
                discovered_device_t *d = &g_wiz.devices[g_wiz.selected_device];
                if (d->in_use) {
                    dialog_warning("Channel already in use. Choose another.");
                } else {
                    g_wiz.selected_adc_bus = d->hw.adc_channel.bus;
                    g_wiz.selected_adc_address = d->hw.adc_channel.adc_address;
                    g_wiz.selected_adc_channel = d->hw.adc_channel.channel;
                    push_state(WIZ_STATE_SENSOR_ADC_TYPE);
                }
            }
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 3C-Type: ADC Sensor Type
 * ========================================================================== */

static void screen_sensor_adc_type(void) {
    char title[64];
    snprintf(title, sizeof(title), "ADS1115 Channel %d: Sensor Type",
             g_wiz.selected_adc_channel);

    draw_header(title, "What analog sensor is connected to this channel?");

    static int selected = 0;

    int row = 5;
    for (int i = 0; i < ADC_SENSOR_TYPE_COUNT; i++) {
        draw_menu_item(row, selected, i,
                       adc_sensor_types[i].name,
                       adc_sensor_types[i].description,
                       false);
        row += 3;
    }

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            if (selected > 0) selected--;
            break;
        case KEY_DOWN:
            if (selected < ADC_SENSOR_TYPE_COUNT - 1) selected++;
            break;
        case '\n':
        case KEY_ENTER:
            g_wiz.selected_sensor_type = selected;
            push_state(WIZ_STATE_NAME);
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 2B: Actuator Output Type
 * ========================================================================== */

static void screen_actuator_type(void) {
    draw_header("OUTPUT: What Are You Controlling?",
                "What kind of output is this?");

    static int selected = 0;

    int row = 6;

    /* Relay option */
    if (selected == 0) wattron(g_wiz.win, A_REVERSE);
    mvwprintw(g_wiz.win, row, 5, " Relay / Digital Output                           ");
    if (selected == 0) wattroff(g_wiz.win, A_REVERSE);
    wattron(g_wiz.win, A_DIM);
    mvwprintw(g_wiz.win, row + 1, 10, "Simple on/off control (pumps, valves, lights)");
    wattroff(g_wiz.win, A_DIM);
    row += 4;

    /* PWM option */
    if (selected == 1) wattron(g_wiz.win, A_REVERSE);
    mvwprintw(g_wiz.win, row, 5, " PWM / Variable Speed                             ");
    if (selected == 1) wattroff(g_wiz.win, A_REVERSE);
    wattron(g_wiz.win, A_DIM);
    mvwprintw(g_wiz.win, row + 1, 10, "Proportional control (VFD pumps, dimming)");
    wattroff(g_wiz.win, A_DIM);

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            selected = 0;
            break;
        case KEY_DOWN:
            selected = 1;
            break;
        case '\n':
        case KEY_ENTER:
            g_wiz.is_pwm = (selected == 1);
            push_state(WIZ_STATE_ACTUATOR_PIN);
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 3D: Actuator Pin Selection
 * ========================================================================== */

static void screen_actuator_pin(void) {
    draw_header("GPIO Output Pin Selection",
                "Which GPIO pin controls your relay/actuator?");

    /* Build pin list */
    struct {
        int pin;
        const char *label;
        bool is_pwm;
        bool in_use;
        char used_by[32];
    } pins[12];
    int pin_count = 0;

    /* Add relay pins */
    mvwprintw(g_wiz.win, 4, 5, "Board: %s", g_wiz.board.name);
    mvwprintw(g_wiz.win, 5, 5, "Suggested Relay Pins:");

    if (g_wiz.board.pins.gpio_relay_1 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_relay_1;
        pins[pin_count].label = "Relay 1";
        pins[pin_count].is_pwm = false;
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }
    if (g_wiz.board.pins.gpio_relay_2 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_relay_2;
        pins[pin_count].label = "Relay 2";
        pins[pin_count].is_pwm = false;
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }
    if (g_wiz.board.pins.gpio_relay_3 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_relay_3;
        pins[pin_count].label = "Relay 3";
        pins[pin_count].is_pwm = false;
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }
    if (g_wiz.board.pins.gpio_relay_4 >= 0) {
        pins[pin_count].pin = g_wiz.board.pins.gpio_relay_4;
        pins[pin_count].label = "Relay 4";
        pins[pin_count].is_pwm = false;
        pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                      pins[pin_count].used_by, 32);
        pin_count++;
    }

    /* Add PWM pins if user selected PWM */
    if (g_wiz.is_pwm) {
        if (g_wiz.board.pins.pwm_channel_0 >= 0) {
            pins[pin_count].pin = g_wiz.board.pins.pwm_channel_0;
            pins[pin_count].label = "PWM 0";
            pins[pin_count].is_pwm = true;
            pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                          pins[pin_count].used_by, 32);
            pin_count++;
        }
        if (g_wiz.board.pins.pwm_channel_1 >= 0) {
            pins[pin_count].pin = g_wiz.board.pins.pwm_channel_1;
            pins[pin_count].label = "PWM 1";
            pins[pin_count].is_pwm = true;
            pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                          pins[pin_count].used_by, 32);
            pin_count++;
        }
    }

    /* Fallback */
    if (pin_count == 0) {
        int fallback[] = {17, 27, 22, 23};
        for (int i = 0; i < 4; i++) {
            pins[pin_count].pin = fallback[i];
            pins[pin_count].label = "GPIO";
            pins[pin_count].is_pwm = false;
            pins[pin_count].in_use = check_gpio_conflict(pins[pin_count].pin,
                                                          pins[pin_count].used_by, 32);
            pin_count++;
        }
    }

    static int selected = 0;
    if (selected >= pin_count) selected = 0;

    int row = 7;
    for (int i = 0; i < pin_count && row < WIZARD_HEIGHT - 4; i++) {
        if (i == selected) wattron(g_wiz.win, A_REVERSE);

        if (pins[i].in_use) {
            wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
            mvwprintw(g_wiz.win, row, 5, " GPIO %2d - %-10s [in use: %s]",
                      pins[i].pin, pins[i].label, pins[i].used_by);
            wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_ERROR));
        } else {
            mvwprintw(g_wiz.win, row, 5, " GPIO %2d - %-10s [available]      ",
                      pins[i].pin, pins[i].label);
        }

        if (i == selected) wattroff(g_wiz.win, A_REVERSE);
        row++;
    }

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [M] Manual  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            if (selected > 0) selected--;
            break;
        case KEY_DOWN:
            if (selected < pin_count - 1) selected++;
            break;
        case 'm': case 'M':
            {
                int manual_pin = 17;
                if (dialog_input_int("Manual GPIO", "Enter GPIO pin number",
                                     &manual_pin, 0, 40)) {
                    char used_by[32];
                    if (check_gpio_conflict(manual_pin, used_by, sizeof(used_by))) {
                        char msg[96];
                        snprintf(msg, sizeof(msg), "GPIO %d in use by '%s'", manual_pin, used_by);
                        dialog_warning(msg);
                    } else {
                        g_wiz.selected_gpio_pin = manual_pin;
                        push_state(WIZ_STATE_ACTUATOR_DEVICE);
                    }
                }
            }
            break;
        case '\n':
        case KEY_ENTER:
            if (pins[selected].in_use) {
                char msg[96];
                snprintf(msg, sizeof(msg), "GPIO %d in use by '%s'",
                         pins[selected].pin, pins[selected].used_by);
                dialog_warning(msg);
            } else {
                g_wiz.selected_gpio_pin = pins[selected].pin;
                push_state(WIZ_STATE_ACTUATOR_DEVICE);
            }
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 3D-Type: Actuator Device Type
 * ========================================================================== */

static void screen_actuator_device(void) {
    char title[64];
    snprintf(title, sizeof(title), "GPIO %d: Output Type", g_wiz.selected_gpio_pin);

    draw_header(title, "What is this GPIO controlling?");

    static int selected = 0;

    int row = 6;
    for (int i = 0; i < ACTUATOR_TYPE_COUNT; i++) {
        draw_menu_item(row, selected, i,
                       actuator_types[i].name,
                       actuator_types[i].description,
                       false);
        row += 3;
    }

    draw_nav_hints("[Up/Down] Navigate  |  [Enter] Select  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_UP:
            if (selected > 0) selected--;
            break;
        case KEY_DOWN:
            if (selected < ACTUATOR_TYPE_COUNT - 1) selected++;
            break;
        case '\n':
        case KEY_ENTER:
            g_wiz.selected_actuator_type = selected;
            push_state(WIZ_STATE_NAME);
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 4: Name It
 * ========================================================================== */

static void screen_name(void) {
    const char *what = g_wiz.is_sensor ? "Sensor" : "Actuator";
    char title[64];
    snprintf(title, sizeof(title), "Name Your %s", what);

    draw_header(title, NULL);

    /* Show what we're adding */
    int row = 4;
    mvwprintw(g_wiz.win, row++, 5, "You're adding:");
    row++;

    wattron(g_wiz.win, A_BOLD);
    if (g_wiz.is_sensor) {
        /* Determine sensor description */
        if (g_wiz.selected_onewire_id[0]) {
            mvwprintw(g_wiz.win, row++, 7, "DS18B20 Temperature Sensor");
            wattroff(g_wiz.win, A_BOLD);
            mvwprintw(g_wiz.win, row++, 7, "1-Wire ID: %s", g_wiz.selected_onewire_id);
            if (g_wiz.onewire_current_value > -100) {
                mvwprintw(g_wiz.win, row++, 7, "Current reading: %.1f°C",
                          g_wiz.onewire_current_value);
            }
        } else if (g_wiz.selected_i2c_type != I2C_DEVICE_UNKNOWN) {
            mvwprintw(g_wiz.win, row++, 7, "%s",
                      i2c_device_type_name(g_wiz.selected_i2c_type));
            wattroff(g_wiz.win, A_BOLD);
            mvwprintw(g_wiz.win, row++, 7, "I2C bus %d, address 0x%02X",
                      g_wiz.selected_i2c_bus, g_wiz.selected_i2c_address);
        } else if (g_wiz.selected_adc_channel >= 0) {
            mvwprintw(g_wiz.win, row++, 7, "%s",
                      adc_sensor_types[g_wiz.selected_sensor_type].name);
            wattroff(g_wiz.win, A_BOLD);
            mvwprintw(g_wiz.win, row++, 7, "ADS1115 Channel %d (0x%02X)",
                      g_wiz.selected_adc_channel, g_wiz.selected_adc_address);
        } else {
            mvwprintw(g_wiz.win, row++, 7, "%s",
                      gpio_sensor_types[g_wiz.selected_sensor_type].name);
            wattroff(g_wiz.win, A_BOLD);
            mvwprintw(g_wiz.win, row++, 7, "GPIO %d", g_wiz.selected_gpio_pin);
        }
    } else {
        mvwprintw(g_wiz.win, row++, 7, "%s (%s)",
                  actuator_types[g_wiz.selected_actuator_type].name,
                  g_wiz.is_pwm ? "PWM" : "Relay");
        wattroff(g_wiz.win, A_BOLD);
        mvwprintw(g_wiz.win, row++, 7, "GPIO %d", g_wiz.selected_gpio_pin);
    }

    row += 2;
    mvwhline(g_wiz.win, row++, 3, ACS_HLINE, WIZARD_WIDTH - 6);
    row++;

    mvwprintw(g_wiz.win, row++, 5, "Name:");

    /* Input field */
    wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_INPUT));
    mvwprintw(g_wiz.win, row, 5, "%-40s", g_wiz.name);
    wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_INPUT));

    row += 2;
    wattron(g_wiz.win, A_DIM);
    mvwprintw(g_wiz.win, row++, 5, "Use lowercase with underscores. Examples:");
    if (g_wiz.is_sensor) {
        mvwprintw(g_wiz.win, row++, 7, "intake_pressure, clearwell_level, chlorine_flow");
    } else {
        mvwprintw(g_wiz.win, row++, 7, "intake_valve, backwash_pump, chlorine_solenoid");
    }
    wattroff(g_wiz.win, A_DIM);

    draw_nav_hints("[Enter] Edit name  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case '\n':
        case KEY_ENTER:
            {
                /* Edit name inline */
                char buffer[64];
                strncpy(buffer, g_wiz.name, sizeof(buffer) - 1);

                if (dialog_input_string("Enter Name",
                                        "lowercase_with_underscores",
                                        buffer, sizeof(buffer))) {
                    char error[128];
                    if (validate_name(buffer, error, sizeof(error))) {
                        strncpy(g_wiz.name, buffer, sizeof(g_wiz.name) - 1);
                        push_state(WIZ_STATE_CONFIRM);
                    } else {
                        dialog_error(error);
                    }
                }
            }
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Screen 5: Confirm
 * ========================================================================== */

static void screen_confirm(void) {
    const char *what = g_wiz.is_sensor ? "Sensor" : "Actuator";
    char title[64];
    snprintf(title, sizeof(title), "Confirm New %s", what);

    draw_header(title, "Ready to add this configuration:");

    /* Auto-assign slot */
    g_wiz.assigned_slot = find_next_slot(g_wiz.is_sensor);

    int row = 5;

    /* Summary */
    mvwprintw(g_wiz.win, row++, 5, "Name:       %s", g_wiz.name);
    row++;

    if (g_wiz.is_sensor) {
        if (g_wiz.selected_onewire_id[0]) {
            mvwprintw(g_wiz.win, row++, 5, "Type:       DS18B20 Temperature");
            mvwprintw(g_wiz.win, row++, 5, "Connection: 1-Wire (%s)", g_wiz.selected_onewire_id);
            mvwprintw(g_wiz.win, row++, 5, "Unit:       °C");
        } else if (g_wiz.selected_i2c_type != I2C_DEVICE_UNKNOWN) {
            mvwprintw(g_wiz.win, row++, 5, "Type:       %s",
                      i2c_device_type_name(g_wiz.selected_i2c_type));
            mvwprintw(g_wiz.win, row++, 5, "Connection: I2C (0x%02X)", g_wiz.selected_i2c_address);
        } else if (g_wiz.selected_adc_channel >= 0) {
            const sensor_type_def_t *st = &adc_sensor_types[g_wiz.selected_sensor_type];
            mvwprintw(g_wiz.win, row++, 5, "Type:       %s", st->name);
            mvwprintw(g_wiz.win, row++, 5, "Connection: ADS1115 Ch%d (0x%02X)",
                      g_wiz.selected_adc_channel, g_wiz.selected_adc_address);
            mvwprintw(g_wiz.win, row++, 5, "Unit:       %s", st->unit);
        } else {
            const sensor_type_def_t *st = &gpio_sensor_types[g_wiz.selected_sensor_type];
            mvwprintw(g_wiz.win, row++, 5, "Type:       %s", st->name);
            mvwprintw(g_wiz.win, row++, 5, "Connection: GPIO %d", g_wiz.selected_gpio_pin);
            mvwprintw(g_wiz.win, row++, 5, "Unit:       %s", st->unit);
        }
        mvwprintw(g_wiz.win, row++, 5, "Poll Rate:  1000 ms");
    } else {
        mvwprintw(g_wiz.win, row++, 5, "Type:       %s (%s)",
                  actuator_types[g_wiz.selected_actuator_type].name,
                  g_wiz.is_pwm ? "PWM" : "Relay");
        mvwprintw(g_wiz.win, row++, 5, "GPIO Pin:   %d", g_wiz.selected_gpio_pin);
        mvwprintw(g_wiz.win, row++, 5, "Safe State: OFF (de-energize on fault)");
    }

    mvwprintw(g_wiz.win, row++, 5, "PROFINET:   Slot %d, Subslot 1", g_wiz.assigned_slot);

    row++;

    /* Current reading for verification */
    if (g_wiz.is_sensor && g_wiz.onewire_current_value > -100) {
        wattron(g_wiz.win, COLOR_PAIR(TUI_COLOR_STATUS));
        mvwprintw(g_wiz.win, row++, 5, "Current reading: %.1f°C ✓",
                  g_wiz.onewire_current_value);
        wattroff(g_wiz.win, COLOR_PAIR(TUI_COLOR_STATUS));
    }

    row++;
    mvwhline(g_wiz.win, row++, 3, ACS_HLINE, WIZARD_WIDTH - 6);

    /* Buttons */
    static int button = 0;
    const char *buttons[] = {"Save", "Edit Advanced", "Cancel"};

    row++;
    for (int i = 0; i < 3; i++) {
        int col = 8 + i * 20;
        if (i == button) wattron(g_wiz.win, A_REVERSE);
        mvwprintw(g_wiz.win, row, col, "[ %s ]", buttons[i]);
        if (i == button) wattroff(g_wiz.win, A_REVERSE);
    }

    draw_nav_hints("[Left/Right] Select  |  [Enter] Confirm  |  [ESC] Back");
    wrefresh(g_wiz.win);

    int ch = wgetch(g_wiz.win);
    switch (ch) {
        case KEY_LEFT:
            if (button > 0) button--;
            break;
        case KEY_RIGHT:
            if (button < 2) button++;
            break;
        case '\n':
        case KEY_ENTER:
            if (button == 0) {
                /* Save - implement database write */
                g_wiz.state = WIZ_STATE_DONE;
            } else if (button == 1) {
                /* Edit Advanced - show old form for power users */
                dialog_message("Advanced", "Use the old form for advanced settings.");
                g_wiz.state = WIZ_STATE_CANCELLED;  /* Exit and let user use old dialog */
            } else {
                g_wiz.state = WIZ_STATE_CANCELLED;
            }
            break;
        case 27:
            pop_state();
            break;
    }
}

/* ============================================================================
 * Save Functions
 * ========================================================================== */

static bool save_sensor(void) {
    database_t *db = tui_get_database();
    if (!db) return false;

    /* Create module record */
    db_module_t module = {0};
    module.slot = g_wiz.assigned_slot;
    module.subslot = 1;
    SAFE_STRNCPY(module.name, g_wiz.name, sizeof(module.name));
    module.module_ident = 0x00000001;
    module.submodule_ident = 0x00000001;
    strcpy(module.status, "inactive");

    int sensor_id = 0;

    /* Determine sensor type and create appropriate record */
    if (g_wiz.selected_onewire_id[0]) {
        /* DS18B20 Temperature Sensor */
        strcpy(module.module_type, "physical");

        result_t r = db_module_create(db, &module, &sensor_id);
        if (r != RESULT_OK) return false;

        db_physical_sensor_t phys = {0};
        phys.module_id = sensor_id;
        strcpy(phys.sensor_type, "DS18B20");
        strcpy(phys.hardware_type, "DS18B20");
        strcpy(phys.interface, "1wire");
        SAFE_STRNCPY(phys.address, g_wiz.selected_onewire_id, sizeof(phys.address));
        phys.bus = 0;
        phys.channel = 0;
        strcpy(phys.unit, "°C");
        phys.min_value = -55.0f;
        phys.max_value = 125.0f;
        phys.poll_rate_ms = 1000;

        db_physical_sensor_create(db, &phys);

    } else if (g_wiz.selected_i2c_type != I2C_DEVICE_UNKNOWN) {
        /* I2C Sensor (BME280, etc.) */
        strcpy(module.module_type, "physical");

        result_t r = db_module_create(db, &module, &sensor_id);
        if (r != RESULT_OK) return false;

        db_physical_sensor_t phys = {0};
        phys.module_id = sensor_id;
        SAFE_STRNCPY(phys.sensor_type, i2c_device_type_name(g_wiz.selected_i2c_type),
                     sizeof(phys.sensor_type));
        SAFE_STRNCPY(phys.hardware_type, i2c_device_type_name(g_wiz.selected_i2c_type),
                     sizeof(phys.hardware_type));
        strcpy(phys.interface, "i2c");
        snprintf(phys.address, sizeof(phys.address), "0x%02X", g_wiz.selected_i2c_address);
        phys.bus = g_wiz.selected_i2c_bus;
        phys.poll_rate_ms = 1000;

        /* Set defaults based on sensor type */
        if (g_wiz.selected_i2c_type == I2C_DEVICE_BME280 ||
            g_wiz.selected_i2c_type == I2C_DEVICE_BMP280) {
            strcpy(phys.unit, "°C");
            phys.min_value = -40.0f;
            phys.max_value = 85.0f;
        }

        db_physical_sensor_create(db, &phys);

    } else if (g_wiz.selected_adc_channel >= 0) {
        /* ADC-based sensor */
        strcpy(module.module_type, "adc");

        result_t r = db_module_create(db, &module, &sensor_id);
        if (r != RESULT_OK) return false;

        const sensor_type_def_t *st = &adc_sensor_types[g_wiz.selected_sensor_type];

        db_adc_sensor_t adc = {0};
        adc.module_id = sensor_id;
        strcpy(adc.adc_type, "ADS1115");
        strcpy(adc.interface, "i2c");
        snprintf(adc.address, sizeof(adc.address), "0x%02X", g_wiz.selected_adc_address);
        adc.bus = g_wiz.selected_adc_bus;
        adc.channel = g_wiz.selected_adc_channel;
        adc.gain = 1;  /* Default gain */
        adc.reference_voltage = 4.096f;
        SAFE_STRNCPY(adc.unit, st->unit, sizeof(adc.unit));
        adc.eng_min = st->range_min;
        adc.eng_max = st->range_max;
        adc.poll_rate_ms = 1000;

        db_adc_sensor_create(db, &adc);

    } else {
        /* GPIO-based sensor */
        strcpy(module.module_type, "physical");

        result_t r = db_module_create(db, &module, &sensor_id);
        if (r != RESULT_OK) return false;

        const sensor_type_def_t *st = &gpio_sensor_types[g_wiz.selected_sensor_type];

        db_physical_sensor_t phys = {0};
        phys.module_id = sensor_id;
        SAFE_STRNCPY(phys.sensor_type, st->name, sizeof(phys.sensor_type));
        SAFE_STRNCPY(phys.hardware_type, st->name, sizeof(phys.hardware_type));
        strcpy(phys.interface, "gpio");
        snprintf(phys.address, sizeof(phys.address), "%d", g_wiz.selected_gpio_pin);
        phys.bus = 0;
        phys.channel = 0;
        SAFE_STRNCPY(phys.unit, st->unit, sizeof(phys.unit));
        phys.min_value = st->range_min;
        phys.max_value = st->range_max;
        phys.poll_rate_ms = 1000;

        db_physical_sensor_create(db, &phys);
    }

    g_wiz.result.type = IO_WIZARD_RESULT_SENSOR;
    g_wiz.result.created_id = sensor_id;
    g_wiz.result.assigned_slot = g_wiz.assigned_slot;
    SAFE_STRNCPY(g_wiz.result.name, g_wiz.name, sizeof(g_wiz.result.name));

    LOG_INFO("IO Wizard: Created sensor '%s' at slot %d (ID: %d)",
             g_wiz.name, g_wiz.assigned_slot, sensor_id);

    return true;
}

static bool save_actuator(void) {
    database_t *db = tui_get_database();
    if (!db) return false;

    db_actuator_t act = {0};
    SAFE_STRNCPY(act.name, g_wiz.name, sizeof(act.name));
    act.slot = g_wiz.assigned_slot;
    act.subslot = 1;
    act.gpio_pin = g_wiz.selected_gpio_pin;
    SAFE_STRNCPY(act.gpio_chip, g_wiz.board.pins.gpio_chip, sizeof(act.gpio_chip));
    act.active_low = false;
    act.safe_state = SAFE_STATE_OFF;
    act.enabled = true;

    /* Set type based on selection */
    if (g_wiz.selected_actuator_type == 0) {
        act.type = ACTUATOR_TYPE_PUMP;
    } else if (g_wiz.selected_actuator_type == 1) {
        act.type = ACTUATOR_TYPE_VALVE;
    } else {
        act.type = ACTUATOR_TYPE_RELAY;
    }

    if (g_wiz.is_pwm) {
        act.type = ACTUATOR_TYPE_PWM;
        act.pwm_frequency_hz = 1000;
    }

    int actuator_id = 0;
    result_t r = db_actuator_create(db, &act, &actuator_id);
    if (r != RESULT_OK) return false;

    g_wiz.result.type = IO_WIZARD_RESULT_ACTUATOR;
    g_wiz.result.created_id = actuator_id;
    g_wiz.result.assigned_slot = g_wiz.assigned_slot;
    SAFE_STRNCPY(g_wiz.result.name, g_wiz.name, sizeof(g_wiz.result.name));

    LOG_INFO("IO Wizard: Created actuator '%s' at slot %d (ID: %d)",
             g_wiz.name, g_wiz.assigned_slot, actuator_id);

    return true;
}

/* ============================================================================
 * Main Wizard Loop
 * ========================================================================== */

static void wizard_init(void) {
    memset(&g_wiz, 0, sizeof(g_wiz));
    g_wiz.state = WIZ_STATE_IO_TYPE;
    g_wiz.selected_adc_channel = -1;
    g_wiz.selected_i2c_type = I2C_DEVICE_UNKNOWN;

    /* Detect board for pin mappings */
    board_detect(&g_wiz.board);

    /* Create centered window */
    int start_y = (LINES - WIZARD_HEIGHT) / 2;
    int start_x = (COLS - WIZARD_WIDTH) / 2;
    g_wiz.win = newwin(WIZARD_HEIGHT, WIZARD_WIDTH, start_y, start_x);
    keypad(g_wiz.win, TRUE);
}

static void wizard_cleanup(void) {
    if (g_wiz.win) {
        werase(g_wiz.win);
        wrefresh(g_wiz.win);
        delwin(g_wiz.win);
        g_wiz.win = NULL;
    }
}

bool dialog_io_wizard_run(io_wizard_result_t *result) {
    wizard_init();

    while (g_wiz.state != WIZ_STATE_DONE && g_wiz.state != WIZ_STATE_CANCELLED) {
        switch (g_wiz.state) {
            case WIZ_STATE_IO_TYPE:
                screen_io_type();
                break;
            case WIZ_STATE_SENSOR_CONNECTION:
                screen_sensor_connection();
                break;
            case WIZ_STATE_SENSOR_SCAN:
                screen_sensor_scan();
                break;
            case WIZ_STATE_SENSOR_SCAN_PICK:
                screen_sensor_scan_pick();
                break;
            case WIZ_STATE_SENSOR_GPIO_PIN:
                screen_sensor_gpio_pin();
                break;
            case WIZ_STATE_SENSOR_GPIO_TYPE:
                screen_sensor_gpio_type();
                break;
            case WIZ_STATE_SENSOR_ADC_SCAN:
                screen_sensor_adc_scan();
                break;
            case WIZ_STATE_SENSOR_ADC_PICK:
                screen_sensor_adc_pick();
                break;
            case WIZ_STATE_SENSOR_ADC_TYPE:
                screen_sensor_adc_type();
                break;
            case WIZ_STATE_ACTUATOR_TYPE:
                screen_actuator_type();
                break;
            case WIZ_STATE_ACTUATOR_PIN:
                screen_actuator_pin();
                break;
            case WIZ_STATE_ACTUATOR_DEVICE:
                screen_actuator_device();
                break;
            case WIZ_STATE_NAME:
                screen_name();
                break;
            case WIZ_STATE_CONFIRM:
                screen_confirm();
                break;
            default:
                g_wiz.state = WIZ_STATE_CANCELLED;
                break;
        }
    }

    bool success = false;

    if (g_wiz.state == WIZ_STATE_DONE) {
        if (g_wiz.is_sensor) {
            success = save_sensor();
        } else {
            success = save_actuator();
        }
    }

    if (result) {
        *result = g_wiz.result;
    }

    wizard_cleanup();
    return success;
}

bool dialog_io_wizard_add_sensor(io_wizard_result_t *result) {
    wizard_init();

    /* Skip to sensor path */
    g_wiz.is_sensor = true;
    g_wiz.state = WIZ_STATE_SENSOR_CONNECTION;

    while (g_wiz.state != WIZ_STATE_DONE && g_wiz.state != WIZ_STATE_CANCELLED) {
        switch (g_wiz.state) {
            case WIZ_STATE_SENSOR_CONNECTION:
                screen_sensor_connection();
                break;
            case WIZ_STATE_SENSOR_SCAN:
                screen_sensor_scan();
                break;
            case WIZ_STATE_SENSOR_SCAN_PICK:
                screen_sensor_scan_pick();
                break;
            case WIZ_STATE_SENSOR_GPIO_PIN:
                screen_sensor_gpio_pin();
                break;
            case WIZ_STATE_SENSOR_GPIO_TYPE:
                screen_sensor_gpio_type();
                break;
            case WIZ_STATE_SENSOR_ADC_SCAN:
                screen_sensor_adc_scan();
                break;
            case WIZ_STATE_SENSOR_ADC_PICK:
                screen_sensor_adc_pick();
                break;
            case WIZ_STATE_SENSOR_ADC_TYPE:
                screen_sensor_adc_type();
                break;
            case WIZ_STATE_NAME:
                screen_name();
                break;
            case WIZ_STATE_CONFIRM:
                screen_confirm();
                break;
            default:
                g_wiz.state = WIZ_STATE_CANCELLED;
                break;
        }
    }

    bool success = false;
    if (g_wiz.state == WIZ_STATE_DONE) {
        success = save_sensor();
    }

    if (result) {
        *result = g_wiz.result;
    }

    wizard_cleanup();
    return success;
}

bool dialog_io_wizard_add_actuator(io_wizard_result_t *result) {
    wizard_init();

    /* Skip to actuator path */
    g_wiz.is_sensor = false;
    g_wiz.state = WIZ_STATE_ACTUATOR_TYPE;

    while (g_wiz.state != WIZ_STATE_DONE && g_wiz.state != WIZ_STATE_CANCELLED) {
        switch (g_wiz.state) {
            case WIZ_STATE_ACTUATOR_TYPE:
                screen_actuator_type();
                break;
            case WIZ_STATE_ACTUATOR_PIN:
                screen_actuator_pin();
                break;
            case WIZ_STATE_ACTUATOR_DEVICE:
                screen_actuator_device();
                break;
            case WIZ_STATE_NAME:
                screen_name();
                break;
            case WIZ_STATE_CONFIRM:
                screen_confirm();
                break;
            default:
                g_wiz.state = WIZ_STATE_CANCELLED;
                break;
        }
    }

    bool success = false;
    if (g_wiz.state == WIZ_STATE_DONE) {
        success = save_actuator();
    }

    if (result) {
        *result = g_wiz.result;
    }

    wizard_cleanup();
    return success;
}
