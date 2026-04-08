# HIL Findings — Real RTU at 192.168.6.21

**Target:** rtu-ec3b (Odroid-XU4, Samsung Exynos5422, armv7l, Linux 6.6.113-current-odroidxu4)
**Binary:** `/usr/local/bin/water-treat vfedfe94` built Feb 10 2026 (predates current main)
**Deploy state:** bare device; `/etc/water-treat/water-treat.conf` from Feb 6 2026; all sensor/actuator DB tables empty
**Daemon:** PID 1025 running as root, systemd Type=notify, watchdog 30s
**Evidence captured:** live SSH, `sudo` via password, HTTP to localhost:9081, sqlite3 on `/var/lib/water-treat/water-treat.db`, `strings` on the binary

---

## HALLUCINATION candidates

### [HIL-1] HTTP route prefix drift — CLAUDE.md says `/api/v1/...` but binary serves `/...`, AND a stale log message inside the code repeats the wrong URL

**CLAUDE.md claim** (RTU HTTP API Contract section):
> Endpoint `/api/v1/slots` GET → current PROFINET slot configuration
> Endpoint `/api/v1/gsdml` GET → raw GSDML XML file

**Runtime evidence on device:**
```
$ curl http://localhost:9081/api/v1/slots
{"error": "Not Found", "endpoints": ["/health", "/metrics", "/ready", "/live", "/config", "/slots", "/gsdml"]}
$ curl http://localhost:9081/slots
{"slot_count": 1, "slots": [...]}
```

**Code evidence:**
- `src/health/health_check.c:881` — handler: `else if (strcmp(path, "/slots") == 0)`
- `src/health/health_check.c:890` — handler: `else if (strcmp(path, "/gsdml") == 0)`
- `src/health/health_check.c:897-898` — 404 response endpoints list: `/health, /metrics, /ready, /live, /config, /slots, /gsdml` — no `/api/v1/` prefix anywhere
- `src/health/health_check.c:930` — **STALE LOG ERROR** still says: `LOG_ERROR("Controller discovery via /api/v1/slots and /api/v1/gsdml will be unavailable");`
  - This log line fires when HTTP socket creation fails; it tells the operator to check the wrong URLs
- No `/api/v1/*` string appears anywhere else in the binary (confirmed via `strings`)

**Impact:**
1. Any consumer following CLAUDE.md's documented URLs will 404
2. The Water-Controller repo is the primary consumer — its discovery chain must be cross-checked (see docs/CONTROLLER_IMPLEMENTATION_GUIDE.md for the controller-side contract)
3. On HTTP bind failure, the operator gets misled by the log message into troubleshooting the wrong URLs

**Fix:**
1. Decide the canonical scheme — `/slots`+`/gsdml` (current code) or `/api/v1/slots`+`/api/v1/gsdml` (CLAUDE.md)
2. Update whichever side loses (most efficient: update CLAUDE.md to match the simpler code)
3. Fix the stale log message at `src/health/health_check.c:930`
4. Verify Water-Controller client code uses the same URLs — if it's on `/api/v1/*`, it's broken and a joint fix is needed

**Severity:** HIGH (documentation and code are in conflict; Water-Controller may be broken; operator-facing log message lies)

---

### [HIL-2] `/slots` endpoint contract drift — CLAUDE.md says "DB-only" but code intentionally mixes DB + runtime CPU-temp sensor

**Original symptom:** `/slots` returns slot_count=1 while all DB sensor tables are empty.

**Root cause (traced in code):**

1. `src/sensors/sensor_manager.c:23-29` — intentional design:
   > "Every Linux board has CPU temperature available via sysfs thermal zone. Registered as slot 1, temperature sensor type."
2. `src/sensors/sensor_manager.c:477-509` (`sensor_manager_init`) — always creates a CPU temperature sensor at `CPU_TEMP_SLOT` (slot 1) at startup, regardless of DB state.
3. `src/sensors/sensor_manager.c:421` — slot 1 is a reserved slot; the DB-backed add-sensor path rejects attempts to reuse it.
4. `src/profinet/profinet_manager.h:113-116` — the `profinet_manager_get_slot_list()` doc string explicitly says:
   > "Returns ALL slots registered with PROFINET manager, including: Database-configured sensors/actuators; Runtime-created sensors (e.g., CPU temperature at slot 1)"
5. `src/profinet/profinet_manager.c:1467-1501` — `profinet_manager_get_slot_list()` reads from `g_pn.slots[]` (the runtime plug table), NOT from `db_module_list()`.
6. `src/health/health_check.c:599` — the `/slots` endpoint's own doc comment is aligned with the code:
   > "/slots returns ALL plugged modules (database + runtime like CPU temp). DAP (slot 0) is NOT included — its configuration is fixed in the GSDML."

