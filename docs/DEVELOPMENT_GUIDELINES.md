# Water-Treat RTU Development, Validation, and Deployment Guidelines

## Preamble: Purpose and Scope

This document establishes enforceable standards for producing, validating, documenting, and deploying the Water-Treat SCADA RTU system. These are not suggestions. They are constraints that produce deterministic, auditable, production-ready firmware for critical infrastructure field devices.

**Target System:** Water-Treat (SBC #2 / RTU)
- PROFINET I/O Device (using p-net stack)
- ncurses TUI for local configuration and diagnostics
- C language implementation
- SQLite configuration persistence
- Multi-protocol sensor abstraction (I2C, SPI, 1-Wire, GPIO, Analog via ADC)
- Actuator control (Relay, PWM, Solenoid, Pump)
- Local safety interlocks and offline autonomy

**Core Thesis:** An RTU that loses communication with its controller must continue operating safely. Every sensor failure must degrade gracefully with quality indicators. Every actuator command must have a watchdog timeout. The operator must never wonder if the system is working.

---

## Part 1: System Context and Architecture

### 1.1 Two-Plane Architecture Position

Water-Treat operates as **SBC #2** in a two-tier SCADA architecture:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         SCADA Network                                    │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                    ┌────────────┴────────────┐
                    │      SBC #1             │
                    │   Water-Controller      │
                    │  - PROFINET Controller  │
                    │  - HMI (React/FastAPI)  │
                    │  - Historian            │
                    │  - Alarm Management     │
                    │  - Modbus Gateway       │
                    └────────────┬────────────┘
                                 │ PROFINET RT
                                 │ (EtherType 0x8892)
                    ┌────────────┴────────────┐
                    │      SBC #2             │
                    │   Water-Treat (RTU)     │
                    │  - PROFINET I/O Device  │
                    │  - Sensor Drivers       │
                    │  - Actuator Control     │
                    │  - Local Interlocks     │
                    │  - Offline Autonomy     │
                    └────────────┬────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
         ┌────┴────┐       ┌─────┴─────┐      ┌────┴────┐
         │ Sensors │       │  Pumps    │      │ Valves  │
         │ pH/TDS  │       │  Motors   │      │ Relays  │
         │ Turbid  │       │  Dosing   │      │ Solenoid│
         └─────────┘       └───────────┘      └─────────┘
```

### 1.2 PROFINET I/O Data Exchange

**Critical Understanding:** The RTU is a PROFINET I/O Device. The Controller is the PROFINET IO Controller. Data flows cyclically.

**Cyclic Data Exchange (Every 10-1000ms configurable):**

```
CONTROLLER → RTU (Output Data):
┌──────────────────────────────────────────────┐
│ Byte 0:    Actuator Control Bits             │
│            Bit 0: Pump 1 (1=ON, 0=OFF)       │
│            Bit 1: Pump 2 (1=ON, 0=OFF)       │
│            Bit 2: Dosing Pump 1              │
│            Bit 3: Dosing Pump 2              │
│            Bit 4: Solenoid Valve 1           │
│            Bit 5: Solenoid Valve 2           │
│            Bit 6-7: Reserved                 │
│                                              │
│ Byte 1:    PWM Duty Cycle (0-255)            │
│ Byte 2:    Mode (0=Manual, 1=Auto, 2=Stop)   │
│ Byte 3:    Reserved                          │
└──────────────────────────────────────────────┘

RTU → CONTROLLER (Input Data):
┌──────────────────────────────────────────────┐
│ Byte 0-3:   Sensor 1 Value (Float32, BE)     │
│ Byte 4:     Sensor 1 Quality (OPC UA)        │
│ Byte 5-8:   Sensor 2 Value (Float32, BE)     │
│ Byte 9:     Sensor 2 Quality (OPC UA)        │
│ ...                                          │
│ Byte N:     Status Bits                      │
│             Bit 0: Pump 1 Running            │
│             Bit 1: Pump 2 Running            │
│             Bit 2: E-STOP Active             │
│             Bit 3: Communication Fault       │
│             Bit 4-7: Alarm Flags             │
└──────────────────────────────────────────────┘
```

**Byte Order:** PROFINET uses big-endian (network byte order). Use `htons()`, `htonl()`, `ntohs()`, `ntohl()` for all multi-byte values.

### 1.3 Slot/Module/Submodule Architecture

PROFINET organizes data hierarchically:

```
Device (Water-Treat RTU)
  └─ API 0 (Application Process Identifier)
      ├─ Slot 0: DAP (Device Access Point) - MANDATORY
      │   └─ Subslot 0x0001: Device Identity
      │
      ├─ Slot 1: Sensor Module (pH)
      │   └─ Subslot 0x0001: pH Value + Quality (5 bytes input)
      │
      ├─ Slot 2: Sensor Module (TDS)
      │   └─ Subslot 0x0001: TDS Value + Quality (5 bytes input)
      │
      ├─ Slot 3: Sensor Module (Turbidity)
      │   └─ Subslot 0x0001: Turbidity Value + Quality (5 bytes input)
      │
      ├─ Slot 4: Sensor Module (Flow)
      │   └─ Subslot 0x0001: Flow Value + Quality (5 bytes input)
      │
      ├─ Slot 5: Sensor Module (Level)
      │   └─ Subslot 0x0001: Level Value + Quality (5 bytes input)
      │
      ├─ Slot 6: Sensor Module (Temperature)
      │   └─ Subslot 0x0001: Temp Value + Quality (5 bytes input)
      │
      ├─ Slot 7: Sensor Module (Color/Clarity)
      │   └─ Subslot 0x0001: Color Value + Quality (5 bytes input)
      │
      ├─ Slot 8: Digital Input Module
      │   └─ Subslot 0x0001: Float Switches, Status (2 bytes input)
      │
      └─ Slot 9-15: Actuator Modules
          └─ Subslot 0x0001: Command + Feedback (4 bytes each direction)
```

**Dynamic Configuration:** The slot layout is defined in the GSDML file and must match what the Controller expects. The TUI allows runtime configuration, which must be persisted to SQLite and regenerated into GSDML.

---

## Part 2: Production Standards

### 2.1 Console Discipline (The Console is Sacred)

**Non-Negotiable:** When the TUI is active, NOTHING writes directly to stdout/stderr.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                       CONSOLE OUTPUT ROUTING                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Error/Log Event Occurs                                                  │
│         │                                                                │
│         ▼                                                                │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                    Logger Subsystem                              │    │
│  │   log_write(level, module, message)                             │    │
│  └───────────────────────────┬─────────────────────────────────────┘    │
│                              │                                           │
│         ┌────────────────────┼────────────────────┐                     │
│         │                    │                    │                     │
│         ▼                    ▼                    ▼                     │
│  ┌─────────────┐      ┌─────────────┐      ┌─────────────┐             │
│  │ Ring Buffer │      │ TUI Active? │      │  Severity   │             │
│  │ (Always)    │      │             │      │  Filter     │             │
│  └──────┬──────┘      └──────┬──────┘      └──────┬──────┘             │
│         │                    │                    │                     │
│         │              ┌─────┴─────┐              │                     │
│         │              │           │              │                     │
│         │         YES  ▼      NO   ▼              │                     │
│         │    ┌─────────────┐ ┌─────────────┐      │                     │
│         │    │ TUI Message │ │   stderr    │      │                     │
│         │    │ Area ONLY   │ │  (OK here)  │      │                     │
│         │    └─────────────┘ └─────────────┘      │                     │
│         │                                         │                     │
│         ▼ (Batched, periodic flush)              │                     │
│  ┌─────────────┐                                  │                     │
│  │ Persistent  │◄─────────────────────────────────┘                     │
│  │ Storage     │  (If severity >= threshold)                           │
│  └─────────────┘                                                        │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

**Logger Implementation Requirements:**

```c
// PROHIBITED - Direct console output
printf("Sensor initialized\n");
fprintf(stderr, "Error: %s\n", msg);
perror("I2C failed");

// REQUIRED - Routed through logger
LOG_INFO("sensor", "Sensor initialized");
LOG_ERROR("i2c", "I2C operation failed: %s", strerror(errno));

// Logger must check TUI state
void log_write(log_level_t level, const char *module, const char *fmt, ...) {
    // 1. Always write to ring buffer
    ring_buffer_push(&g_log_ring, level, module, formatted_msg);

    // 2. Route to appropriate output
    if (tui_is_active()) {
        tui_message_area_append(level, formatted_msg);
    } else {
        fprintf(stderr, "[%s] [%s] %s\n", level_str, module, formatted_msg);
    }

    // 3. Trigger persistent write if needed
    if (level >= g_log_config.persist_threshold) {
        log_queue_persist(formatted_msg);
    }
}
```

### 2.2 TUI Navigation Specification

**Reference:** IO_CONFIGURATION_UI_SPEC.md

**Navigation Model:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         TUI NAVIGATION                                   │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  THREE-LEVEL HIERARCHY:                                                  │
│                                                                          │
│  LEVEL 0: Screen/Tab Selection (horizontal)                             │
│      ←/→ or Tab/Shift+Tab: Cycle through screens                       │
│      F1-F8: Jump directly to screen (power user shortcut)              │
│                                                                          │
│  LEVEL 1: List/Item Selection (vertical)                                │
│      ↑/↓: Move selection cursor                                         │
│      Enter or →: Drill into selected item (edit/detail)                │
│      a: Add new item                                                     │
│      d: Delete selected item                                             │
│      e: Edit selected item                                               │
│      r: Refresh list                                                     │
│                                                                          │
│  LEVEL 2: Edit/Dialog Mode (modal)                                      │
│      Tab/Shift+Tab: Cycle through fields                                │
│      Enter: Save and return to Level 1                                  │
│      ESC: Cancel and return to Level 1                                  │
│                                                                          │
│  ESC KEY BEHAVIOR (NON-NEGOTIABLE):                                     │
│      Level 2 (dialog open): Cancel dialog, discard changes             │
│      Level 1 (list view): Return to previous screen                    │
│      Level 0 (top level): Show quit confirmation                       │
│      ESC must NEVER be ignored or do nothing                           │
│                                                                          │
│  GLOBAL KEYS (Available at ALL levels):                                 │
│      E: Emergency Stop (immediate, shows confirmation)                  │
│      F10 or q: Quit application (with confirmation)                    │
│      ?: Show help overlay                                               │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

**Screen Layout:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Water-Treat RTU v1.2.3  │  station-name-01               HH:MM:SS      │
│ ► Sensors   (3/12)                                                      │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│ [System][Sensors][Network][PROFINET][Status][Alarms][Logging][Actuators]│
│                                                                         │
│  Slot  Name              Type        Value      Quality   Alarm        │
│ ──────────────────────────────────────────────────────────────────────  │
│   1    inlet_ph          pH          7.21       GOOD      -            │
│ ► 2    outlet_tds        TDS         342 ppm    GOOD      -            │
│   3    turbidity_1       Turbidity   2.3 NTU    UNCERTAIN LOW          │
│   4    flow_main         Flow        12.4 L/m   GOOD      -            │
│                                                                         │
├─────────────────────────────────────────────────────────────────────────┤
│ 14:32:07 Sensor 'turbidity_1' reading uncertain (range check)          │
├─────────────────────────────────────────────────────────────────────────┤
│ ←→:Screen ↑↓:Select Enter:Edit  a:Add d:Delete E:E-STOP  F10:Quit      │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.3 Sensor Abstraction Layer

**Supported Sensor Interfaces:**

| Interface | Driver | Sensors |
|-----------|--------|---------|
| I2C | ADS1115 | 16-bit ADC for pH, TDS, Turbidity (analog) |
| I2C | TCS34725 | RGB Color Sensor |
| I2C | BME280 | Temperature, Humidity, Pressure |
| SPI | MCP3008 | 10-bit ADC alternative |
| 1-Wire | DS18B20 | Waterproof Temperature |
| GPIO | DHT22 | Temperature/Humidity |
| GPIO | JSN-SR04T | Ultrasonic Level |
| GPIO | HX711 | Load Cell Amplifier |
| GPIO | Float Switch | Binary Level |
| GPIO | Hall Effect | Flow Pulse Counter |
| HTTP | Web Poll | External API Data Source |
| Calculated | Formula | Derived from other sensors |

**Sensor Driver Interface:**

```c
// Every sensor driver implements this interface
typedef struct {
    result_t (*init)(void **handle, const sensor_config_t *config);
    result_t (*read)(void *handle, sensor_reading_t *reading);
    result_t (*calibrate)(void *handle, const calibration_t *cal);
    result_t (*get_status)(void *handle, sensor_status_t *status);
    void     (*destroy)(void *handle);
} sensor_driver_vtable_t;

typedef struct {
    float value;                    // Engineering units
    data_quality_t quality;         // GOOD, UNCERTAIN, BAD, NOT_CONNECTED
    uint64_t timestamp_us;          // Microseconds since epoch
    uint32_t raw_value;             // Raw ADC/register value
    uint8_t consecutive_failures;   // For degradation detection
} sensor_reading_t;
```

**Analog Sensor Calibration (Two-Point for pH):**

```c
// pH Nernst equation with two-point calibration
typedef struct {
    float cal_low_ph;       // e.g., 4.00
    float cal_low_voltage;  // Voltage at pH 4.00
    float cal_high_ph;      // e.g., 7.00
    float cal_high_voltage; // Voltage at pH 7.00
    float temp_compensation; // Temperature coefficient
} ph_calibration_t;

float ph_calculate(float voltage, const ph_calibration_t *cal, float temp_c) {
    float slope = (cal->cal_high_ph - cal->cal_low_ph) /
                  (cal->cal_high_voltage - cal->cal_low_voltage);
    float offset = cal->cal_low_ph - (slope * cal->cal_low_voltage);
    float ph_raw = (slope * voltage) + offset;

    // Temperature compensation (Nernst: 59.16mV/pH at 25°C)
    float temp_factor = (temp_c + 273.15) / 298.15;
    return ph_raw * temp_factor;
}
```

### 2.4 Data Quality Propagation

**Non-Negotiable:** Every sensor reading carries quality metadata. Stale data is never presented as current.

```c
typedef enum {
    QUALITY_GOOD          = 0x00,  // Fresh, valid reading
    QUALITY_UNCERTAIN     = 0x40,  // May be stale or sensor degraded
    QUALITY_BAD           = 0x80,  // Sensor failure, invalid reading
    QUALITY_NOT_CONNECTED = 0xC0,  // No communication with sensor
} data_quality_t;

// Quality determination logic
data_quality_t determine_quality(const sensor_reading_t *reading,
                                  const sensor_config_t *config) {
    // Check staleness
    uint64_t age_ms = (now_us() - reading->timestamp_us) / 1000;
    if (age_ms > config->stale_timeout_ms) {
        return QUALITY_UNCERTAIN;
    }

    // Check consecutive failures
    if (reading->consecutive_failures >= config->failure_threshold) {
        return QUALITY_BAD;
    }

    // Check range
    if (reading->value < config->range_min ||
        reading->value > config->range_max) {
        return QUALITY_UNCERTAIN;
    }

    return QUALITY_GOOD;
}
```

**TUI Quality Indication:**

| Quality | Display | Color | Symbol |
|---------|---------|-------|--------|
| GOOD | Normal value | Green/White | (none) |
| UNCERTAIN | Value with ? | Yellow | ? |
| BAD | "FAULT" | Red | X |
| NOT_CONNECTED | "---" | Grey | - |

### 2.5 Actuator Control and Safety

**Actuator Types:**

| Type | Control | Feedback | Safety |
|------|---------|----------|--------|
| Relay | GPIO ON/OFF | GPIO readback | Watchdog timeout |
| PWM | Duty cycle 0-100% | Current sense | Stall detection |
| Solenoid | GPIO pulse | Position switch | Max cycle rate |
| Pump | Relay + PWM | Flow sensor | Dry-run protection |

**Watchdog Timeout (Non-Negotiable):**

Every actuator command includes an implicit watchdog. If no new command is received within the timeout period, the actuator returns to safe state.

```c
typedef struct {
    actuator_state_t commanded_state;
    actuator_state_t safe_state;      // State on timeout or E-STOP
    uint32_t watchdog_timeout_ms;     // Max time without refresh
    uint64_t last_command_time_us;    // Timestamp of last valid command
    bool watchdog_active;             // True if currently under command
} actuator_control_t;

void actuator_watchdog_check(actuator_control_t *act) {
    if (!act->watchdog_active) return;

    uint64_t age_ms = (now_us() - act->last_command_time_us) / 1000;
    if (age_ms > act->watchdog_timeout_ms) {
        LOG_WARNING("actuator", "Watchdog timeout, reverting to safe state");
        actuator_set_state(act, act->safe_state);
        act->watchdog_active = false;
        // Raise PROFINET diagnostic alarm
        profinet_alarm_send(ALARM_WATCHDOG_TIMEOUT, act->slot);
    }
}
```

**Local Safety Interlocks:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    LOCAL INTERLOCK LOGIC (RTU-SIDE)                      │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  These interlocks are NEVER bypassed by Controller commands:            │
│                                                                          │
│  1. EMERGENCY STOP                                                       │
│     - Physical E-STOP button OR software E-STOP command                 │
│     - All actuators → safe state immediately                            │
│     - Requires manual reset                                              │
│                                                                          │
│  2. LOW LEVEL INTERLOCK                                                  │
│     - Float switch indicates tank empty                                  │
│     - Pump disabled (dry-run protection)                                │
│     - Alarm raised to Controller                                         │
│                                                                          │
│  3. HIGH LEVEL INTERLOCK                                                 │
│     - Float switch indicates tank full                                   │
│     - Inlet valve closed                                                 │
│     - Alarm raised to Controller                                         │
│                                                                          │
│  4. OVER-TEMPERATURE                                                     │
│     - Temperature sensor exceeds limit                                   │
│     - Heaters disabled                                                   │
│     - Alarm raised to Controller                                         │
│                                                                          │
│  5. WATCHDOG TIMEOUT                                                     │
│     - No command refresh within timeout                                  │
│     - Individual actuator → safe state                                  │
│     - Communication alarm to Controller                                  │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.6 Offline Autonomy

**Behavior When Controller Disconnects:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    CONTROLLER DISCONNECT HANDLING                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Detection:                                                              │
│  - PROFINET AR (Application Relationship) closed                        │
│  - Watchdog timeout on cyclic data                                       │
│  - DCP/LLDP neighbor loss                                               │
│                                                                          │
│  Immediate Actions:                                                      │
│  1. Set communication_fault = true                                       │
│  2. Mark all actuators for watchdog evaluation                          │
│  3. Continue sensor polling (local data still valid)                    │
│  4. Display "OFFLINE" prominently in TUI                                │
│                                                                          │
│  Actuator Behavior (configurable per actuator):                         │
│  - HOLD_LAST: Maintain last commanded state (default for pumps)         │
│  - SAFE_STATE: Revert to defined safe state (default for valves)       │
│  - CONTINUE_AUTO: Continue local PID loop if configured                 │
│                                                                          │
│  Data Handling:                                                          │
│  - Continue logging to local SQLite                                      │
│  - Queue alarms for transmission on reconnect                           │
│  - Cache last N readings for store-and-forward                          │
│                                                                          │
│  Reconnection:                                                           │
│  1. Detect PROFINET AR establishment                                     │
│  2. Clear communication_fault flag                                       │
│  3. Flush queued alarms and logs to Controller                          │
│  4. Resume normal cyclic exchange                                        │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.7 SD Card Write Protection

**SD Write Realities:**
- Consumer SD cards: ~10,000 write cycles per cell
- Industrial SD cards: ~100,000 write cycles per cell
- Write amplification from small writes reduces effective lifespan
- A system writing every second can kill an SD card in months

**Write Management Strategy:**

```c
typedef struct {
    bool dirty;                     // Configuration changed
    uint64_t last_write_time_us;    // Timestamp of last persist
    uint64_t dirty_since_us;        // When dirty flag was set
    uint32_t debounce_ms;           // Minimum time between writes
    uint32_t max_dirty_ms;          // Maximum time before forced write
} write_manager_t;

result_t write_manager_request_write(write_manager_t *wm) {
    uint64_t now = now_us();
    uint64_t since_last = (now - wm->last_write_time_us) / 1000;
    uint64_t dirty_age = (now - wm->dirty_since_us) / 1000;

    wm->dirty = true;
    if (wm->dirty_since_us == 0) {
        wm->dirty_since_us = now;
    }

    // Check debounce
    if (since_last < wm->debounce_ms && dirty_age < wm->max_dirty_ms) {
        return RESULT_DEFERRED;  // Will write later
    }

    // Perform atomic write
    result_t result = perform_atomic_write();
    if (result == RESULT_OK) {
        wm->dirty = false;
        wm->dirty_since_us = 0;
        wm->last_write_time_us = now;
    }
    return result;
}
```

**Atomic Write Pattern:**

```c
result_t config_save_atomic(const char *path, const config_t *config) {
    char temp_path[MAX_PATH_LEN];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d", path, getpid());

    // Write to temporary file
    result_t result = config_write_file(temp_path, config);
    if (result != RESULT_OK) {
        unlink(temp_path);
        return result;
    }

    // Sync to ensure data is on disk
    int fd = open(temp_path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }

    // Atomic rename
    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        return RESULT_IO_ERROR;
    }

    // Sync directory
    char *dir = dirname(strdup(path));
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        fsync(fd);
    }
    free(dir);

    return RESULT_OK;
}
```

### 2.8 C Code Standards

**Compiler Flags (Non-Negotiable):**

```cmake
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wformat=2
    -Wformat-security
    -Wnull-dereference
    -Wstack-protector
    -fstack-protector-strong
    -Wconversion
    -Wsign-conversion
)

# Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g3 -O0 -DDEBUG)
endif()

# Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -DNDEBUG)
    # Disable debug logging at compile time
    add_compile_definitions(LOG_LEVEL_MIN=LOG_LEVEL_INFO)
endif()
```

**Function Documentation Standard:**

```c
/**
 * @brief Read a sensor value with quality assessment.
 *
 * Performs a synchronous read from the specified sensor, applying
 * calibration and range checking. Updates the reading structure
 * with value, quality, and timestamp.
 *
 * @param[in]  sensor_id  Unique identifier for the sensor (1-255)
 * @param[out] reading    Pointer to reading structure to populate
 *
 * @return RESULT_OK on successful read
 * @return RESULT_INVALID_PARAM if sensor_id invalid or reading NULL
 * @return RESULT_IO_ERROR if hardware communication failed
 * @return RESULT_TIMEOUT if sensor did not respond
 *
 * @note Thread safety: SAFE (uses per-sensor mutex)
 * @note Memory: NO_ALLOC (reading structure provided by caller)
 * @note Timing: Blocks up to sensor_config.timeout_ms
 *
 * @see sensor_config_t for timeout configuration
 * @see sensor_reading_t for output structure
 */
result_t sensor_read(uint8_t sensor_id, sensor_reading_t *reading);
```

**Error Handling Pattern:**

```c
// PROHIBITED - Silent error swallowing
result = do_operation();
// continues regardless

