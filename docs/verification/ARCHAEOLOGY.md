# Sensor & Actuator Subsystem Archaeology

Codebase map of the sensor and actuator slice. Maps and inventories only;
no fixes proposed. Every claim is cited with file:line. Anything I could
not statically verify is marked **UNVERIFIED**.

Working tree: `/mnt/cephfs/shared/projects/Water-treat`
Branch: `main` (commit `1fa7e20`)

---

## 1. Module-Dependency Graph (sensor + actuator slice)

Edges = `#include` or direct symbol call. Limited to in-scope files.

```
                              ┌─────────────────────────────────┐
                              │  include/gsdml_modules.h        │
                              │  - GSDML_MOD_SENSOR_*           │
                              │  - GSDML_MOD_ACTUATOR_*         │
                              │  - gsdml_sensor_module_from_    │
                              │    string()                     │
                              └─────────┬───────────────────────┘
                                        │ included by
              ┌─────────────────────────┼─────────────────────┐
              │                         │                     │
              ▼                         ▼                     ▼
┌──────────────────────┐  ┌────────────────────────┐  ┌────────────────────┐
│ src/sensors/         │  │ src/actuators/         │  │ src/tui/dialogs/   │
│   sensor_manager.c   │  │   actuator_manager.c   │  │   dialog_sensor.c  │
└──────────┬───────────┘  └───────────┬────────────┘  └────────┬───────────┘
           │ owns                     │ owns                   │
           ▼                          ▼                        ▼
┌──────────────────────┐  ┌────────────────────────┐  ┌────────────────────┐
│ sensor_instance_t[]  │  │ actuator_instance_t[]  │  │ sensor_form_t      │
│ (sensor_instance.c)  │  │ (actuator_manager.c)   │  │ (15 fields)        │
└──────────┬───────────┘  └───────────┬────────────┘  └────────┬───────────┘
           │ string-                  │ enum-                  │ writes only
           │ dispatched               │ dispatched             │ physical_sensors
           ▼                          ▼                        │ adc_sensors
┌──────────────────────┐  ┌────────────────────────┐           ▼
│ src/sensors/drivers/ │  │ src/drivers/digital/   │  ┌────────────────────┐
│ driver_ads1115.c     │  │   relay_output.c       │  │ src/db/db_modules.c│
│ driver_bme280.c      │  └───────────┬────────────┘  │ src/db/db_actuators│
│ driver_dht22.c       │              │               └────────────────────┘
│ driver_ds18b20.c     │              ▼
│ driver_float_switch  │  ┌────────────────────────┐
│ driver_hx711.c       │  │ src/drivers/bus/       │
│ driver_jsn_sr04t.c   │  │   gpio_hal.c           │
│ driver_mcp3008.c     │  └────────────────────────┘
│ driver_tcs34725.c    │
│ driver_web_poll.c    │
└──────────┬───────────┘
           │ float_switch + jsn_sr04t use:
           ▼
┌──────────────────────┐
│ src/sensors/hardware/│  ← parallel GPIO HAL #2
│   hw_interface.c     │
└──────────────────────┘

(driver_dht22.c uses neither HAL — mmaps /dev/gpiomem with hardcoded
 BCM2835 register offsets, see driver_dht22.c:20-81 — Pi-specific)
```

### Entry points

| Entry point | File:line | Purpose |
|---|---|---|
| `sensor_manager_init()` | `src/sensors/sensor_manager.c:301` | Called from `main.c`, calls `sensor_manager_reload_sensors()` |
| `sensor_manager_reload_sensors()` | `src/sensors/sensor_manager.c:370` | Walks `db_module_list()`, calls `sensor_instance_create_from_db()` for each row |
| `sensor_worker_thread()` | `src/sensors/sensor_manager.c:132` | Cyclic poll loop; calls `sensor_instance_read()` then `profinet_manager_update_input_with_quality()` |
| `actuator_manager_init()` / `actuator_manager_start()` | `src/actuators/actuator_manager.c:374,394` | Initializes manager, registers PROFINET callbacks (`profinet_output_handler` line 362) |
| `actuator_manager_reload()` | `src/actuators/actuator_manager.c:802` | Walks `db_actuator_list()`, calls `actuator_manager_add()` |
| `actuator_manager_handle_output()` | `src/actuators/actuator_manager.c:585` | Bridge from PROFINET poll → physical actuator |
| `dialog_sensor_add()` / `_edit()` | `src/tui/dialogs/dialog_sensor.c:298,356` | TUI sensor save path |
| `save_sensor()` / `save_actuator()` (wizard) | `src/tui/dialogs/dialog_io_wizard.c:1673,1801` | TUI wizard save path |
| `dialog_actuator_show()` | `src/tui/dialogs/dialog_actuator.c:677` | TUI actuator save path |
| `poll_output_slots()` | `src/profinet/profinet_manager.c:290` | Cyclic poll of p-net output buffers; dispatches to `on_data_received` (the actuator manager handler) |
| `profinet_manager_update_input_with_quality()` | `src/profinet/profinet_manager.c:1348` | Sole sensor → wire writer; encodes 5-byte (BE float + quality) |

### Cycles

- None detected at the file/header level. The sensor and actuator paths
  are two trees that share `gsdml_modules.h`, `db/database.h`,
  `profinet/profinet_manager.h`, and `common.h`.

### Orphans (no incoming edges from in-scope code)

- `src/sensors/sensor_api.h` and `src/sensors/analog/analog_sensor.{c,h}`
  — included only by themselves and `dialog_io_wizard.c` (which uses one
  enum, `sensor_channel_t`, but never instantiates `sensor_driver_t`).
  See §6 dead-code.
- `src/drivers/adc/adc_driver.h` — header only; no `.c` exists. See §6.
- `src/sensors/drivers/driver_pump.h`, `driver_solenoid.h` — no `.c`,
  no callers. See §6.

---

## 2. Canonical Data Model

### 2.1 Sensors — DB schema (authoritative)

CREATE TABLE statements live at `src/db/database.c:6-37`. Direct
inspection (not the user-supplied summary) shows the actual columns:

| Table | Columns (from `database.c`) |
|---|---|
| `modules` | id, slot, subslot, name, **module_type**, module_ident, submodule_ident, status, created_at, updated_at — `database.c:6-10` |
| `physical_sensors` | id, module_id, **sensor_type** (NOT NULL), **hardware_type**, interface, address, bus, channel, resolution, unit, min_value, max_value, poll_rate_ms, timeout_ms — `database.c:12-17` |
| `adc_sensors` | id, module_id, adc_type, interface, address, bus, channel, gain, reference_voltage, unit, raw_min, raw_max, eng_min, eng_max, poll_rate_ms — `database.c:19-24` |
| `web_poll_sensors` | id, module_id, url, method, headers, json_path, poll_rate_ms, timeout_ms — `database.c:26-29` |
| `calculated_sensors` | id, module_id, formula, input_sensors, unit, update_rate_ms — `database.c:31-33` |
| `static_sensors` | id, module_id, value, unit, writable — `database.c:35-37` |
| `sensor_status` | module_id, value, status, last_update, consecutive_failures — `database.c:39-41` |
| `sensor_data_log` | id, module_id, value, status, timestamp — `database.c:43-45` |

Note: `physical_sensors` has TWO type columns — `sensor_type` (NOT NULL)
and `hardware_type` (nullable). They are written identically by all
production callers. See §3 and §7-D2 for the divergence.

### 2.2 Sensors — In-memory runtime struct

The runtime canonical struct is `sensor_instance_t` defined at
`src/sensors/sensor_instance.h:76-136`. It is a single struct shared by
ALL five DB sensor types — type-discriminated by the
`sensor_instance_type_t` enum (`sensor_instance.h:11-18`):

```
SENSOR_INSTANCE_PHYSICAL    SENSOR_INSTANCE_ADC
SENSOR_INSTANCE_CALCULATED  SENSOR_INSTANCE_WEB_POLL
SENSOR_INSTANCE_STATIC      SENSOR_INSTANCE_SYSTEM
```