**Conflict:** CLAUDE.md says (RTU HTTP API Contract, Implementation Rules):
> "Data source: `db_module_list()` (database), NOT p-net runtime state. Available before `pnet_init()`."

The code does the opposite, intentionally, and has done so for at least as long as the CPU-temp-slot comment has existed.

**Revised finding:** This is a **documentation bug in CLAUDE.md**, not a runtime bug. The implementation is internally consistent (sensor_manager.h, profinet_manager.h, profinet_manager.c, health_check.c all agree that the endpoint reports runtime state). The CLAUDE.md "db_module_list() only" rule is stale.

**Impact on fresh deploy:**
- The controller WILL see at least one slot (the CPU temp) on any RTU, even a bare one. This is probably desirable — it proves the RTU is reachable and configured enough to report data.
- If the Water-Controller repo's discovery chain (the consumer) is coded to the CLAUDE.md rule, it may be surprised by the CPU-temp slot. Needs cross-check against Water-Controller.
- The CLAUDE.md doc lies about the contract and will cause future Claude (and human) investigators to mis-diagnose.

**Fix**: Update CLAUDE.md to match reality. Text should say: "Data source: `profinet_manager_get_slot_list()` which returns runtime plug state including the reserved slot 1 CPU temperature sensor and all database-configured application modules. DAP (slot 0) is excluded from the response. Empty DB still yields one slot (CPU temp) so the controller can discover a basic input before any field configuration."

**Severity:** MEDIUM (documentation drift, not a code defect; downgraded from CRITICAL after root-cause analysis)

---

## MISMATCH findings

### [HIL-3] Hardware type strings have mixed case in the binary — risk of silent dispatch failure

**Lowercase hardware_type strings in binary:**
```
ads1115, dht22, ds18b20, mcp3008, calculated, physical, static, web_poll, relay
```

**Uppercase variants also present:**
```
ADS1115, DHT22, DS18B20, MCP3008, BME280, FLOAT_SWITCH, HX711, TCS34725, Pump, Relay
```

**SQL query in binary uses UPPERCASE:**
```sql
SELECT m.name FROM physical_sensors ps JOIN modules m ON ps.module_id = m.id
  WHERE ps.address = ? AND ps.sensor_type IN ('DHT22', 'DHT11', 'FLOAT_SWITCH', 'GPIO');
```

**Impact:** If the TUI writes a hardware_type in one case and the dispatch table compares in the other case, the dispatch silently fails (sensor never reads) or the sensor is considered "unknown" and dropped. The reality-checker + archaeologist agents need to confirm: which case does the TUI write? Which case does the dispatch `strcmp()` against? Which case does the SQL query use?

**Known facts:**
- `BME280`, `FLOAT_SWITCH`, `HX711`, `TCS34725` are **only** present in uppercase
- `ADS1115`, `DHT22`, `DS18B20`, `MCP3008` are present in **both** cases
- The sensor_type SQL `IN` clause is exclusively uppercase

This is a canonical data-model divergence. The archaeologist's dispatch-table audit will close this.

**Severity:** HIGH (silent failure; data loss; bug will only manifest when a user adds a sensor of the wrong-cased type)

---

### [HIL-4] Default `gpio_chip = 'gpiochip0'` is nonsensical on Odroid-XU4

**DB schema default (verified from `.schema` output):**
```sql
gpio_chip TEXT DEFAULT 'gpiochip0'
```

**Actual `gpiochip0` on the target hardware** (from `gpiodetect`):
```
gpiochip0 [gpy7] (8 lines)
```

`gpy7` is an internal Exynos5422 bank, not the Odroid-XU4 40-pin expansion header. The header pins are distributed across `gpa0`, `gpa1`, `gpa2`, `gpb0`, `gpb1`, `gpb2`, `gpx1`, `gpx2` — each exposed as its own gpiochipN.

**Impact:** If a field tech adds an actuator in the TUI and accepts the default `gpio_chip`, they land on `gpy7` pins 0-7, which are not exposed on the header. The actuator will either fail to request the line, or will toggle a GPIO that's wired to nothing — or worse, wired to something internal. On Raspberry Pi the default would work. On Odroid it does not.

The existing board-detection code (`board_detect.c` per CLAUDE.md) should be populating a sane default at config-generation time, but the DB schema-level default is still `gpiochip0`. If a sensor/actuator is added via SQL or via a TUI dialog that doesn't set `gpio_chip`, the default kicks in and it's wrong.

