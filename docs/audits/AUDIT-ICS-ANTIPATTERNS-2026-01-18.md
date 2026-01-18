# ICS Code Audit Report: Water-Treat RTU

**Audit Date**: 2026-01-18
**Auditor**: Claude (Embedded Systems Code Auditor)
**Scope**: Anti-patterns in safety-critical paths, TUI rendering, and configuration completeness
**Codebase**: Water-Treat RTU PROFINET I/O Device

---

## Executive Summary

This audit identified **10 findings** across three categories:
- **Safety-Critical Paths**: 4 findings (2 critical, 2 high)
- **TUI Rendering**: 3 findings (2 medium, 1 low)
- **Configuration Completeness**: 3 findings (1 high, 2 medium)

Priority remediation should focus on mutex protection for degraded mode status and interlock release behavior.

---

## Finding 1: Static Variable Persists Across Thread Lifecycle

- **File(s)**: `src/actuators/actuator_manager.c:243`
- **Pattern**: Safety-Critical Path - Degraded Mode Detection
- **Severity**: HIGH

### Current Code
The watchdog thread uses `static uint64_t no_command_start = 0;` inside the function scope. This persists if the actuator manager is stopped and restarted without process termination.

### Risk
If the system is restarted (manager destroyed/recreated), the stale `no_command_start` timestamp may trigger immediate degraded mode on next startup, causing unexpected "last-state-saved" behavior or false alarms.

### Recommended Fix
Move `no_command_start` to the `actuator_manager_t` struct and reset it in `actuator_manager_init()`:

```c
// In actuator_manager_t:
uint64_t no_command_start_ms;

// In actuator_manager_init:
mgr->no_command_start_ms = 0;
```

### Test Required
Restart actuator manager without process restart; verify no false degraded mode entry.

---

## Finding 2: Missing Mutex on Degraded Mode Status Read

- **File(s)**: `src/actuators/actuator_manager.c:728-731`
- **Pattern**: Safety-Critical Path - Thread Safety
- **Severity**: CRITICAL

### Current Code
```c
bool actuator_manager_is_degraded(actuator_manager_t *mgr) {
    if (!mgr) return false;
    return mgr->degraded_mode;  // No mutex lock
}
```

### Risk
Race condition between watchdog thread writing `degraded_mode` and TUI/status readers. Could display stale status during state transitions, misleading operators during critical situations.

### Recommended Fix
Add mutex protection:
```c
bool actuator_manager_is_degraded(actuator_manager_t *mgr) {
    if (!mgr) return false;
    pthread_mutex_lock(&mgr->mutex);
    bool degraded = mgr->degraded_mode;
    pthread_mutex_unlock(&mgr->mutex);
    return degraded;
}
```

### Test Required
Concurrent stress test with frequent degraded mode transitions while TUI polls status.

---

## Finding 3: PWM Fallback Uses Unsafe Threshold

- **File(s)**: `src/drivers/digital/relay_output.c:90-94`
- **Pattern**: Safety-Critical Path - Actuator Control
- **Severity**: HIGH

### Current Code
```c
static result_t gpio_set_pwm(int pin, float duty_cycle, int frequency_hz) {
    UNUSED(frequency_hz);
    if (duty_cycle > 0.5f) {
        return gpio_set_output(pin, true);
    } else {
        return gpio_set_output(pin, false);
    }
}
```

### Risk
A requested 49% PWM silently becomes OFF, while 51% becomes full ON. For dosing pumps, this could cause under-treatment or over-treatment of water. No logging indicates the fallback occurred.

### Recommended Fix
Log the degradation and consider configurable threshold or rejection:
```c
LOG_WARNING("PWM requested (%.1f%%) but not supported on GPIO %d, falling back to %s",
            duty_cycle * 100, pin, duty_cycle > 0.5f ? "ON" : "OFF");
```

### Test Required
Request PWM values at 45%, 50%, 55% and verify logged warnings appear.

---

## Finding 4: Unicode Emoji in TUI May Not Render

- **File(s)**: `src/tui/pages/page_actuators.c:216`
- **Pattern**: TUI Rendering
- **Severity**: LOW

### Current Code
```c
mvwprintw(win, row, 2, "⚠ DEGRADED MODE - Controller disconnected...");
```

### Risk
ICS terminals (serial consoles, older SSH clients, embedded displays) may not support Unicode. The warning symbol `⚠` could render as garbage characters or blank, reducing operator awareness of degraded state.

### Recommended Fix
Use ASCII-safe indicator:
```c
mvwprintw(win, row, 2, "[!] DEGRADED MODE - Controller disconnected...");
```

### Test Required
Test TUI on VT100/ANSI terminal emulator with `LANG=C`.

---

## Finding 5: Input Parsing Uses Unsafe atoi()/atof() Despite Safe Alternatives

