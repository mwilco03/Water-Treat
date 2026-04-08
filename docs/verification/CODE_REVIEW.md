# Sensor & Actuator Subsystem Code Review

**Scope**: `src/sensors/`, `src/actuators/`, `src/drivers/{adc,bus,digital}/`, `src/db/db_modules.c`, `src/db/db_actuators.c`, and the sensor/actuator data-flow functions in `src/profinet/profinet_manager.c`.

**Out of scope**: auth, user_sync, network detection, station name, PROFINET Connect/AR/DCP/Release/diagnosis-alarm/liveness, enrollment, bootstrap.sh, HTTP API, refactoring, naming, style.

**Severity**: `CRITICAL` = data loss, security, incorrect physical control. `HIGH` = functional defect, broken invariant. `MEDIUM` = edge case / suboptimal error handling. `LOW` = maintainability.

---

## CRITICAL findings

### [CRITICAL] correctness — DHT22/HX711 changes scheduling policy of the entire process
Location: `src/sensors/drivers/driver_dht22.c:122-123`, `src/sensors/drivers/driver_dht22.c:168-169`, `src/sensors/drivers/driver_hx711.c:113-114`, `src/sensors/drivers/driver_hx711.c:136-137`

```c
// driver_dht22.c
struct sched_param sp = {.sched_priority = 50};
sched_setscheduler(0, SCHED_FIFO, &sp);
...
sp.sched_priority = 0;
sched_setscheduler(0, SCHED_OTHER, &sp);
```

Defect: `sched_setscheduler(0, ...)` operates on the **calling process**, not the calling thread. From `sensor_worker_thread`, this elevates the **entire RTU process** (TUI thread, alarm manager, profinet cyclic thread, watchdog, db writer) to SCHED_FIFO 50 for the duration of the read (~25 ms for DHT22, several ms per read for HX711). Worse, when the function exits it drops the entire process to `SCHED_OTHER 0`, which silently demotes any other threads that were intentionally running at elevated priority (e.g., the PROFINET cyclic worker, which on a 1 ms cycle is timing-sensitive).

Impact: PROFINET cyclic timing is corrupted whenever a DHT22 or HX711 read runs. On Odroid-XU4 with the p-net 1 ms cycle target, a 25 ms SCHED_FIFO 50 burst from a sensor worker thread starves the PROFINET stack and can cause cycle overruns visible to the controller as missed input frames. The post-read `SCHED_OTHER 0` call also clears any priority the operator/init script set on the RTU process, breaking real-time tuning.

Recommendation: Replace both `sched_setscheduler` calls with `pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)` (and matching restore via the previously-saved policy/param from `pthread_getschedparam`). Per-thread, not per-process.

---

### [CRITICAL] data integrity — web_poll returns RESULT_OK with stale-or-zero value on every fetch failure
Location: `src/sensors/drivers/driver_web_poll.c:164-174`, `src/sensors/drivers/driver_web_poll.c:184-187`

```c
if (res != CURLE_OK) {
    LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
    free(chunk.memory);

    if (dev->cache_on_error) {
        *value = dev->last_value;
        return RESULT_OK;        // <-- masks the failure
    }
    return RESULT_ERROR;
}
...
} else if (dev->cache_on_error) {
    *value = dev->last_value;
    result = RESULT_OK;          // <-- same pattern, also masks failure
}
```

Defect: `cache_on_error` is initialized to `true` in `web_poll_init` (line 52). On the **first** call, `dev->last_value` is still zero from `memset`. So a web_poll sensor whose URL is unreachable returns `value=0.0` with `RESULT_OK`. `sensor_instance_read` then takes the success path (line 539 of `sensor_instance.c`): zeroes `consecutive_failures`, sets `connected = true`, calls `update_quality` which returns `QUALITY_GOOD` (since the value 0 falls in the default range `[-FLT_MAX, FLT_MAX]`). The cyclic update path then writes `0.0` to the PROFINET input buffer with IOPS=GOOD. On subsequent failures, the same masking continues — the controller will never see the sensor as failed, and no diagnosis alarm is sent.

Impact: A failing web_poll sensor is reported to the SCADA controller as a healthy sensor stuck at 0.0. For a level/pressure/pH input feeding a control loop, this is a worst-case silent fault: the loop reacts to a fabricated value with full confidence. No alarm, no watchdog, no degradation — just a wrong number.

Recommendation: Remove the `RESULT_OK` returns from both error paths. Either:
(a) return `RESULT_ERROR` and let `sensor_instance_read` propagate BAD quality and let the cyclic path send the cached `last_value` with IOPS=BAD (which it already does on lines 246-260 of `sensor_manager.c`), or
(b) keep the cache-on-error behavior but return a distinct status (e.g., `RESULT_STALE`) and update `sensor_instance_read` to treat it as a failed read for quality tracking while still using the cached value.
The first option is the smaller change and matches the cyclic update path's existing stale-data semantics.

---

### [CRITICAL] data integrity — actuator safe_state from DB is silently dropped, all actuators forced OFF on disconnect
Location: `src/actuators/actuator_manager.c:232-266` (`apply_safe_state`), `src/actuators/actuator_manager.c:867-881` (config conversion in `actuator_manager_reload`), `src/actuators/actuator_manager.h` (`actuator_config_t` definition)

```c
// actuator_manager.c:232 - apply_safe_state always forces OFF
static void apply_safe_state(actuator_manager_t *mgr) {
    ...
    for (int i = 0; i < mgr->actuator_count; i++) {
        actuator_instance_t *act = &mgr->actuators[i];
        if (act->state == ACTUATOR_STATE_ON) {
            ...
            act->state = ACTUATOR_STATE_OFF;
            act->pwm_duty = 0;
            apply_actuator_state(act);
            ...
        }
    }
    ...
}

// actuator_manager.c:867 - safe_state never copied from db_actuator_t to actuator_config_t
actuator_config_t config = {0};
config.id = db_act->id;
SAFE_STRNCPY(config.name, db_act->name, sizeof(config.name));
config.type = db_act->type;
config.profinet_slot = db_act->slot;
... (no safe_state assignment) ...
```

Defect: The `actuators` table has a `safe_state TEXT DEFAULT 'hold'` column with valid values `off`, `on`, `hold` (per `db_actuators.c::string_to_safe_state`). `db_actuator_get`/`db_actuator_list` correctly populates `db_actuator_t.safe_state`. But when `actuator_manager_reload` constructs `actuator_config_t` from `db_actuator_t`, the `safe_state` field is never copied — `actuator_config_t` does not even contain a `safe_state` field. `apply_safe_state` then unconditionally drives every ON actuator to OFF, regardless of how it was configured.

Impact: Three classes of actuators are misdriven on PROFINET disconnect:
1. **safe_state="hold"** (the database default for a fresh deploy): Operator intent is "freeze in current position." Code instead forces OFF after the safe-state timeout. A valve mid-stroke is dropped to closed.
2. **safe_state="on"**: Some chemical-dosing or fail-open valve installations require energized-on-fail. Code forces OFF. The valve closes when it must remain open.
3. **Latching solenoids**: Pulsing OFF is meaningless — the solenoid is mechanically latched. The code wastes a pulse that may also confuse interlock state.

For a water-treatment RTU, item (2) is the most dangerous: a chlorine pump configured to fail-open can be silently driven closed during a controller outage, with no diagnostic indication that the configured safe state was ignored.

Recommendation:
1. Add a `safe_state_t safe_state;` field to `actuator_config_t` in `actuator_manager.h`.
2. In `actuator_manager_reload` (line 867 area), add `config.safe_state = db_act->safe_state;`.
3. In `apply_safe_state` (line 232), branch on `act->config.safe_state`:
   - `SAFE_STATE_OFF`: current behavior (force OFF).
   - `SAFE_STATE_ON`: force ON, pwm_duty=100.
   - `SAFE_STATE_HOLD`: leave state and pwm_duty untouched (do not call `apply_actuator_state`); only set `safe_state_applied = true` and log.

---

### [CRITICAL] correctness — actuator interlock TOCTOU race allows two actuators in same group to activate simultaneously
Location: `src/drivers/digital/relay_output.c:236-272` (`output_set`)

