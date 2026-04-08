# Reality Check — Sensor & Actuator Subsystem Audit

**Mode:** AUDIT
**Scope:** `src/sensors/`, `src/actuators/`, `src/drivers/`,
sensor/actuator TUI dialogs and pages, `include/gsdml_modules.h`,
sensor/actuator data flow in `src/profinet/profinet_manager.c`,
sensor/actuator paths in `src/db/db_modules.c` and `src/db/db_actuators.c`.
**Date:** 2026-04-08
**Auditor:** RealityChecker

**Ground truth used:**
- DB schema as supplied in the audit instructions (verified live on RTU
  192.168.6.21).
- `gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml` for module ident
  cross-checks.
- `include/constants.h` and `include/gsdml_modules.h` for symbol
  resolution.
- p-net pinned tag is `v0.2.0` (`scripts/install-deps.sh:626`).
- p-net headers are NOT present in the repo and `docs/verification/SOURCES.md`
  does not exist yet, so all `pnet_*` API verifications are UNVERIFIED with
  closure criterion noted on each finding.

---

## Section 1 — HALLUCINATIONs

### [HALLUCINATION] H1 — `adc_driver.h` API has no implementation file
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/drivers/adc/adc_driver.h:81-117`
  Source:   Filesystem — there is no `.c` file in `src/drivers/adc/`.
  Evidence: `adc_driver.h` declares `adc_create`, `adc_destroy`, `adc_read_raw`,
            `adc_read_voltage`, `adc_set_gain`, `adc_get_resolution`,
            `adc_get_vref`, `adc_get_channels`, `adc_create_backend`. The
            directory listing of `src/drivers/adc/` contains only the header.
            Grep for the symbols in `src/` returns only the header itself.
  Reasoning: Every entry point of the `adc_backend_t` API is a dangling
             extern that links to nothing. Any caller would fail to link.

### [HALLUCINATION] H2 — Calculated sensor configuration is unreachable from the TUI
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/tui/dialogs/dialog_sensor.c:42`
  Source:   Same file, fields list at `dialog_sensor.c:78-83`, save paths
            at `dialog_sensor.c:264-293` and `dialog_sensor.c:446-475`;
            `src/tui/dialogs/dialog_io_wizard.c` has no calculated path.
  Evidence: `static const char *sensor_types[] = {"physical", "adc",
            "web_poll", "calculated", "static"};` advertises five types,
            but the dialog form has no `formula` or `input_sensors` field
            and the save path only handles `"physical"` and `"adc"`. The
            wizard never enters `WIZ_STATE_*` for calculated either.
  Reasoning: The "calculated" option is offered to the user but the dialog
             cannot collect a formula or persist it — selecting it creates
             a `modules` row with no companion `calculated_sensors` row.

### [HALLUCINATION] H3 — `pump_*` and `solenoid_*` driver APIs declared with no implementation and no callers
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/drivers/driver_pump.h:16-21`
            `/mnt/cephfs/shared/projects/Water-treat/src/sensors/drivers/driver_solenoid.h`
  Source:   Grep across `src/` for `pump_init|pump_start|pump_stop|pump_set_speed|solenoid_init|solenoid_open|solenoid_close` finds matches only inside the two header files themselves.
  Evidence: `driver_pump.h:16-21` declares `pump_init/destroy/start/stop/
            set_speed/get_state` against a `water_pump_t` struct. No `.c`
            file defines them and no in-scope code includes either header.
  Reasoning: Pure dead-letter declarations. Anything that ever links these
            in will fail with unresolved symbols.

### [HALLUCINATION] H4 — `sensor_api.h` driver vtable API has no implementation
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/sensor_api.h:260-288`
  Source:   Grep across `src/` for the symbols; only `analog_sensor.c`
            implements `sensor_apply_calibration`, `sensor_channel_name`,
            and `sensor_status_string`.
  Evidence: Header declares `sensor_create`, `sensor_destroy`, `sensor_read`,
            `sensor_read_cached`, `sensor_write`, `sensor_calibrate`,
            `sensor_set_two_point_cal`, `sensor_register_driver`,
            `sensor_unregister_driver`. None of these are defined in any
            `.c` file under `src/`.
  Reasoning: A complete public API is declared with zero implementations.
             It is also never called by in-scope code, so it does not break
             the link, but it advertises functionality that does not exist.