Note `SENSOR_INSTANCE_SYSTEM` has no DB representation — it is the
runtime-only CPU-temperature sensor injected at slot 1 by
`create_cpu_temp_sensor()` (`sensor_manager.c:58-115`).

### 2.3 DB column ↔ `sensor_instance_t` field map

| DB column | `sensor_instance_t` field | Read at | Notes |
|---|---|---|---|
| `modules.id` | `id` | `sensor_instance.c:114` (indirectly via `module->id`) | OK |
| `modules.slot` | `slot` | `sensor_instance.c:115` | OK |
| `modules.subslot` | `subslot` | `sensor_instance.c:116` | OK |
| `modules.name` | `name` | `sensor_instance.c:117` | OK |
| `modules.module_type` | (used for dispatch only, not stored in instance) | `sensor_instance.c:133,221,255,274,286` | string-compared, not retained |
| `modules.module_ident` | NOT MAPPED | — | Read by `db_module_list()` into `db_module_t`, then DROPPED. The runtime recomputes the ident in `sensor_manager.c:443-456` from `phys.sensor_type` / `adc.unit` strings via `gsdml_sensor_module_from_string()`. |
| `modules.submodule_ident` | NOT MAPPED | — | Same — dropped, recomputed |
| `modules.status` | NOT MAPPED | `sensor_manager.c:409` (only the literal string `"disabled"` is checked) | Anything other than `"disabled"` enables the sensor |
| `physical_sensors.sensor_type` | (driver dispatch only) | `sensor_instance.c:146,149,154,163,168,186,201` | Used for switching driver; not retained |
| `physical_sensors.hardware_type` | NOT READ in runtime | — | Field exists in `db_physical_sensor_t` but `sensor_instance_create_from_db` never references `sensor.hardware_type`. See §7-D2 |
| `physical_sensors.interface` | NOT READ in dispatch | — | Stored in `db_physical_sensor_t.interface` but never branched on by the dispatch ladder. The dispatch keys off `sensor_type` only. |
| `physical_sensors.address` | parsed inline per-driver | `sensor_instance.c:148,152,157-160,167,176,193,208` | Format varies per driver (GPIO pin int, "trig,echo", "0xNN", 1-Wire ROM) |
| `physical_sensors.bus` | passed to driver | `sensor_instance.c:161,178` | OK for I2C drivers |
| `physical_sensors.channel` | NOT READ for `physical` sensors | — | Field exists in `db_physical_sensor_t` (loaded by `db_physical_sensor_get`) but never used in any physical-driver init |
| `physical_sensors.resolution` | NOT READ | — | Loaded into `db_physical_sensor_t.resolution` but never accessed |
| `physical_sensors.unit` | NOT READ at runtime | — | Loaded but unused |
| `physical_sensors.min_value` / `max_value` | NOT READ | — | Loaded but unused. Note that `instance->range_min/max` are set to `-FLT_MAX/FLT_MAX` at line 125-126 with no DB override |
| `physical_sensors.poll_rate_ms` | `poll_rate_ms` | `sensor_instance.c:142` | OK |
| `physical_sensors.timeout_ms` | `timeout_ms` | `sensor_instance.c:143` | OK |
| `adc_sensors.adc_type` | (driver dispatch only) | `sensor_instance.c:237,244` | OK; "ADS1015" string matched but routed to ADS1115 driver |
| `adc_sensors.interface` | partially used | `sensor_instance.c:240` | only `"i2c"` branch — `"spi"` falls through silently |
| `adc_sensors.address` | passed to driver | `sensor_instance.c:241,247` | for SPI parsed as "bus.device" |
| `adc_sensors.bus` | passed | `sensor_instance.c:242` | OK |
| `adc_sensors.channel` | passed | `sensor_instance.c:242,248` | OK |
| `adc_sensors.gain` | passed | `sensor_instance.c:242` | only ADS1115 path |
| `adc_sensors.reference_voltage` | passed | `sensor_instance.c:248` | only MCP3008 path |
| `adc_sensors.unit` | NOT READ at runtime | — | Loaded but unused at instance level. (`sensor_manager.c:453` uses it ONCE to derive PROFINET module ident.) |
| `adc_sensors.raw_min/max` | `raw_min/raw_max` | `sensor_instance.c:231,232` | Used by `apply_calibration()` |
| `adc_sensors.eng_min/max` | `eng_min/eng_max` | `sensor_instance.c:233,234` | Used by `apply_calibration()` |
| `adc_sensors.poll_rate_ms` | `poll_rate_ms` | `sensor_instance.c:230` | OK |
| `web_poll_sensors.url` | passed | `sensor_instance.c:267` | OK |
| `web_poll_sensors.method` | passed | `sensor_instance.c:267` | OK |
| `web_poll_sensors.headers` | passed | `sensor_instance.c:270` | OK |
| `web_poll_sensors.json_path` | passed | `sensor_instance.c:271` | OK |
| `web_poll_sensors.poll_rate_ms` | `poll_rate_ms` | `sensor_instance.c:264` | OK |
| `web_poll_sensors.timeout_ms` | `timeout_ms` | `sensor_instance.c:265` | OK |
| `calculated_sensors.formula` | `formula` | `sensor_instance.c:295,323` | OK |
| `calculated_sensors.input_sensors` | parsed → `input_slots[]` | `sensor_instance.c:300-312` | OK |
| `calculated_sensors.unit` | NOT READ at runtime | — | Loaded but unused |
| `calculated_sensors.update_rate_ms` | `poll_rate_ms` | `sensor_instance.c:296` | OK |
| `static_sensors.value` | `current_value` | `sensor_instance.c:283` | OK |
| `static_sensors.unit` | NOT READ at runtime | — | Loaded but unused |
| `static_sensors.writable` | NOT READ at runtime | — | Loaded but unused. The setter `db_static_sensor_set_value()` (`db_modules.c:617`) writes back to the DB but no in-scope caller invokes it. |

In `sensor_instance_t` itself, several fields are **declared but never assigned** by any production code:

| Unused field | Declared at |
|---|---|
| `cal_scale`, `cal_offset` | `sensor_instance.h:99,100` (only `system_temp` path sets them, `sensor_manager.c:87-88`) |
| `consecutive_successes` | `sensor_instance.h:116` — incremented at `sensor_instance.c:546`, never read |
| `current_raw_value` (uses ADC path only) | only ADC sets it — physical/web/static/calc never do |

### 2.4 DB column ↔ `sensor_form_t` (TUI dialog) map

`sensor_form_t` is defined at `src/tui/dialogs/dialog_sensor.c:22-40`.
15 fields total. Compared to the union of all 5 sensor table column sets:

| DB field | sensor_form field | Note |
|---|---|---|
| name | name | OK |
| slot | slot | OK |
| subslot | subslot | OK |
| module_type | module_type | OK |
| module_ident | module_ident | hard-set to `GSDML_MOD_SENSOR_GENERIC` at `dialog_sensor.c:67`, never changed |
| submodule_ident | submodule_ident | hard-set, never changed |
| sensor_type / adc_type | sensor_type | OK |
| interface | interface | OK |
| address | address | OK |
| bus | bus | OK |
| channel | channel | OK |
| gain | gain | OK |
| reference_voltage | reference_voltage | OK |
| unit | unit | OK |
| min_value / eng_min | min_value | OK (collapsed) |
| max_value / eng_max | max_value | OK (collapsed) |
| poll_rate_ms | poll_rate_ms | OK |
| **timeout_ms** | **MISSING** | physical/web_poll have timeout_ms columns; form cannot set it |
| **resolution** | **MISSING** | physical_sensors column unreachable from form |
| **raw_min / raw_max** | **MISSING** | adc_sensors columns unreachable; saved as 0 (defaults) |
| **hardware_type** | conflated with sensor_type | `dialog_sensor.c:268` writes `phys.hardware_type = form.sensor_type` |
| **formula / input_sensors** | **MISSING** | calculated sensors cannot be created via this dialog |
| **value / writable** | **MISSING** | static sensors cannot be created via this dialog |
| **url / method / headers / json_path** | **MISSING** | web_poll sensors cannot be created via this dialog |