```c
// Check interlock
if (on && drv->config.interlock_group) {
    if (!check_interlock_available(drv->config.interlock_id, drv)) {
        ...
        return RESULT_BUSY;
    }
}

// Apply output
...
result_t r = gpio_set_output(drv->config.gpio_pin, gpio_value);
...

if (old_state != drv->status.state) {
    ...
    if (on) {
        priv->on_start_time = now;
        if (drv->config.interlock_group) {
            register_interlock_active(drv->config.interlock_id, drv);  // <-- registered AFTER GPIO is on
        }
    }
    ...
}
```

Defect: `check_interlock_available` and `register_interlock_active` each take and release `g_interlock_mutex` independently. Between the check on line 238 and the register on line 271, **the mutex is not held**. Two threads (e.g., two PROFINET output handlers triggered by the same controller frame on different actuators in the same interlock group, or one PROFINET handler and one TUI manual_set) can both pass `check_interlock_available` and both call `gpio_set_output` before either calls `register_interlock_active`. The relay GPIOs are physically asserted simultaneously.

Impact: Safety-critical mutual exclusion is broken. For two pumps interlocked because they share a common discharge line, both can run simultaneously, possibly causing pressure surge. For two solenoids interlocked because they control opposing valves, both can open simultaneously, possibly causing flow short-circuit. The interlock mechanism gives a false sense of safety.

Recommendation: Combine check + register into a single critical section. Either:
(a) Add a new helper `try_acquire_interlock(group_id, drv)` that locks `g_interlock_mutex`, runs the check loop, and on success writes the active_output pointer before unlocking. Call this from `output_set` instead of the separate check/register pair. Then call the GPIO write only if the helper returned true.
(b) Or wrap the entire `output_set` body from line 218 to line 287 in `g_interlock_mutex`. Simpler but holds the mutex across GPIO sysfs I/O which can be ~100 µs.

Option (a) is the targeted fix.

---

### [CRITICAL] data integrity — relay output transient pulse on first activation for active_low actuators
Location: `src/drivers/digital/relay_output.c:39-81` (`gpio_set_output`), `src/drivers/digital/relay_output.c:183` (`output_create`)

```c
static result_t gpio_set_output(int pin, bool value) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    FILE *f = fopen(path, "w");
    if (!f) {
        // Try to export first
        FILE *exp = fopen("/sys/class/gpio/export", "w");
        if (exp) {
            fprintf(exp, "%d", pin);
            fclose(exp);
            // Set direction
            snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
            FILE *dir = fopen(path, "w");
            if (dir) {
                fprintf(dir, "out");   // <-- kernel asserts logical 0 here
                fclose(dir);
            }
            ...
            f = fopen(path, "w");      // <-- now we can write the desired value
        }
    }
    if (f) {
        fprintf(f, "%d", value ? 1 : 0);
        ...
```

Defect: Linux gpiolib sets a freshly-configured output line to logical 0 when `direction=out` is written via sysfs (the kernel does not have a "set direction with initial value" sysfs entry; that exists only in the chardev/libgpiod interface). For an `active_low` actuator, logical 0 means **electrically asserted** = relay closed = pump/valve ON. Between the `fprintf(dir, "out")` on line 57 and the `fprintf(f, "%d", 1)` on line 68 — separated by at least one fopen, two stdio buffer flushes, and a syscall — the relay is physically actuated. The window is on the order of hundreds of microseconds to a few milliseconds.

Impact: Every active_low relay/solenoid/pump has a transient pulse to ON during `output_create`. For a fast contactor or solenoid, the duration is typically below the mechanical response time and is invisible. For a fast SSR driving a pump's logic input, the pulse can be long enough to register a brief start command. For a digital input on a downstream PLC monitoring the actuator state, the pulse is visible. For a chemical dosing pump that takes any pulse as a dose, a single drop is delivered on every reload.

Recommendation: Use the `gpio_hal.c` libgpiod abstraction instead of the embedded sysfs path in `relay_output.c`. The libgpiod v2 API in `gpio_configure` (line 307-309) sets `GPIOD_LINE_VALUE_INACTIVE` atomically with the direction request, eliminating the window. Targeted fix in this file: add a fourth parameter `bool initial_value` to `gpio_set_output`, and on the export branch, write to `direction` the value `"high"` (active high init) or `"low"` (active low init) instead of `"out"`. The Linux sysfs gpiolib accepts `"high"` and `"low"` to set direction to output with an initial value. Replace `fprintf(dir, "out")` with `fprintf(dir, value ? "high" : "low")`.

---

### [CRITICAL] correctness — TCS34725 color register byte order swapped (big-endian read of little-endian device)
Location: `src/sensors/drivers/driver_tcs34725.c:119-124`, calling `i2c_read_word` from `src/sensors/hardware/hw_interface.c:75`

```c
// driver_tcs34725.c:119
if (i2c_read_word(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_CDATAL, &c) != RESULT_OK ||
    i2c_read_word(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_RDATAL, &r) != RESULT_OK ||
    ...

// hw_interface.c:75
*value = (buffer[0] << 8) | buffer[1];   // big-endian assembly
```

Defect: The TCS34725 datasheet specifies the color data registers as little-endian pairs: `xDATAL` (low byte at lower address) and `xDATAH` (high byte at next address). The driver requests the LOW register address and asks for two bytes via `i2c_read_word`, expecting the device to return `[L, H]`. But `i2c_read_word` then assembles the two bytes as **big-endian**: `(buffer[0] << 8) | buffer[1]` puts the low byte (intended LSB) into the high byte position. Every C/R/G/B reading is byte-swapped.

Impact: All color, lux, and color-temperature outputs from TCS34725 sensors are wrong. A reading of `0x0123` (decimal 291) is interpreted as `0x2301` (decimal 8961) — a 30x error. The downstream `tcs34725_calculate_color_temperature` and `tcs34725_calculate_lux` then operate on garbage. For any control or alarming on a TCS34725 sensor (e.g., turbidity-style analysis), the SCADA gets numerically meaningless values.

Recommendation: TCS34725 needs a little-endian read. Change the four lines in `driver_tcs34725.c:119-122` to use `i2c_read_bytes` and assemble manually:
```c
uint8_t buf[2];
if (i2c_read_bytes(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_CDATAL, buf, 2) != RESULT_OK) return RESULT_ERROR;
c = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
// repeat for r, g, b
```
Do not change `i2c_read_word` itself; ADS1115 (which is correctly big-endian) depends on the current behavior.

---

## HIGH findings

### [HIGH] correctness — ADC sensor double calibration (driver-internal scaling + sensor_instance scaling stacked)
Location: `src/sensors/sensor_instance.c:472-490`, `src/sensors/drivers/driver_mcp3008.c:159-175`, `src/sensors/drivers/driver_ads1115.c:218-230`

```c
// sensor_instance.c:472
case SENSOR_INSTANCE_ADC:
    if (instance->driver_handle) {
        switch (instance->driver_type) {
            case ADC_DRIVER_ADS1115:
                result = driver_ads1115_read(instance->driver_handle, &raw_value);
                break;
            case ADC_DRIVER_MCP3008:
                result = driver_mcp3008_read(instance->driver_handle, &raw_value);
                break;
            ...
        }
        if (result == RESULT_OK) {
            instance->current_raw_value = (int32_t)(raw_value * 1000);  // Store as mV
            raw_value = apply_calibration(instance, instance->current_raw_value);
        }
    }
```

```c
// driver_mcp3008.c:159 - already does raw->eng linear interpolation
static result_t mcp3008_driver_read(void *handle, float *value) {
    ...
    float normalized = (float)(raw - inst->raw_min) / (float)(inst->raw_max - inst->raw_min);
    float eng_value = inst->eng_min + normalized * (inst->eng_max - inst->eng_min);
    *value = driver_apply_calibration(inst, eng_value);
    return RESULT_OK;
}
```

```c
// driver_ads1115.c:218 - returns volts
static result_t ads1115_driver_read(void *handle, float *value) {
    ...
    float voltage;
    result_t r = ads1115_read_channel(&inst->device, inst->channel, &voltage);
    if (r != RESULT_OK) return r;
    *value = driver_apply_calibration(inst, voltage);   // already calibrated by driver_common
    return RESULT_OK;
}
```

