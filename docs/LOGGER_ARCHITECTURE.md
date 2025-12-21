# Console Pollution Audit Report
## Water-Treat RTU Codebase
## Date: 2025-12-21

---

## Executive Summary

| Metric | Count |
|--------|-------|
| Total violations found | **0** |
| Critical | 0 |
| High | 0 |
| Medium | 0 |
| Low | 0 |
| Info (acceptable patterns) | 5 |

**Status: CLEAN** - The codebase implements proper TUI-aware logging with no console pollution during TUI operation.

---

## Logger Architecture Analysis

### Macro Definitions

**Location:** `src/utils/logger.h:45-50`

```c
#define LOG_TRACE(fmt, ...) logger_log(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) logger_log(LOG_LEVEL_WARNING, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) logger_log(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
```

### Implementation Function

**Location:** `src/utils/logger.c:102-157`

**Function:** `logger_log(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...)`

### TUI-Active Check

**Status:** ✅ YES - Properly implemented

**Check Location:** `src/utils/logger.c:125`

```c
if (g_logger.config.destinations & LOG_DEST_CONSOLE) {
    if (tui_is_active()) {
        /* Route through TUI message area - never write directly to console */
        tui_log_message(level, msg);
    } else {
        /* TUI not active - safe to write to console */
        FILE *out = (level >= LOG_LEVEL_WARNING) ? stderr : stdout;
        if (ts[0]) fprintf(out, "%s ", ts);
        fprintf(out, "%s[%-5s]\033[0m %s\n", level_colors[level], level_names[level], msg);
        fflush(out);
    }
}
```

### Branch Behavior

| Condition | Destination | Output Method |
|-----------|-------------|---------------|
| `tui_is_active() == true` | TUI message ring buffer + status bar | `tui_log_message()` |
| `tui_is_active() == false` | stdout (INFO and below) or stderr (WARNING+) | `fprintf()` with ANSI colors |

### TUI State Function

**Location:** `src/tui/tui_main.c:451-458`

```c
bool tui_is_active(void) {
    /*
     * Returns true if TUI is initialized and should receive log messages.
     * The logger checks this to route messages through the TUI message area
     * instead of writing directly to stdout/stderr (which corrupts the display).
     */
    return g_tui.initialized;
}
```

### Message Routing (TUI Active)

**Location:** `src/tui/tui_main.c:460-484`

```c
void tui_log_message(int level, const char *message) {
    /*
     * Route log messages through TUI instead of direct console write.
     * Messages are stored in a ring buffer and displayed in the status area.
     */
    if (!g_tui.initialized || !message) return;

    /* Add to ring buffer */
    tui_msg_entry_t *entry = &g_tui.msg_ring[g_tui.msg_ring_head];
    SAFE_STRNCPY(entry->message, message, TUI_MSG_MAX_LEN);
    entry->level = level;
    entry->timestamp = time(NULL);

    /* Update ring buffer pointers */
    g_tui.msg_ring_head = (g_tui.msg_ring_head + 1) % TUI_MSG_RING_SIZE;
    if (g_tui.msg_ring_count < TUI_MSG_RING_SIZE) {
        g_tui.msg_ring_count++;
    }

    /* Also update status bar for immediate visibility */
    SAFE_STRNCPY(g_tui.status_message, message, sizeof(g_tui.status_message));
    g_tui.status_time = time(NULL);
    g_tui.needs_redraw = true;
}
```

---

## Control Flow Diagram

```
                    ┌─────────────────────────────────────────┐
                    │         LOG_INFO("message")             │
                    │         (or any LOG_* macro)            │
                    └─────────────────┬───────────────────────┘
                                      │
                                      ▼
                    ┌─────────────────────────────────────────┐
                    │   logger_log() in src/utils/logger.c    │
                    │   - Formats message with timestamp      │
                    │   - Thread-safe via mutex               │
                    └─────────────────┬───────────────────────┘
                                      │
                                      ▼
                    ┌─────────────────────────────────────────┐
                    │  Is LOG_DEST_CONSOLE enabled?           │
                    └────────┬─────────────────┬──────────────┘
                             │                 │
                         YES ▼             NO  ▼
                    ┌────────────────┐    ┌───────────────────┐
                    │ tui_is_active()│    │ Skip console      │
                    │    check       │    │ (file/syslog only)│
                    └───┬──────┬─────┘    └───────────────────┘
                        │      │
                 TRUE   ▼      ▼ FALSE
           ┌────────────────┐  ┌─────────────────────────────┐
           │tui_log_message │  │ fprintf(stdout/stderr)      │
           │ - Ring buffer  │  │ - With ANSI colors          │
           │ - Status bar   │  │ - Immediate flush           │
           │ - Trigger draw │  └─────────────────────────────┘
           └────────────────┘
```