The form's TYPE picker (`dialog_sensor.c:42`) advertises 5 types
`{"physical","adc","web_poll","calculated","static"}` but the save path
(`dialog_sensor.c:264-293`) only INSERTs into `physical_sensors` or
`adc_sensors`. Choosing web_poll/calculated/static creates an
**orphaned `modules` row with no sub-record**, which then fails at
`sensor_instance_create_from_db()` `RESULT_NOT_FOUND` from
`db_web_poll_sensor_get()` etc.

### 2.5 PROFINET input buffer ↔ sensor reading

There is exactly **one writer** of sensor data into a p-net input
buffer: `profinet_manager_update_input()` at
`src/profinet/profinet_manager.c:1283`. It is wrapped by:

- `profinet_manager_update_input_float()` — `profinet_manager.c:1324`
  (4-byte BE float, no quality byte; **no production caller**, see §5)
- `profinet_manager_update_input_with_quality()` —
  `profinet_manager.c:1348` (5-byte BE float + 1-byte quality)

`sensor_worker_thread()` is the sole production caller, at
`sensor_manager.c:222`. The wire format produced by
`update_input_with_quality()` is:

```
Byte 0..3 : float32 value, big-endian (htonl on the bit pattern)
Byte 4    : data_quality_t cast to uint8_t
```

This matches the documented contract in
`docs/PROFINET_DATA_FORMAT_SPECIFICATION.md` (UNVERIFIED — I did not
read it; statement based on the doc reference at
`profinet_manager.c:1340-1345` and `sensor_manager.c:215-220`).

`GSDML_SENSOR_INPUT_SIZE = 5` is defined at `gsdml_modules.h:75` and
used as the input length when registering the slot at
`sensor_manager.c:464,503`. Consistent with the writer.

### 2.6 Actuators — DB schema (authoritative)

CREATE TABLE at `src/db/database.c:75-87`:

| Table | Columns |
|---|---|
| `actuators` | id, **slot** (UNIQUE), subslot, name, type, gpio_pin, gpio_chip, active_low, safe_state, min_on_time_ms, max_on_time_ms, pwm_frequency_hz, status, enabled, created_at, updated_at |
| `actuator_state` | actuator_id, state, pwm_duty, last_state_change, total_on_time_ms, cycle_count |

Confirmed — no `module_id` foreign key. Actuators are first-class
keyed by their own `slot`.

### 2.7 Actuator DB ↔ `actuator_instance_t` field map

Two structs are involved on the runtime side:

- `db_actuator_t` — `src/db/db_actuators.h:31-46`, the DTO loaded from DB.
- `actuator_config_t` — `src/actuators/actuator_manager.h:59-80`, the runtime config.
- `actuator_instance_t` — `src/actuators/actuator_manager.h:86-107`, holds the config + runtime state.

The conversion `db_actuator_t → actuator_config_t` happens in
`actuator_manager_reload()` at `actuator_manager.c:868-881`.

| DB column | `actuator_config_t` field | Notes |
|---|---|---|
| id | id | OK (`actuator_manager.c:869`) |
| slot | profinet_slot | OK (`:872`) |
| subslot | profinet_subslot | OK (`:873`, defaulted to 1 if 0) |
| name | name | OK (`:870`) |
| type | type | OK (`:871`) |
| gpio_pin | gpio_pin | OK (`:874`) |
| **gpio_chip** | **DROPPED** | `actuator_config_t` has no `gpio_chip` field. Never copied. The actuator driver init (`init_actuator_driver`, `:86`) hands `output_config_t` to `output_create()` (`:110`) and `output_config_t` (`relay_output.h:40-64`) also has no `gpio_chip` field. |
| active_low | active_low | OK (`:875`) |
| **safe_state** | **DROPPED** | `actuator_config_t` has no `safe_state` field. The DB value is read into `db_actuator_t.safe_state` and then ignored. The "safe state" applied at disconnect (`apply_safe_state`, `actuator_manager.c:232`) hard-codes OFF for everything. |
| min_on_time_ms | **`min_cycle_time_ms`** | Renamed during copy (`:881`). Two different concepts being collapsed: DB column comments call this "anti-short-cycle" minimum on-time, runtime treats it as a debounce/anti-chatter cycle time used at `:632`. |
| max_on_time_ms | max_on_time_sec | Lossy conversion: divided by 1000 (rounded up at `:880`). Anything < 999 ms becomes 1 sec. |
| pwm_frequency_hz | pwm_frequency_hz | OK (`:878`) |
| **status** | **DROPPED** | Not used at runtime |
| enabled | (filter only) | Used at `:854,826` to skip disabled rows; not stored |

`actuator_config_t` also has fields not derived from any DB column:
- `pwm_capable` (bool) — derived from `type == PUMP || PWM` at `:876-877`.

### 2.8 Actuator DB ↔ `actuator_form_t` (TUI dialog) map

`actuator_form_t` at `src/tui/dialogs/dialog_actuator.h:20-39` covers
ALL DB columns including `gpio_chip`, `safe_state`, `min_on_time_ms`,
`max_on_time_ms`. Confirmed by load/save functions:
`dialog_actuator.c:629-674`. **The TUI form is faithful to the DB
schema** — the lossy conversion happens later in the runtime
(`actuator_manager.c`).

### 2.9 PROFINET output buffer ↔ actuator command

There is exactly **one reader** of actuator data from p-net:
`poll_output_slots()` at `profinet_manager.c:290-334`. It dispatches
each new payload to `g_pn.on_data_received` (`:323`). The only
registered listener is the actuator manager
(`actuator_manager.c:362-368`'s `profinet_output_handler` →
`actuator_manager_handle_output` `:585`).

The 4-byte format is defined at `actuator_manager.h:40-48`:

```c
typedef struct {
    uint8_t command;     // 0x00=OFF, 0x01=ON, 0x02=PWM
    uint8_t pwm_duty;    // 0-100
    uint8_t reserved[2];
} __attribute__((packed)) actuator_output_data_t;
```

`GSDML_ACTUATOR_OUTPUT_SIZE = 4` (`gsdml_modules.h:78`) matches
`sizeof(actuator_output_data_t)`. Slot registration uses this size at
`actuator_manager.c:528`.

### 2.10 The big architectural question: actuators vs modules

Actuators are NOT inserted into the `modules` table. The `actuators`
table has its own `slot` column with UNIQUE constraint (`database.c:76`)
**but the constraint is only across the `actuators` table itself** —
nothing prevents a `modules.slot=5` and `actuators.slot=5` from
coexisting.

The actuator manager registers each actuator directly with PROFINET
via `profinet_manager_add_module()` (`actuator_manager.c:522`),
bypassing `modules` entirely. Sensor manager does the same for the
CPU temperature sensor (`sensor_manager.c:497`). Both modules treat
the PROFINET slot table — not the `modules` SQL table — as the
source of truth for "what's plugged at slot N."

Implication: the slot keyspace (2-246) is shared by sensor and
actuator code paths but enforced only inside each subsystem's own
load function. The `dialog_io_wizard.c` `find_next_slot()` function
(`:386-426`) checks the *appropriate* table for the IO type being
created but **does NOT cross-check the other table**. This means a
wizard run for a sensor at slot 5 will succeed even if an actuator
already lives at slot 5 (and vice-versa). On reload, both will register
with PROFINET against the same slot — last-writer-wins, with no error.

There is also a fixed-size buffer overflow risk in
`find_next_slot()`: `bool used[17]` at `dialog_io_wizard.c:394` is
indexed by `modules[i].slot` up to `SENSOR_SLOT_MAX = 246`. This is a
real OOB write — UNVERIFIED whether `min_slot ≤ slot ≤ max_slot` ever
exceeds 16 in practice but the code has no defense.

The actuator code does correctly treat actuators as first-class
PROFINET slots without a `modules` row. Sensor and actuator subsystems
do not share any slot-tracking state.

---

## 3. Dispatch Tables

### 3.1 Sensor dispatch — **canonical (production) site**

**File:** `src/sensors/sensor_instance.c`
**Function:** `sensor_instance_create_from_db()` (`:109-343`)

Two-level dispatch — first on `module_type` string, then on type-specific subtype string.

