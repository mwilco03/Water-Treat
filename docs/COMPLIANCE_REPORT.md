# Water-Treat Development Guidelines Compliance Report

**Report Date:** 2025-12-22
**Repository:** Water-Treat (RTU - SBC #2)
**Sister Repository:** Water-Controller (IO Controller - SBC #1)

---

## Executive Summary

This report evaluates the Water-Treat codebase against the standards defined in `docs/DEVELOPMENT_GUIDELINES.md`. The codebase demonstrates strong compliance in many areas but has several gaps requiring attention before production deployment.

| Category | Status | Score |
|----------|--------|-------|
| Console Discipline | **COMPLIANT** | 95% |
| TUI Navigation | **PARTIALLY COMPLIANT** | 75% |
| Sensor Abstraction | **PARTIALLY COMPLIANT** | 65% |
| Data Quality | **NON-COMPLIANT** | 20% |
| Actuator Safety | **COMPLIANT** | 90% |
| PROFINET Integration | **PARTIALLY COMPLIANT** | 70% |
| C Code Standards | **PARTIALLY COMPLIANT** | 70% |
| SD Card Protection | **PARTIALLY COMPLIANT** | 60% |
| Documentation | **COMPLIANT** | 85% |
| Testing | **PARTIALLY COMPLIANT** | 50% |

**Overall Compliance: 68%**

---

## Detailed Analysis

### 1. Console Discipline

**Guideline:** "When the TUI is active, NOTHING writes directly to stdout/stderr."

**Status: COMPLIANT (95%)**

**Evidence:**
- `src/utils/logger.c:124-134` properly checks `tui_is_active()` before console output
- Routes log messages through `tui_log_message()` when TUI is active
- Ring buffer implementation exists (`TUI_MSG_RING_SIZE = 32`)

**Code Reference:**
```c
// src/utils/logger.c:124-127
if (tui_is_active()) {
    /* Route through TUI message area - never write directly to console */
    tui_log_message(level, msg);
} else { ... }
```

**Minor Issues:**
- `src/main.c:569-582` uses direct `printf()` for --help and --version (acceptable - TUI not active)
- `src/config/config.c:120-123` uses `fprintf()` for config file writing (acceptable - file output)

---

### 2. TUI Navigation

**Guideline:** "ESC ALWAYS goes back one level. Left/Right cycles screens."

**Status: PARTIALLY COMPLIANT (75%)**

**What Works:**
- ESC key properly navigates back through history stack (`tui_main.c:443-466`)
- At root level, ESC shows quit confirmation dialog
- Left/Right arrow keys cycle through screens without affecting history
- Tab/Shift+Tab also cycles screens
- F1-F8 shortcuts work correctly

**Code Reference:**
```c
// src/tui/tui_main.c:443-466
case 27:  /* ESC key */
    if (!navigate_back()) {
        /* At root screen - show quit confirmation */
        if (tui_confirm(g_tui.main_win, "Exit Water-Treat RTU?")) {
            g_tui.running = false;
        }
    }
```

**Missing:**
- **E key for E-STOP is NOT global** - only works on Actuators page (`page_actuators.c:432`)
- Guidelines require: "E key triggers E-STOP from any screen"

**Recommendation:** Add global E-STOP handler in `tui_main.c` main input loop

---

### 3. Sensor Abstraction Layer

**Guideline:** "Every driver implements: init, read, calibrate, get_status, destroy"

**Status: PARTIALLY COMPLIANT (65%)**

**What Exists:**
- Multiple sensor drivers (12+ protocols):
  - DS18B20, DHT22, ADS1115, MCP3008, Web Poll, TCS34725, HX711, Float Switch, BME280, JSN-SR04T
- `sensor_instance_t` structure with lifecycle management
- Calibration support via database storage
- Moving average filtering

**What's Missing:**
- **No unified `sensor_driver_vtable_t` interface** as specified in guidelines
- Current drivers have inconsistent interfaces:
  - Some: `driver_xxx_init()`, `driver_xxx_read()`, `driver_xxx_close()`
  - Missing: `calibrate()`, `get_status()` in most drivers
- No standardized `sensor_reading_t` structure with quality + timestamp

**Current Structure (sensor_instance.c:269-338):**
```c
result_t sensor_instance_read(sensor_instance_t *instance, float *value);
// Returns float only - no quality, timestamp, raw_value
```

**Required Structure (per guidelines):**
```c
typedef struct {
    float value;
    data_quality_t quality;  // MISSING
    uint64_t timestamp_us;   // MISSING (has last_read_ms instead)
    uint32_t raw_value;      // Partially implemented
    uint8_t consecutive_failures;  // EXISTS
} sensor_reading_t;
```

---

### 4. Data Quality Propagation

**Guideline:** "Every sensor reading carries quality metadata. GOOD, UNCERTAIN, BAD, NOT_CONNECTED"

**Status: NON-COMPLIANT (20%)**

**Critical Gap:**
- The `data_quality_t` enum defined in guidelines **does not exist in the codebase**
- Only reference is in `docs/DEVELOPMENT_GUIDELINES.md` itself
- PROFINET input data uses `PNET_IOXS_GOOD/BAD` but not the OPC UA quality codes

**What Exists:**
- `sensor_instance.connected` (bool) - basic connected status
- `sensor_instance.consecutive_failures` - failure tracking
- `input_iops` in PROFINET slots - but only GOOD/BAD

**Missing Implementation:**
```c
// Required per guidelines - NOT IMPLEMENTED
typedef enum {
    QUALITY_GOOD          = 0x00,
    QUALITY_UNCERTAIN     = 0x40,
    QUALITY_BAD           = 0x80,
    QUALITY_NOT_CONNECTED = 0xC0,
} data_quality_t;
```

**Impact:**
- TUI cannot display quality indicators (?, X, -)
- PROFINET cannot send proper quality bytes to controller
- Controller cannot distinguish stale vs bad data

---

### 5. Actuator Control & Safety

**Guideline:** "Every command has implicit watchdog timeout. Local interlocks NEVER bypassed."

**Status: COMPLIANT (90%)**

**What Works:**
- Watchdog thread monitors command freshness (`actuator_manager.c:200-245`)
- `COMMAND_TIMEOUT_MS = 5000` for disconnect detection
- Max on-time enforcement (`check_safety_limits()`)
- Minimum cycle time enforcement (anti-short-cycle)
- Emergency stop implementation (`actuator_manager_emergency_stop()`)
- Degraded mode handling on controller disconnect

**Code Reference:**
```c
// src/actuators/actuator_manager.c:168-198
static void check_safety_limits(actuator_manager_t *mgr) {
    // Check max on time
    if (act->config.max_on_time_sec > 0 && act->state == ACTUATOR_STATE_ON) {
        if (on_duration_ms >= max_on_ms) {
            LOG_WARNING("Actuator %s exceeded max on time, forcing OFF");
            act->state = ACTUATOR_STATE_OFF;
            apply_actuator_state(act);
        }
    }
}
```

**Minor Gaps:**
- No physical E-STOP input handling (GPIO interrupt)
- No configurable safe state per actuator in current schema
- Interlock conditions (low level, high level) not implemented

---

### 6. PROFINET Integration

**Guideline:** "Big-endian byte order for all multi-byte values. Use htons(), htonl()..."

**Status: PARTIALLY COMPLIANT (70%)**

**What Works:**
- p-net stack integration (`profinet_manager.c`)
- Cyclic data exchange functional
- Module plug/unplug handling
- Connect/disconnect callbacks
- Alarm send capability

**Issues:**

1. **Byte Order Handling:**
   - Manual byte swap in `profinet_manager_update_input_float()`:
   ```c
   // src/profinet/profinet_manager.c:373-382
   data[0] = (*p >> 24) & 0xFF;  // Manual swap, not htonl()
   ```
   - No use of `htons()`, `htonl()` per guidelines
   - Only one use of `htons()` found (in health_check.c for HTTP server)

2. **Quality Byte:**
   - Input data is float only (4 bytes)
   - Guidelines specify 5 bytes: float + quality byte
   - Current: `slot->input_size = 4;` (`profinet_manager.c:164`)

3. **GSDML:**
   - `gsd/GSDML-V2.4-WaterTreat-RTU-20241216.xml` exists
   - Not verified against current slot configuration

---

### 7. C Code Standards

**Guideline:** "-Wall -Wextra -Werror -Wpedantic"

**Status: PARTIALLY COMPLIANT (70%)**

**CMakeLists.txt (line 42):**
```cmake
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter
    -Wno-stringop-truncation -Wno-format-truncation -Wno-sign-compare")
```

**Missing:**
- `-Werror` (warnings as errors)
- `-Wpedantic`
- `-Wformat=2`
- `-Wformat-security`
- `-Wnull-dereference`
- `-Wstack-protector`
- `-fstack-protector-strong`
- `-Wconversion`
- `-Wsign-conversion`

**What Works:**
- `SAFE_STRNCPY` macro implemented correctly (`common.h:49-58`)
- `CHECK_NULL` macro exists (`common.h:68`)
- `CHECK_RESULT` macro exists (`common.h:69`)
- C11 standard enforced

**Code Hygiene:**
- **1 TODO found:** `src/tui/dialogs/dialog_io_wizard.c:620`
  ```c
  d->in_use = false;  /* TODO: check database */
  ```
- **No FIXME markers found**

---

### 8. SD Card Write Protection

**Guideline:** "Debounce writes (30+ second intervals). Atomic write pattern."

**Status: PARTIALLY COMPLIANT (60%)**

**What Exists:**
- SQLite WAL mode available (config option `wal_mode`)
- Some atomic write patterns in config saving

**What's Missing:**
- No `write_manager_t` structure as specified
- No debounce/dirty flag tracking for configuration saves
- No `max_dirty_ms` forced write threshold
- No "unsaved changes" indicator in TUI

**Configuration Reference:**
```yaml
# From guidelines install script
write_manager:
  debounce_ms: 30000
  max_dirty_ms: 300000
```

This is not implemented in current `config.c`.

---

### 9. Offline Autonomy

**Guideline:** "Detect Controller disconnect within 3x cycle time."

**Status: COMPLIANT (85%)**

**Implementation:**
- `DEGRADED_ALARM_DELAY_MS = 3000` in actuator_manager.c
- `enter_degraded_mode()` / `exit_degraded_mode()` functions
- Maintains last commanded state on disconnect
- Event logging during degraded mode
- Callback system for degraded mode notification

**Code Reference:**
```c
// src/actuators/actuator_manager.c:120-142
static void enter_degraded_mode(actuator_manager_t *mgr) {
    mgr->degraded_mode = true;
    LOG_WARNING("Entering DEGRADED MODE - controller disconnected");
    LOG_WARNING("Actuators will maintain last commanded state");
    // Note: We do NOT change actuator states here
    // This implements "last-state-saved" behavior
}
```

---

### 10. Testing

**Guideline:** "Unit tests for all sensor drivers. Integration tests for PROFINET."

**Status: PARTIALLY COMPLIANT (50%)**

**What Exists:**
- Test framework (`tests/test_main.c`)
- Formula evaluator tests (`test_formula.c`)
- Calibration tests (`test_calibration.c`)
- Alarm tests (`test_alarms.c`)
- PROFINET data tests (`test_profinet_data.c`)
- Config tests (`test_config.c`)

**What's Missing:**
- No unit tests for individual sensor drivers
- No integration tests for PROFINET data flow
- No E-STOP response time verification
- No offline behavior tests
- No `valgrind` memory check in CI
- No `cppcheck` static analysis in CI

---

## Parity with Water-Controller

**Repository:** https://github.com/mwilco03/Water-Controller

### Architecture Alignment

| Feature | Water-Controller | Water-Treat | Parity |
|---------|-----------------|-------------|--------|
| Role | IO Controller (PLC) | IO Device (RTU) | **Aligned** |
| PROFINET | Controller stack | Device stack (p-net) | **Aligned** |
| Data Format | Expects big-endian floats | Sends big-endian floats | **Aligned** |
| Slot Config | Dynamic slot detection | Static slot registration | **Partial** |
| Alarms | ISA-18.2 alarm system | Basic alarm manager | **Gap** |
| Historian | Time-series with compression | Local SQLite logging | **Different scope** |

### Communication Contract

**Output (Controller → RTU):**
```
Byte 0:    Actuator command (OFF=0, ON=1, PWM=2)
Byte 1:    PWM duty cycle (0-255)
Byte 2-3:  Reserved
```
**Water-Treat:** Implemented in `actuator_output_data_t`

**Input (RTU → Controller):**
```
Bytes 0-3:  Sensor value (Float32, BE)
Byte 4:     Quality (0x00=GOOD, 0x40=UNCERTAIN, 0x80=BAD)
```
**Water-Treat:** **INCOMPLETE** - Only sends float, no quality byte

### Protocol Gaps

1. **Quality Byte Missing** - Controller expects 5 bytes per sensor, RTU sends 4
2. **Slot Mapping** - Controller uses dynamic discovery, RTU has hardcoded slots
3. **Diagnostic Alarms** - Controller expects alarm callbacks, RTU has basic implementation

---

## Recommendations

### Critical (Must Fix Before Production)

1. **Implement Data Quality System**
   - Add `data_quality_t` enum to `common.h`
   - Update `sensor_reading_t` to include quality + timestamp
   - Modify PROFINET input data to include quality byte
   - Display quality in TUI

2. **Add Global E-STOP Handler**
   - Move E-STOP handling to `tui_main.c` main loop
   - Make 'E' key work from any screen

3. **Enable Strict Compiler Flags**
   - Add `-Werror -Wpedantic` to CMakeLists.txt
   - Fix resulting warnings

### High Priority

4. **Standardize Sensor Driver Interface**
   - Create `sensor_driver_vtable_t` abstraction
   - Add `calibrate()` and `get_status()` to all drivers

5. **Implement Write Debouncing**
   - Add `write_manager_t` for SD card protection
   - Add "unsaved changes" indicator to TUI

6. **Use Network Byte Order Functions**
   - Replace manual byte swapping with `htonl()`/`ntohl()`

### Medium Priority

7. **Add Integration Tests**
   - PROFINET connection/disconnection tests
   - Sensor-to-PROFINET data flow tests
   - E-STOP response time tests

8. **Implement Safety Interlocks**
   - Add float switch interlock for pumps
   - Add configurable safe state per actuator

9. **Remove TODO Comment**
   - `dialog_io_wizard.c:620` - implement database check

### Low Priority

10. **Add Static Analysis to CI**
    - Run `cppcheck --enable=all`
    - Run `valgrind` on test suite

---

## Conclusion

The Water-Treat codebase has a solid foundation with good console discipline, TUI navigation, and actuator safety. However, the **missing data quality system** is a critical gap that breaks the communication contract with the sister Water-Controller repository. This should be the top priority for remediation.

The codebase follows many C best practices but needs stricter compiler flags to meet production standards. The testing infrastructure exists but needs expansion to cover sensor drivers and integration scenarios.

**Next Steps:**
1. Implement `data_quality_t` and modify PROFINET input to 5-byte format
2. Add global E-STOP handler
3. Enable `-Werror -Wpedantic` and fix warnings
4. Update GSDML file to reflect 5-byte input modules

---

*Report generated by codebase analysis against DEVELOPMENT_GUIDELINES.md*