Defect: For ADS1115 the driver returns calibrated **volts**. For MCP3008 the driver returns calibrated **engineering units** (raw_min/raw_max -> eng_min/eng_max as already configured in `driver_mcp3008_init`). Then `sensor_instance.c:488` casts the result to `mV` (multiplies by 1000), stores it in `current_raw_value`, and runs `apply_calibration(instance, current_raw_value)` which performs **a second linear interpolation** using the `instance->raw_min/raw_max/eng_min/eng_max` fields populated from `db_adc_sensor_t` in lines 231-234.

For MCP3008 this is a triple transformation: (1) driver does raw counts -> volts using its internal raw_min=0, raw_max=1023, eng_min=0, eng_max=vref; (2) driver applies its own calibration scale/offset; (3) sensor_instance multiplies by 1000 and calls `apply_calibration` with the SAME raw_min/raw_max but treating volts*1000 as if it were in the original raw-count domain — totally meaningless.

For ADS1115 the driver returns volts; sensor_instance converts to mV; then re-interprets that mV as a raw count and applies a SECOND linear interpolation.

Impact: ADC sensor readings are not in any consistent unit. The configured `raw_min`/`raw_max`/`eng_min`/`eng_max` fields in the `adc_sensors` table are silently misinterpreted. A user who configures ADS1115 with `raw_min=0`, `raw_max=32768`, `eng_min=0`, `eng_max=14` (pH scale, expecting raw ADC counts as input) will get a `normalized = (2500 - 0) / (32768 - 0) = 0.076`, `eng_value = 0.076 * 14 = 1.07` — pH 1.07 reported instead of the actual pH that 2.5V from the probe represents. Every ADC sensor configured through the TUI gives wrong outputs.

Recommendation: Decide on one calibration domain and stick to it. The simplest fix: in `sensor_instance.c:472-490`, remove lines 488-489 entirely. Trust the driver to return engineering units. The `raw_min`/`raw_max`/`eng_min`/`eng_max` fields in `db_adc_sensor_t` should be passed to the driver init function (`driver_mcp3008_init` already accepts these via `driver_mcp3008_set_scaling`; add a similar setter to `driver_ads1115`). Then `instance->current_raw_value` should track the actual raw ADC count returned by the driver via a new `*_read_raw` op, used only for diagnostics, not for re-applying calibration.

---

### [HIGH] correctness — `profinet_manager_add_module` reads/writes shared slot list without locking
Location: `src/profinet/profinet_manager.c:2053-2103`, compared with `src/profinet/profinet_manager.c:2130-2148` (`remove_slot`) and `src/profinet/profinet_manager.c:1283-1321` (`update_input`)

```c
result_t profinet_manager_add_module(void *mgr, int slot, uint32_t module_ident,
                                     int subslot, uint32_t submodule_ident,
                                     size_t input_len, size_t output_len) {
    UNUSED(mgr);
    if (g_pn.running) { LOG_WARNING(...); }

    /* Check if slot already exists ... */
    profinet_slot_t *s = find_slot(slot, subslot);   // <-- reads g_pn.slots, g_pn.slot_count, NO LOCK
    if (s) {
        s->module_ident = module_ident;              // <-- mutates slot, NO LOCK
        ...
        return RESULT_OK;
    }
    if (g_pn.slot_count >= MAX_PROFINET_SLOTS) { ... }
    s = &g_pn.slots[g_pn.slot_count++];              // <-- mutates slot_count, NO LOCK
    memset(s, 0, sizeof(*s));
    s->slot = slot; ...
    return RESULT_OK;
}
```

Defect: Every other function in `profinet_manager.c` that touches `g_pn.slots[]` and `g_pn.slot_count` acquires `g_pn.mutex` first (`update_input`, `set_input_iops`, `remove_slot`, `clear_app_slots`, `get_slot_list`). `add_module` is the only mutator that doesn't lock. `find_slot`, the static helper it calls, is not internally locked — every caller is supposed to hold the lock first.

This races with:
- The PROFINET cyclic stack thread reading slot data via `update_input`/`set_input_iops` (the controller is sending real cyclic frames at the negotiated period).
- `actuator_manager_reload` which calls `profinet_manager_remove_slot` from one thread while `sensor_manager_reload_sensors` calls `profinet_manager_add_module` from another, or while reloading itself iterates over the array.

Impact: Reload-while-running can torn-write `g_pn.slot_count`, leave a half-initialized slot visible to the cyclic thread, or skip a slot during the linear search. Symptoms include: input data going to the wrong slot for one frame, IOPS mis-set, occasional BAD frames sent for slots that are healthy. Hard to reproduce but capable of triggering controller-side diagnosis alarms.

Recommendation: Wrap the entire body of `profinet_manager_add_module` in `pthread_mutex_lock(&g_pn.mutex)` / `pthread_mutex_unlock(&g_pn.mutex)`. The function is not on a hot path (only called during reload), so the lock cost is negligible. Move the `g_pn.running` log message before the lock to avoid logging under lock.

---

### [HIGH] correctness — JSN-SR04T ultrasonic timing measured via sysfs GPIO is unusably inaccurate
Location: `src/sensors/drivers/driver_jsn_sr04t.c:73-117`

```c
// Wait for echo to go high
while (timeout_counter < max_timeout) {
    hwif_gpio_read(&dev->echo, &echo_state);   // <-- sysfs lseek+read, ~20-100 us per call
    if (echo_state) break;
    usleep(1);
    timeout_counter++;
}
clock_gettime(CLOCK_MONOTONIC, &start_time);
// Wait for echo to go low (end of pulse)
...
clock_gettime(CLOCK_MONOTONIC, &end_time);
long duration_us = (end_time.tv_sec - start_time.tv_sec) * 1000000L +
                   (end_time.tv_nsec - start_time.tv_nsec) / 1000L;
```

Defect: The driver measures the echo pulse width by polling a sysfs GPIO file in a loop, with `clock_gettime` taken before and after the polling loop for the high-pulse interval. Every `hwif_gpio_read` call invokes `lseek(fd, 0, SEEK_SET)` then `read(fd, buf, 4)` then a string compare — measured at 20-100 µs on Odroid-XU4 sysfs. The ultrasonic echo pulse width for the JSN-SR04T at 600 cm is ~35,000 µs; the timing resolution from this loop is on the same order as the smallest measurable pulse (20 cm ~ 1,200 µs).

Worse, the start_time is captured **after** the loop that waits for echo-high already exits. There's no per-iteration time check; the start_time captures a time stamp some unspecified number of microseconds into the echo-high state, not when the pulse started. The end_time has the same problem on the falling edge.

Impact: Distance readings are systematically biased and noisy. Two consecutive reads of a fixed target return values that differ by tens of centimeters. The sensor is unusable for tank level measurement (its primary application). The `range_min`/`range_max` check in the calling code may filter out the worst readings, but the in-range readings are still wrong.

Recommendation: This driver fundamentally cannot work without either (a) a real-time GPIO-edge interface like libgpiod's edge events with kernel timestamps, or (b) hardware capture timer (PWM input mode). Targeted fix without rewrite: drop driver registration so it cannot be selected from the TUI until rewritten, and document the limitation in the driver header. If the driver must remain selectable, change `jsn_sr04t_read_distance_cm` to call `hwif_gpio_wait_for_edge(&dev->echo, dev->timeout_us / 1000)` (which uses `poll(POLLPRI)` and is much closer to real edge timing), and capture `clock_gettime` immediately after each successful poll return.

---

### [HIGH] correctness — DHT22 driver hardcoded to BCM2835/BCM2711 will not work on Odroid-XU4 target
Location: `src/sensors/drivers/driver_dht22.c:17-58`, `src/sensors/drivers/driver_hx711.c:28-32`

```c
#define BCM2835_PERI_BASE   0x3F000000  // RPi 2/3
#define BCM2711_PERI_BASE   0xFE000000  // RPi 4
#define GPIO_BASE_OFFSET    0x200000

static volatile uint32_t *map_gpio(int *fd) {
    *fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    ...
    // detects only BCM2711 vs BCM2835
}
```