// PROHIBITED - Bare return codes
if (result != 0) return result;

// REQUIRED - Explicit error handling with context
result = do_operation();
if (result != RESULT_OK) {
    LOG_ERROR("module", "Operation failed: %s (code=%d)",
              result_to_string(result), result);
    cleanup_partial_state();
    return result;
}

// REQUIRED - Macro for common pattern
#define CHECK_RESULT(expr) do { \
    result_t _r = (expr); \
    if (_r != RESULT_OK) { \
        LOG_ERROR(__func__, #expr " failed: %s", result_to_string(_r)); \
        return _r; \
    } \
} while(0)

// Usage
CHECK_RESULT(sensor_init(&handle, &config));
CHECK_RESULT(sensor_calibrate(&handle, &cal));
```

**Memory Safety:**

```c
// PROHIBITED - Unbounded string operations
strcpy(dest, src);
sprintf(buf, "Value: %s", input);

// REQUIRED - Bounded operations with truncation handling
#define SAFE_STRNCPY(dest, src, size) do { \
    strncpy((dest), (src), (size) - 1); \
    (dest)[(size) - 1] = '\0'; \
} while(0)

int written = snprintf(buf, sizeof(buf), "Value: %s", input);
if (written >= (int)sizeof(buf)) {
    LOG_WARNING("module", "String truncated: needed %d, had %zu",
                written, sizeof(buf));
}

