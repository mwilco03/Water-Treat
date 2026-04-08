# DEPLOY_FAILURES.md — What will break on the fresh deploy

**Target:** Fresh `bootstrap.sh fresh` deploy to Odroid-XU4 RTU (currently at 192.168.6.21 running `vfedfe94` from Feb 10 2026)
**Date of review:** 2026-04-08
**Reviewer:** live HIL recon + source grep on `/mnt/cephfs/shared/projects/Water-treat/`
**Upstream agent reports pending:** research (SOURCES.md), archaeology (ARCHAEOLOGY.md), reality-checker (REALITY_CHECK.md), code-reviewer (CODE_REVIEW.md), ux-designer (UX_AUDIT.md). This document will be updated once those land; the items below are what I could confirm from HIL + repo inspection alone.

Priority legend:
- **BLOCKER** — the deploy will not complete, or the daemon will not start, or core connectivity will be broken. Fix before deploying.
- **P0** — deploy completes but a core advertised feature will silently fail the first time a user exercises it. Fix before deploying.
- **P1** — feature works but with degraded behavior, silent wrong defaults, or broken error surfacing. Can deploy, but fix is required for a clean field experience.
- **P2** — documentation drift, stale log messages, operator-facing strings that will mislead troubleshooting. Fix before the first external user touches the deploy.
- **P3** — cosmetic.

---

## BLOCKER (none confirmed)

No BLOCKER items confirmed from HIL + repo inspection alone. The bootstrap flow itself and the daemon's ability to come up to "degraded, waiting for controller connection" is currently validated on the live device — that bar will be met on fresh deploy.

The agent reports (once they land) may surface BLOCKERS hiding in code paths I did not exercise. This section will be updated.

---

## P0 — silent feature failures that will bite first-time users

### P0-1 Kernel modules for SPI and 1-wire are never loaded by the deploy

**Evidence:**
- `grep -rnE "modprobe|spidev|w1_gpio|w1_therm" bootstrap.sh scripts/ systemd/` — **zero matches** for module loading.
- `scripts/99-water-treat.rules:5` has `SUBSYSTEM=="spidev", MODE="0666"` — a udev rule that only fires if `/dev/spidev*` already exists.
- `systemd/water-treat.service:58` has `DeviceAllow=/dev/spidev* rw` — grants access IF the device node exists. Does not create it.
- Live device confirms: `/dev/spidev*` does not exist, `/sys/bus/w1/devices/` does not exist, `lsmod | grep -iE "spi|w1"` shows nothing loaded.

**What fails:**
- Any user who adds an **MCP3008** sensor via the TUI will get a sensor that fails at open time (no `/dev/spidev0.0`).
- Any user who adds a **DS18B20** temperature sensor (commonly wanted on a water-treatment RTU) will hit an empty `/sys/bus/w1/devices/`.

**Why this matters for fresh deploy:** both drivers are documented in `gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml` and exposed via the TUI. A field tech trying to add a DS18B20 on day one will hit a silent failure and have no in-product guidance to run `modprobe w1_gpio w1_therm` as root.

**Fix (choose one):**
1. Add `modprobe spidev w1_gpio w1_therm` to `bootstrap.sh fresh` path (prefer: do this at first boot, not at deploy time, so the modules are reloaded after every reboot).
2. Create `/etc/modules-load.d/water-treat.conf` during `install_files()` in bootstrap.sh with lines `spidev`, `w1-gpio`, `w1-therm`, `i2c-dev`. This is the systemd-native approach and survives reboots.
3. Add `ExecStartPre=/sbin/modprobe spidev w1-gpio w1-therm` to `systemd/water-treat.service`. Requires root (already root) and `CAP_SYS_MODULE` — need to check whether `ProtectSystem=strict` blocks modprobe.

**Recommended:** option 2 (`modules-load.d` file) — simplest, systemd-native, no security hardening conflicts.

---

### P0-2 Hardware-type string case drift — sensors of certain types will silently not dispatch

**Evidence (from `strings /usr/local/bin/water-treat`):**

In the binary I found BOTH cases of some hardware types:
- `ads1115` AND `ADS1115`
- `dht22` AND `DHT22`
- `ds18b20` AND `DS18B20`
- `mcp3008` AND `MCP3008`