**Severity:** HIGH (broken defaults; will bite anyone who doesn't manually override)

---

### [HIL-5] Vendor/Device ID in CLAUDE.md regression note conflicts with actual runtime

**CLAUDE.md** (Station Name Investigation Results):
> device identity (Vendor 0x0493, Device 0x0001) **MATCHES** between controller and RTU

**Runtime** (live `/config`):
```json
"vendor_id": 626,   // 0x0272
"device_id": 3520   // 0x0DC0
```

**GSDML in binary:**
```
Vendor ID: 0x0272 (Phoenix Guardians)
Device ID: 0x0DC0 (Defensive Cyber Operations)
```

**water-treat.conf:**
```
vendor_id = 0x0272
device_id = 0x0DC0
```

**Impact:** CLAUDE.md's regression-thread investigation note is stale or refers to a different test environment. The actual running code is internally consistent on `0x0272/0x0DC0`. This isn't a code bug — it's a documentation bug in CLAUDE.md. Low impact on code, but a reader following the regression investigation will be misled.

**Severity:** LOW (docs-only; but should be corrected in CLAUDE.md to avoid red-herring troubleshooting)

---

## DEPLOY-STATE findings (blocks fresh deploy if not handled)

### [HIL-6] No kernel modules pre-loaded for sensor drivers

`lsmod | grep -iE "gpio|i2c|spi|w1"` returns only `gpio_keys`. Missing:
- `spidev` — required for MCP3008 (module exists in `/lib/modules/6.6.113-current-odroidxu4/kernel/drivers/spi/spidev.ko`, not loaded)
- `w1_gpio` — required for 1-wire (DS18B20)
- `w1_therm` — required for DS18B20 temperature slaves
- `i2c_dev` — apparently built-in (both `/dev/i2c-2` and `/dev/i2c-4` present)

`/sys/bus/w1/devices/` does not exist — 1-wire subsystem is entirely absent.
`/dev/spidev*` does not exist — spidev is not loaded.

**Impact:** If the fresh deploy adds a DS18B20 or MCP3008 sensor before `modprobe` loads these modules, the driver will fail at open time. Three options:
1. `bootstrap.sh` must run `modprobe w1_gpio w1_therm spidev` at first boot
2. Systemd unit must depend on a "hardware ready" target that loads modules
3. Each driver must gracefully detect absence and fail-at-add-time (not fail-at-first-read-time)

**Severity:** HIGH for deploy planning (the fresh deploy will fail silently for these drivers without intervention)

---

### [HIL-7] I2C bus 2 runs at 65 kHz — non-standard clock rate

From dmesg:
```
[    1.843636] s3c-i2c 12c80000.i2c: bus frequency set to 65 KHz
```

Standard I2C speeds are 100 kHz (standard), 400 kHz (fast), 1 MHz (fast+), 3.4 MHz (high). 65 kHz is unusual. Either:
- Set for reliability on long traces
- Set by device-tree default on this Armbian image
- A leftover from debugging

**Impact on cyclic budget:** At 65 kHz, a single-byte I2C transaction costs ~200µs (9 clock bits + overhead); a 2-byte ADS1115 read costs ~400µs. If the cyclic loop targets 1 ms, one I2C read consumes 40% of the budget. For 5+ I2C sensors this is untenable.

The code reviewer and test strategist need to be aware of this bus speed when reasoning about the cyclic budget.

**Severity:** MEDIUM (performance; may cause cyclic overruns with multiple I2C sensors)

---

### [HIL-8] Journal is flooded with LLDP DEBUG frames

Every 5 seconds the journal records a full LLDP send and occasionally an LLDP receive, at log level DEBUG. Over 2400 seconds of uptime that's 480+ LLDP entries, drowning out real events.

**Impact:** On a production RTU with `log_level = info` (per water-treat.conf), this is filtered out at the application level — but the lines quoted above are coming from journald which only filters by systemd priority, and the app appears to be sending at `LOG_DEBUG` level unconditionally. If the deploy sets `log_level = info` but the LLDP frames still appear, that's a logging-level bug.

Need to verify: does `log_level = info` actually suppress LLDP(1008) in the binary?

**Severity:** MEDIUM (noise; masks real errors; SD-card wear if persistent)

---

## VERIFIED findings (good news, captured for completeness)

### [HIL-V1] SQL statements are parameterized

All the `INSERT`, `UPDATE`, `DELETE` and `SELECT` statements extracted from the binary use `?` placeholders. No string concatenation into SQL visible in the binary. The formula evaluator and sensor name paths still need to be checked by the code reviewer, but the DB CRUD layer is clean.

### [HIL-V2] Duplicate GPIO pin check considers both `gpio_pin` AND `gpio_chip`

```sql
SELECT id, name FROM actuators WHERE gpio_pin = ? AND gpio_chip = ? AND id != ?;
```

Two actuators can both use pin 5 if they're on different chips. Correct multi-chip behavior.

### [HIL-V3] Five sensor tables match the schema I inspected

Every `INSERT INTO` and `SELECT FROM` in the binary references one of: `physical_sensors`, `adc_sensors`, `web_poll_sensors`, `calculated_sensors`, `static_sensors`, `modules`, `actuators`, `actuator_state`. No references to a nonexistent `sensors` table.

### [HIL-V4] HTTP JSON shape matches CLAUDE.md spec (modulo the route prefix bug)

The actual `/slots` response has exactly the documented six fields per slot (`slot`, `subslot`, `module_ident`, `submodule_ident`, `direction`, `data_size`) — no device-level metadata, no `module_type` or `name`. CLAUDE.md's spec matches the implementation at the JSON-shape level.

### [HIL-V5] Station name auto-generation follows IEC 61158-6

`rtu-ec3b` derived from MAC `00:1E:06:39:EC:3B` last four hex chars, lowercase. Matches the format spec.

---

## Additional HIL findings (appended from second probe)

### [HIL-9] CPU-temp reserved-slot enforcement is in the add path

`src/sensors/sensor_manager.c:421` — a defensive check rejects attempts to add a sensor at `CPU_TEMP_SLOT`. Confirmed in source.

**Question for UX audit** (hand off): does the TUI surface this rejection with a specific message ("slot 1 is reserved for CPU temperature sensor"), or does it show a generic "failed to add sensor" error?

### [HIL-10] Actuator watchdog is real and armed

Journal log line at startup:
```
Actuator watchdog config: interval=1000ms, command_timeout=5000ms, degraded_delay=3000ms, safe_state_timeout=30000ms
```

And the binary contains the string:
```
WATCHDOG TIMEOUT: Actuator '%s' (slot %d, GPIO %d) exceeded max on time %d sec (was on for %llu sec), forcing OFF
```

Good — actuators have a safety watchdog forcing OFF on max-on-time overrun. This is the right kind of defensive engineering for a pump/solenoid device.

**Verification needed** (hand off to code reviewer): when `max_on_time_ms == 0` (the DB default), is the watchdog disabled or does it treat 0 as "immediate timeout"? This is a critical edge case for default-configured actuators.

### [HIL-11] Sensor status table never written

`SELECT * FROM sensor_status` returns zero rows even though the CPU-temp sensor is active and reporting via `/metrics` (`water_treat_sensors_active 1`).

The `sensor_data_log` table is also empty despite a stated 60s logger interval.

Two possibilities:
1. The CPU-temp runtime sensor deliberately doesn't persist because it has no `module_id` in the DB (it's not in the `modules` table)
2. The data logger never persists runtime-only sensors