// REQUIRED - Null checks
#define CHECK_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        LOG_ERROR(__func__, "Null pointer: " #ptr); \
        return RESULT_INVALID_PARAM; \
    } \
} while(0)
```

---

## Part 3: Validation Standards

### 3.1 Test Requirements

**Unit Test Coverage:**

| Component | Minimum Coverage | Focus Areas |
|-----------|------------------|-------------|
| Sensor Drivers | 80% | Read paths, calibration, error handling |
| Actuator Control | 90% | Watchdog, interlocks, state transitions |
| PROFINET Callbacks | 85% | Data assembly, alarm generation |
| Configuration | 80% | Load, save, validation, migration |
| TUI Pages | 70% | Input handling, state rendering |

**Integration Test Scenarios:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    INTEGRATION TEST SCENARIOS                            │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  1. SENSOR-TO-PROFINET FLOW                                              │
│     - Configure sensor via TUI                                           │
│     - Verify sensor appears in PROFINET slot                            │
│     - Read sensor, verify value in cyclic data                          │
│     - Inject fault, verify quality degrades                             │
│     - Verify PROFINET diagnostic alarm generated                        │
│                                                                          │
│  2. ACTUATOR COMMAND FLOW                                                │
│     - Receive command via PROFINET output data                          │
│     - Verify actuator state changes                                      │
│     - Verify feedback in PROFINET input data                            │
│     - Stop commands, verify watchdog triggers                           │
│     - Verify safe state reached                                          │
│                                                                          │
│  3. OFFLINE OPERATION                                                    │
│     - Establish PROFINET connection                                      │
│     - Disconnect Controller                                              │
│     - Verify detection within 3x cycle time                             │
│     - Verify actuators follow offline policy                            │
│     - Verify sensor polling continues                                    │
│     - Reconnect, verify data sync                                        │
│                                                                          │
│  4. E-STOP SEQUENCE                                                      │
│     - Activate E-STOP (physical or TUI)                                 │
│     - Verify ALL actuators reach safe state < 100ms                     │
│     - Verify E-STOP status in PROFINET data                             │
│     - Verify TUI shows E-STOP active                                    │
│     - Attempt actuator command, verify rejected                         │
│     - Reset E-STOP, verify normal operation resumes                     │
│                                                                          │
│  5. CONFIGURATION PERSISTENCE                                            │
│     - Modify sensor configuration via TUI                               │
│     - Verify debounced write to SQLite                                   │
│     - Restart application                                                │
│     - Verify configuration restored                                      │
│     - Verify PROFINET slot mapping restored                             │
│                                                                          │
│  6. INTERLOCK BEHAVIOR                                                   │
│     - Simulate low level (float switch)                                  │
│     - Command pump ON via PROFINET                                       │
│     - Verify pump does NOT start (interlock active)                     │
│     - Verify interlock alarm sent to Controller                         │
│     - Clear low level condition                                          │
│     - Verify pump now responds to commands                              │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Hardware-in-Loop Testing

**Test Harness Requirements:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    HIL TEST CONFIGURATION                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Test Controller (Raspberry Pi or PC):                                   │
│  - Runs PROFINET IO Controller (soft PLC or p-net controller)           │
│  - Generates test command sequences                                      │
│  - Validates response timing                                             │
│  - Injects fault conditions                                              │
│                                                                          │
│  Device Under Test (Target SBC):                                         │
│  - Runs Water-Treat firmware                                             │
│  - Connected sensors (real or simulated)                                │
│  - Connected actuators (real or simulated)                              │
│                                                                          │
│  Sensor Simulation Options:                                              │
│  - DAC board to generate analog voltages                                │
│  - I2C slave emulator for digital sensors                               │
│  - GPIO test jig for discrete inputs                                    │
│                                                                          │
│  Actuator Verification:                                                  │
│  - GPIO readback for relay state                                         │
│  - Current sense for motor operation                                     │
│  - Position switch for valve state                                       │
│                                                                          │
│  Network Capture:                                                        │
│  - Wireshark/tcpdump for PROFINET analysis                              │
│  - Cycle time jitter measurement                                         │
│  - Alarm message verification                                            │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.3 Performance Validation

| Metric | Target | Measurement Method |
|--------|--------|-------------------|
| PROFINET cycle time | <= configured (default 1000ms) | Wireshark frame timing |
| Sensor poll latency | < 50ms per sensor | Instrumented driver |
| TUI input response | < 16ms | Keypress to render timing |
| Actuator command latency | < 100ms | Command to GPIO change |
| E-STOP response | < 100ms | Button to all-safe state |
| Watchdog detection | <= 3x cycle time | Disconnect to alarm timing |
| Boot to operational | < 30 seconds | Power-on to PROFINET ready |

### 3.4 Build Verification

**Pre-Commit Requirements:**

```bash
#!/bin/bash
# pre-commit-check.sh