### [HALLUCINATION] H5 — `analog_sensor` "preset calibrations" public API is dead
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/analog/analog_sensor.h:79-119`
  Source:   Grep `analog_sensor_create|analog_sensor_set_adc|analog_sensor_cal_point|analog_sensor_read_raw|analog_sensor_read_voltage|analog_sensor_factory|CAL_PRESET_PH_GENERIC|CAL_PRESET_TDS_GENERIC|CAL_PRESET_TURBIDITY_GENERIC|CAL_PRESET_PRESSURE_100PSI|CAL_PRESET_4_20MA` in `src/`.
  Evidence: All defined in `analog_sensor.c` but never called from outside
            that file. The whole `adc_backend_t` plumbing they consume
            (see H1) is also empty.
  Reasoning: An entire pH/TDS/turbidity/pressure analog-sensor framework
             exists in the headers and source but is wired to nothing. A
             reader looking at the headers would believe these features are
             available; they are not.

---

## Section 2 — MISMATCHes

### [MISMATCH] M1 — `actuator_type_to_string()` cannot encode PUMP or VALVE
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/db/db_actuators.c:10-18`
  Source:   `include/constants.h:297-300` (only RELAY/PWM/LATCHING/MOMENTARY
            string macros exist; no `_PUMP_STR` or `_VALVE_STR`) and
            `db_actuators.h:10-17` (enum has PUMP=4 and VALVE=5).
  Evidence: ```c
            const char* actuator_type_to_string(actuator_type_t type) {
                switch (type) {
                    case ACTUATOR_TYPE_RELAY:    return ACTUATOR_TYPE_RELAY_STR;
                    case ACTUATOR_TYPE_PWM:      return ACTUATOR_TYPE_PWM_STR;
                    case ACTUATOR_TYPE_LATCHING: return ACTUATOR_TYPE_LATCHING_STR;
                    case ACTUATOR_TYPE_MOMENTARY:return ACTUATOR_TYPE_MOMENTARY_STR;
                    default: return "unknown";
                }
            }
            ```
  Reasoning: Inserting a PUMP (4) or VALVE (5) actuator binds the literal
             `"unknown"` into `actuators.type`. Round-tripping back through
             `string_to_actuator_type("unknown")` returns RELAY (the
             default), so PUMP/VALVE silently degrade to RELAY across
             every reload. The wizard at `dialog_io_wizard.c:1816-1819`
             does set these enum values, so this is reachable.

### [MISMATCH] M2 — `string_to_actuator_type()` does not recognize PUMP or VALVE either
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/db/db_actuators.c:29-35`
  Source:   Same file's reverse function table; `db_actuators.h:10-17`
            for the canonical enum.
  Evidence: ```c
            static actuator_type_t string_to_actuator_type(const char *str) {
                if (!str) return ACTUATOR_TYPE_RELAY;
                if (strcmp(str, ACTUATOR_TYPE_PWM_STR) == 0)       return ACTUATOR_TYPE_PWM;
                if (strcmp(str, ACTUATOR_TYPE_LATCHING_STR) == 0)  return ACTUATOR_TYPE_LATCHING;
                if (strcmp(str, ACTUATOR_TYPE_MOMENTARY_STR) == 0) return ACTUATOR_TYPE_MOMENTARY;
                return ACTUATOR_TYPE_RELAY;
            }
            ```
  Reasoning: Even if a row's `type` column happened to contain `'pump'` or
             `'valve'` (e.g. inserted by another tool), this loader would
             coerce it to RELAY. Asymmetric with the enum's stated values.

### [MISMATCH] M3 — Sensor GPIO conflict check ignores `'Float Switch'`
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/db/db_actuators.c:421-424`
  Source:   `src/tui/dialogs/dialog_sensor.c:268` (writes the literal
            `"Float Switch"` to `physical_sensors.sensor_type`) and
            `src/sensors/sensor_instance.c:201-202` (dispatches on both
            `"Float Switch"` and `"FLOAT_SWITCH"`).
  Evidence: ```sql
            SELECT m.name FROM physical_sensors ps
            JOIN modules m ON ps.module_id = m.id
            WHERE ps.address = ? AND ps.sensor_type IN
                  ('DHT22','DHT11','FLOAT_SWITCH','GPIO');
            ```
            Stored value: `"Float Switch"` (mixed case, with space).
  Reasoning: The IN-list mismatches the actual stored string, so a float
             switch sensor never registers a GPIO conflict against a new
             actuator on the same pin. The check protects DHT22/DHT11 but
             leaves float switches unguarded.