#### Outer dispatch — `module_type` string
| String | Branch | Line |
|---|---|---|
| `MODULE_TYPE_PHYSICAL` (`"physical"`) | physical sensor sub-dispatch | `:133` |
| `MODULE_TYPE_ADC` (`"adc"`) | ADC sub-dispatch | `:221` |
| `MODULE_TYPE_WEB_POLL` (`"web_poll"`) | web_poll init | `:255` |
| `"static"` (literal — no constant) | static load | `:274` |
| `"calculated"` (literal — no constant) | calculated load | `:286` |

#### Inner dispatch — `physical_sensors.sensor_type` string
| String literal | `driver_type` enum | Init function | Line |
|---|---|---|---|
| `DRIVER_NAME_DS18B20` (`"DS18B20"`) | `PHYSICAL_DRIVER_DS18B20` | `driver_ds18b20_init` | `:146` |
| `DRIVER_NAME_DHT22` (`"DHT22"`) **OR** `"DHT11"` (literal) | `PHYSICAL_DRIVER_DHT22` | `driver_dht22_init` | `:149` |
| `DRIVER_NAME_BME280` (`"BME280"`) **OR** `DRIVER_NAME_BMP280` (`"BMP280"`) | `PHYSICAL_DRIVER_BME280` | `driver_bme280_init` | `:154` |
| `DRIVER_NAME_HX711` (`"HX711"`) | `PHYSICAL_DRIVER_HX711` | `driver_hx711_init` | `:163` |
| `DRIVER_NAME_TCS34725` (`"TCS34725"`) | `PHYSICAL_DRIVER_TCS34725` | `tcs34725_init` | `:168` |
| `DRIVER_NAME_JSN_SR04T` (`"JSN-SR04T"`) | `PHYSICAL_DRIVER_JSN_SR04T` | `jsn_sr04t_init` | `:186` |
| `"Float Switch"` (literal, with space) **OR** `"FLOAT_SWITCH"` (literal) | `PHYSICAL_DRIVER_FLOAT_SWITCH` | `float_switch_init` | `:201` |
| anything else | `RESULT_NOT_SUPPORTED`, error log | `:217` |

#### Inner dispatch — `adc_sensors.adc_type` string
| String literal | `driver_type` enum | Init function | Line |
|---|---|---|---|
| `DRIVER_NAME_ADS1115` (`"ADS1115"`) **OR** `"ADS1015"` (literal) | `ADC_DRIVER_ADS1115` | `driver_ads1115_init` | `:237` |
| `DRIVER_NAME_MCP3008` (`"MCP3008"`) | `ADC_DRIVER_MCP3008` | `driver_mcp3008_init` | `:244` |
| anything else | `RESULT_NOT_SUPPORTED` | `:251` |

**Read-side dispatch** is a parallel `switch (driver_type)` at
`sensor_instance.c:425-466` (physical) and `:475-486` (ADC). The
destroy path is a third parallel switch at `:350-389`. **All three
switches must be kept in sync by hand.** No central registry.

### 3.2 Sensor dispatch — secondary (dead) site

**File:** `src/sensors/drivers/driver_common.c`
**Tables:**
- `driver_registry[]` at `:57-69` (10 entries)
- `driver_get_ops_by_type()` at `:71-78`
- `driver_get_ops_by_name()` at `:80-104` (also handles aliases DHT11, AM2302, BMP280, ADS1015)
- `driver_get_type_by_name()` at `:106-127`

**Status: DEAD.** Confirmed via Grep — no caller of any of these
functions exists outside `driver_common.{c,h}` itself. The associated
`driver_ops_t` instances (`driver_ds18b20_ops` at
`driver_ds18b20.c:148`, etc.) are linked but referenced only by this
dead registry. See §6.

### 3.3 Sensor dispatch — tertiary (dead) site

**File:** `src/sensors/sensor_api.h` + `src/sensors/analog/analog_sensor.c`

A third generation of the sensor model defines `sensor_driver_t`,
`sensor_config_t`, `sensor_ops_t`, and a `sensor_register_driver()` /
`sensor_unregister_driver()` registry API
(`sensor_api.h:285-290`). The `analog_sensor` implementation provides
`analog_sensor_create()`, `analog_sensor_factory()`,
`analog_sensor_set_adc()`, `analog_sensor_cal_point()`.

**Status: DEAD.** No production caller. Confirmed:

- `analog_sensor_create` / `analog_sensor_factory` / `analog_sensor_set_adc` / `analog_sensor_cal_point` — 0 callers outside the file itself (Grep confirmed at `src/sensors/analog/analog_sensor.c:267,295,304,362`).
- `sensor_register_driver` / `sensor_unregister_driver` — declared, never defined.
- The only external user of `sensor_api.h` is `dialog_io_wizard.c:25`, which uses the `sensor_channel_t` enum for the wizard's `gpio_sensor_types[]` and `adc_sensor_types[]` tables — not the driver framework.

See §6 and §7-D1.

### 3.4 Actuator dispatch

**File:** `src/actuators/actuator_manager.c`
**Function:** `init_actuator_driver()` (`:86-121`)

Single switch on `actuator_type_t` enum:

| Enum | Result | Line |
|---|---|---|
| `ACTUATOR_TYPE_PUMP` | `OUTPUT_TYPE_PWM` if `pwm_capable`, else `OUTPUT_TYPE_RELAY` | `:94` |
| `ACTUATOR_TYPE_VALVE` | `OUTPUT_TYPE_RELAY` | `:99` |
| `ACTUATOR_TYPE_RELAY`, `_LATCHING`, `_MOMENTARY`, default | `OUTPUT_TYPE_RELAY` | `:102` |

There is no string-based actuator type dispatch. The 6 enum values from
`db_actuators.h:10-17` collapse to 2 driver behaviors (`PWM` or
`RELAY`). `LATCHING` and `MOMENTARY` are accepted at the DB and TUI
level but **are silently treated as plain RELAY** (`init_actuator_driver`
`:102-104`). The pulse logic implied by their names is not implemented.
The `relay_output.h:25-26` enum defines `OUTPUT_TYPE_LATCHING` and
`OUTPUT_TYPE_MOMENTARY`, but the actuator manager never selects them.

The PWM dispatch in `apply_actuator_state()` (`:55-84`) keys off
`act->config.pwm_capable && act->pwm_duty < 100` (line 62). If
`pwm_duty == 100`, it falls through to a non-PWM `output_set(true)`
call even on a pump.

### 3.5 Hardware-type list comparison

| Catalog | Strings |
|---|---|
| **constants.h `DRIVER_NAME_*` macros** | DS18B20, DHT22, **DHT11**, BME280, BMP280, ADS1115, ADS1015, MCP3008, HX711, TCS34725, JSN-SR04T, **AT24C**, **PCF8574**, **MCP23017**, **FloatSwitch**, **WebPoll** (`constants.h:264-279`) |
| **`sensor_instance.c` dispatch (production)** | DS18B20, DHT22 (+`"DHT11"` literal), BME280, BMP280, HX711, TCS34725, JSN-SR04T, `"Float Switch"` (literal w/ space), `"FLOAT_SWITCH"` (literal), ADS1115 (+`"ADS1015"` literal), MCP3008 |
| **`driver_common.c` registry (dead)** | DS18B20, DHT22, BME280, HX711, JSN-SR04T, TCS34725, FloatSwitch, ADS1115, MCP3008, WebPoll (and aliases DHT11/AM2302/BMP280/ADS1015) |
| **`dialog_sensor.c` `hardware_types[]`** | ADS1115, MCP3008, DS18B20, DHT22, BME280, HX711, TCS34725, JSN-SR04T, "Flow Sensor", "pH Sensor", "TDS Sensor", "Turbidity", "Float Switch", "Generic" (`dialog_sensor.c:44-48`) |
| **`dialog_io_wizard.c` `gpio_sensor_types[]`** | "Flow Meter / Pulse Counter", "Float Switch / Level Sensor", "DHT22 Temperature/Humidity", "Generic Digital Input" (`:139-148`) |
| **`dialog_io_wizard.c` `adc_sensor_types[]`** | "pH Probe", "Pressure Transducer", "TDS Sensor", "Turbidity Sensor", "ORP Sensor", "Generic 0-5V Analog" (`:152-165`) |
| **`hw_discover.c` `i2c_device_type_name()`** | ADS1115, ADS1015, MCP3421, BME280, BMP280, **SHT31**, **HTU21D**, **INA219**, **PCA9685**, PCF8574, MCP23017, AT24C, DS3231, SSD1306 (`hw_discover.c:407-427`) |
| **`gsdml_modules.h` `gsdml_sensor_module_from_string()` substring matches** | `"pH"/"ph"/"PH"`, `"TDS"/"tds"/"Conductivity"`, `"Turbidity"/"NTU"`, `"Temp"/"DS18B20"/"DHT"/"BME"`, `"Flow"/"Pressure"`, `"Level"/"Float"/"HX711"/"Distance"/"JSN"` (`:158-194`) |