set -e

echo "=== Build Check ==="
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

echo "=== Static Analysis ==="
cppcheck --enable=all --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    ../src/

echo "=== Unit Tests ==="
./run_tests

echo "=== Memory Check (Debug Build) ==="
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
valgrind --leak-check=full --error-exitcode=1 ./run_tests

echo "=== Format Check ==="
find ../src -name "*.c" -o -name "*.h" | xargs clang-format --dry-run --Werror

echo "=== All checks passed ==="
```

---

## Part 4: Documentation Standards

### 4.1 Required Documentation

```
docs/
|-- README.md                    # Project overview, quick start
|-- INSTALL.md                   # Complete installation guide
|-- OPERATOR.md                  # TUI usage, daily operations
|-- HARDWARE.md                  # Wiring, sensor connections
|-- PROFINET.md                  # PROFINET configuration, GSD
|-- TROUBLESHOOTING.md           # Common issues and solutions
|-- CALIBRATION.md               # Sensor calibration procedures
|-- development/
|   |-- ARCHITECTURE.md          # System design, data flow
|   |-- CODING_STANDARDS.md      # Style guide, patterns
|   |-- TESTING.md               # Test procedures
|   +-- DRIVERS.md               # Adding new sensor drivers
+-- gsd/
    +-- GSDML-V2.4-WaterTreat-RTU-*.xml
```

### 4.2 Sensor Hardware Documentation

**Per-Sensor Documentation Requirements:**

```markdown
# pH Sensor (Teyleten Robot pH Module)

## Hardware Connections

| Sensor Pin | SBC Connection | Notes |
|------------|----------------|-------|
| VCC | 5V | Sensor requires 5V |
| GND | GND | |
| Signal | ADS1115 A0 | 0-5V analog output |

## Wiring Diagram

[pH Probe] -> [BNC Connector] -> [pH Module] -> [ADS1115] -> [I2C Bus]
                                    |
                                    +-- VCC (5V)
                                    +-- GND
                                    +-- Signal -> A0

## Calibration Procedure

1. Prepare calibration solutions (pH 4.00 and pH 7.00)
2. Navigate to Sensors -> Select pH sensor -> Edit -> Calibrate
3. Immerse probe in pH 4.00 solution
4. Wait for reading to stabilize (30 seconds)
5. Enter "4.00" and press "Set Low Point"
6. Rinse probe with distilled water
7. Immerse probe in pH 7.00 solution
8. Wait for reading to stabilize (30 seconds)
9. Enter "7.00" and press "Set High Point"
10. Verify reading accuracy with pH 10.00 solution (optional)

## Maintenance

- Clean probe weekly with distilled water
- Store in pH 4.00 storage solution
- Recalibrate monthly or if readings drift
- Replace probe annually or when calibration fails

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| Reading stuck at 7.0 | Probe not connected | Check BNC connection |
| Noisy readings | Electrical interference | Add shielding, check grounds |
| Slow response | Dirty probe | Clean with probe cleaning solution |
| Cannot calibrate | Probe exhausted | Replace probe |
```

### 4.3 PROFINET GSD Documentation

```markdown
# PROFINET Device Configuration

## Device Identity

| Parameter | Value |
|-----------|-------|
| Vendor ID | 0x0493 |
| Device ID | 0x0001 |
| Station Name | water-treat-rtu |
| GSD Version | V2.4 |

## Importing GSD File

1. Locate GSD file: `gsd/GSDML-V2.4-WaterTreat-RTU-20241216.xml`
2. In TIA Portal: Options -> Manage General Station Description Files
3. Click "Install" and select the GSDML file
4. Device appears in Hardware Catalog under "Other field devices"

## Module Configuration