But these are **uppercase only:**
- `BME280`, `HX711`, `TCS34725`, `FLOAT_SWITCH`

And this SQL query extracted from the binary compares uppercase exclusively:
```sql
SELECT m.name FROM physical_sensors ps JOIN modules m ON ps.module_id = m.id
  WHERE ps.address = ? AND ps.sensor_type IN ('DHT22', 'DHT11', 'FLOAT_SWITCH', 'GPIO');
```

**What fails:** If the TUI dialog writes a lowercase `hardware_type` (e.g., from a lowercase dispatch label) and the driver dispatch or a SQL filter compares against uppercase, the sensor is silently mis-dispatched or filtered out. The user sees "I added it, why isn't it reading?"

**Needs confirmation from archaeology + reality-check agents** on which side is authoritative — the dispatch table or the TUI writer. The output of those agents will update this finding.

**Fix:** canonicalize at the DB insert path. Either uppercase everything (matches the existing SQL filter) or lowercase everything (matches a subset of the binary strings). Pick one, enforce in `sensor_manager_add()` or at the TUI dialog save, and add a migration for any existing mixed-case rows.

**Impact on fresh deploy:** since the bare DB has 0 sensor rows, a fresh deploy itself won't trigger the bug. But the first time a user adds a sensor, they're at risk — especially if they add one of the "uppercase-only" types (BME280, HX711, TCS34725, FLOAT_SWITCH) and the TUI writes it differently than the dispatch expects.

---

### P0-3 `gpio_chip` default of `'gpiochip0'` is wrong for Odroid-XU4 (wrong hardware bank)

**Evidence:**
- DB schema default: `gpio_chip TEXT DEFAULT 'gpiochip0'` (verified by live `.schema` output on the device)
- `gpiodetect` on the live device shows `gpiochip0 [gpy7] (8 lines)` — `gpy7` is an internal Exynos5422 bank, NOT the 40-pin expansion header.
- The Odroid-XU4 expansion header pins are distributed across `gpa0`, `gpa1`, `gpa2`, `gpb0`, `gpb1`, `gpb2`, `gpx1`, `gpx2` (multiple chipN devices).

**What fails:** A field tech adds an actuator (relay/pump/solenoid), accepts the default gpio_chip, picks gpio_pin 5 because it's pin 5 on the Pi-style header, and saves. The actuator is now bound to `gpy7 line 5` which is an internal pin with unknown wiring. Either nothing happens on toggle, or something internal toggles unpredictably.