---

## Findings by Category

### Category A: Direct Console Output

| # | File | Line | Code | Severity | Status |
|---|------|------|------|----------|--------|
| 1 | src/main.c | 569-576 | `printf("Usage...")` | INFO | Pre-TUI init, exits immediately |
| 2 | src/main.c | 580-582 | `printf("Version...")` | INFO | Pre-TUI init, exits immediately |
| 3 | src/main.c | 630 | `printf("\n")` | INFO | Pre-TUI init, before logger starts |
| 4 | src/main.c | 838 | `printf("...stopped.\n")` | INFO | Post-TUI shutdown, after logger stops |
| 5 | tests/test_main.c | 20-44 | `printf(...)` | INFO | Test code, TUI never runs |

**All findings are ACCEPTABLE** - They occur:
- Before TUI initialization (command-line help/version)
- After TUI shutdown (exit message)
- In test code that never runs TUI

### Category B: Logging Macro Usage

| Metric | Count |
|--------|-------|
| Total LOG_* macro uses | 472 |
| Files using LOG_* | 43 |

**Status:** ✅ All LOG_* macros route through `logger_log()` which properly checks `tui_is_active()`.

### Category C: Assertion/Panic Output

| Pattern | Count |
|---------|-------|
| `assert()` | 0 |
| `abort()` | 0 |
| `exit()` | Command-line help only |

**Status:** ✅ No assertions that could print to console.

### Category D: Debug Remnants

| Pattern | Count |
|---------|-------|
| `#ifdef DEBUG` | 0 |
| `#ifndef NDEBUG` | 0 |

**Status:** ✅ No debug-conditional code that writes to console.

### Category E: Library Error Messages

| Pattern | Usage | Status |
|---------|-------|--------|
| `sqlite3_errmsg()` | Always passed to LOG_ERROR | ✅ Safe |
| `CURLOPT_VERBOSE` | Not used | ✅ N/A |

### Category F: ncurses Output

| Pattern | Count | Status |
|---------|-------|--------|
| `wprintw()` | 317 | ✅ Writes to ncurses windows |
| `mvwprintw()` | Many | ✅ Writes to ncurses windows |
| `mvprintw()` | Many | ✅ Writes to stdscr (ncurses) |
| `endwin()` | 2 | ✅ Proper shutdown/resize handling |

**Status:** ✅ All ncurses functions write to window buffers, not console.

---

## Architecture Verification

### Lifecycle Analysis

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           APPLICATION LIFECYCLE                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  main() starts                                                               │
│       │                                                                      │
│       ├── print_version() ← printf() OK (TUI not started)                   │
│       ├── printf("\n")    ← OK (TUI not started)                            │
│       ├── logger_init()                                                      │
│       ├── LOG_INFO("Starting...") ← Goes to console (TUI not active)        │
│       │                                                                      │
│       ├── tui_init()                                                         │
│       │       └── g_tui.initialized = true ← TUI now active!                │
│       │                                                                      │
│       ├── tui_run()                                                          │
│       │       │                                                              │
│       │       └── [All LOG_* calls route through tui_log_message()]         │
│       │                                                                      │
│       ├── tui_shutdown()                                                     │
│       │       ├── g_tui.initialized = false ← TUI no longer active          │
│       │       └── endwin()                                                   │
│       │                                                                      │
│       ├── LOG_INFO("Shutdown...") ← Goes to console (TUI not active)        │
│       ├── logger_shutdown()                                                  │
│       └── printf("...stopped.\n") ← OK (logger stopped, TUI ended)          │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Thread Safety

The logger implementation is thread-safe:
- Uses `pthread_mutex_t` to protect shared state
- All LOG_* calls acquire mutex before writing
- TUI message ring buffer protected by same flow

---

## Files Summary

### Files with Console Output (All Acceptable)

| File | Pattern Count | Status |
|------|---------------|--------|
| src/main.c | 4 printf | ✅ Pre-init/post-shutdown only |
| tests/test_main.c | 10 printf | ✅ Test code, no TUI |
| src/utils/logger.c | 2 fprintf | ✅ Guarded by tui_is_active() |