**Mismatch list (TUI string ↔ dispatch ladder):**

| Source | String | Reachable in dispatch? |
|---|---|---|
| `dialog_sensor.c` picker | `"Flow Sensor"` | NO — not matched |
| `dialog_sensor.c` picker | `"pH Sensor"` | NO |
| `dialog_sensor.c` picker | `"TDS Sensor"` | NO |
| `dialog_sensor.c` picker | `"Turbidity"` | NO |
| `dialog_sensor.c` picker | `"Float Switch"` | YES |
| `dialog_sensor.c` picker | `"Generic"` | NO |
| `dialog_io_wizard.c` GPIO save | `"Flow Meter / Pulse Counter"` | NO |
| `dialog_io_wizard.c` GPIO save | `"Float Switch / Level Sensor"` | NO (string mismatch with `"Float Switch"`) |
| `dialog_io_wizard.c` GPIO save | `"DHT22 Temperature/Humidity"` | NO |
| `dialog_io_wizard.c` GPIO save | `"Generic Digital Input"` | NO |
| `dialog_io_wizard.c` ADC save | hardcoded `DRIVER_NAME_ADS1115` regardless of selected ADC | YES — but loses the actual chip identity |
| `hw_discover.c` I2C name | `"SHT31"`, `"HTU21D"`, `"INA219"`, `"MCP3421"`, `"PCA9685"`, `"DS3231"`, `"SSD1306"` | NO — discoverable but no driver |

**Constants not used by any dispatch:**

- `DRIVER_NAME_DHT11` — sensor_instance.c uses raw `"DHT11"` literal at `:150`
- `DRIVER_NAME_AT24C`, `DRIVER_NAME_PCF8574`, `DRIVER_NAME_MCP23017` — referenced only by `hw_discover.c` for display; no driver exists
- `DRIVER_NAME_FLOAT_SWITCH` (`"FloatSwitch"`, no space) — sensor_instance.c compares `"Float Switch"` (with space) and `"FLOAT_SWITCH"` (uppercase). The registered constant matches neither.
- `DRIVER_NAME_WEB_POLL` (`"WebPoll"`) — used only by the dead `driver_common.c` registry. The actual web_poll route uses `module_type == "web_poll"`.

**`gsdml_modules.h` ident map vs sensor types** — `gsdml_sensor_module_from_string()` does substring matching and falls back to `GSDML_MOD_SENSOR_GENERIC`. It is reachable for any sensor name; it never errors. The mapping is fuzzy and consumed at `sensor_manager.c:446,453` to choose the PROFINET module ident at registration time. Strings produced by io_wizard's GPIO path (e.g. `"Flow Meter / Pulse Counter"`) match `"Flow"` substring → `GSDML_MOD_SENSOR_FLOW`, even though no driver will load them.

---

## 4. Driver Inventory

### 4.1 `src/sensors/drivers/`

| File | Purpose | Public symbols actually called by sensor_instance.c | Reachable from production dispatch? | Datasheet match |
|---|---|---|---|---|
| `driver_ads1115.c` (252 lines) | ADS1115/ADS1015 I2C ADC | `driver_ads1115_init` (`:241`), `driver_ads1115_read` (`:477`), `driver_ads1115_close` (`:382`) | YES | TI ADS1115 |
| `driver_bme280.c` (313) | BME280/BMP280 I2C T/H/P sensor | `driver_bme280_init` (`:161`), `driver_bme280_read` (`:433`), `driver_bme280_close` (`:358`) | YES (but only `BME280_READ_TEMPERATURE` flag — humidity/pressure unreachable from this call site) | Bosch BME280 |
| `driver_common.c` (127) | Driver registry framework | (none) | NO — entire file is dead | N/A |
| `driver_dht22.c` (297) | DHT22/AM2302 GPIO temp/humidity | `driver_dht22_init` (`:153`), `driver_dht22_read` (`:430`), `driver_dht22_close` (`:355`) | YES — but uses Pi-only mmap of /dev/gpiomem with hardcoded BCM2835 register offsets (`:20-81`), bypasses both GPIO HALs. UNVERIFIED whether this works on Odroid-XU4 (the documented target board); the BCM register layout is Broadcom-specific and the Odroid uses a Samsung Exynos 5422. |
| `driver_ds18b20.c` (217) | DS18B20 1-Wire temperature | `driver_ds18b20_init` (`:148`), `driver_ds18b20_read` (`:427`), `driver_ds18b20_close` (`:352`) | YES | Maxim DS18B20 |
| `driver_float_switch.c` (38) | Float switch GPIO input | `float_switch_init` (`:204`), `float_switch_read` (`:454`), `float_switch_destroy` (`:376`) | YES | Generic float switch |
| `driver_hx711.c` (339) | HX711 24-bit load cell ADC | `driver_hx711_init` (`:167`), `driver_hx711_read` (`:436`), `driver_hx711_close` (`:361`) | YES | Avia HX711 |
| `driver_jsn_sr04t.c` (188) | JSN-SR04T waterproof ultrasonic | `jsn_sr04t_init` (`:188`), `jsn_sr04t_read_distance_cm` (`:450`), `jsn_sr04t_destroy` (`:369`) | YES | JSN-SR04T |
| `driver_mcp3008.c` (214) | MCP3008 SPI ADC | `driver_mcp3008_init` (`:244`), `driver_mcp3008_read` (`:480`), `driver_mcp3008_close` (`:385`) | YES | Microchip MCP3008 |
| `driver_pump.h` (23) | (none — header only) | NONE | NO — no `.c` exists, no #include | UNKNOWN |
| `driver_solenoid.h` (19) | (none — header only) | NONE | NO — no `.c` exists, no #include | UNKNOWN |
| `driver_tcs34725.c` (158) | TCS34725 I2C color sensor | `tcs34725_init` (`:170`), `tcs34725_enable` (`:178`), `tcs34725_read_raw` (`:441`), `tcs34725_calculate_lux` (`:443`), `tcs34725_destroy` (`:365`) | YES | AMS TCS34725 |
| `driver_web_poll.c` (220) | HTTP JSON polling | `web_poll_init` (`:267`), `web_poll_set_headers` (`:270`), `web_poll_set_json_path` (`:271`), `web_poll_fetch` (`:497`), `web_poll_destroy` (`:395`) | YES | N/A — software |

**Each "live" driver carries TWO public surfaces:** the
`driver_xxx_init/read/close` C functions used by the production
dispatch, and a `driver_xxx_ops` `driver_ops_t` table referenced only
by the dead `driver_common.c` registry. Confirmed for ds18b20
(`driver_ds18b20.c:148`), dht22 (`:232`), bme280 (`:245`), hx711
(`:274`), ads1115 (`:192`), mcp3008 (`:131`).

### 4.2 `src/drivers/`

| File | Purpose | Used by | Reachable? |
|---|---|---|---|
| `adc/adc_driver.h` (119) | Generic ADC abstraction header | nothing — no `.c` file exists, no `#include` outside the header itself | NO — entire header is dead |
| `bus/gpio_hal.{c,h}` | GPIO HAL #1 (libgpiod-style based on glob of file) | `relay_output.c` only | YES |
| `digital/relay_output.{c,h}` | Unified output driver (`output_create`, `output_set`, `output_set_pwm`) | `actuator_manager.c` only | YES |