**Fix (choose one):**
1. Make `board_detect.c` populate a board-specific default into `water-treat.conf` at bootstrap time (e.g., `[defaults] actuator_gpio_chip = gpiochip26` for the gpa0 bank on Odroid-XU4). Read this in the TUI dialog as the default instead of the DB column default.
2. Require the user to explicitly pick a `gpio_chip` before save — no default. Block save if not set.
3. Change the DB schema default to a board-specific value (but this requires schema migrations and doesn't help unless board_detect runs before the DB is created).

**Recommended:** option 1 — let `board_detect.c` be the single source of truth for board-specific defaults and read from it in the TUI.

**Impact on fresh deploy:** high — the default is exercised on every "add actuator" unless the user knows to override it. On an Odroid-XU4 deployment this is every field-tech's first mistake.

---

## P1 — degraded behavior post-deploy

### P1-1 HTTP route prefix is `/slots`, `/gsdml` — CLAUDE.md says `/api/v1/slots`, `/api/v1/gsdml`

**Evidence:**
- `src/health/health_check.c:881` — handles `/slots` (not `/api/v1/slots`)
- `src/health/health_check.c:890` — handles `/gsdml`
- `src/health/health_check.c:897-898` — 404 response endpoint list does not include any `/api/v1/*`
- `src/health/health_check.c:930` — **STALE LOG ERROR** still references `/api/v1/slots` and `/api/v1/gsdml` in an error log when HTTP socket creation fails
- Live test: `curl http://localhost:9081/api/v1/slots` → 404; `curl http://localhost:9081/slots` → works
- `strings` confirms no `/api/v1/*` exists in the binary

**What fails after deploy:**
1. The Water-Controller repo (per CLAUDE.md "Related Repositories", it's the PROFINET IO Controller that consumes this RTU's API). If the controller's discovery chain is coded to `/api/v1/slots` or `/api/v1/gsdml` (as CLAUDE.md documents), discovery will 404 and the controller will fall back to step 5 (DAP-only connect + Record Read 0xF844) — slower but functional.
2. Anyone reading CLAUDE.md to debug a discovery problem will test the wrong URL and draw wrong conclusions.
3. If the log error at `health_check.c:930` fires during startup failure, it tells the operator the wrong URL to check.

**Fix:**
1. Align CLAUDE.md to the code: `/slots` and `/gsdml`, no `/api/v1/` prefix.
2. Fix the stale log message at `src/health/health_check.c:930`.
3. Open an issue against the Water-Controller repo to confirm the client is using the simple paths, not `/api/v1/*`.

**Impact on fresh deploy:** the RTU will still serve the endpoints and respond; this is primarily a documentation and operator-experience bug. If the Water-Controller client was written to the simple paths (likely, since this is a common sibling repo by the same author), nothing breaks at runtime.

---

### P1-2 LLDP DEBUG traffic floods the journal every 5 seconds regardless of configured log level

**Evidence:** live journal shows LLDP(1008) frames logged every 5 seconds at DEBUG level despite `water-treat.conf` setting `log_level = info`. Over 2400 seconds of uptime the journal captured 480+ LLDP entries, burying anything interesting.

**What fails:** post-deploy SD card wear (journald is default-persistent on Armbian), harder troubleshooting (real errors get lost in LLDP noise), and `journalctl -u water-treat` is unusable for quick triage.

**Fix:** either (a) the LLDP logging call respects `g_app_config.system.log_level` and suppresses DEBUG when configured as `info`, or (b) the LLDP frame logging is moved to TRACE/VERBOSE level (a level below DEBUG) that is compiled out unless explicitly enabled.

**Needs code-review-agent confirmation** on where the `LLDP(1008)` and `LLDP(1999)` log calls live.

---

### P1-3 Reserved CPU-temp slot 1 rejection may not be surfaced well in the TUI

**Evidence:**
- `src/sensors/sensor_manager.c:421` — defensive rejection of sensor add at `CPU_TEMP_SLOT`
- `src/sensors/sensor_manager.c:477-509` — CPU temp sensor is always created at slot 1 at startup
- I have not yet verified whether the TUI surfaces this rejection with a specific message; that is the UX agent's job (report pending)

**What could fail:** a field tech picks slot 1 in the add-sensor dialog, gets a generic "failed to add sensor" error, and has no way to figure out that slot 1 is reserved for CPU temp. Loops or quits frustrated.

**Fix:** the TUI dialog should either (a) pre-populate the next available slot number as the default instead of letting the user pick slot 1, (b) reject slot 1 at field validation time with a specific "slot 1 is reserved for the CPU temperature sensor" message, or (c) auto-increment to slot 2 if the user types 1.

**Pending:** UX audit will confirm current behavior.

---

### P1-4 I2C bus 2 runs at 65 kHz — will constrain cyclic budget with 5+ I2C sensors

**Evidence:** dmesg on live device: `s3c-i2c 12c80000.i2c: bus frequency set to 65 KHz`. Standard is 100 kHz / 400 kHz.

**Math:** at 65 kHz, a 2-byte I2C read costs ~400 µs. A 1 ms cyclic loop can afford ~2 such reads before consuming 80% of the budget.

**What fails:** if a deployment adds 5+ I2C sensors (e.g., bank of ADS1115 + BME280 + TCS34725), the cyclic path will exceed its budget and IOPS propagation becomes unreliable.

**Fix:** tune the I2C clock via device-tree overlay on Odroid-XU4 (increase to 400 kHz), or cap the maximum number of I2C sensors per bus in the TUI + document the constraint.

**Impact on fresh deploy:** only if the deployment configures many I2C sensors. Flagging for deploy planning so expectations are set.

---

## P2 — documentation and operator-experience drift

### P2-1 CLAUDE.md says `/slots` reads `db_module_list()` only — code intentionally mixes DB + runtime CPU-temp sensor

**Evidence:**
- CLAUDE.md (RTU HTTP API Contract, Implementation Rules): "Data source: `db_module_list()` (database), NOT p-net runtime state."
- `src/health/health_check.c:599` code comment: "/slots returns ALL plugged modules (database + runtime like CPU temp)."
- `src/profinet/profinet_manager.h:116`: "Runtime-created sensors (e.g., CPU temperature at slot 1)"
- `src/profinet/profinet_manager.c:1467-1501` — `profinet_manager_get_slot_list()` reads `g_pn.slots[]`, not the database

**Fix:** update CLAUDE.md to describe the actual behavior — that the endpoint returns the runtime plug table, which always includes the reserved CPU-temp sensor at slot 1 regardless of DB state. This is intentional and desirable (it means controllers can always discover at least one input), but the docs need to catch up.

---

### P2-2 CLAUDE.md regression thread references Vendor 0x0493 / Device 0x0001 — actual config is 0x0272 / 0x0DC0

**Evidence:** CLAUDE.md regression investigation section says "device identity (Vendor 0x0493, Device 0x0001) MATCHES between controller and RTU" — but:
- `water-treat.conf` on the live device: `vendor_id = 0x0272`
- `/config` HTTP endpoint: `vendor_id: 626 (=0x0272), device_id: 3520 (=0x0DC0)`
- GSDML XML: `Vendor ID: 0x0272 (Phoenix Guardians), Device ID: 0x0DC0 (Defensive Cyber Operations)`

**Fix:** update the CLAUDE.md regression note to cite the correct IDs, or remove the note if the regression investigation is closed.

**Impact on fresh deploy:** cosmetic only — the actual runtime is internally consistent on 0x0272/0x0DC0. But a reader troubleshooting a PROFINET discovery issue will waste time chasing the wrong vendor ID.

---

## P3 — cosmetic / informational

### P3-1 Three `.pcapng` files still present in `/opt/water-treat/` on the current device

Repo commit `1fa7e20` (most recent on main) is titled "chore: remove debug pcap captures from repo". Fresh deploy from current `main` will not include them. Non-issue, just expected.

---

## Items pending agent output (not yet prioritized)

These items will be added once the background agents land. Each will go into the appropriate priority bucket when scored.

1. **ARCHAEOLOGY** (codebase-archaeologist) — canonical data-model divergences, dispatch-table drift, dead code, hardware_type casing reconciliation. Will inform P0-2 (case drift) and may surface new P0s.
2. **REALITY_CHECK** (reality-checker) — fabricated SQL, fabricated p-net API calls, missing prototypes, nonexistent includes. Any HALLUCINATION finding automatically goes to BLOCKER or P0.
3. **CODE_REVIEW** (code-reviewer) — correctness/security/data-integrity bugs on the cyclic path and driver layer. Any CRITICAL finding goes to BLOCKER or P0.
4. **UX_AUDIT** (ux-designer) — TUI dialog defects. Any CRITICAL finding goes to P0 (it blocks field-tech workflow).
5. **SOURCES** (research-agent) — upstream evidence library for downstream citation verification. Does not itself produce deploy-failure items, but closes UNVERIFIED findings into VERIFIED or MISMATCH.

---

## Top 5 deploy-gating items (action list for the deploy engineer)

In priority order, these are the items that need to be resolved — or consciously accepted — before the fresh deploy:

1. **P0-1** (kernel modules): add `/etc/modules-load.d/water-treat.conf` in bootstrap.sh. Ship `spidev`, `w1-gpio`, `w1-therm`, `i2c-dev`. One-liner change to `install_files()` in `bootstrap.sh`.
2. **P0-3** (gpio_chip default): make `board_detect.c` populate a board-specific actuator gpio_chip default into `water-treat.conf`, and make the TUI dialog read that instead of the DB column default. Requires changes to `board_detect.c`, `bootstrap.sh` (write into conf), and the TUI dialog (read from conf).
3. **P0-2** (hardware_type case): canonicalize case at sensor add time. Blocked on archaeology + reality-check agent output — do not fix until the canonical case is confirmed.
4. **P1-1** (HTTP route prefix): fix the stale log line at `health_check.c:930` and align CLAUDE.md. Cross-check Water-Controller client repo in the same PR.
5. **P1-2** (LLDP spam): respect configured `log_level = info`. Quick fix once the LLDP call site is located (code-review agent will find it).

## Update log

- 2026-04-08: initial version from HIL + repo inspection, pending 5 agent reports