### [MISMATCH] M4 — `physical_sensors.hardware_type` always equals `sensor_type`
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/tui/dialogs/dialog_sensor.c:267-268`
            `/mnt/cephfs/shared/projects/Water-treat/src/tui/dialogs/dialog_sensor.c:449-450`
            `/mnt/cephfs/shared/projects/Water-treat/src/tui/dialogs/dialog_io_wizard.c:1699-1700,1722-1724,1776-1777`
  Source:   Verified DB schema for `physical_sensors` lists both columns
            as distinct fields.
  Evidence: ```c
            SAFE_STRNCPY(phys.sensor_type,   form->sensor_type, ...);
            SAFE_STRNCPY(phys.hardware_type, form->sensor_type, ...);
            ```
  Reasoning: Every code path that creates or edits a physical sensor binds
             the same string into both columns. The schema separates them
             (presumably one is the user-facing "what is it measuring",
             the other is the chip family) but the implementation
             collapses them. Anyone querying `hardware_type` for dispatch
             will get whatever the user typed in the "Type" picker.

### [MISMATCH] M5 — `db_web_poll_sensor_t` has fields with no schema column
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/db/db_modules.h:53-65`
  Source:   Verified DB schema:
            `web_poll_sensors (id, module_id, url, method, headers,
            json_path, poll_rate_ms, timeout_ms)`. No `auth_type`,
            `auth_token`, or `unit` columns.
  Evidence: Struct lists `char auth_type[16]; char auth_token[256]; char
            unit[16];`. Neither
            `db_web_poll_sensor_create` (`db_modules.c:454-479`) nor
            `db_web_poll_sensor_get` (`db_modules.c:481-507`) bind or
            extract these fields.
  Reasoning: Phantom fields. Reading the struct gives the impression that
             auth and unit are tracked; the database neither stores nor
             returns them. Any caller that sets `sensor->auth_token` and
             expects it to be persisted will silently lose the data.

### [MISMATCH] M6 — Wizard's `actuator_types[].actuator_type` ints encode the wrong enum values
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/tui/dialogs/dialog_io_wizard.c:175-180`
  Source:   `src/db/db_actuators.h:10-17` enum: RELAY=0, PWM=1, LATCHING=2,
            MOMENTARY=3, PUMP=4, VALVE=5.
  Evidence: ```c
            static const actuator_type_def_t actuator_types[] = {
                {"Pump",            "...", 0},   /* claims = RELAY */
                {"Valve / Solenoid","...", 5},   /* = VALVE, OK */
                {"Generic Relay",   "...", 0},   /* = RELAY, OK */
            };
            ```
            The "Pump" entry's `actuator_type` field is `0` (RELAY),
            not `4` (PUMP).
  Reasoning: The field is inconsistent with its labels. It is not
             currently exercised at runtime (`save_actuator()` at
             `dialog_io_wizard.c:1816-1822` ignores it and instead
             switches on the array index), but it is a latent bug for
             anyone who wires it up. The "third column" lies about what
             it represents.

### [MISMATCH] M7 — `page_actuators.c` type-display switch only handles PUMP/VALVE/RELAY, drops LATCHING/MOMENTARY/PWM
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/tui/pages/page_actuators.c:99-111`
  Source:   `src/db/db_actuators.h:10-17` (six enum values defined).
  Evidence: ```c
            switch (actuators[i].type) {
                case ACTUATOR_TYPE_PUMP:  ...
                case ACTUATOR_TYPE_VALVE: ...
                case ACTUATOR_TYPE_RELAY: ...
                default: SAFE_STRNCPY(a->type, "Unknown", ...);
            }
            ```
  Reasoning: PWM, LATCHING, MOMENTARY actuators all render as `"Unknown"`
             in the page. They are valid configurations the system can
             persist but the page lies about what they are.