### 4.3 Other sensor support files

| File | Purpose | Reachable? |
|---|---|---|
| `src/sensors/sensor_api.h` | Parallel sensor framework #2 | NO — see §3.3, §6 |
| `src/sensors/analog/analog_sensor.{c,h}` | Implementation of #2 | NO — see §3.3, §6 |
| `src/sensors/hardware/hw_interface.{c,h}` | GPIO HAL #2 — sysfs-based (`hwif_gpio_export`, etc.) | YES — used by `driver_float_switch.c` and `driver_jsn_sr04t.c` |
| `src/sensors/formula_evaluator.{c,h}` | TinyExpr-style formula compiler | YES — used only by `sensor_instance.c` and `tests/test_formula.c` |

---

## 5. Duplication / CSE Opportunities

### 5.1 Three sensor frameworks coexisting

Three independent sensor framework generations live in the tree, with
the oldest two unreachable from production:

1. **Generation 1 (DEAD):** `sensor_api.h` + `analog_sensor.c` —
   `sensor_driver_t` + `sensor_config_t` + `sensor_ops_t` vtable +
   `sensor_register_driver` registry. Unifies all sensors behind one
   driver interface. ~290 + ~400 lines.
2. **Generation 2 (DEAD):** `driver_common.{c,h}` — `driver_ops_t` +
   `driver_registry[]` + `driver_get_ops_by_name`. ~330 lines including
   the per-driver `driver_xxx_ops` tables in each driver file.
3. **Generation 3 (LIVE):** `sensor_instance.c` — string-compare
   dispatch ladder + per-driver `driver_xxx_init/read/close` C
   functions. ~676 lines. This is what actually runs.

Each generation has its own type discriminator (`sensor_hw_type_t`,
`driver_type_t`, `sensor_instance_type_t` + `sensor_driver_type_t`).
None match. None reference each other.

### 5.2 Three GPIO access patterns

| HAL | Files | Used by |
|---|---|---|
| `src/drivers/bus/gpio_hal.{c,h}` | gpiod-style (assumed) | `relay_output.c` (actuator path) |
| `src/sensors/hardware/hw_interface.{c,h}` | sysfs `/sys/class/gpio` style | `driver_float_switch.c`, `driver_jsn_sr04t.c` |
| inline `/dev/gpiomem` mmap | hardcoded BCM2835 register offsets | `driver_dht22.c` |

The DHT22 driver is **board-coupled to Raspberry Pi**. It will not
work on the documented target (Odroid-XU4 + Armbian) without
modification — the Exynos 5422 GPIO controller is not at the BCM2835
register layout. UNVERIFIED whether anyone has run this on the actual
Odroid hardware.

### 5.3 Multiple raw → engineering scaling implementations

| Implementation | File:line | Behavior |
|---|---|---|
| `apply_calibration()` (linear-interp `raw_min/max → eng_min/max`, plus offset and scale_factor) | `sensor_instance.c:72-88` | Used only for ADC sensors |
| `sensor_apply_calibration()` (5-mode: linear, two-point, polynomial, lookup, Steinhart-Hart) | `analog_sensor.c:95-165` | Dead — see §3.3 |
| `driver_apply_calibration()` (`value * scale + offset`) inline | `driver_common.h:86-89` | Dead — see §3.2 |
| Per-driver post-processing in driver `_read` calls | various | Each driver can apply its own scaling before returning |

**Production runtime has only one path** (`apply_calibration()` for
ADC, raw-passthrough for everything else). The two parallel
implementations are unreachable.

### 5.4 Multiple status/quality update sites

`update_quality()` at `sensor_instance.c:33-60` is the canonical
quality computer. It is invoked at the end of `sensor_instance_read()`
(`:562`). The legacy `sensor_status` SQL table is updated separately by
`db_sensor_status_update()` (`db_modules.c:639`) — UNVERIFIED whether
any in-scope code calls it; Grep shows callers but they are outside
the in-scope file set.

### 5.5 Single PROFINET writer / reader

Confirmed by Grep:
- Sensor → wire: only `profinet_manager_update_input_with_quality()` is called from sensor_manager (`sensor_manager.c:222`). `update_input_float()` (`profinet_manager.c:1324`) has zero in-scope callers.
- Wire → actuator: only `poll_output_slots()` (`profinet_manager.c:290`) reads p-net output buffers; the only registered listener is `actuator_manager`'s `profinet_output_handler` (`actuator_manager.c:362`).

---

## 6. Dead Code

All findings here have been verified by Grep across the in-scope tree.
None of these are recommendations for deletion — they are an
inventory.

### 6.1 Driver files with no dispatch entry

| File | Status |
|---|---|
| `src/sensors/drivers/driver_pump.h` | Header only, no `.c`, no `#include` outside itself. Confirmed by Grep — only matches in `docs/`, `SOURCES.md`, `CMakeLists.txt`. |
| `src/sensors/drivers/driver_solenoid.h` | Same as above. |
| `src/sensors/drivers/driver_common.{c,h}` | Registry + ops table never queried by any in-scope caller. The `extern` `driver_ops_t` instances in each driver file (`driver_ds18b20_ops`, etc.) are linked but referenced only by this dead registry. |
| `src/drivers/adc/adc_driver.h` | Header declares `adc_create`, `adc_read_raw`, etc.; no `.c` file exists; Grep finds zero callers anywhere in the tree. |
| `src/sensors/sensor_api.h` (entire framework) | `sensor_driver_t`, `sensor_config_t`, `sensor_ops_t`, `sensor_create`, `sensor_read`, `sensor_register_driver` — declared, never instantiated by production code. The `sensor_channel_t` enum is the only symbol from this header used externally (by `dialog_io_wizard.c`). |
| `src/sensors/analog/analog_sensor.{c,h}` (entire) | Implements gen-1 framework. `analog_sensor_create`, `analog_sensor_factory`, `analog_sensor_set_adc`, `analog_sensor_cal_point` — Grep confirms zero callers outside the file. The 5 calibration presets at `analog_sensor.c:22-67` are also unused. The two utility functions `sensor_channel_name()` (`:371`) and `sensor_status_string()` (`:392`) are also unused at runtime — UNVERIFIED whether they leak into a debug or test path (a single Grep showed only the file itself). |

### 6.2 Hardware types in `gsdml_modules.h` ↔ no driver

`gsdml_modules.h` declares 7 sensor module types and 3 actuator module
types (`:39-68`). Every type can be selected by the GSDML mapper, but
the relationship to drivers is loose:

- `GSDML_MOD_SENSOR_PH/TDS/TURBIDITY` — no dedicated driver. These slot types are derivable only via an analog ADC reading + post-processing; the ADC driver path doesn't know what kind of probe is attached.
- `GSDML_MOD_SENSOR_FLOW` — same.
- `GSDML_MOD_SENSOR_LEVEL` — `HX711` matches via the substring "HX711"; otherwise generic.
- `GSDML_MOD_SENSOR_TEMP` — DS18B20, DHT22, BME280 all match via substring rules.

This is intentional in the sense that PROFINET module idents are
"channel categories" and the RTU's job is to feed them ANY sensor that
produces a value in the right unit. But there is no validation that a
chosen `module_ident` matches the actual `sensor_type` selected.

### 6.3 Unused DB columns

Verified by Grep against the union of all in-scope C files:

| Table | Column | Notes |
|---|---|---|
| `physical_sensors` | `hardware_type` | Written (always equal to `sensor_type`) but never read by `sensor_instance_create_from_db` |
| `physical_sensors` | `channel` | Written, never read |
| `physical_sensors` | `resolution` | Written, never read |
| `physical_sensors` | `unit` | Written, never read at runtime (read only by HTTP API health check, UNVERIFIED — out of scope) |
| `physical_sensors` | `min_value` / `max_value` | Written; `instance->range_min/max` are never updated from these |
| `adc_sensors` | `unit` | Read once at `sensor_manager.c:453` for ident derivation, not on the sensor instance |
| `web_poll_sensors` | (none — all read) | OK |
| `calculated_sensors` | `unit` | Written, never read |
| `static_sensors` | `unit` | Written, never read |
| `static_sensors` | `writable` | Written, never read in production. `db_static_sensor_set_value()` (`db_modules.c:617`) gates on it but has no in-scope caller |
| `actuators` | `gpio_chip` | Loaded into `db_actuator_t.gpio_chip` but never copied to runtime config (see §7-D5) |
| `actuators` | `safe_state` | Loaded but never copied; `apply_safe_state` hard-codes OFF |
| `actuators` | `status` | Loaded but never used at runtime |
| `sensor_status.consecutive_failures` | UNVERIFIED — column updated by `db_sensor_status_update` which has out-of-scope callers |