Defect: Both DHT22 and HX711 drivers `mmap` the BCM2835/BCM2711 GPIO physical address range. The Odroid-XU4 (Exynos5422) uses a completely different GPIO controller at a different physical address. `/dev/gpiomem` exists only on Raspberry Pi (provided by `bcm2835-gpiomem` kernel module). On Odroid these drivers will either:
- fail at `open("/dev/gpiomem")`, fall through to `/dev/mem`, mmap the wrong physical address, and crash with SIGBUS on first GPIO access; or
- on a kernel where `/dev/gpiomem` happens to exist with different semantics, scribble on whatever happens to be at offset 0x200000 from the mapped base.

Impact: Any DHT22 or HX711 sensor configured on the Odroid-XU4 RTU will crash the entire RTU process or corrupt unrelated memory. CLAUDE.md states "This codebase must run on ANY supported hardware platform. Never hardcode hardware-specific values." This violates that.

Recommendation: Replace the per-driver GPIO mmap with calls into `gpio_hal.c` (libgpiod). For DHT22, use `gpio_configure(pin, GPIO_DIR_INPUT, GPIO_PULL_UP)` then `gpio_read(pin, &state)` in the bit-banging loop. For HX711, use `gpio_configure` for SCK as output and DOUT as input. Bit-banging via libgpiod is slower than mmap (each call is a syscall) but works on every Linux board, and is the only legitimate way to support the documented Odroid-XU4 target. If the timing penalty makes HX711 unreliable, mark the driver unsupported on non-Pi targets in the TUI rather than silently providing a crashing implementation.

---

### [HIGH] correctness/security — integer overflow in `actuator_manager_reload` ms-to-sec conversion
Location: `src/actuators/actuator_manager.c:880`

```c
config.max_on_time_sec = (db_act->max_on_time_ms + 999) / 1000;
```

Defect: `db_actuator_t.max_on_time_ms` is `int`. `actuator_config_t.max_on_time_sec` is `int`. If `max_on_time_ms` >= `INT_MAX - 998` (i.e. roughly >= 2,147,482,649 ≈ 24.8 days), the addition `max_on_time_ms + 999` is signed integer overflow, undefined behavior in C. With `-Werror` and aggressive optimization (e.g., `-O2`), the compiler may assume the addition does not overflow and elide the bounds check, or compute a wrap-around small/negative value that then divides to a small `max_on_time_sec`. In the latter case the actuator's safety watchdog triggers far earlier than configured — possibly immediately — and the actuator is forced OFF in `check_safety_limits`.

The `max_on_time_ms` value is reachable from PROFINET acyclic record write 0xF843 (per CLAUDE.md the controller can write actuator config). A controller (or compromised network) can write `max_on_time_ms = INT_MAX` deliberately and trigger the overflow. This is a configurable disable-the-actuator vector.

Impact: Either (a) every reload of an actuator with a very large max_on_time_ms triggers undefined behavior, possibly disabling the actuator; or (b) a controller-driven attack makes legitimate actuators unusable. For valves controlling fail-safe water flow, this is an availability issue.

Recommendation: Validate the input range in `actuator_manager_reload`:
```c
if (db_act->max_on_time_ms < 0 || db_act->max_on_time_ms > 86400000) {
    LOG_WARNING("Actuator %s: max_on_time_ms out of range (%d), clamping to 24h",
                db_act->name, db_act->max_on_time_ms);
    config.max_on_time_sec = 86400;  /* clamp to 24h */
} else {
    config.max_on_time_sec = (db_act->max_on_time_ms + 999) / 1000;
}
```
Also add the same validation in the `db_actuator_create`/`db_actuator_update` paths to reject the bad input at write time, and in the PROFINET 0xF843 record handler.

---

### [HIGH] correctness — sensor_manager_reload calls profinet_manager_remove_slot with wrong subslot, removes nothing
Location: `src/sensors/sensor_manager.c:374-380`

```c
if (mgr->profinet_enabled) {
    for (int i = 0; i < mgr->instance_count; i++) {
        if (mgr->instances[i] && mgr->instances[i]->slot > 0) {
            profinet_manager_remove_slot(mgr->instances[i]->slot, 0);   // <-- subslot=0
        }
    }
}
```

Defect: Sensor instances are created with `subslot = module->subslot` (typically 1, the standard PROFINET application subslot). `profinet_manager_remove_slot` requires both slot AND subslot to match exactly:
```c
if (g_pn.slots[i].slot == slot && g_pn.slots[i].subslot == subslot)
```
Passing `subslot=0` therefore never matches. The reload's "remove old PROFINET slots" pass is a no-op. The subsequent `profinet_manager_add_module` calls with the correct subslot find the existing slot and update it in place, so end-to-end the function appears to work — but only by accident, and only because `add_module` accepts both insert and update semantics. A subtler symptom: if the new sensor configuration deletes a slot entirely (rather than reusing it), the old PROFINET slot persists with stale identifiers.

Impact: After a reload that deletes sensors (e.g., user removes a sensor from the TUI), the PROFINET slot list still contains the old slot, with the old `module_ident`. The controller sees a module that no longer corresponds to anything in the RTU, and the cyclic update path will never write fresh data to that slot. Eventually the controller times out or sends a misconfiguration alarm.

Recommendation: Fix the call: `profinet_manager_remove_slot(mgr->instances[i]->slot, mgr->instances[i]->subslot);`. Or, if all sensors always use subslot 1, use the constant 1. Don't pass 0.

---

### [HIGH] data integrity — sensor and actuator slot spaces overlap silently (no cross-table conflict check)
Location: `src/sensors/sensor_manager.c:430-466` (sensor slot registration), `src/actuators/actuator_manager.c:487-545` (actuator slot registration), `src/db/db_actuators.c:375-445` (`db_actuator_gpio_conflict_check` checks GPIO but not slot)

Defect: Sensors are stored with their slot in `modules.slot`. Actuators are stored with their slot in `actuators.slot`. Both populate the same PROFINET slot list via `profinet_manager_add_module`. There is no check anywhere — neither in the database layer, nor in `sensor_manager_reload_sensors`, nor in `actuator_manager_reload`, nor in the TUI dialogs — that prevents a sensor at slot N and an actuator at slot N from coexisting. If both exist, the second one to call `profinet_manager_add_module` for slot N finds the existing slot in `find_slot` (line 2070) and overwrites its `module_ident`, `submodule_ident`, `input_size`, and `output_size`. Whichever subsystem reloaded last "wins" the slot, and the other subsystem's PROFINET data path silently writes to the wrong slot's input/output buffer.

Impact: A misconfigured database (whether by user, by SCADA push via 0xF842/0xF843, or by manual SQLite edit) leads to a silently corrupted PROFINET module list. The controller may reject the connection due to module-ident mismatch, or worse, accept it and route sensor data into actuator memory and vice versa. For a slot configured as both a sensor (5-byte input) and an actuator (4-byte output), the cyclic update path writes 5 bytes into an output-only slot, overflowing whatever it lands in.

Recommendation: In `sensor_manager_reload_sensors`, before calling `profinet_manager_add_module` for each sensor, query `db_actuator_get_by_slot(mgr->db, instance->slot, &dummy)` and skip with `LOG_ERROR` if it returns RESULT_OK. Symmetric check in `actuator_manager_reload` against `db_module_get_by_slot`. Additionally, the TUI sensor and actuator wizards should query the opposite table during slot validation.

---

### [HIGH] correctness — sensor_manager_reload locks mgr mutex but profinet_manager_remove_slot/add_module take a separate mutex; ordering is unprotected
Location: `src/sensors/sensor_manager.c:370-518`

```c
result_t sensor_manager_reload_sensors(sensor_manager_t *mgr) {
    pthread_mutex_lock(&mgr->mutex);
    ...
    profinet_manager_remove_slot(mgr->instances[i]->slot, 0);  // takes g_pn.mutex
    ...
    profinet_manager_add_module(NULL, ...);                    // does NOT take g_pn.mutex (HIGH bug above)
    ...
    pthread_mutex_unlock(&mgr->mutex);
}
```

Defect: The two mutexes (`mgr->mutex` and `g_pn.mutex`) are taken in fixed order: first the sensor manager mutex, then transitively the profinet manager mutex inside the remove call. If anywhere else in the codebase the order is reversed (profinet first, then sensor manager), this is a deadlock. There is no annotation or convention enforcing the order. Also, with the `profinet_manager_add_module` lock-bug above fixed, this reload would acquire `g_pn.mutex` while holding `mgr->mutex` — fine here, but a hazard for any future code that takes `mgr->mutex` from a profinet callback.