**Implication for fresh deploy:** after deploy, `sensor_status` will presumably be populated for DB-backed sensors. But the CPU-temp sensor will remain a ghost — present in `/slots`, `/health`, and `/metrics` but never in any SQL query. Any tooling or SCADA query that reads `sensor_status` to inventory sensors will miss the CPU temp. Probably OK but should be documented.

### [HIL-12] Three PROFINET `.pcapng` files shipped in `/opt/water-treat/`

Device has `28ddae8_.pcapng`, `profi.pcapng`, plus the source tree. The `28ddae8_.pcapng` matches the filename referenced in CLAUDE.md's "POTENTIAL REGRESSION WARNING" section for the 88191bc investigation.

The repo commit `1fa7e20` (most recent on main) is titled "chore: remove debug pcap captures from repo" — so these pcaps were DELETED from the repo on or after that commit, but the DEPLOYED installation on this RTU still has them. Fresh deploy will not include them. Non-issue, just noted.

### [HIL-13] Five thermal zones available

`/sys/class/thermal/`:
- `thermal_zone0: cpu0-thermal / 46000` (46.0°C)
- `thermal_zone1: cpu1-thermal / 46000`
- `thermal_zone2: cpu2-thermal / 52000`
- `thermal_zone3: cpu3-thermal / 47000`
- `thermal_zone4: gpu-thermal / 42000`