- **File(s)**: `src/tui/tui_common.c:282, 296`
- **Pattern**: TUI Rendering - Input Validation
- **Severity**: MEDIUM

### Current Code
```c
int tui_get_int(...) {
    int new_val = atoi(buffer);  // Silent failure on invalid input
    ...
}
int tui_get_float(...) {
    float new_val = atof(buffer);  // Silent failure on invalid input
    ...
}
```

### Risk
Malformed input (e.g., "10abc" for a slot number) silently parses as 10 instead of rejection. Operators may unknowingly enter invalid configurations. The codebase has `safe_parse_int()` and `safe_parse_float()` in `common.h` that are not being used.

### Recommended Fix
Use the safe parsing functions:
```c
int tui_get_int(...) {
    int new_val;
    if (safe_parse_int(buffer, &new_val, min_val, max_val) == RESULT_OK) {
        *value = new_val;
        return 1;
    }
    return 0;
}
```

### Test Required
Enter "12xyz" for slot number; verify rejection with clear feedback.

---

## Finding 6: Dialog Uses strncpy() Instead of SAFE_STRNCPY Macro

- **File(s)**: `src/tui/dialogs/dialog_sensor.c:74, 77-79, 112-114, 123, 129, 135, 140-141, 151`
- **Pattern**: TUI Rendering - String Safety
- **Severity**: LOW

### Current Code
Multiple instances of `strncpy(value, form->name, 63);` and similar patterns throughout the dialog code.

### Risk
`strncpy()` does not guarantee null-termination when source exceeds destination size. If user enters max-length values, buffer may lack terminator causing display corruption or crashes.

### Recommended Fix
Replace all `strncpy()` calls with `SAFE_STRNCPY()`:
```c
SAFE_STRNCPY(value, form->name, sizeof(value));
```

### Test Required
Enter 63+ character sensor name; verify no buffer overflow or display corruption.

---

## Finding 7: Inconsistent Time Unit Conversion in Actuator Reload

- **File(s)**: `src/actuators/actuator_manager.c:794-795`
- **Pattern**: Configuration Completeness
- **Severity**: MEDIUM

### Current Code
```c
config.max_on_time_sec = db_act->max_on_time_ms / 1000;
config.min_cycle_time_ms = db_act->min_on_time_ms;
```

### Risk
`max_on_time` converts ms to sec (loses precision via integer division), while `min_cycle_time` stays in ms. If database stores 1500ms max on time, it becomes 1 second (33% loss). Field naming mismatch (`min_on_time_ms` used for cycle time) adds confusion.

### Recommended Fix
Keep consistent units (milliseconds internally) or round appropriately:
```c
config.max_on_time_sec = (db_act->max_on_time_ms + 500) / 1000;  // Round instead of truncate
```
And clarify the field mapping in comments.

### Test Required
Configure actuator with max_on_time_ms=1999; verify it enforces ~2 sec not 1 sec.

---

## Finding 8: Missing PROFINET Slot Range Validation

- **File(s)**: `src/config/config_validate.c` (missing), `src/actuators/actuator_manager.c:474`
- **Pattern**: Configuration Completeness
- **Severity**: MEDIUM

### Current Code
Actuator slots are validated only at runtime via `ACTUATOR_MAX_SLOT (64)` check in `actuator_manager_add()`, but invalid slots in database are loaded without validation at startup.

### Risk
An actuator configured with slot 100 will fail silently at runtime. No startup validation warns operators before deployment.

### Recommended Fix
Add validation in `config_validate.c` or during database load:
```c
if (actuator->slot < 0 || actuator->slot > ACTUATOR_MAX_SLOT) {
    add_message(result, "ERROR: Actuator slot out of range (0-64)");
    result->error_count++;
}
```

### Test Required
Configure actuator at slot 99; verify startup error message.

---

## Finding 9: Interlock Release Always Sets Actuator to OFF

- **File(s)**: `src/alarms/alarm_manager.c:247-260`
- **Pattern**: Safety-Critical Path - Interlock Behavior
- **Severity**: CRITICAL

### Current Code
```c
if (rule->interlock_enabled && rule->interlock_slot > 0 && rule->release_on_clear) {
    actuator_manager_manual_set(&g_actuator_mgr, rule->interlock_slot,
                                ACTUATOR_STATE_OFF, 0)
```

### Risk
When an alarm clears, the interlock always forces the actuator OFF regardless of what state the controller was commanding. For a high-level alarm that triggered a pump ON as safety response, clearing the alarm would turn it OFF, which might not be safe if the condition hasn't fully recovered.