### [MISMATCH] M8 — `gsdml_sensor_module_from_string()` ignores the `DRIVER_NAME_*` constants
  Code:     `/mnt/cephfs/shared/projects/Water-treat/include/gsdml_modules.h:158-202`
  Source:   `include/constants.h:264-279` defines `DRIVER_NAME_DS18B20`,
            `DRIVER_NAME_BME280`, `DRIVER_NAME_HX711`, etc.
  Evidence: The function does substring matches on hand-rolled string
            literals (`"DS18B20"`, `"BME"`, `"HX711"`, `"JSN"`, `"DHT"`)
            instead of comparing against the canonical
            `DRIVER_NAME_*` macros next door in `constants.h`.
  Reasoning: Two parallel sources of truth for hardware-type strings.
             A future rename of e.g. `DRIVER_NAME_BME280` will not flow
             into this dispatcher. Currently both happen to agree, but
             the coupling is invisible.

### [MISMATCH] M9 — Constants `DRIVER_NAME_FLOAT_SWITCH`, `DRIVER_NAME_DHT11`, `DRIVER_NAME_WEB_POLL`, `DRIVER_NAME_ADS1015` are never used by any in-scope code
  Code:     `/mnt/cephfs/shared/projects/Water-treat/include/constants.h:266,271,278,279`
  Source:   Grep across `src/` finds zero references to these macros.
  Evidence: `DRIVER_NAME_FLOAT_SWITCH` is `"FloatSwitch"` (CamelCase, no
            space) but the actual stored sensor_type is `"Float Switch"`
            (`dialog_sensor.c:48`). The constant and the value disagree
            and the constant is unreferenced.
  Reasoning: The constants advertise a contract that the code does not
             keep. Anyone refactoring with these macros would create a
             second-string-literal bug.