### Files Using LOG_* Macros (All Safe)

| File | LOG_* Count |
|------|-------------|
| src/main.c | 73 |
| src/sensors/hardware/hw_interface.c | 38 |
| src/actuators/actuator_manager.c | 36 |
| src/profinet/profinet_callbacks.c | 22 |
| src/drivers/bus/gpio_hal.c | 22 |
| src/platform/board_detect.c | 20 |
| src/logging/data_logger.c | 20 |
| ... (35 more files) | ... |

---

## Verification Plan

### Manual Verification Steps

1. **Build in release mode:**
   ```bash
   make clean && make RELEASE=1
   ```

2. **Run with strace to monitor console writes:**
   ```bash
   strace -e write -o /tmp/writes.log ./water-treat 2>&1
   grep "write(1\|write(2" /tmp/writes.log
   ```

3. **Expected output during TUI operation:**
   - Only ncurses escape sequences to fd 1
   - No raw text writes to fd 1 or fd 2
   - All log messages appear in TUI status area

4. **Verify message routing:**
   - Trigger a LOG_ERROR in sensor code
   - Confirm message appears in TUI message ring (F7 - Logging page)
   - Confirm no console corruption

### Automated Test Script

```bash
#!/bin/bash
# verify_no_console_pollution.sh

# Start app in background, capture any stderr
./water-treat 2>/tmp/stderr.log &
PID=$!

# Wait for TUI to initialize
sleep 2

# Send some input to trigger logging
# (simulate sensor read failure, etc.)

# Check stderr for any unexpected output
if [ -s /tmp/stderr.log ]; then
    echo "FAIL: Unexpected stderr output during TUI operation"
    cat /tmp/stderr.log
    kill $PID 2>/dev/null
    exit 1
fi

kill $PID 2>/dev/null
echo "PASS: No console pollution detected"
```

---

## Recommendations

### Current Status: No Changes Required

The logger architecture is correctly implemented:

1. ✅ LOG_* macros call central `logger_log()` function
2. ✅ `logger_log()` checks `tui_is_active()` before console writes
3. ✅ When TUI active, messages route to `tui_log_message()`
4. ✅ Message ring buffer provides history in Logging page
5. ✅ Status bar shows most recent message
6. ✅ Thread-safe with mutex protection
7. ✅ All direct printf calls are in pre-init or post-shutdown code

### Best Practices for Future Development

1. **Always use LOG_* macros** - Never use printf/fprintf in operational code
2. **Test with TUI active** - Verify new logging doesn't corrupt display
3. **Check library output** - When adding new libraries, ensure they don't write to console
4. **Use LOG_TRACE for verbose output** - Can be filtered at runtime

---

## Audit Methodology

### Tools Used

1. `grep -rn "pattern" src/ --include="*.c"` - Pattern search
2. Manual code review of logger.c and tui_main.c
3. Control flow analysis from LOG_* to output

### Patterns Searched

| Category | Patterns |
|----------|----------|
| Direct output | `printf(`, `fprintf(stdout`, `fprintf(stderr`, `puts(`, `fputs(`, `perror(` |
| Logging | `LOG_DEBUG(`, `LOG_INFO(`, `LOG_WARNING(`, `LOG_ERROR(`, `LOG_FATAL(` |
| Debug | `#ifdef DEBUG`, `#ifndef NDEBUG` |
| Assertions | `assert(`, `abort(` |
| ncurses | `wprintw(`, `printw(`, `endwin(` |
| Libraries | `sqlite3_errmsg`, `CURLOPT_VERBOSE` |

### Files Analyzed

- `src/utils/logger.h` - Macro definitions
- `src/utils/logger.c` - Logger implementation
- `src/tui/tui_main.c` - TUI state management
- `src/main.c` - Application lifecycle
- All 43 files using LOG_* macros

---

## Conclusion

The Water-Treat RTU codebase implements a **correct and robust** TUI-aware logging architecture:

- **Zero violations** that could cause console pollution during TUI operation
- **Proper routing** of log messages through TUI message area when active
- **Thread-safe** implementation with mutex protection
- **Clean separation** of pre-init, operational, and post-shutdown phases

No remediation is required. The architecture serves as a good reference implementation for TUI-aware logging in ncurses applications.