Impact: A subtle deadlock waiting to happen on the next refactor. Lower priority because no current code path inverts the order, but worth flagging because the lock-ordering convention is undocumented.

Recommendation: Add a comment block at the top of `sensor_manager.c` and `profinet_manager.c` documenting the lock order: "Acquire `g_sensor_mgr->mutex` BEFORE `g_pn.mutex`. Never the other way around." Reviewers can then flag any inversion in future PRs.

---

### [HIGH] data integrity — sensor_worker holds mgr->mutex across blocking I/O for ALL sensors sequentially
Location: `src/sensors/sensor_manager.c:148-182`

```c
pthread_mutex_lock(&mgr->mutex);
for (int i = 0; i < mgr->instance_count && update_count < MAX_SENSOR_UPDATES; i++) {
    sensor_instance_t *instance = mgr->instances[i];
    if (!instance) continue;
    uint64_t now_ms = get_time_ms();
    uint64_t elapsed_ms = now_ms - instance->last_read_ms;
    if (elapsed_ms >= (uint64_t)instance->poll_rate_ms) {
        ...
        result_t result = sensor_instance_read(instance, &upd->value);   // <-- blocking I/O
        ...
    }
}
pthread_mutex_unlock(&mgr->mutex);
```

Defect: The sensor worker thread holds `mgr->mutex` across `sensor_instance_read` calls for every due sensor in the loop. Many drivers block:
- DS18B20: ~750 ms (kernel 1-Wire driver does not return until conversion completes)
- Web poll: up to 10 seconds (curl timeout)
- DHT22: ~25 ms (bit-banging GPIO)
- HX711: variable, can be 10s of ms while waiting for DOUT ready
- BME280: ~10 ms

While the worker holds `mgr->mutex`, any other thread that wants to read sensor state — TUI for live display, alarm manager for value checks, HTTP API for `/api/v1/slots`, profinet config sync — blocks for the cumulative duration of the slow-sensor reads. With one DS18B20 + one web_poll sensor, the lock can be held for 10+ seconds per cycle.