### [MISMATCH] M10 — `sensor_instance_create_from_db()` accepts `"DHT11"` but `dialog_sensor.c` never offers it
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/sensor_instance.c:149-150`
            `/mnt/cephfs/shared/projects/Water-treat/src/tui/dialogs/dialog_sensor.c:44-49`
  Source:   The hardware-type list in dialog_sensor.c lists DHT22 but
            not DHT11.
  Evidence: ```c
            } else if (strcmp(sensor.sensor_type, DRIVER_NAME_DHT22) == 0 ||
                       strcmp(sensor.sensor_type, "DHT11") == 0) {
            ```
  Reasoning: The driver dispatch claims DHT11 support but the picker
             never offers it. Either DHT11 should be removed from the
             dispatch as dead code or added to the picker.

### [MISMATCH] M11 — `sensor_instance_create_from_db()` references hardware variants the picker exposes only via mixed-case literals
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/sensor_instance.c:201-202`
            `/mnt/cephfs/shared/projects/Water-treat/src/sensors/sensor_instance.c:237-238`
  Source:   `src/tui/dialogs/dialog_sensor.c:44-61` (only `"ADS1115"`,
            `"MCP3008"`, `"Float Switch"` etc. are written).
  Evidence: ```c
            } else if (strcmp(sensor.sensor_type, "Float Switch") == 0 ||
                       strcmp(sensor.sensor_type, "FLOAT_SWITCH") == 0) {
            ...
            if (strcmp(sensor.adc_type, DRIVER_NAME_ADS1115) == 0 ||
                strcmp(sensor.adc_type, "ADS1015") == 0) {
            ```
            The TUI never writes `"FLOAT_SWITCH"` or `"ADS1015"`.
  Reasoning: Half of these branches are unreachable from the in-scope
             code paths. They suggest API surface that the TUI does not
             expose.

### [MISMATCH] M12 — Formula evaluator fallback only matches a tiny grammar but advertises none of it to the user
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/formula_evaluator.c:179-242`
  Source:   The same file's `simple_eval()` body.
  Evidence: When `HAVE_TINYEXPR` is unset, `formula_evaluator_evaluate()`
            calls `simple_eval()` which does substring matches:
            `"avg"`, `"+"+"/"`, pure `"+"` -> sum, pure `"*"` -> product,
            `"min"`, `"max"`. Anything else returns `values[0]`. No TUI
            dialog or in-scope doc string explains this grammar.
  Reasoning: If TinyExpr is not linked in, an operator typing `x0 - x1`
             gets `values[0]` silently. There is no user-facing help
             text in any of `dialog_sensor.c`, `dialog_io_wizard.c`, or
             page_*.c that documents what is or isn't supported. Combined
             with H2 (calculated sensors are unreachable from the TUI),
             this whole branch is invisible to the operator.

### [MISMATCH] M13 — Two `gpio_direction_t` / `gpio_edge_t` enums coexist with identical names
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/hardware/hw_interface.h:47-57`
            `/mnt/cephfs/shared/projects/Water-treat/src/drivers/bus/gpio_hal.h:18-34`
  Source:   Both files define typedefs named `gpio_direction_t` and
            `gpio_edge_t` with non-identical enumerator names
            (`GPIO_DIR_IN` vs `GPIO_DIR_INPUT`, `GPIO_DIR_OUT` vs
            `GPIO_DIR_OUTPUT`).
  Evidence: hw_interface.h: `typedef enum { GPIO_DIR_IN = 0, GPIO_DIR_OUT } gpio_direction_t;`
            gpio_hal.h:    `typedef enum { GPIO_DIR_INPUT = 0, GPIO_DIR_OUTPUT, } gpio_direction_t;`
  Reasoning: Including both headers in the same translation unit is a
             compile error (two definitions of the same typedef). No
             in-scope file does so today, but the duplicate names mean
             any future module that pulls in both GPIO HALs breaks. The
             two HALs should be unified or namespaced.

### [MISMATCH] M14 — Wizard `find_next_slot()` walks bus 0..16 but accepts slot range 2..246
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/tui/dialogs/dialog_io_wizard.c:386-426`
  Source:   Same file, lines 47-50 define `SENSOR_SLOT_MAX 246`,
            `ACTUATOR_SLOT_MAX 246`.
  Evidence: ```c
            bool used[17] = {false};
            ...
            for (int slot = min_slot; slot <= max_slot; slot++) {
                if (!used[slot]) return slot;
            }
            ```
            The `used` array is sized 17 but `max_slot` can be 246.
  Reasoning: Out-of-bounds writes if any persisted module/actuator has
             a slot in [17, 246]. Latent buffer overflow in the wizard's
             slot allocator.

### [MISMATCH] M15 — `relay_output.c` uses deprecated sysfs GPIO interface even though `gpio_hal.h` advertises libgpiod abstraction
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/drivers/digital/relay_output.c:39-81`
  Source:   `src/drivers/bus/gpio_hal.h:43-67` (declares `gpio_init`,
            `gpio_configure`, `gpio_write`, etc., abstracted from sysfs).
  Evidence: relay_output.c writes directly to `/sys/class/gpio/export` and
            `/sys/class/gpio/gpio%d/value` instead of calling the
            `gpio_hal` functions it includes. The sysfs `/sys/class/gpio`
            interface is removed in modern Linux kernels (>=6.x default
            CONFIG_GPIO_SYSFS=n).
  Reasoning: Two parallel GPIO output paths exist; only one is hardware-
             agnostic. The actuator output driver picks the wrong one and
             will fail on any board where sysfs GPIO is not enabled.
             Closes on SOURCES.md availability (linux/gpio.h section)
             before this can be promoted from MISMATCH to HALLUCINATION.

### [MISMATCH] M16 — `actuator_config_t.max_on_time_sec` (seconds) populated from `db_actuator_t.max_on_time_ms` with rounding-up that misrepresents user intent
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/actuators/actuator_manager.c:880`
  Source:   `src/actuators/actuator_manager.h:77` (`max_on_time_sec`),
            `src/db/db_actuators.h:42` (`max_on_time_ms`).
  Evidence: ```c
            /* Round up to avoid truncation: 1500ms -> 2sec instead of 1sec */
            config.max_on_time_sec = (db_act->max_on_time_ms + 999) / 1000;
            ```
  Reasoning: The `actuators` schema stores millisecond precision but
             the in-memory model only keeps seconds, with a coarse round-
             up. Setting max_on_time_ms = 100 becomes 1 second (10× the
             configured limit). This is a precision-loss mismatch
             between the database contract and the runtime contract.

---

## Section 3 — UNVERIFIED

### [UNVERIFIED] U1 — `pnet_input_set_data_and_iops()` argument order and types
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/profinet/profinet_manager.c:1315-1316,2040-2041,1802`
  Source:   p-net `pnet_api.h` at `v0.2.0` — header is not present in this
            repo and `docs/verification/SOURCES.md` does not yet exist.
  Evidence: Caller pattern in profinet_manager.c:
            `pnet_input_set_data_and_iops(g_pn.pnet, 0, slot, subslot,
            s->input_data, s->input_size, iops);`
  Reasoning: Cannot verify the parameter list, types, or return value
             against the actual installed p-net version without the
             header. **Closes on SOURCES.md availability** for the
             p-net `v0.2.0` `pnet_api.h` excerpt covering this function.

### [UNVERIFIED] U2 — `pnet_output_get_data_and_iops()` argument order and types
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/profinet/profinet_manager.c:305-306,1382-1383`
  Source:   p-net `pnet_api.h` at `v0.2.0` — not in repo.
  Evidence: `pnet_output_get_data_and_iops(g_pn.pnet, 0, slot, subslot,
            &new_data, data, &len, &iops);`
  Reasoning: Cannot verify the `bool *new_data` / `uint16_t *len` /
             `uint8_t *iops` types and ordering at the pinned tag.
             **Closes on SOURCES.md availability.**

### [UNVERIFIED] U3 — `pnet_alarm_send_process_alarm()` signature
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/profinet/profinet_manager.c:1580-1589`
  Source:   p-net `pnet_api.h` at `v0.2.0` — not in repo.
  Evidence: ```c
            int ret = pnet_alarm_send_process_alarm(
                g_pn.pnet, g_pn.arep, 0,
                (uint16_t)slot, (uint16_t)subslot,
                alarm_type, (uint16_t)data_len, data);
            ```
            Comment in code claims this is the v0.2.0 positional form.
  Reasoning: The comment `// p-net v0.2.0 API: positional arguments
             instead of struct` is suggestive but unverified. Cannot
             confirm without the header. **Closes on SOURCES.md availability.**

### [UNVERIFIED] U4 — `pnet_diag_add()` and `pnet_diag_remove()` signatures and the `pnet_diag_source_t` struct fields
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/profinet/profinet_manager.c:1669-1703`
  Source:   p-net `pnet_api.h` at `v0.2.0` — not in repo.
  Evidence: The code constructs a `pnet_diag_source_t` with fields `api`,
            `slot`, `subslot`, `ch`, `ch_grouping`, `ch_direction` and
            uses constants `PNET_DIAG_CH_INDIVIDUAL_CHANNEL`,
            `PNET_DIAG_CH_PROP_DIR_INPUT`,
            `PNET_DIAG_CH_PROP_TYPE_UNSPECIFIED`,
            `PNET_DIAG_CH_PROP_MAINT_FAULT`. It then calls `pnet_diag_add`
            with 11 positional arguments and `pnet_diag_remove` with 5.
  Reasoning: Cannot verify any of the constants, struct fields, or call
             arity against the actual v0.2.0 header. The accompanying
             `DIAGNOSIS_ALARM_AUDIT.md` exists in the repo but I have not
             cross-checked it against ground truth in this audit.
             **Closes on SOURCES.md availability.**

### [UNVERIFIED] U5 — Kernel I2C / SPI `ioctl` argument types
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/hardware/hw_interface.c:30,138-152,178`
  Source:   `linux/i2c-dev.h`, `linux/spi/spidev.h` — not yet documented
            in `docs/verification/SOURCES.md`.
  Evidence: I2C: `ioctl(dev->fd, I2C_SLAVE, address)` where `address`
            is `uint8_t`.
            SPI: `ioctl(dev->fd, SPI_IOC_WR_MODE, &dev->mode)` where
            `mode` is `uint8_t`; `ioctl(dev->fd, SPI_IOC_MESSAGE(1),
            &transfer)`.
  Reasoning: Linux kernel ABI requires `I2C_SLAVE` argument as
             `unsigned long` (passed as the third ioctl arg, naturally
             promoted), and `SPI_IOC_WR_MODE` expects a `uint8_t *`. The
             current code looks correct on the face of it, but I have not
             verified against the actual kernel UAPI headers in this
             pass. **Closes on SOURCES.md availability** of the relevant
             linux kernel UAPI excerpts.

### [UNVERIFIED] U6 — `gpio_hal.c` libgpiod API usage
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/drivers/bus/gpio_hal.c` (not read in this pass)
  Source:   `libgpiod.h` v1 vs v2 — `scripts/install-deps.sh` mentions
            handling both, but I have not verified which API the
            `gpio_hal.c` implementation actually targets, nor have I
            checked the call signatures.
  Reasoning: Out of read budget for this pass. The header exposes a
             pin-int based abstraction but libgpiod is line-handle based,
             so there is significant adapter code that I have not
             reviewed. **Closes on SOURCES.md availability** plus a
             dedicated read pass over `gpio_hal.c`.

### [UNVERIFIED] U7 — `pnet_plug_module()` / module-add lifecycle assumed by `profinet_manager_add_module()`
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/profinet/profinet_manager.c:2053-2103`
  Source:   p-net `pnet_api.h` at `v0.2.0` — not in repo.
  Evidence: Comment at `profinet_manager.c:2061-2063`:
            `"p-net v0.2.0 does not support hot-plugging modules — pnet_plug_module() must be called before the first connection."`
  Reasoning: The comment makes a specific claim about the v0.2.0 API. I
             cannot verify it against the header. The cyclic-path
             implication is significant: any actuator added via the TUI
             after PROFINET start will not actually appear on the wire
             until restart. **Closes on SOURCES.md availability.**

### [UNVERIFIED] U8 — `te_compile`, `te_eval`, `te_free`, `te_variable` (TinyExpr) API
  Code:     `/mnt/cephfs/shared/projects/Water-treat/src/sensors/formula_evaluator.c:54-66,78-82,113`
  Source:   `tinyexpr.h` — not in repo, not documented in
            `docs/verification/SOURCES.md`.
  Evidence: ```c
            te_variable *vars = calloc(variable_count, sizeof(te_variable));
            for (int i = 0; i < variable_count; i++) {
                vars[i].name = ...; vars[i].address = ...;
                vars[i].type = 0; vars[i].context = NULL;
            }
            eval->te_expr = te_compile(formula, vars, variable_count, &error_pos);
            ```
  Reasoning: TinyExpr's struct field names and `te_compile()` argument
             order are not verified. Whether the `type=0` and
             `context=NULL` initialization is the documented "plain
             variable" pattern is unverified. **Closes on SOURCES.md
             availability** of the TinyExpr public header excerpt.

---

## Section 4 — VERIFIED (count by category)

| Category                                            | Count | Notes |
|-----------------------------------------------------|------:|-------|
| SQL statements vs verified DB schema                |    24 | All `INSERT`, `UPDATE`, `SELECT`, `DELETE` against `modules`, `physical_sensors`, `adc_sensors`, `web_poll_sensors`, `calculated_sensors`, `static_sensors`, `sensor_status`, `sensor_data_log`, `actuators`, `actuator_state` matched the schema. No `FROM sensors` (the nonexistent table) anywhere. |
| `gsdml_modules.h` constants vs `gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml` | 16 | All sensor and actuator `ModuleIdentNumber` / `SubmoduleIdentNumber` values in the GSDML XML match the `#define`s exactly (`0x00000010`/`0x00000011` pH, `0x00000020`/`0x00000021` TDS, …, `0x00000120`/`0x00000121` Generic DO). |
| Forward-cited function prototypes inside scope      |    19 | Every cross-module call into `profinet_manager_*`, `db_module_*`, `db_physical_sensor_*`, `db_adc_sensor_*`, `db_actuator_*`, `actuator_manager_*`, `sensor_manager_*`, and the driver `driver_<chip>_*` family resolves to a real prototype with matching arity/types. |
| Driver init signatures vs callers in `sensor_instance.c` | 9 | `driver_ds18b20_init`, `driver_dht22_init`, `driver_bme280_init`, `driver_hx711_init`, `tcs34725_init`, `jsn_sr04t_init`, `float_switch_init`, `driver_ads1115_init`, `driver_mcp3008_init` — call sites match declared signatures. |
| `actuators.gpio_chip` TEXT handling                 |     2 | `db_actuator_create` / `db_actuator_get` / `db_actuator_list` / `db_actuator_get_by_slot` all use `sqlite3_bind_text` / `sqlite3_column_text`; no integer mishandling of the `gpio_chip` column. |
| `actuators.safe_state` TEXT handling                |     2 | `safe_state_to_string()` and `string_to_safe_state()` correctly translate between the `safe_state_t` enum and the literal strings `"on"`, `"off"`, `"hold"`. |
| Includes resolve to existing files (in-scope)       |     ≈30 | Every `#include "..."` in the in-scope files resolves to a real file under `src/` or `include/`. |

---

## Summary

```
HALLUCINATION: 5 | MISMATCH: 16 | UNVERIFIED: 8 | VERIFIED: ~82
```

### Severity bullets

- **Highest-priority fixes (HALLUCINATION):**
  - **H1** — `adc_driver.h` API has no implementation file; entire ADC
    backend is dead-letter declarations.
  - **H2** — calculated sensors cannot be created or edited via the TUI;
    the option is offered but the dialog has no formula field.
  - **H4 / H5** — `sensor_api.h` and `analog_sensor.h` advertise full
    public APIs that link to nothing meaningful.

- **Most user-visible MISMATCHes:**
  - **M1 / M2** — PUMP and VALVE actuator types silently degrade to
    RELAY on every save/reload because their string mapping is missing.
  - **M3** — float-switch sensors do not register a GPIO conflict
    against actuators; pin can be double-allocated.
  - **M14** — `find_next_slot()` writes past the end of a 17-element
    array when any persisted slot is in [17, 246]; latent buffer
    overflow in the wizard's slot picker.
  - **M15** — `relay_output.c` uses the removed sysfs GPIO interface
    instead of the libgpiod abstraction it includes.

- **What blocks closing the UNVERIFIED items:** all 8 of U1–U8 wait on
  `docs/verification/SOURCES.md` (the parallel research agent's output)
  to provide the `pnet_api.h@v0.2.0`, `linux/i2c-dev.h`,
  `linux/spi/spidev.h`, libgpiod, and `tinyexpr.h` reference excerpts.
  Once SOURCES.md lands, every UNVERIFIED can be promoted to a terminal
  verdict in a follow-up pass.

### Out of scope (logged but not investigated)

- The PROFINET Connect/AR lifecycle regression thread tracked in
  `CLAUDE.md` (commit `88191bc` and friends).
- Any auth, user-sync, station-name, enrollment, or bootstrap code.
- HTTP API beyond sensor/actuator slot fields.
- `gpio_hal.c` body (deferred as U6).