### 6.4 Constants declared but not used by any dispatch

All from `include/constants.h:264-289`:

- `MODULE_TYPE_SIMULATED` (`"simulated"`) — defined at `:289`, zero callers anywhere
- `DRIVER_NAME_DHT11` (`"DHT11"`) — sensor_instance.c uses the raw literal
- `DRIVER_NAME_AT24C`, `DRIVER_NAME_PCF8574`, `DRIVER_NAME_MCP23017` — referenced only by `hw_discover.c` for display; no drivers exist for these chips
- `DRIVER_NAME_FLOAT_SWITCH` (`"FloatSwitch"`) — defined but no production caller. sensor_instance.c uses `"Float Switch"` (with space) and `"FLOAT_SWITCH"` (uppercase) literals at `:201-202`.
- `DRIVER_NAME_WEB_POLL` (`"WebPoll"`) — only used by the dead `driver_common.c` registry

### 6.5 Struct fields declared but never assigned

| Field | Declared | Status |
|---|---|---|
| `sensor_instance_t.cal_scale`, `cal_offset` | `sensor_instance.h:99,100` | Set only by `create_cpu_temp_sensor` (`sensor_manager.c:87-88`); never used in calibration math |
| `sensor_instance_t.consecutive_successes` | `sensor_instance.h:116` | Incremented at `sensor_instance.c:546`, never read |
| `db_actuator_state_t.last_state_change`, `total_on_time_ms`, `cycle_count` | `db_actuators.h:55-57` | UNVERIFIED — `actuator_state` table updates are out of scope |

---

## 7. Canonical Data-Model Divergences (headline list)

Each finding lists what is on each side, where it lives, and a one-line
assessment. **Severity tags** (**B**locker / **W**arning / **N**it) are
this archaeologist's read; downstream agents may reweigh.

---

### D1 — Three coexisting sensor frameworks (B)

**On each side:**
- Generation 1 (`sensor_api.h` + `analog_sensor.c`): unified `sensor_driver_t` framework with vtable, registry, calibration presets. ~700 lines.
- Generation 2 (`driver_common.{c,h}` + `driver_xxx_ops` tables in each driver): unified `driver_ops_t` registry. ~330 lines.
- Generation 3 (`sensor_instance.c` string-dispatch ladder): the actual production runtime. ~676 lines.

**Citations:** `src/sensors/sensor_api.h:248-290`, `src/sensors/analog/analog_sensor.c:267-365`, `src/sensors/drivers/driver_common.c:57-127`, `src/sensors/sensor_instance.c:109-343,412-567`.

**Assessment:** Two-thirds of the sensor framework code is dead. The live runtime is the most awkward of the three (manual three-way switch synchronization for create/read/destroy). This is the highest-leverage cleanup target in the slice.

---

### D2 — `physical_sensors.sensor_type` vs `physical_sensors.hardware_type` (W)

**On each side:**
- DB schema: BOTH columns exist (`database.c:13`), `sensor_type` is `NOT NULL`, `hardware_type` is nullable.
- All write paths set `phys.hardware_type = phys.sensor_type` (e.g. `dialog_sensor.c:267-268`, `dialog_io_wizard.c:1700,1723`).
- Production read path at `sensor_instance.c:146-217` reads only `sensor_type`. `hardware_type` is loaded into `db_physical_sensor_t.hardware_type` but never used.

**Assessment:** `hardware_type` is dead-write storage. Two columns express one concept; nobody reads the second one.

---

### D3 — TUI sensor types vs dispatch coverage (B)

**On each side:**
- `dialog_sensor.c:42` advertises 5 module types: physical, adc, web_poll, calculated, static.
- `dialog_sensor.c:264-293` (save path) only INSERTs into `physical_sensors` or `adc_sensors`. The other three module types create an orphaned `modules` row.
- `sensor_instance.c:255-331` dispatch DOES handle web_poll, static, calculated — but only if a sub-record exists.
- `dialog_sensor.c:44-48` `hardware_types[]` includes 6 strings ("Flow Sensor", "pH Sensor", "TDS Sensor", "Turbidity", "Generic", "Float Switch") of which only `"Float Switch"` matches a dispatch entry.
- `dialog_io_wizard.c:139-148` GPIO sensor types use display-style names (`"Flow Meter / Pulse Counter"`, `"Float Switch / Level Sensor"`, `"DHT22 Temperature/Humidity"`, `"Generic Digital Input"`) — none match a dispatch entry; all are saved into `phys.sensor_type` verbatim at `dialog_io_wizard.c:1776`.
- `hw_discover.c:407-427` returns `"SHT31"`, `"HTU21D"`, `"INA219"`, `"MCP3421"`, `"PCA9685"`, `"DS3231"`, `"SSD1306"` for I2C devices; the wizard saves these verbatim at `dialog_io_wizard.c:1721`. None have a driver.

**Assessment:** A typical wizard run for a discovered I2C SHT31, or a TUI add of a pH Sensor, will create a DB row that LOG_ERRORs at sensor load with `Unsupported physical sensor type: <name>`. The user-facing "Add Sensor" surface advertises configurations the runtime cannot service.

---

### D4 — `dialog_io_wizard` ADC save hardcodes ADS1115 (W)

**On each side:**
- The wizard discovers ADCs by scanning I2C and presents the user a chip choice.
- `save_sensor()` at `dialog_io_wizard.c:1751` writes `adc.adc_type = DRIVER_NAME_ADS1115` regardless of which chip was selected.
- ADS1015 fed through this path will be misidentified but still works because the dispatch at `sensor_instance.c:237-238` treats both strings identically.
- An MCP3008 selected via this path would be saved as ADS1115 — the save would corrupt the type.

**Assessment:** Real but masked because ADS1115 dominates the supported list. Becomes a bug the moment the wizard discovers any non-ADS1115 ADC.

---

### D5 — Actuator DB column `gpio_chip` is dropped at runtime (B)

**On each side:**
- DB schema: `actuators.gpio_chip TEXT DEFAULT 'gpiochip0'` (`database.c:77`).
- `db_actuator_t.gpio_chip` (`db_actuators.h:39`) — loaded by `db_actuator_get` (UNVERIFIED — db_actuators.c:0 not read but the symbol exists).
- `dialog_actuator.c:629-674` form correctly captures and persists it.
- `actuator_manager_reload()` at `actuator_manager.c:868-881` does NOT copy `gpio_chip` to `actuator_config_t`.
- `actuator_config_t` at `actuator_manager.h:59-80` has no `gpio_chip` field.
- `init_actuator_driver()` at `actuator_manager.c:86-121` constructs `output_config_t` with no chip name.
- `output_config_t` at `relay_output.h:40-64` also has no `gpio_chip` field.

**Assessment:** Multi-chip GPIO boards are unsupported despite the DB and TUI capturing the field. Whatever `gpio_hal` defaults to is what every actuator uses. On a board with `gpiochip0` AND `gpiochip1`, an actuator pin saved against `gpiochip1` will silently land on the wrong chip.

---

### D6 — Actuator DB column `safe_state` is dropped at runtime (B)

**On each side:**
- DB schema: `actuators.safe_state TEXT DEFAULT 'hold'` (`database.c:78`), enum `SAFE_STATE_OFF/ON/HOLD` (`db_actuators.h:23-26`).
- TUI captures it (`dialog_actuator.h:34`, `dialog_actuator.c:367`).
- `actuator_manager_reload()` does not copy `db_act->safe_state` to runtime.
- `actuator_config_t` has no `safe_state` field.
- `apply_safe_state()` at `actuator_manager.c:232-266` hard-codes `ACTUATOR_STATE_OFF` for every actuator.