| Slot | Module ID | Description | Input Bytes | Output Bytes |
|------|-----------|-------------|-------------|--------------|
| 0 | 0x0000 | DAP | 0 | 0 |
| 1-8 | 0x0001 | Sensor Module | 5 (float + quality) | 0 |
| 9-15 | 0x0002 | Actuator Module | 4 (feedback) | 4 (command) |

## Cycle Time Configuration

Minimum: 1ms (if hardware supports)
Recommended: 100ms for water treatment applications
Maximum: 1000ms

## Diagnostic Alarms

| Alarm Code | Description | Severity |
|------------|-------------|----------|
| 0x0001 | Sensor Failure | High |
| 0x0002 | Actuator Fault | High |
| 0x0003 | Watchdog Timeout | Medium |
| 0x0004 | Communication Loss | Medium |
| 0x0005 | E-STOP Active | Critical |
| 0x0006 | Interlock Active | Medium |
```

---

## Part 5: Installation Process

### 5.1 Installation Script

```bash
#!/usr/bin/env bash
#
# Water-Treat RTU Installation Script
#
# Usage: sudo ./install.sh [OPTIONS]
#
# Options:
#   --prefix PATH       Installation prefix (default: /opt/water-treat)
#   --config PATH       Configuration directory (default: /etc/water-treat)
#   --interface NAME    PROFINET network interface (default: eth0)
#   --skip-deps         Skip dependency installation
#   --skip-verify       Skip post-install verification
#   --uninstall         Remove installation
#   --dry-run           Show what would be done
#   -h, --help          Show this help
#
# Exit Codes:
#   0   Success
#   1   General error
#   2   Invalid arguments
#   3   Missing dependencies
#   4   Permission denied
#   5   Build failure
#   6   Verification failure

set -euo pipefail
IFS=$'\n\t'

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly LOG_FILE="/var/log/water-treat-install.log"
readonly TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Default configuration
PREFIX="/opt/water-treat"
CONFIG_DIR="/etc/water-treat"
DATA_DIR="/var/lib/water-treat"
INTERFACE="eth0"
SKIP_DEPS=false
SKIP_VERIFY=false
UNINSTALL=false
DRY_RUN=false

log() {
    local level="$1"
    shift
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [${level}] $*" | tee -a "${LOG_FILE}"
}

log_info()  { log "INFO"  "$@"; }
log_warn()  { log "WARN"  "$@"; }
log_error() { log "ERROR" "$@"; }

die() {
    log_error "$@"
    exit 1
}

check_root() {
    [[ $EUID -eq 0 ]] || die "This script must be run as root"
}

check_os() {
    if [[ ! -f /etc/os-release ]]; then
        die "Cannot determine OS version"
    fi
    source /etc/os-release

    case "${ID}" in
        raspbian|debian|ubuntu)
            log_info "Detected OS: ${PRETTY_NAME}"
            ;;
        *)
            log_warn "Untested OS: ${ID}. Proceeding with caution."
            ;;
    esac
}

check_architecture() {
    local arch=$(uname -m)
    case "${arch}" in
        armv7l|aarch64|x86_64)
            log_info "Architecture: ${arch}"
            ;;
        *)
            die "Unsupported architecture: ${arch}"
            ;;
    esac
}

check_network_interface() {
    if ! ip link show "${INTERFACE}" &>/dev/null; then
        die "Network interface not found: ${INTERFACE}"
    fi
    log_info "PROFINET interface: ${INTERFACE}"
}

install_dependencies() {
    log_info "Installing system dependencies..."

    apt-get update
    apt-get install -y \
        build-essential \
        cmake \
        pkg-config \
        libncurses5-dev \
        libsqlite3-dev \
        libcurl4-openssl-dev \
        libcjson-dev \
        libgpiod-dev \
        i2c-tools \
        python3-smbus

    # Enable I2C and SPI
    if command -v raspi-config &>/dev/null; then
        raspi-config nonint do_i2c 0
        raspi-config nonint do_spi 0
    fi

    log_info "Dependencies installed"
}

build_application() {
    log_info "Building Water-Treat..."

    cd "${SCRIPT_DIR}"
    mkdir -p build
    cd build

    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
          -DPROFINET_INTERFACE="${INTERFACE}" \
          ..

    make -j$(nproc)

    log_info "Build complete"
}

install_application() {
    log_info "Installing to ${PREFIX}..."

    cd "${SCRIPT_DIR}/build"
    make install

    # Create directories
    mkdir -p "${CONFIG_DIR}"
    mkdir -p "${DATA_DIR}"/{db,logs,cache}

    # Install default configuration
    if [[ ! -f "${CONFIG_DIR}/config.yaml" ]]; then
        install -m 640 "${SCRIPT_DIR}/etc/config.yaml.example" \
                       "${CONFIG_DIR}/config.yaml"
    fi

    # Install GSD file
    mkdir -p "${PREFIX}/share/gsd"
    install -m 644 "${SCRIPT_DIR}/gsd/"*.xml "${PREFIX}/share/gsd/"

    # Set permissions
    chmod 750 "${DATA_DIR}"
    chmod 640 "${CONFIG_DIR}/config.yaml"

    log_info "Application installed"
}

install_systemd_service() {
    log_info "Installing systemd service..."

    cat > /etc/systemd/system/water-treat.service << EOF
[Unit]
Description=Water-Treat PROFINET RTU
After=network.target
Wants=network.target

[Service]
Type=simple
ExecStart=${PREFIX}/bin/water-treat --config ${CONFIG_DIR}/config.yaml
Restart=on-failure
RestartSec=5
User=root
Group=root

# Resource limits
MemoryMax=256M
CPUQuota=80%

# Security hardening (where possible for GPIO access)
ProtectSystem=strict
ReadWritePaths=${DATA_DIR}
PrivateTmp=true

# Console handling
StandardOutput=null
StandardError=null

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable water-treat.service

    log_info "systemd service installed"
}

generate_configuration() {
    local config_file="${CONFIG_DIR}/config.yaml"

    if [[ -f "${config_file}" ]]; then
        log_info "Configuration exists, preserving"
        return
    fi

    log_info "Generating default configuration..."

    cat > "${config_file}" << EOF
# Water-Treat RTU Configuration
# Generated: $(date -Iseconds)

# PROFINET Configuration
profinet:
  interface: ${INTERFACE}
  station_name: water-treat-rtu
  cycle_time_ms: 1000
  vendor_id: 0x0493
  device_id: 0x0001

# Database
database:
  path: ${DATA_DIR}/db/config.db
  wal_mode: true
  busy_timeout_ms: 5000

# Logging
logging:
  level: INFO
  file: ${DATA_DIR}/logs/water-treat.log
  max_size_mb: 10
  backup_count: 3
  console_when_no_tui: true

# Write Management (SD card protection)
write_manager:
  debounce_ms: 30000
  max_dirty_ms: 300000

# Sensor Defaults
sensors:
  poll_interval_ms: 1000
  stale_timeout_ms: 5000
  failure_threshold: 3

# Actuator Defaults
actuators:
  watchdog_timeout_ms: 5000
  default_safe_state: OFF

# TUI Configuration
tui:
  enabled: true
  refresh_rate_hz: 10
  color_theme: default
EOF

    chmod 640 "${config_file}"
    log_info "Configuration generated at ${config_file}"
}