Impact: TUI freezes during sensor reads. HTTP `/api/v1/slots` may time out (it's read at controller startup, blocking the discovery handshake). Most importantly, the manager mutex is also taken by `sensor_manager_get_sensor_value` and `sensor_manager_reload_sensors`, so any reload during a slow sensor read waits for the read to complete — a config push from the SCADA controller via 0xF842 can take 10 seconds before applying.

Recommendation: Refactor the loop to copy what's needed under the lock, then read outside the lock. The existing `sensor_read_result_t updates[]` buffer pattern was clearly intended to enable this; the actual read just needs to move below the unlock. Specifically: under the lock, build a list of `(instance pointer, slot, subslot, last_value, prev_quality)` for sensors that are due. Unlock. Then call `sensor_instance_read` on each instance pointer (each instance has its OWN per-instance mutex `instance->mutex` which is taken inside `sensor_instance_read`, so the instance is still consistent). This eliminates the cumulative blocking of the manager mutex.

Note: this requires verifying that no code path destroys an instance while holding only `mgr->mutex` and not waiting for the worker — but since the worker is responsible for reads, and `sensor_manager_destroy`/`reload_sensors` must wait for the worker to be idle (or at least be aware), the existing `mgr->running` flag plus pthread_join in destroy is sufficient. Reload would need an additional "pause worker" handshake.

---

### [HIGH] correctness — `actuator_manager_reload` reads/mutates manager state without locking mgr->mutex
Location: `src/actuators/actuator_manager.c:802-898`

```c
result_t actuator_manager_reload(actuator_manager_t *mgr) {
    ...
    /* Iterate backwards so index shifts from actuator_manager_remove don't ... */
    for (int i = mgr->actuator_count - 1; i >= 0; i--) {
        int slot = mgr->actuators[i].config.profinet_slot;   // <-- read mgr->actuators, NO LOCK
        ...
        if (!found) {
            ...
            actuator_manager_remove(mgr, slot);              // <-- acquires lock internally
        }
    }
    ...
    for (int i = 0; i < db_count; i++) {
        ...
        if (find_actuator_by_slot(mgr, db_act->slot)) {      // <-- reads slot_map, NO LOCK
            continue;
        }
        ...
        r = actuator_manager_add(mgr, &config);              // <-- acquires lock internally
    }
}
```

Defect: `actuator_manager_reload` reads `mgr->actuators[]`, `mgr->actuator_count`, and `mgr->slot_map` without holding `mgr->mutex`. The watchdog thread (`watchdog_thread` line 268) holds `mgr->mutex` while iterating and mutating these same structures (e.g., `apply_safe_state` line 248 sets `act->state = ACTUATOR_STATE_OFF` and may shift entries via the indirect path through `apply_actuator_state`). The PROFINET output handler (`actuator_manager_handle_output` line 597) also locks and mutates. Reload races against both.

Impact: A reload concurrent with the watchdog can read a partially-shifted `mgr->actuators[]` array and either skip a real actuator (treating it as stale and removing it) or operate on a stale `profinet_slot` value (removing the wrong actuator from PROFINET while leaving it in the runtime list). After a few iterations the manager and PROFINET slot list disagree on which actuators exist.

Recommendation: Acquire `mgr->mutex` at the top of `actuator_manager_reload` and release at the bottom. Since `actuator_manager_remove` and `actuator_manager_add` lock internally, they would need a `_locked` variant — or, simpler, restructure reload to (a) under lock, build a snapshot of currently-loaded slots; (b) under lock still, compute the delta against `db_actuators`; (c) under lock still, perform all add/remove operations directly via internal helpers that assume the lock is held. This is more invasive than the other fixes; minimum-change alternative: take `mgr->mutex` only around the `mgr->actuators[i].config.profinet_slot` read on line 822 and around `find_actuator_by_slot` on line 861, and accept that the add/remove calls re-lock.

---

### [HIGH] correctness — Polynomial calibration `degree` field allows out-of-bounds read of coefficients[]
Location: `src/sensors/analog/analog_sensor.c:114-122`, `src/sensors/sensor_api.h:108-111`

```c
case CAL_TYPE_POLYNOMIAL: {
    float result = cal->polynomial.coefficients[0];
    float x_power = 1.0f;
    for (int i = 1; i <= cal->polynomial.degree; i++) {       // <-- no bounds check
        x_power *= raw;
        result += cal->polynomial.coefficients[i] * x_power;  // <-- coefficients[degree] can be >= MAX_CAL_COEFFICIENTS
    }
    return result;
}
```

```c
// sensor_api.h:108
struct {
    float coefficients[MAX_CAL_COEFFICIENTS];   // MAX_CAL_COEFFICIENTS = 6
    int degree;
} polynomial;
```

Defect: The loop iterates `i = 1 .. degree` inclusive, indexing `coefficients[i]`. Valid array indices are 0..5. If `degree >= 6`, the loop reads `coefficients[6]` and beyond — out-of-bounds read into the adjacent `degree` field and beyond. Currently the only producers of `CAL_TYPE_POLYNOMIAL` are the hardcoded preset `CAL_PRESET_TDS_GENERIC` (degree=3) and any future calibration set via `analog_calibrate`. There is no explicit clamp.

Impact: Today the bug is dormant because no production code path sets degree > 3. As soon as a calibration source (TUI calibration wizard, controller config push, or CSV import) sets degree=6 or higher, the function reads garbage and produces wrong results, potentially crashes if the read crosses an unmapped page, and is exploitable from any code path that lets an external value reach `polynomial.degree` without validation.

Recommendation: Add a clamp at the top of the case:
```c
case CAL_TYPE_POLYNOMIAL: {
    int deg = cal->polynomial.degree;
    if (deg < 0) deg = 0;
    if (deg >= MAX_CAL_COEFFICIENTS) deg = MAX_CAL_COEFFICIENTS - 1;
    float result = cal->polynomial.coefficients[0];
    float x_power = 1.0f;
    for (int i = 1; i <= deg; i++) { ... }
}
```
Also add the same validation in any function that writes `polynomial.degree`.

---

### [HIGH] correctness — TCS34725 division by zero in color_temperature, NaN cast to uint16_t is UB
Location: `src/sensors/drivers/driver_tcs34725.c:137-151`

```c
result_t tcs34725_calculate_color_temperature(tcs34725_data_t *data) {
    float x = (-0.14282f * data->r) + (1.54924f * data->g) + (-0.95641f * data->b);
    float y = (-0.32466f * data->r) + (1.57837f * data->g) + (-0.73191f * data->b);
    float z = (-0.68202f * data->r) + (0.77073f * data->g) + (0.56332f * data->b);

    float xc = x / (x + y + z);                        // <-- div by zero if all RGB are zero
    float yc = y / (x + y + z);

    float n = (xc - 0.3320f) / (0.1858f - yc);         // <-- div by zero if yc == 0.1858
    data->color_temp = (uint16_t)(449.0f * powf(n, 3) + 3525.0f * powf(n, 2) + 6823.3f * n + 5520.33f);
    // ^ Casting NaN/Inf to uint16_t is undefined behavior in C
    return RESULT_OK;
}
```

Defect: When the sensor is in darkness, `data->r/g/b` are all zero (or all the same small dark-current value). Then `x + y + z` can be zero or near-zero, producing `Inf` or `NaN` in `xc`/`yc`. The subsequent `(0.1858f - yc)` may also be zero. The final result is NaN/Inf, which is then cast to `uint16_t` — per C11 6.3.1.4, this is **undefined behavior**.

Impact: Reading a TCS34725 in darkness can produce undefined values in `data->color_temp`, possibly garbage values, and on some compilers/architectures may trigger a floating-point exception. On Odroid-XU4 the cast typically produces 0 silently, but the behavior is not guaranteed.

Recommendation: Add finite checks:
```c
float sum = x + y + z;
if (fabsf(sum) < 1e-6f) { data->color_temp = 0; return RESULT_ERROR; }
float xc = x / sum;
float yc = y / sum;
float yden = 0.1858f - yc;
if (fabsf(yden) < 1e-6f) { data->color_temp = 0; return RESULT_ERROR; }
float n = (xc - 0.3320f) / yden;
float ct = 449.0f * powf(n, 3) + 3525.0f * powf(n, 2) + 6823.3f * n + 5520.33f;
if (!isfinite(ct) || ct < 0 || ct > 65535) { data->color_temp = 0; return RESULT_ERROR; }
data->color_temp = (uint16_t)ct;
```

---

## MEDIUM findings

### [MEDIUM] error handling — sensor_instance_create_from_db leaks pthread mutex on error returns
Location: `src/sensors/sensor_instance.c:128-340`

```c
pthread_mutex_init(&instance->mutex, NULL);
result_t result = RESULT_OK;
if (strcmp(module->module_type, MODULE_TYPE_PHYSICAL) == 0) {
    ...
    if (db_physical_sensor_get(db, module->id, &sensor) != RESULT_OK) {
        LOG_ERROR("Failed to load physical sensor for module %d", module->id);
        return RESULT_ERROR;            // <-- mutex initialized but never destroyed
    }
    ...
}
```

Defect: `pthread_mutex_init` is called on line 128. Several early-return paths (lines 139, 226, 261, 280, 291, 326) return without `pthread_mutex_destroy(&instance->mutex)`. The caller (`sensor_manager_reload_sensors` line 469) does `free(instance)` directly. On glibc, an uninitialized pthread mutex in freed memory is not a fatal leak (the underlying state is just bytes), but it leaves Valgrind/helgrind reporting and is a portability hazard on platforms where mutex_init allocates.

Recommendation: At each early return path that comes after `pthread_mutex_init`, call `pthread_mutex_destroy(&instance->mutex)`. Or, restructure to only call `pthread_mutex_init` AFTER the type-specific db load and driver init have succeeded. The latter is the smaller change.

---

### [MEDIUM] correctness — actuator_manager.c shifts entries without updating slot_map for the removed slot when slot 0 actuator was at index N
Location: `src/actuators/actuator_manager.c:547-583`

```c
result_t actuator_manager_remove(actuator_manager_t *mgr, int profinet_slot) {
    ...
    if (idx >= 0 && idx < mgr->actuator_count) {
        destroy_actuator_driver(&mgr->actuators[idx]);
        mgr->slot_map[profinet_slot] = -1;
        for (int j = idx; j < mgr->actuator_count - 1; j++) {
            mgr->actuators[j] = mgr->actuators[j + 1];
            int shifted_slot = mgr->actuators[j].config.profinet_slot;
            if (shifted_slot >= 0 && shifted_slot <= ACTUATOR_MAX_SLOT) {
                mgr->slot_map[shifted_slot] = j;
            }
        }
        mgr->actuator_count--;
        ...
    }
}
```

Defect: The loop correctly updates `slot_map[shifted_slot] = j` for each shifted entry, but the **previous index** of that shifted entry — `j+1` — is not cleared. After the shift loop, `slot_map[]` for the OLD position of the last shifted entry still points to the (now stale) index `actuator_count - 1`. If two actuators happen to have the same `profinet_slot` (which is supposed to be impossible due to the duplicate-slot check in `actuator_manager_add`, but is a concern if the slot_map is consulted before `add` rejects the duplicate), the slot_map and the actuator array can disagree.

More concretely: imagine actuators at slots [10, 20, 30] in indices [0, 1, 2]. Remove slot 10: idx=0, clear slot_map[10]=-1. Shift: actuators[0] = actuators[1] (slot 20), set slot_map[20]=0. actuators[1] = actuators[2] (slot 30), set slot_map[30]=1. Loop ends. actuator_count becomes 2. **But slot_map[20] correctly points to 0, slot_map[30] correctly points to 1, and slot_map[10] is -1.** All three entries correct. The shift doesn't actually leave stale entries because each shifted slot's old index gets overwritten by the next shift.

Re-checking: after each shift, the slot_map for the source position (j+1) is the next destination position (j+1, overwritten on next iteration). So the only "stale" position is `actuator_count - 1` after the loop ends, but its slot_map entry is the same one we just set (it points to itself before the count decrement, then to the now-removed index). Wait — let me trace again. Start: actuators[0]=A(s10), [1]=B(s20), [2]=C(s30), count=3. Remove s10: idx=0, slot_map[10]=-1. j=0: actuators[0]=B, slot_map[20]=0. j=1: actuators[1]=C, slot_map[30]=1. Loop exits (j=2 fails j<count-1 check since count is still 3 and 2<2 is false). count becomes 2. slot_map[20]=0, slot_map[30]=1, slot_map[10]=-1. Indices 0 and 1 hold B and C. CORRECT.

Edge case: removing the LAST actuator (idx=count-1). The shift loop doesn't execute (j=count-1 fails j<count-1). slot_map[profinet_slot] is correctly cleared. count decrements. CORRECT.

So this is actually OK — I was wrong to flag it. **Withdrawing this finding.** Removing from report.

---

### [MEDIUM] correctness — relay_output.c sysfs path ignores libgpiod abstraction; duplicate GPIO subsystems
Location: `src/drivers/digital/relay_output.c:39-96`, `src/drivers/bus/gpio_hal.c` (libgpiod, separate)

Defect: The codebase has two parallel GPIO implementations: the libgpiod-based `gpio_hal.c` (used by sensor drivers via `hwif_gpio_*` and `gpio_*` functions) and the direct sysfs path embedded in `relay_output.c::gpio_set_output`. The sysfs path is deprecated since Linux 4.8 (per the comment in `gpio_hal.c:14`) and is missing several features the libgpiod path provides (atomic direction-with-value initialization, proper error reporting).

Impact: Future kernel removal of sysfs GPIO will break actuators while leaving sensors functional. More immediately, the inconsistency means actuator behavior is different from sensor GPIO behavior on the same board (e.g., different export semantics, different chip selection — sysfs always uses gpiochip0 implicitly).

Recommendation: Replace `relay_output.c::gpio_set_output` with calls to `gpio_configure(pin, GPIO_DIR_OUTPUT, GPIO_PULL_DISABLE)` followed by `gpio_write(pin, value)` from `gpio_hal.h`. The libgpiod path also fixes the active_low transient pulse (CRITICAL above) automatically since it sets the initial value atomically with the direction. Coordinate with the CRITICAL fix.

---

### [MEDIUM] error handling — `web_poll_fetch` never honors `dev->timeout_ms`, hardcodes 10 second timeout
Location: `src/sensors/drivers/driver_web_poll.c:155`

```c
curl_easy_setopt(dev->curl, CURLOPT_TIMEOUT, 10L);
```

Defect: The `db_web_poll_sensor_t` struct includes a `timeout_ms` field, populated from the database (`web_poll_sensors.timeout_ms`). `sensor_instance_create_from_db` reads it into `instance->timeout_ms` (line 265). But `web_poll_fetch` ignores it and hardcodes 10 seconds. The user's configured timeout is silently dropped.

Impact: A web_poll sensor with `timeout_ms = 500` (configured for a fast intranet endpoint) will still hang for 10 seconds on a network failure. Combined with the sensor_worker mutex-holding bug above, this means a single failing web_poll sensor can hang the TUI for 10 seconds. Combined with the cache_on_error CRITICAL bug above, the failure is also masked from the controller.

Recommendation: Pass timeout_ms through to web_poll. Add a setter `web_poll_set_timeout(dev, timeout_ms)` and call it from `sensor_instance_create_from_db` after `web_poll_init`. In `web_poll_fetch`, use `curl_easy_setopt(dev->curl, CURLOPT_TIMEOUT_MS, dev->timeout_ms ? dev->timeout_ms : 10000L)`.

---

### [MEDIUM] security — `web_poll` write_callback realloc has no upper bound
Location: `src/sensors/drivers/driver_web_poll.c:22-37`

```c
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memory_struct *mem = (struct memory_struct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);   // <-- unbounded growth
    if (!ptr) {
        LOG_ERROR("Out of memory");
        return 0;
    }
    ...
}
```

Defect: There is no maximum response size cap. A malicious or misconfigured server can stream gigabytes to the RTU, causing memory exhaustion. The URL is configurable from the controller via PROFINET record write 0xF842, so an attacker who can write controller config can point web_poll at a malicious server. Even without an attacker, a misconfigured public API that accidentally returns a large dataset can OOM the RTU.

Impact: Denial-of-service. Once the RTU process is OOM-killed, no PROFINET communication, no actuator safe-state, no logging.

Recommendation: Add a maximum response size, e.g., 64 KB for sensor JSON responses:
```c
#define WEB_POLL_MAX_RESPONSE_SIZE (64 * 1024)
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memory_struct *mem = (struct memory_struct *)userp;
    if (mem->size + realsize > WEB_POLL_MAX_RESPONSE_SIZE) {
        LOG_ERROR("Web poll response exceeds %d bytes, aborting", WEB_POLL_MAX_RESPONSE_SIZE);
        return 0;  /* curl treats this as an error */
    }
    ...
}
```

---

### [MEDIUM] error handling — `sensor_instance.c::apply_calibration` divides by zero on raw_max == raw_min after explicit check
Location: `src/sensors/sensor_instance.c:71-88`

```c
static float apply_calibration(sensor_instance_t *instance, int32_t raw_value) {
    if (instance->raw_max == instance->raw_min) {
        return (float)raw_value * instance->scale_factor + instance->offset;
    }
    float normalized = (float)(raw_value - instance->raw_min) /
                      (float)(instance->raw_max - instance->raw_min);
    ...
}
```

Defect: The early-return guards against integer division by zero by comparing `raw_max == raw_min` first. The fallback path uses `scale_factor` which defaults to 1.0 from `sensor_instance_create_from_db` line 118. **However**, `scale_factor` can later be set to 0 via the calibration setter (none currently exists, but the field is exposed via `instance->cal_scale = 1.0f` and others). When scale_factor is 0, `(float)raw_value * 0 + offset = offset`. Not divide-by-zero, but returns a constant — a silent calibration failure.

This is a sub-MEDIUM concern — flagging because the early-return path is currently unreachable (raw_min defaults to 0, raw_max defaults to 0, so for any sensor that doesn't explicitly set them, the early-return ALWAYS triggers and `scale_factor=1`, `offset=0` produces an identity passthrough). For ADC sensors that DO set raw_min/raw_max, the early return is skipped. So no current bug. But the function is fragile and the condition is an unreliable guard.

Recommendation: Document the contract on `raw_min/raw_max` (must satisfy `raw_max > raw_min` for valid calibration) and on `scale_factor` (must be nonzero), and validate at the input boundary (TUI dialogs and PROFINET config record handlers). No runtime change needed today since the bug above (CRITICAL — ADC double-calibration) makes this code largely irrelevant once fixed.

---

### [MEDIUM] correctness — `sensor_instance_read` for SENSOR_INSTANCE_CALCULATED is a no-op (returns last value)
Location: `src/sensors/sensor_instance.c:508-513`

```c
case SENSOR_INSTANCE_CALCULATED:
    // Handled by sensor_manager
    raw_value = instance->current_value;
    result = RESULT_OK;
    break;
```

Defect: The comment says "Handled by sensor_manager" but `sensor_manager.c` does NOT compute calculated sensors. The calculated-sensor evaluation function `sensor_instance_evaluate_calculated` exists (line 627) but is never called from `sensor_manager_worker_thread`. So a calculated sensor is registered, evaluated never, and reports its `current_value` (which is initialized to 0 by calloc in `sensor_manager_reload_sensors`) forever.

Impact: Calculated sensors return 0 with quality GOOD on every read. Combined with the CPU-temp sensor pattern that creates a default sensor at slot 1, a user who configures a calculated sensor (e.g., "average of slot 2 and slot 3 temperatures") will see a constant 0 reported via PROFINET. The downstream controller logic operating on the calculated value gets wrong inputs.

Recommendation: In `sensor_worker_thread`, after the per-sensor read pass, run a second pass that evaluates calculated sensors:
1. For each calculated sensor instance, gather the input values from the input slots via `mgr->slot_map[input_slot]->current_value` (which were just refreshed in the read pass above).
2. Call `sensor_instance_evaluate_calculated(instance, input_values, &result)`.
3. Store result into `instance->current_value`, update timestamp, mark connected, run `update_quality`.
4. Push to PROFINET via the same `update_input_with_quality` + `set_input_iops` pattern as physical sensors.

This is the smallest change that makes calculated sensors functional.

---

### [MEDIUM] error handling — `formula_evaluator_init` (TinyExpr path) leaks `vars` allocation when calloc returns OK but te_compile fails
Location: `src/sensors/formula_evaluator.c:53-72`

```c
te_variable *vars = calloc(variable_count, sizeof(te_variable));
for (int i = 0; i < variable_count; i++) { ... }

int error_pos;
eval->te_expr = te_compile(formula, vars, variable_count, &error_pos);
free(vars);                            // <-- always freed, OK

if (!eval->te_expr) {
    LOG_ERROR("Formula parse error at position %d: %s", error_pos, formula);
    formula_evaluator_destroy(eval);  // <-- destroy frees variable_names/values but not vars (already freed)
    return RESULT_ERROR;
}
```

Re-reading: vars is freed unconditionally on line 66 before the te_expr null check. So no leak. **Withdrawing.**

---

### [MEDIUM] data integrity — sensor `range_min`/`range_max` defaults of -FLT_MAX/+FLT_MAX skip range checking forever
Location: `src/sensors/sensor_instance.c:125-126`, `src/sensors/sensor_instance.c:51-57` (`update_quality`)

```c
// sensor_instance_create_from_db
instance->range_min = -FLT_MAX;
instance->range_max = FLT_MAX;
```

```c
// update_quality
if (instance->current_value < instance->range_min ||
    instance->current_value > instance->range_max) {
    instance->quality = QUALITY_UNCERTAIN;
    return;
}
```

Defect: For non-physical sensors (ADC, web_poll, calculated, static), `range_min`/`range_max` are never set anywhere from db_*_sensor_get, so they remain at -FLT_MAX/+FLT_MAX. The range check `current_value < -FLT_MAX || current_value > FLT_MAX` is always false (except for actual `inf`), so the sensor's range is effectively unbounded. Bad sensor data (e.g., ADS1115 returning a wrong byte order, MCP3008 returning a stuck-bit pattern) is reported as in-range, with quality GOOD if it's not also stale.

For physical sensors, `instance->range_min = -40.0f; instance->range_max = 125.0f;` is set ONLY for the CPU temp sensor (lines 96-97 of sensor_manager.c). All other physical sensors use the FLT_MAX defaults.

Impact: Sensor range validation is a passthrough for all configured sensors. Any wrong reading short of NaN is accepted with quality GOOD.

Recommendation: For each sensor type's create-from-db path, copy the min/max from the database struct into `instance->range_min`/`range_max`. For physical sensors, `db_physical_sensor_t.min_value/max_value` (lines 311-312 of `db_modules.c`) — these are populated. For ADC, `db_adc_sensor_t.eng_min/eng_max` are the engineering-unit bounds. For web_poll, no min/max field exists in the DB schema; either add one or leave the unconstrained default with documentation.

---

### [MEDIUM] correctness — `find_thermal_zone` snprintf size hardcoded as 128 instead of using sizeof
Location: `src/sensors/sensor_manager.c:41-50`

```c
static bool find_thermal_zone(char *path_out) {
    for (int i = 0; i < THERMAL_ZONE_MAX; i++) {
        snprintf(path_out, 128, "%s%d/temp", THERMAL_ZONE_BASE, i);   // <-- hardcoded 128
        if (access(path_out, R_OK) == 0) { ... }
    }
}
```

Defect: The function takes a `char *path_out` with no size parameter. The snprintf size is hardcoded as 128. Caller (`create_cpu_temp_sensor` line 60) declares `char thermal_path[128];` which matches by convention but is not enforced by the type system. If a future caller passes a smaller buffer, snprintf will write past it.

Impact: Latent buffer overflow risk if the function is called from another context. Currently safe.

Recommendation: Take a size parameter: `static bool find_thermal_zone(char *path_out, size_t path_size)`. Use `snprintf(path_out, path_size, ...)`. Caller passes `sizeof(thermal_path)`.

---

### [MEDIUM] correctness — `formula_evaluator` simple-fallback `simple_eval` infers operator type by `strstr` matching, can misinterpret formulas
Location: `src/sensors/formula_evaluator.c:178-242`

```c
/* Average: "(a + b + ...) / N" or "avg(...)" */
if (strstr(formula, "avg") || (strstr(formula, "+") && strstr(formula, "/"))) {
    float sum = 0.0f;
    for (int i = 0; i < count; i++) sum += values[i];
    return sum / (float)count;
}
/* Sum: "a + b + ..." (no other operators) */
if (strstr(formula, "+") && !strstr(formula, "-") && !strstr(formula, "*") && !strstr(formula, "/")) { ... }
/* Multiply / scale: "a * b" or "a * constant" */
if (strstr(formula, "*") && ...) { ... }
```

Defect: The fallback evaluator uses `strstr` to recognize formula types. A formula like `x0 - x1 + x2` (sum-with-subtraction) is not matched by any branch and falls through to `return values[0]` — only the first variable is used. A formula like `x0 / x1` matches the avg branch (because it contains `/` and has `+` somewhere... wait, no, `+` must also be present. `x0 / x1` does NOT have `+` so it doesn't match avg. It doesn't match sum, multiply, min, or max. Falls through to first value).

Impact: When TinyExpr is not built in, calculated sensors with non-trivial formulas silently produce wrong results. The user thinks they have a working calculation; they have a passthrough of x0.

Recommendation: This fallback implementation cannot correctly evaluate arbitrary formulas. Either (a) make TinyExpr a hard build dependency and remove the fallback entirely, or (b) document that the fallback only supports specific exact patterns (e.g., `avg(x0,x1,...)`, `sum(...)`, etc.) and reject any formula that doesn't match those exact patterns rather than silently returning x0. Option (a) is the simpler fix; the entire fallback file branch should be deleted and the build should fail without TinyExpr.

---

### [MEDIUM] error handling — `db_sensor_log_cleanup` builds DELETE SQL via snprintf (no parameter binding for retention_days)
Location: `src/db/db_modules.c:842-858`

```c
result_t db_sensor_log_cleanup(database_t *db, int retention_days) {
    ...
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM sensor_data_log WHERE timestamp < datetime('now', '-%d days');", retention_days);
    ...
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    ...
}
```

Defect: This is the one SQL string formatted from a parameter rather than bound. `retention_days` is `int` and validated `> 0` on line 844, so SQL injection is not directly possible (an int formatted as `%d` cannot inject). But it's the only place in the db layer that bypasses parameter binding, and it sets a bad precedent. If `retention_days` ever becomes user-controllable as a string, this becomes injectable.

Impact: Currently safe due to the int type and >0 validation. Future hazard.

Recommendation: Use a parameterized statement:
```c
const char *sql = "DELETE FROM sensor_data_log WHERE timestamp < datetime('now', ?);";
sqlite3_stmt *stmt;
if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
char interval[32];
snprintf(interval, sizeof(interval), "-%d days", retention_days);
sqlite3_bind_text(stmt, 1, interval, -1, SQLITE_TRANSIENT);
int rc = sqlite3_step(stmt);
sqlite3_finalize(stmt);
```

---

## LOW findings

### [LOW] maintainability — `SAFE_STRNCPY` macro evaluates `src` argument multiple times
Location: `include/common.h:141-150`

```c
#define SAFE_STRNCPY(dst, src, size) do { \
    if ((src) != NULL) { \
        size_t _srclen = strlen(src); \
        size_t _copylen = (_srclen < (size) - 1) ? _srclen : (size) - 1; \
        memcpy((dst), (src), _copylen); \
        (dst)[_copylen] = '\0'; \
    } else { \
        (dst)[0] = '\0'; \
    } \
} while(0)
```

Defect: `src` is referenced four times (NULL check, strlen, memcpy, and the not-NULL branch). When passed `(const char*)sqlite3_column_text(stmt, N)`, the function is called four times. Each call returns the same pointer (sqlite caches it), so it's safe in this case, but the pattern is fragile. Changing the macro to use a temporary local variable would prevent the multi-evaluation.

Recommendation: Cache `src` in a local:
```c
#define SAFE_STRNCPY(dst, src, size) do { \
    const char *_src = (src); \
    if (_src != NULL) { \
        size_t _srclen = strlen(_src); \
        size_t _copylen = (_srclen < (size) - 1) ? _srclen : (size) - 1; \
        memcpy((dst), _src, _copylen); \
        (dst)[_copylen] = '\0'; \
    } else { \
        (dst)[0] = '\0'; \
    } \
} while(0)
```

---

### [LOW] maintainability — `driver_web_poll.c::parse_json_value` uses non-thread-safe `strtok`
Location: `src/sensors/drivers/driver_web_poll.c:93-102`

```c
char *token = strtok(path_copy, ".");
while (token != NULL) {
    current = cJSON_GetObjectItem(current, token);
    ...
    token = strtok(NULL, ".");
}
```

Defect: `strtok` uses static state. If `web_poll_fetch` is ever called from multiple threads concurrently (currently it isn't — only the sensor_worker calls it), the parses corrupt each other's state. The reentrant `strtok_r` is used elsewhere in the codebase (e.g., `sensor_instance.c:304`).

Recommendation: Replace with `strtok_r(path_copy, ".", &saveptr)` to make the function reentrant.

---

### [LOW] maintainability — sensor_instance.c hardcodes module_type strings instead of using MODULE_TYPE_* constants
Location: `src/sensors/sensor_instance.c:274`, `src/sensors/sensor_instance.c:286`

```c
} else if (strcmp(module->module_type, "static") == 0) {
...
} else if (strcmp(module->module_type, "calculated") == 0) {
```

Other branches use `MODULE_TYPE_PHYSICAL`, `MODULE_TYPE_ADC`, `MODULE_TYPE_WEB_POLL` constants. The "static" and "calculated" strings are hardcoded inline. If the database schema changes the spelling (e.g., to lowercase-with-hyphens), the constants are updated in one place but these two strings will silently mismatch and the sensor type will be unrecognized.

Recommendation: Add `MODULE_TYPE_STATIC` and `MODULE_TYPE_CALCULATED` constants in the same header that defines the others, and use them here.

---

## Severity summary

`CRITICAL: 6 | HIGH: 11 | MEDIUM: 9 | LOW: 3`