The CPU-temp sensor at slot 1 reads whichever zone `sensor_manager.c:53` picks as "available." Which one? The code reviewer should verify the fallback order matches what the Odroid-XU4 user would expect — probably `cpu0-thermal` as the primary.

### [HIL-14] No PROFINET controller is present on the wire

5-second tcpdump filter `ether proto 0x8892 or udp port 34964` captured **zero** PROFINET frames. Only traffic at ethertype `0x9104` from two Extreme Networks switches (EDP/EAPS — non-PROFINET). This means:

- The current daemon health status `"profinet": "Waiting for controller connection"` is accurate
- The `88191bc` regression thread (RTU doesn't respond to PROFINET Connect) cannot be investigated from this device alone — no controller is attempting to connect. Out of scope for this campaign anyway.
- Fresh deploy should verify controller connectivity on first-boot post-deploy.

### [HIL-15] systemd notify and watchdog wired correctly

Strings in binary confirm `sd_notify` + `READY=1` + `WATCHDOG=1` + `STOPPING=1` + `STATUS=<message>` + `RELOADING=1`.

Journal confirms: `[INFO] Notified systemd: service ready` and `[INFO] Systemd watchdog enabled (interval: 30000 ms)`.

Also confirms `water-treat.service` uses `Type=notify`, `WatchdogSec=30`, `RuntimeDirectory=water-treat` — all correct.

**Verified** — systemd integration is good.

## Summary

| Severity | Count | IDs |
|---|---|---|
| CRITICAL | 0 | (HIL-2 downgraded after root-cause to MEDIUM) |
| HIGH | 3 | HIL-1, HIL-3, HIL-4, HIL-6 (HIL-6 may be HIGH or MEDIUM depending on whether bootstrap loads modules — TBD) |
| MEDIUM | 3 | HIL-2 (docs), HIL-7 (I2C bus speed), HIL-8 (LLDP spam) |
| LOW | 1 | HIL-5 (vendor ID docs drift) |
| VERIFIED | 6 | HIL-V1..V5 + HIL-15 |
| INFO | 3 | HIL-9, HIL-11, HIL-12, HIL-13, HIL-14 |

**Top items for the fresh deploy (ordered by severity + effort-to-fix):**

1. **HIL-1** (HIGH): Decide `/api/v1/slots` vs `/slots`. The code handles `/slots`, CLAUDE.md documents `/api/v1/slots`, and `health_check.c:930` still has a stale log message referencing `/api/v1/*`. Pick one and align CLAUDE.md, the log message, and the Water-Controller client. Simplest: update CLAUDE.md + fix the log line.
2. **HIL-3** (HIGH): Hardware type case drift — `BME280`/`HX711`/`TCS34725`/`FLOAT_SWITCH` are uppercase-only in the binary, `ads1115`/`dht22`/`ds18b20`/`mcp3008` appear in both cases. Needs canonical case enforcement at write time (ideally at the TUI dialog + at the DB insert path).
3. **HIL-4** (HIGH): `gpio_chip TEXT DEFAULT 'gpiochip0'` lands on `gpy7` on Odroid-XU4, not the header pins. Either change the default to match the detected board or block TUI save without an explicit chip selection.
4. **HIL-6** (HIGH-or-MEDIUM): Verify `bootstrap.sh` loads `spidev`, `w1_gpio`, `w1_therm` at first boot. If it doesn't, DS18B20 and MCP3008 sensors will fail silently post-deploy. Read `bootstrap.sh` and confirm.
5. **HIL-2** (MEDIUM): Update CLAUDE.md to match the actual `/slots` behavior ("DB + runtime CPU temp"), since the implementation is intentional and internally consistent but the docs lie.
6. **HIL-7** (MEDIUM): I2C bus 2 running at 65 kHz bites the cyclic budget if 5+ I2C sensors are configured. Decide whether to tune via device-tree or to flag this for deploy planning.
7. **HIL-8** (MEDIUM): LLDP DEBUG logging needs to respect `log_level = info` configuration.

**For downstream agents:**
- Code reviewer: verify actuator watchdog `max_on_time_ms == 0` edge case (HIL-10)
- UX auditor: verify TUI surfaces reserved-slot rejection message for slot 1 (HIL-9)
- Test strategist: the repo already has `tests/test_calibration.c`, `test_formula.c`, `test_profinet_data.c`, `test_alarms.c`, `test_config.c` — build on these rather than duplicating
- Historical context: a prior `/opt/water-treat/REVIEW.md` (dated 2025-12-17) was a production-readiness review by Claude Opus 4.5. The in-repo version at `REVIEW.md` may have the same content or may have diverged. Worth a skim for prior findings — but only if fast.