### Recommended Fix
Add configurable release behavior (restore-previous vs force-off vs release-to-controller):
```c
typedef enum {
    INTERLOCK_RELEASE_OFF,          // Current behavior - force OFF
    INTERLOCK_RELEASE_TO_CONTROLLER, // Clear manual_mode, let controller resume
    INTERLOCK_RELEASE_RESTORE        // Return to pre-interlock state
} interlock_release_action_t;

switch (rule->release_action) {
    case INTERLOCK_RELEASE_OFF:
        actuator_manager_manual_set(&g_actuator_mgr, rule->interlock_slot,
                                    ACTUATOR_STATE_OFF, 0);
        break;
    case INTERLOCK_RELEASE_TO_CONTROLLER:
        // Clear manual mode flag only
        break;
    // etc.
}
```

### Test Required
Document expected behavior with process engineers; implement configurable release action.

---

## Finding 10: Watchdog Timing Parameters Not Validated

- **File(s)**: `src/config/config_validate.c` (missing validation)
- **Pattern**: Configuration Completeness
- **Severity**: HIGH

### Current Code
`watchdog_interval_ms`, `command_timeout_ms`, and `degraded_alarm_delay_ms` are loaded from config but never validated for sanity.

### Risk
- Setting `command_timeout_ms=0` would cause immediate degraded mode
- Setting `watchdog_interval_ms` larger than `command_timeout_ms` would miss timeouts
- Invalid ratios create unpredictable behavior

### Recommended Fix
Add to `config_validate()`:
```c
if (config->watchdog.command_timeout_ms < 1000 ||
    config->watchdog.command_timeout_ms > 60000) {
    add_message(result, "ERROR: command_timeout_ms must be 1000-60000");
    result->error_count++;
}
if (config->watchdog.watchdog_interval_ms > config->watchdog.command_timeout_ms / 2) {
    add_message(result, "WARNING: watchdog_interval_ms should be <= command_timeout_ms/2");
    result->warning_count++;
}
if (config->watchdog.degraded_alarm_delay_ms > config->watchdog.command_timeout_ms) {
    add_message(result, "WARNING: degraded_alarm_delay_ms > command_timeout_ms may delay alerts");
    result->warning_count++;
}
```

### Test Required
Set `command_timeout_ms=100`; verify startup error message.

---

## Summary by Priority

| Priority | Finding | Category | File |
|----------|---------|----------|------|
| **CRITICAL** | #2 Missing mutex on degraded mode read | Safety | actuator_manager.c:728 |
| **CRITICAL** | #9 Interlock release always forces OFF | Safety | alarm_manager.c:247 |
| **HIGH** | #1 Static variable in watchdog thread | Safety | actuator_manager.c:243 |
| **HIGH** | #3 PWM fallback unsafe threshold | Safety | relay_output.c:90 |
| **HIGH** | #10 Watchdog timing not validated | Config | config_validate.c |
| **MEDIUM** | #5 Unsafe atoi()/atof() in TUI | TUI | tui_common.c:282,296 |
| **MEDIUM** | #7 Inconsistent time unit conversion | Config | actuator_manager.c:794 |
| **MEDIUM** | #8 Missing PROFINET slot validation | Config | config_validate.c |
| **LOW** | #4 Unicode emoji in degraded warning | TUI | page_actuators.c:216 |
| **LOW** | #6 strncpy() instead of SAFE_STRNCPY | TUI | dialog_sensor.c |

---

## Recommendations

### Immediate Actions (Before Next Deployment)
1. Fix Finding #2 (mutex) - simple one-line fix with high safety impact
2. Review Finding #9 with process engineers to define correct interlock release behavior

### Short-Term (Within Sprint)
3. Address all HIGH priority findings (#1, #3, #10)
4. Replace `atoi()`/`atof()` with safe parsing functions (#5)

### Medium-Term (Technical Debt)
5. Refactor `dialog_sensor.c` to use `SAFE_STRNCPY()` throughout (#6)
6. Add comprehensive config validation for PROFINET slots (#8)
7. Standardize time units in actuator configuration (#7)

### Low Priority (Future Enhancement)
8. Replace Unicode with ASCII in TUI for terminal compatibility (#4)

---

## Appendix: Files Reviewed

| File | Lines | Category |
|------|-------|----------|
| src/actuators/actuator_manager.c | 813 | Safety-Critical |
| src/actuators/actuator_manager.h | 232 | Safety-Critical |
| src/alarms/alarm_manager.c | 540 | Safety-Critical |
| src/profinet/profinet_manager.c | 643 | PROFINET |
| src/drivers/digital/relay_output.c | 418 | Actuator Driver |
| src/tui/tui_main.c | 581 | TUI |
| src/tui/tui_common.c | 452 | TUI |
| src/tui/pages/page_actuators.c | 436 | TUI |
| src/tui/dialogs/dialog_sensor.c | 418 | TUI |
| src/config/config.c | 401 | Configuration |
| src/config/config_validate.c | 365 | Configuration |
| src/db/db_actuators.c | 445 | Database |
| include/common.h | 300 | Core |
| include/config_defaults.h | 102 | Configuration |