verify_installation() {
    log_info "Verifying installation..."

    local errors=0

    # Check binary exists
    if [[ ! -x "${PREFIX}/bin/water-treat" ]]; then
        log_error "Binary not found: ${PREFIX}/bin/water-treat"
        ((errors++))
    fi

    # Check configuration
    if [[ ! -f "${CONFIG_DIR}/config.yaml" ]]; then
        log_error "Configuration not found: ${CONFIG_DIR}/config.yaml"
        ((errors++))
    fi

    # Check I2C bus
    if [[ -e /dev/i2c-1 ]]; then
        log_info "I2C bus available: /dev/i2c-1"
    else
        log_warn "I2C bus not found. Sensor support may be limited."
    fi

    # Check GPIO access
    if [[ -d /sys/class/gpio ]] || [[ -e /dev/gpiochip0 ]]; then
        log_info "GPIO access available"
    else
        log_warn "GPIO access not available. Actuator support may be limited."
    fi

    # Check network interface
    if ip link show "${INTERFACE}" &>/dev/null; then
        log_info "PROFINET interface available: ${INTERFACE}"
    else
        log_error "PROFINET interface not found: ${INTERFACE}"
        ((errors++))
    fi

    # Test run (no TUI, quick exit)
    if "${PREFIX}/bin/water-treat" --version &>/dev/null; then
        log_info "Binary executes successfully"
    else
        log_error "Binary execution failed"
        ((errors++))
    fi

    if [[ ${errors} -gt 0 ]]; then
        die "Verification failed with ${errors} error(s)"
    fi

    log_info "Verification complete"
}

start_service() {
    log_info "Starting service..."

    systemctl start water-treat.service
    sleep 2

    if systemctl is-active --quiet water-treat.service; then
        log_info "Service started successfully"
    else
        log_error "Service failed to start"
        journalctl -u water-treat.service --no-pager -n 20
        die "Service start failed"
    fi
}

print_summary() {
    cat << EOF

================================================================================
                     WATER-TREAT RTU INSTALLATION COMPLETE
================================================================================

  Installation Directory:  ${PREFIX}
  Configuration File:      ${CONFIG_DIR}/config.yaml
  Database Directory:      ${DATA_DIR}/db
  Log Directory:           ${DATA_DIR}/logs
  GSD Files:               ${PREFIX}/share/gsd/

  PROFINET Configuration:
    Interface:             ${INTERFACE}
    Station Name:          water-treat-rtu
    Vendor ID:             0x0493
    Device ID:             0x0001

  Commands:
    sudo systemctl status water-treat    # Check status
    sudo journalctl -fu water-treat      # View logs
    sudo systemctl restart water-treat   # Restart
    sudo ${PREFIX}/bin/water-treat --help # CLI options

  TUI Access:
    The TUI is available when running interactively.
    Stop the service first: sudo systemctl stop water-treat
    Then run: sudo ${PREFIX}/bin/water-treat --config ${CONFIG_DIR}/config.yaml

  Next Steps:
    1. Review configuration: ${CONFIG_DIR}/config.yaml
    2. Configure sensors via TUI (F2)
    3. Configure actuators via TUI (F8)
    4. Import GSD file into PROFINET controller
    5. Establish PROFINET connection

================================================================================

EOF
}

main() {
    log_info "Starting Water-Treat RTU installation"

    check_root
    check_os
    check_architecture
    check_network_interface

    if [[ "${SKIP_DEPS}" != "true" ]]; then
        install_dependencies
    fi

    build_application
    install_application
    install_systemd_service
    generate_configuration

    if [[ "${SKIP_VERIFY}" != "true" ]]; then
        verify_installation
    fi

    start_service
    print_summary

    log_info "Installation completed successfully"
}

main "$@"
```

### 5.2 Post-Installation Verification

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    POST-INSTALL VERIFICATION                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  PROCESS VERIFICATION                                                    │
│  [ ] water-treat process running (systemctl status water-treat)         │
│  [ ] No crash loops (journalctl shows clean startup)                    │
│  [ ] Memory usage reasonable (< 128MB typical)                          │
│                                                                          │
│  HARDWARE VERIFICATION                                                   │
│  [ ] I2C bus detected (i2cdetect -y 1 shows expected devices)          │
│  [ ] GPIO accessible (ls /sys/class/gpio or /dev/gpiochip*)            │
│  [ ] Network interface up (ip link show eth0)                           │
│                                                                          │
│  PROFINET VERIFICATION                                                   │
│  [ ] Station name set correctly                                          │
│  [ ] DCP response to identification request                             │
│  [ ] AR establishment with controller (if available)                    │
│                                                                          │
│  SENSOR VERIFICATION (via TUI)                                           │
│  [ ] Each configured sensor shows reading                               │
│  [ ] Quality indicators appropriate                                      │
│  [ ] No stuck readings (values update)                                  │
│                                                                          │
│  ACTUATOR VERIFICATION (via TUI)                                         │
│  [ ] Manual control works (toggle each actuator)                        │
│  [ ] State feedback matches physical state                              │
│  [ ] Watchdog triggers when expected                                    │
│                                                                          │
│  PERSISTENCE VERIFICATION                                                │
│  [ ] Configuration survives restart                                      │
│  [ ] Database not corrupted after power cycle                           │
│  [ ] Logs being written (check log directory)                           │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Part 6: The Prompt

Use this as a development prompt or system instruction for Water-Treat work:

```
You are developing firmware for the Water-Treat SCADA RTU system.
This is SBC #2 in a two-tier Water Treatment architecture:
- PROFINET I/O Device (using p-net stack)
- ncurses TUI for local configuration and diagnostics
- C language implementation (C11, strict warnings)
- SQLite for configuration persistence
- Multi-protocol sensor drivers (I2C, SPI, 1-Wire, GPIO, ADC)
- Actuator control with safety interlocks

Apply these non-negotiable constraints:

CONSOLE DISCIPLINE:
- NEVER write to stdout/stderr when TUI is active
- Route ALL output through the logger subsystem
- Logger checks tui_is_active() before any console write
- TUI message area is the ONLY place runtime messages appear
- Debug output is compile-time gated (LOG_LEVEL_MIN)