**Assessment:** Safety semantics specified by the operator are silently ignored. An operator who configures `safe_state = HOLD` on a critical valve to maintain its position on disconnect will instead see it forced OFF after the safe_state timeout.

---

### D7 — `actuators.min_on_time_ms` semantics drift (W)

**On each side:**
- DB column name: `min_on_time_ms`. Documented in `db_actuators.h:41` as "Minimum on time (anti-short-cycle)".
- TUI form field: `min_on_time_ms` (`dialog_actuator.h:35`).
- Runtime field: `actuator_config_t.min_cycle_time_ms` — name changed during copy at `actuator_manager.c:881`.
- Runtime usage at `actuator_manager.c:632`: treated as a debounce/anti-chatter cycle time gating commands. Comment says "anti-chatter".

**Assessment:** "Minimum ON time" and "minimum cycle time" are different concepts. The first prevents a relay from going OFF too soon after going ON; the second throttles the rate at which commands are accepted. The runtime implements the second; the DB and UI document the first.

---

### D8 — `actuators.max_on_time_ms` lossy ms→sec conversion (W)

**On each side:**
- DB column: `max_on_time_ms` (milliseconds).
- TUI form: `max_on_time_ms` (`dialog_actuator.h:36`).
- Runtime: `actuator_config_t.max_on_time_sec` — divided by 1000 with `(ms+999)/1000` ceiling rounding at `actuator_manager.c:880`.
- Runtime usage at `:194`: `(uint64_t)max_on_time_sec * 1000`.

**Assessment:** `max_on_time = 1500ms` from the DB becomes `2 sec` at runtime, then `2000 ms` for comparison. Sub-second granularity from the operator is lost. Anything from 1ms to 999ms becomes 1 sec. A user-set value below 1 second behaves as if set to 1 second.

---

### D9 — `actuator_type_t` has 6 values, runtime collapses to 2 behaviors (W)

**On each side:**
- Enum: RELAY, PWM, LATCHING, MOMENTARY, PUMP, VALVE (`db_actuators.h:10-17`).
- TUI picker: all 6 names exposed (`dialog_actuator.c:44-51`).
- Runtime dispatch at `actuator_manager.c:93-105`: only RELAY vs PWM behavior selected.
- LATCHING (pulse-to-toggle) and MOMENTARY (timed pulse) — `relay_output.h:25-26` defines these enum values, but `init_actuator_driver` never selects them.

**Assessment:** Real semantic types in the DB and TUI; flat fall-through at runtime. Selecting LATCHING gives the user a relay that holds high. Operator surprise hazard.

---

### D10 — sensor and actuator slot keyspaces overlap, no cross-check (B)

**On each side:**
- Sensors: `modules.slot UNIQUE`, range 2-246 (CPU temp at slot 1).
- Actuators: `actuators.slot UNIQUE`, range 2-246.
- The two UNIQUE constraints are independent — they don't enforce uniqueness across both tables.
- `dialog_io_wizard.c:386-426` `find_next_slot()` queries one table or the other, not both.
- Both subsystems independently call `profinet_manager_add_module()` against the shared slot table.
- `dialog_io_wizard.c:394` `bool used[17]` is sized for 17 entries but indexed up to 246 — buffer overrun risk if even one slot above 16 is in use.

**Assessment:** Bug-prone keyspace collision. Two writers, no coordinator. The buffer overrun is independently bad.

---

### D11 — `module_ident` from DB is dropped, recomputed by string match (W)

**On each side:**
- DB stores `modules.module_ident INTEGER` (`database.c:8`).
- Both TUI write paths set `module.module_ident = GSDML_MOD_SENSOR_GENERIC` regardless of sensor type (`dialog_sensor.c:67`, `dialog_io_wizard.c:1683`).
- `db_module_list()` loads `modules.module_ident` into `db_module_t.module_ident` (`db_modules.c:36`).
- `sensor_manager.c:443-456` ignores `module->module_ident` and recomputes via `gsdml_sensor_module_from_string(phys.sensor_type)` or `(adc.unit)`.

**Assessment:** The persisted `module_ident` is meaningless — always `GSDML_MOD_SENSOR_GENERIC` because nothing else writes it. The runtime works around this with substring matching that only sometimes hits the right ident. Two sources of truth, both unreliable.

---

### D12 — Three GPIO HAL implementations (W)

**On each side:**
- HAL #1: `src/drivers/bus/gpio_hal.{c,h}` — used by actuator path only.
- HAL #2: `src/sensors/hardware/hw_interface.{c,h}` — sysfs-based, used by `driver_float_switch.c` and `driver_jsn_sr04t.c`.
- HAL #3: inline /dev/gpiomem mmap with hardcoded BCM2835 register offsets — used only by `driver_dht22.c` (`:20-81`).

**Assessment:** Pi-specific GPIO #3 makes DHT22 board-coupled. The codebase claims board-agnosticism (CLAUDE.md "Board-Agnostic Development"), but DHT22 violates it at the register-write level. UNVERIFIED whether this works on the documented Odroid target — a quick read of the code suggests the BCM register layout will not match Exynos.

---

### D13 — `MODULE_TYPE_PHYSICAL/_ADC/_WEB_POLL` constants exist; `static`/`calculated` use raw literals (N)

**On each side:**
- `constants.h:286-289` defines `MODULE_TYPE_PHYSICAL`, `_ADC`, `_WEB_POLL`, `_SIMULATED`.
- `sensor_instance.c:274,286` and `sensor_manager.c:443,449` use the macros consistently for the first three.
- For `static` and `calculated`, the raw string literals are used: `sensor_instance.c:274` (`"static"`) and `:286` (`"calculated"`). No constants defined.
- `MODULE_TYPE_SIMULATED` is defined but no caller exists.

**Assessment:** Stylistic asymmetry. Bug surface = a typo in either dispatch site. Easy to fix; not load-bearing.

---

### D14 — `dialog_sensor.c` cannot create web_poll, calculated, or static sensors despite advertising them (W)

**On each side:**
- Picker advertises 5 types (`dialog_sensor.c:42`).
- Save path only writes `physical_sensors` or `adc_sensors` (`:264-293`).

**Assessment:** Selecting any of the unsupported three writes a `modules` row with no sub-record. The next sensor reload then logs an error and the row hangs as a phantom. This is a UI lie.

---

## Summary

**Found 14 canonical-model divergences, 6 dead files (driver_pump.h, driver_solenoid.h, driver_common.{c,h}, adc_driver.h, sensor_api.h-as-framework, analog_sensor.{c,h}), and 5 dispatch-table mismatches (TUI sensor picker → dispatch ladder, io_wizard GPIO names → dispatch, io_wizard ADC type erasure, gsdml ident map vs sensor type, three coexisting frameworks).**

**Top 3 divergences by severity:**

1. **D1 — Three coexisting sensor frameworks (Blocker).** Two-thirds
   of the sensor framework code is dead. Cleanup or commitment to one
   framework is the highest-leverage move in this slice.
   `src/sensors/sensor_api.h`, `src/sensors/analog/analog_sensor.{c,h}`,
   `src/sensors/drivers/driver_common.{c,h}` versus the live
   `src/sensors/sensor_instance.c`.

2. **D5 + D6 — Actuator `gpio_chip` and `safe_state` columns dropped
   at runtime (Blocker).** Operator-configured safety state is
   ignored; multi-chip GPIO boards are unsupported despite the DB and
   TUI both capturing the data correctly. The bug is exclusively in
   `src/actuators/actuator_manager.c:868-881` and the surrounding
   `actuator_config_t` definition.

3. **D3 — TUI advertises sensor types and hardware names that the
   dispatch ladder cannot reach (Blocker).** A typical wizard run for
   a discovered SHT31, INA219, or pH probe creates a DB row that
   refuses to load with `Unsupported physical sensor type`. The
   io_wizard's GPIO sensor names ("Float Switch / Level Sensor",
   "Flow Meter / Pulse Counter", etc.) are also unmatched —
   `dialog_io_wizard.c:139-148` versus `sensor_instance.c:146-217`.
