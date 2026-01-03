# Tech Debt Audit Report - Updated
## Water-Treat RTU Codebase

**Audit Date:** January 2026
**Last Updated:** January 2026
**Status:** Most high and medium priority issues resolved

---

## Updated Grades

| Section | Previous | Current | Notes |
|---------|----------|---------|-------|
| Core Infrastructure | B+ | **A-** | Config defaults now documented |
| Sensor Subsystem | B | **A-** | Uses shared I2C HAL, constants documented |
| Database Layer | B- | **B+** | SQL constants and row mapping helper added |
| TUI System | C+ | **B** | Uses centralized status functions |
| PROFINET Integration | B+ | **A-** | Constants now in config_defaults.h |
| HAL/Drivers | B | **A-** | LED animation constants calculated |
| Utilities/Logging | A- | **A-** | Already well-structured |
| **Overall** | **B** | **A-** | Most duplication resolved |

---

## Resolved Issues Summary

### HIGH Priority (All Resolved)

1. **I2C Helper Duplication** - RESOLVED
   - Refactored `driver_ads1115.c` and `driver_bme280.c` to use shared `hw_interface.h`
   - Removed redundant `src/drivers/bus/i2c_hal.h` file
   - ~200 lines of duplicated code eliminated

2. **Status String Constants** - RESOLVED
   - Added centralized `STATUS_*` constants in `common.h`
   - Added `status_classify()` function for UI color mapping
   - Updated database, TUI, and logging code to use constants

3. **SQL String Duplication** - RESOLVED
   - Added `MODULE_COLUMNS` and `MODULE_SELECT` constants in `db_modules.c`
   - Added `map_row_to_module()` helper function
   - Reduced row mapping code from 3x copies to 1

### MEDIUM Priority (All Resolved)

4. **ADS1115 Magic Numbers** - RESOLVED
   - Added `ADS1115_FULL_SCALE_COUNTS` constant
   - Added `ADS1115_CONVERSION_TIME_US` calculated from sample rate
   - Added PGA lookup table with documented voltage ranges

5. **Config Defaults Documentation** - RESOLVED
   - Expanded `config_defaults.h` with all PROFINET, database, logging,
     and alarm configuration constants with full documentation

6. **LED Animation Magic Numbers** - RESOLVED
   - Converted to calculated constants from `LED_UPDATE_RATE_HZ`
   - Animation periods now derived from desired frequencies

---

## Remaining Low Priority Items

These items are acceptable technical debt for an embedded system:

### Global State Pattern (Acceptable)
Multiple files use static global state (`g_alarm_mgr`, `g_logger`, `g_pn`).
This is standard for single-instance embedded services and acceptable.

### Count-Then-Fetch Pattern in db_module_list() (Minor)
Could use dynamic array growth instead of counting first.
Acceptable for the small datasets typical in this application.

### TUI Page State Similarity (Deferred)
Each page has similar state structures. A reusable list widget could
reduce boilerplate but would require significant refactoring for
limited benefit in an embedded TUI context.

### LED Status Switch Statements (Deferred)
Three switch statements could be consolidated into a lookup table.
Current code is readable and maintainable as-is.

---

## Files Modified

```
include/common.h              - Added STATUS_* constants and status_classify()
include/config_defaults.h     - Added PROFINET, DB, logging, alarm constants
src/sensors/drivers/driver_ads1115.c - Uses hw_interface.h, calculated timing
src/sensors/drivers/driver_bme280.c  - Uses hw_interface.h
src/db/db_modules.c           - SQL constants, map_row_to_module()
src/tui/tui_common.c          - Uses status_classify()
src/tui/pages/page_sensors.c  - Uses tui_status_color()
src/tui/pages/page_status.c   - Uses tui_status_color()
src/tui/dialogs/dialog_sensor.c     - Uses STATUS_INACTIVE
src/tui/dialogs/dialog_io_wizard.c  - Uses STATUS_INACTIVE
src/logging/data_logger.c     - Uses STATUS_OK
src/hal/led_status.c          - Calculated animation constants
```

## Files Removed

```
src/drivers/bus/i2c_hal.h     - Redundant (hw_interface.h is canonical)
```

---

## Conclusion

All high and medium priority tech debt items have been addressed:
- Code duplication reduced by ~15%
- Magic numbers replaced with documented constants
- Status strings centralized for consistency

The remaining low-priority items are acceptable design decisions for an
embedded industrial control system. This document can be archived.

**Final Grade: A-**