TUI NAVIGATION:
- <-/-> cycles through screens (Tab/Shift+Tab also works)
- Up/Down moves through list items
- Enter drills into edit/detail view
- ESC ALWAYS goes back one level (non-negotiable)
- ESC from top level shows quit confirmation
- F1-F8 are shortcuts, not primary navigation
- E key triggers E-STOP from any screen

SENSOR ABSTRACTION:
- Every driver implements: init, read, calibrate, get_status, destroy
- Every reading includes: value, quality, timestamp, raw_value
- Quality: GOOD, UNCERTAIN, BAD, NOT_CONNECTED
- Stale data is UNCERTAIN, never displayed as current
- Failed reads increment consecutive_failures counter
- Calibration coefficients stored in SQLite

DATA QUALITY:
- EVERY sensor reading carries quality metadata
- Quality displayed visually (color, symbol)
- Bad quality triggers PROFINET diagnostic alarm
- Historian (Controller) receives quality with every value
- Never fabricate or interpolate readings

ACTUATOR CONTROL:
- Every command has implicit watchdog timeout
- Timeout without refresh -> safe state
- Local interlocks NEVER bypassed by Controller commands
- E-STOP overrides everything, requires manual reset
- Feedback must match command or alarm raised

OFFLINE AUTONOMY:
- Detect Controller disconnect within 3x cycle time
- Actuators follow configured offline policy (HOLD_LAST, SAFE_STATE)
- Sensor polling continues locally
- Queue alarms for transmission on reconnect
- Display "OFFLINE" prominently in TUI

PROFINET INTEGRATION:
- Big-endian byte order for all multi-byte values
- Slot/module structure matches GSDML
- Diagnostic alarms for sensor failures, watchdog timeouts
- DCP response with correct station name
- Cyclic data assembled from sensor_status table

SD CARD PROTECTION:
- Debounce writes (30+ second intervals)
- Atomic write pattern (temp file + rename + fsync)
- Dirty flag tracking with max-age forced write
- WAL mode for SQLite
- Show "unsaved changes" indicator in TUI

ERROR HANDLING:
- CHECK_RESULT macro for consistent error propagation
- Every error logged with context (module, function, params)
- Cleanup partial state on failure
- Never swallow errors silently
- Return structured error codes, not magic numbers

C CODE STANDARDS:
- C11 standard, -Wall -Wextra -Werror -Wpedantic
- Valgrind clean for all test runs
- SAFE_STRNCPY for all string copies
- CHECK_NULL before pointer dereference
- Doxygen comments on all public functions
- Max 50 lines per function preferred
- Max 4 levels of nesting

CODE COMPLETENESS:
- Zero TODO/FIXME in release builds
- Zero unimplemented!() or placeholder returns
- All switch cases handled (or default with assertion)
- All function paths return appropriate values
- No dead code, no commented-out blocks

BUILD REQUIREMENTS:
- cmake build succeeds with zero warnings
- cppcheck --enable=all passes clean
- All unit tests pass
- valgrind --leak-check=full clean

TESTING:
- Unit tests for all sensor drivers
- Integration tests for PROFINET data flow
- E-STOP response time verification (< 100ms)
- Watchdog timeout verification
- Offline behavior verification

PRODUCTION CRITERIA:
Code is production-ready when:
- Build clean with strict flags
- All tests pass including HIL
- Documentation current
- Installation verified on fresh SD card
- PROFINET connection verified with real controller
- Sensor calibration procedure documented
- Actuator safe states verified
```

---

## Appendix: Quick Reference Checklists

### Pre-Commit Checklist

```
Before every commit:
  [ ] Code compiles with zero warnings (-Wall -Wextra -Werror)
  [ ] cppcheck passes without errors
  [ ] All unit tests pass
  [ ] No printf/fprintf to stdout/stderr in non-TUI code
  [ ] No TODO/FIXME markers added
  [ ] Function documentation complete (Doxygen)
  [ ] ESC key behavior verified if TUI modified
```

### Sensor Driver Checklist

```
For each new sensor driver:
  [ ] Implements sensor_driver_vtable_t interface
  [ ] init() configures hardware and validates connectivity
  [ ] read() returns value + quality + timestamp
  [ ] calibrate() applies coefficients and persists to DB
  [ ] get_status() returns health information
  [ ] destroy() releases all resources
  [ ] Error handling for I2C/SPI/GPIO failures
  [ ] Timeout handling for slow responses
  [ ] Unit tests for all paths
  [ ] Hardware documentation in docs/HARDWARE.md
  [ ] Calibration procedure in docs/CALIBRATION.md
```

### Actuator Control Checklist

```
For each actuator:
  [ ] Safe state defined and documented
  [ ] Watchdog timeout configured
  [ ] Feedback matches command or alarm raised
  [ ] Interlock conditions defined
  [ ] E-STOP behavior verified
  [ ] TUI control works correctly
  [ ] PROFINET command flow verified
  [ ] State persists correctly across restarts
```

### PROFINET Integration Checklist

```
For PROFINET functionality:
  [ ] GSDML file matches slot/module configuration
  [ ] Station name configurable and correct
  [ ] DCP responds to identification requests
  [ ] AR establishment succeeds with controller
  [ ] Cyclic data exchanged at configured rate
  [ ] Diagnostic alarms generated correctly
  [ ] Disconnect detected within 3x cycle time
  [ ] Reconnect resumes operation cleanly
  [ ] Byte order correct (big-endian)
```

### Installation Checklist

```
For every deployment:
  [ ] Target hardware meets requirements
  [ ] OS is supported version
  [ ] I2C/SPI/GPIO enabled
  [ ] Network interface configured
  [ ] Install script runs without errors
  [ ] Configuration file reviewed and customized
  [ ] Sensors wired and detected (i2cdetect)
  [ ] Actuators wired and tested (manual toggle)
  [ ] PROFINET connection verified
  [ ] GSD file imported to controller
  [ ] Calibration performed for analog sensors
  [ ] Interlock behavior verified
  [ ] Backup of configuration created
```

### TUI Consistency Checklist

```
For all TUI pages:
  [ ] ESC returns to previous screen
  [ ] <-/-> cycles between screens
  [ ] Up/Down moves selection cursor
  [ ] Enter activates selected item
  [ ] Help bar shows correct shortcuts
  [ ] No console pollution when active
  [ ] E-STOP (E key) works from this page
  [ ] F-key shortcuts work
  [ ] Color scheme consistent
  [ ] Data quality displayed correctly
```

---

*These guidelines establish the production, validation, documentation, and installation standards for the Water-Treat SCADA RTU system. Deviations require documented justification. The goal is safe, reliable, operator-respecting industrial field device firmware.*
