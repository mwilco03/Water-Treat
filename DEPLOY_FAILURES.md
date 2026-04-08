# RTU Deployment Failure Log — 192.168.7.173

**Target**: `rtu@192.168.7.173` (Le Potato, Armbian 26.2.1 trixie, aarch64, kernel 6.18.15)
**Source**: `/mnt/cephfs/shared/projects/Water-treat` @ commit `1fa7e20`
**Date**: 2026-04-08
**Brief**: Document every failure. Ignore what works.

---

## FAILURE #1 — `scripts/build.sh` hard-rejects libgpiod v2 (contradicts install-deps.sh)

**Where**: `scripts/build.sh:173-184` — function `check_libraries()`
**When**: First run of `bash scripts/build.sh release` on fresh Debian 13 (trixie) install
**Evidence**:

```
[ERROR] libgpiod 2.2.1 found but v1.x required
[ERROR] Run: sudo ./scripts/install-deps.sh
[ERROR] This will build libgpiod v1 from source
```

**Why this is broken**:
1. `scripts/install-deps.sh:541-561` (`handle_libgpiod()`) explicitly treats both v1 and v2
   as supported and returns success for v2: `success "libgpiod v2 API available (CMake will
   auto-detect)"`.
2. `scripts/install-deps.sh:871-879` verification accepts both v1 and v2 without complaint.
3. `INSTALL.md:619` claims RPi 5 (libgpiod 2.0+) is tested.
4. `CMakeLists.txt` supposedly auto-detects via `HAVE_GPIOD`/`HAVE_GPIOD_V2`.
5. Debian 13 trixie ships libgpiod 2.2.1 in-box. **Every fresh Debian 13 target fails
   `build.sh` out of the gate.** The "fix" the script suggests (re-run install-deps.sh)
   will NOT help — install-deps.sh correctly accepts v2 and does nothing to install v1.
   **The user is instructed to run a script that cannot fix the problem.**

**Offending block** (`scripts/build.sh:171-185`):

```bash
local gpiod_ver
gpiod_ver="$(pkg-config --modversion libgpiod 2>/dev/null || echo "0")"
case "${gpiod_ver}" in
    1.*)
        # Good
        ;;
    *)
        breaking "libgpiod ${gpiod_ver} found but v1.x required"
        ...
        return 1
        ;;
esac
```

**Fix (trivial)**: Accept both `1.*` and `2.*`, matching `install-deps.sh` and `CMakeLists.txt`.

**Workaround applied during deploy**: Bypassed `build.sh` entirely; invoked cmake + ninja by hand.

---

## FAILURE #2 — `install.sh` never installs the GSDML file (guaranteed 404 on every production install)

**Where**: `scripts/install.sh` has zero references to `gsdml` or the `gsd/` directory
(`grep GSDML scripts/install.sh` → no matches). `src/health/health_check.c:603-607`
declares four search paths the binary will try:

```c
#define GSDML_INSTALL_PATH "/opt/water-treat/gsd/"           GSDML_FILENAME
#define GSDML_FHS_PATH     "/usr/share/water-treat/gsd/"     GSDML_FILENAME
#define GSDML_DEV_PATH     "gsd/"                            GSDML_FILENAME
#define GSDML_BUILD_PATH   "../gsd/"                         GSDML_FILENAME
```

**When**: Immediately after `sudo bash scripts/install.sh`.

**Evidence from the freshly-deployed RTU**:

```
$ journalctl -u water-treat | grep GSDML
[WARN] GSDML file not found — /api/v1/gsdml endpoint will return 404
[WARN] Controller discovery step 2 (HTTP GSDML fetch) will be unavailable
[WARN] Expected file: GSDML-V2.4-WaterTreat-RTU-20241222.xml

$ ls /opt/water-treat/gsd /usr/share/water-treat/gsd
ls: cannot access '/opt/water-treat/gsd': No such file or directory
ls: cannot access '/usr/share/water-treat/gsd': No such file or directory

$ curl -sS -o /dev/null -w "%{http_code}\n" http://localhost:9081/gsdml
404
```

**Why this is catastrophic**:
`CLAUDE.md` — "Controller Discovery Priority" — puts GSDML HTTP fetch at **step 2** in
the 5-step discovery chain the Water-Controller uses to cache the device description.
With the file missing, every Controller talking to a factory-fresh RTU falls through
to step 3 (cached config — doesn't exist on a new unit), then step 4 (`/slots` — also
broken, see FAILURE #3), then step 5 (DAP-only Record Read 0xF844). **The GSDML HTTP
path was designed to be the preferred path, and the installer doesn't even copy the
file.** It is impossible to reach a working `/gsdml` state using only the scripts in
the repo — the operator must know to `install -m 644 gsd/GSDML-*.xml /usr/share/water-treat/gsd/`
by hand, which is nowhere in `INSTALL.md` or `scripts/install.sh`.

**Extra stink**: `src/health/health_check.c:930` logs `"Controller discovery via
/api/v1/slots and /api/v1/gsdml will be unavailable"`. Whoever wrote the error string
knew the contract was `/api/v1/...` — the actual router doesn't serve that prefix
(see FAILURE #3). The code is internally inconsistent with itself.

**Fix**: `install.sh` must install `${PROJECT_ROOT}/gsd/GSDML-*.xml` to
`/usr/share/water-treat/gsd/` (FHS search path) or `/opt/water-treat/gsd/`. Add to the
install manifest for clean uninstall. Single missing `install -D` call.

---

## FAILURE #3 — HTTP API routing contract is BROKEN: CLAUDE.md says `/api/v1/*`, server serves `/*`

**Where**: `src/health/health_check.c:876-893` route handler. CLAUDE.md "RTU HTTP API
Contract" section contradicts the actual code.

**When**: Any controller following the published API contract.

**CLAUDE.md claims**:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/v1/slots` | GET | Current PROFINET slot configuration |
| `/api/v1/gsdml` | GET | Raw GSDML XML file |

And: *"The controller queries these endpoints for slot discovery before issuing a
PROFINET Connect."*

**What actually happens on the live RTU**:

```
$ curl http://localhost:9081/api/v1/slots
{"error": "Not Found", "endpoints": ["/health", "/metrics", "/ready", "/live",
 "/config", "/slots", "/gsdml"]}   ← HTTP 404

$ curl http://localhost:9081/api/v1/gsdml
HTTP 404

$ curl http://localhost:9081/slots
{"slot_count": 1, "slots":[{"slot": 1, ...}]}   ← HTTP 200
```

**Route handler (`src/health/health_check.c:876-893`)** explicitly strcmp's against the
prefix-free paths:

```c
} else if (strcmp(path, "/config") == 0 || strcmp(path, "/config/export") == 0) {
} else if (strcmp(path, "/slots") == 0) {
} else if (strcmp(path, "/gsdml") == 0) {
```

**No `/api/v1/` prefix anywhere in the router.**

**Why this is catastrophic**:
This is the single most likely cause of the regression documented at the top of
`CLAUDE.md` ("RTU was responding, now it is not responding"). The Water-Controller in
the sister repo is coded to the CLAUDE.md contract (`/api/v1/...`), gets 404 on both
discovery steps 2 and 4, and falls all the way through to DAP-only record reads or
simply gives up. The sniffer shows Connect requests with **no response**, consistent
with the Controller abandoning connection when discovery fails.

**Fix — choose ONE, do not do both**:
(a) Add `/api/v1/slots`, `/api/v1/gsdml`, `/api/v1/config` aliases in the route handler
    (simplest, one-line change per alias).
(b) Rewrite CLAUDE.md contract to match reality and update the controller.

The current state — code says one thing, docs say another, and the log message at line
930 says yet a third thing — is the worst of all worlds.

---

## FAILURE #4 — `install.sh` never installs any configuration file

**Where**: `scripts/install.sh` creates `/etc/water-treat/` and writes only
`.install-manifest`. No copy of `etc/water-treat.conf.example`, no copy of
`etc/water-treat.env`, nothing.

**Evidence**:

```
$ ls -la /etc/water-treat/
drwxr-xr-x  2 root root 4096 Apr  8 09:23 .
-rw-r--r--  1 root root  395 Apr  8 09:23 .install-manifest
```

Source tree has BOTH files sitting in `etc/`:

```
$ ls Water-Treat/etc/
water-treat.conf.example
water-treat.env
```

**Systemd unit** (`systemd/water-treat.service:69`) references
`EnvironmentFile=-/etc/water-treat/water-treat.env` — the `-` prefix makes it optional,
so the unit silently starts with zero environment overrides and compiled-in defaults.
An operator has no idea the env file is expected unless they read the unit manually.
The `.example` config is not discoverable from the installed filesystem at all.

**Why this matters**:
- Operator cannot change `WT_HTTP_PORT`, log level, interface, or cycle time without
  reverse-engineering the source tree.
- No discoverable sample config on a production box → knowledge only lives in the repo.
- Any operator who `apt purge`s the package (if a .deb existed) or uninstalls loses
  even the example.

**Fix**: `install.sh` must `install -m 644 etc/water-treat.env /etc/water-treat/water-treat.env`
(conditional on file not already existing — don't clobber operator edits) and
`install -m 644 etc/water-treat.conf.example /etc/water-treat/water-treat.conf.example`.

---

## FAILURE #5 — `install.sh` never enables the service for boot

**Evidence**:

```
$ systemctl is-enabled water-treat.service
disabled
```

**Script output says**:

```
Commands:
  sudo systemctl start water-treat   - Start the service
  sudo systemctl enable water-treat  - Start on boot
```

— i.e., the installer punts the boot-enablement step to the operator. On a new RTU
coming online ("deploy to new RTU" is the literal task I was given), the unit
installs, the service starts, and then on **first power cycle the RTU silently does
not come up**. This will be discovered in the field, not at deploy time.

**Fix**: `install.sh` should end with `systemctl enable water-treat.service`. If you
want a flag to skip it, fine — but enable-by-default is correct for production
installs.

---

## FAILURE #6 — `install.sh` manifest has `/usr/local/share//firmware` (double slash from order-of-operations bug)

**Evidence** (`/etc/water-treat/.install-manifest`):

```
FIRMWARE_DIR=/usr/local/share//firmware
```

**Root cause**: `scripts/install.sh:308`:

```bash
FIRMWARE_SHARE_DIR="/usr/local/share/${PROJECT_NAME}/firmware"
```

is expanded at module-scope **before** `read_project_name()` fills `PROJECT_NAME` at
line 85. `PROJECT_NAME=""` gives `/usr/local/share//firmware`. This assignment should
live **inside** `build_rp2040_firmware()` or after `read_project_name()` has run, or
be a function-local variable.

This isn't just cosmetic: `uninstall.sh` reads this manifest and will attempt to
remove `/usr/local/share//firmware`, which may or may not be canonicalised to the right
directory depending on how the uninstaller uses it. And if the RP2040 firmware ever
*did* build, it'd be placed in the wrong directory.

---

## FAILURE #7 — p-net `set_network_parameters` helper is missing; DCP Set from controller will silently fail

**Evidence** (first journal line after service start):

```
water-treat[11000]: PNAL(212): Failed to execute in child process.
Is the script file missing or lacks execution permission?
set_network_parameters Search path /bin:/usr/bin:/usr/local/bin
```

This comes from p-net's PNAL Linux OSAL, which shells out to a script named
`set_network_parameters` whenever a PROFINET controller sends a DCP Set (IP/station
rename). Neither the repo nor `install.sh` provides or installs this script, and it
isn't documented in `INSTALL.md` or `CLAUDE.md`.

**Why this matters**:
- Controller-driven IP or station-name changes will appear to succeed from the
  controller's side but silently fail on the RTU.
- The error is only visible during early startup because p-net tests the script with
  an empty invocation — in production this will trigger on the first live DCP Set
  frame, not at startup, so the failure mode is latent.

**Fix**: Either ship a real `set_network_parameters` script under `/usr/local/bin/`
that persists station name + network config using the existing config layer, OR patch
p-net to no-op the fork in `pf_fspm.c` / `pnal_linux.c`. The current "broken by
omission" state is the worst option.

---

## FAILURE #8 — `install.sh` creates a `water-treat` service user that nothing uses (and misses the group that would matter)

**Evidence**:

```
$ id water-treat
uid=997(water-treat) gid=997(water-treat) groups=997(water-treat),20(dialout),998(i2c)

$ grep -E "^User=|^Group=" /etc/systemd/system/water-treat.service
User=root
Group=root
```

The installer creates a system user `water-treat`, tries to add it to `gpio`, `i2c`,
`spi`, `dialout` — only `i2c` and `dialout` actually exist on Debian 13 / Armbian trixie
(no `gpio`, no `spi` groups) — and then the systemd unit **ignores the user entirely
and runs as root**. The comment in the unit file even explains why (`root needed for
GPIO/I2C/SPI hardware access`).

Net result:
- Dead service user sitting in `/etc/passwd`.
- Dead entries in `/etc/group`.
- Two non-breaking warnings on every install about groups that will never exist.
- Uninstall now has to clean up users the installer should never have made.

**Fix**: Either (a) drop the user-creation entirely, or (b) actually use the user
(make the unit `User=water-treat` and handle hardware access via `DeviceAllow=` +
supplementary groups). The current middle-ground is pure installer cruft.

---

## FAILURE #9 — `/var/lib/water-treat` is `mode 770 root:root`, nothing but root can read it

**Evidence**:

```
$ ls -la /var/lib/water-treat/
ls: cannot open directory '/var/lib/water-treat/': Permission denied

$ sudo stat -c "%U:%G %a" /var/lib/water-treat
root:root 770
```

`install.sh:217-218` tries `chown water-treat:water-treat` and falls back to
`root:root` if the user/group don't match. The fallback leaves mode `770` but with
owner `root:root` — i.e., the service user `water-treat` that the installer just
created can't touch its own data dir, and no-one else can either. Only `root` works,
which is the unit's actual User= (see FAILURE #8), so nothing crashes — but the
permission story is nonsense.

**Fix**: pick one ownership model (all root, or dedicated user) and commit. Don't do
both badly.

---

## FAILURE #10 — `systemd/water-treat.service` `ProtectSystem=strict` + `ReadWritePaths` is **missing `/etc/water-treat`**

Unit file excerpt:

```
ProtectSystem=strict
ReadWritePaths=/var/lib/water-treat /var/log/water-treat
```

Under `ProtectSystem=strict`, `/etc` is read-only. The binary's `config_to_json()`
path exports current config, and any runtime reconfig path that writes back to
`/etc/water-treat/water-treat.env` (e.g., `WT_HTTP_PORT` override) will fail with
EROFS. This is not currently observed because the installer doesn't create the env
file to begin with (FAILURE #4), so the failure is masked behind another bug. Fix
both together.

---

## FAILURE #11 — RP2040 toolchain check: `arm-none-eabi-gcc not found → skip` on aarch64 hosts ignores cross-compile entirely

Installer output:

```
[WARN] ARM toolchain not found - skipping RP2040 firmware build
```

The Le Potato is already an aarch64 machine. `install.sh:355` checks for
`arm-none-eabi-gcc` in PATH but does not install it, does not warn that a pre-built
firmware could be shipped, and does not explain what happens if the LED firmware is
actually needed. For the operator this is a silent skip that later causes
"LED support disabled" with no trail back to this skip step.

**Fix** (lightweight): state in the install summary whether the RP2040 firmware was
(a) found pre-built, (b) built from source, or (c) skipped entirely and why. As written
it is indistinguishable from success.

---

## What actually works (kept short per the brief)

- `scripts/install-deps.sh` — picked up trixie's libgpiod v2, detected t64 ABI, built
  p-net v0.2.0 with both debug patches, installed libpnet.so symlink. No failures.
- `cmake` configure via Ninja — clean, picked up libgpiod v2 and p-net paths correctly,
  60 objects, zero warnings under `-Wall -Wextra -Werror` (so the code itself compiles;
  the deployment around it is what's broken).
- Station-name auto-detect produced `rtu-fba7` from MAC `...fe:49:fb:a7`, matching the
  `rtu-XXXX` contract in CLAUDE.md.
- p-net bound UDP 34964 and the application bound TCP 9081. Connect responses are up
  to the controller now.
- HTTP `/health`, `/metrics`, `/ready`, `/live`, `/config`, `/slots` all respond 200
  on the prefix-free paths.

---

## Deployment blast radius summary

| # | Severity | Fix effort | Blocker for controller handshake? |
|---|----------|------------|-----------------------------------|
| 1 | High     | 1 line     | No — workaroundable                |
| 2 | **Critical** | ~5 lines | **Yes — step-2 discovery dead**   |
| 3 | **Critical** | ~3 lines | **Yes — docs vs. code contract**  |
| 4 | Medium   | ~5 lines   | No — runs on defaults             |
| 5 | High     | 1 line     | No on first deploy, yes on reboot |
| 6 | Low      | 1 line     | No                                 |
| 7 | Medium   | TBD        | Latent, fires on DCP Set          |
| 8 | Low      | ~10 lines  | No                                 |
| 9 | Low      | 1 line     | No (masked by FAILURE #8)          |
| 10| Low      | 1 line     | No (masked by FAILURE #4)          |
| 11| Cosmetic | ~3 lines   | No                                 |

**Bottom line**: two of the eleven failures (#2 and #3) alone explain the
"RTU was responding, now it is not responding" regression at the top of CLAUDE.md.
The sniffer evidence (Controller sends Connect, RTU sends nothing back) is consistent
with the Controller bailing out of its 5-step discovery chain because steps 2 and 4
both return 404. Before chasing anything in `src/profinet/`, fix the HTTP routing
contract and install the GSDML file, then re-run the pcap test.

---

## Source-side fixes applied (2026-04-08, in this campaign)

These four were named explicitly by the operator as the source-side fixes the
background agent had to land. All four are now committed in the working tree.

| # | Failure addressed | File | Edit |
|---|---|---|---|
| F1 | #1 — libgpiod v2 rejected | `scripts/build.sh:170-189` | `case 1.*\|2.*)` accepts both, error message updated |
| F2 | #2 — GSDML never installed; #4 — config files never installed | `scripts/install.sh` | New `install_data_files()` function (after `install_service_file`) installs `gsd/GSDML-*.xml` to `/usr/share/${PROJECT_NAME}/gsd/`, copies `etc/water-treat.env` (preserving operator edits) and `etc/water-treat.conf.example`; called from `main()` after `install_service_file` |
| F3 | #5 — service never enabled | `scripts/install.sh::install_service_file` | After `daemon-reload`, runs `systemctl enable ${PROJECT_NAME}.service` (gated by `INSTALL_NO_ENABLE=1` opt-out) |
| F4 | #6 — `/usr/local/share//firmware` double-slash | `scripts/install.sh:75-104, 305-310` | `FIRMWARE_SHARE_DIR` moved into `read_project_name()` so it expands after `PROJECT_NAME` is set; module-scope assignment removed and replaced with an explanatory comment |
| F5 | #3 — HTTP route prefix mismatch | `src/health/health_check.c:876-916` | `/api/v1/slots`, `/api/v1/gsdml`, `/api/v1/config`, `/api/v1/config/export` aliases added alongside the legacy prefix-free routes; 404 endpoint list updated to advertise both forms |

`bash -n` passes on both edited shell scripts. C control-flow in
`health_check.c` is preserved (only `||` conditions extended; no new braces
or parens).

**Not addressed in this pass (operator must decide):**

- **#7** — `set_network_parameters` script. Two paths: ship a real script
  that persists station name + network config via the existing config
  layer, or patch p-net to no-op the fork in `pf_fspm.c` / `pnal_linux.c`.
  Either is a larger change than a one-line fix. Recommend ship a script.
- **#8/#9** — service-user vs root inconsistency. Pick one ownership model
  and commit. Suggest dropping `create_service_user()` since the unit runs
  as root (required for GPIO/I2C/SPI access on this hardware). Cleanup of
  `/var/lib/${PROJECT_NAME}` mode 770 follows from whichever model wins.
- **#10** — `ProtectSystem=strict` + missing `/etc/water-treat` in
  `ReadWritePaths`. Currently masked by #4. Will fire as soon as F2 lands
  and a runtime config reload tries to write back. Add `/etc/water-treat`
  to `ReadWritePaths` in `systemd/water-treat.service` in the same PR.
- **#11** — RP2040 toolchain skip is silent. Cosmetic; deferred.

---

## Tier 2 — Runtime correctness defects from verification agents

**Source:** Five verification agents ran in parallel against the source tree
at commit `1fa7e20`. Four landed reports; one (research-agent) blocked on a
permission gate before fabricating any source. Reports under
`docs/verification/`:

- `ARCHAEOLOGY.md` — codebase-archaeologist (cross-module structure)
- `REALITY_CHECK.md` — reality-checker (hallucination scan)
- `CODE_REVIEW.md` — code-reviewer (cyclic-path correctness/security/data integrity)
- `UX_AUDIT.md` — ux-designer (TUI add/edit/delete dialog audit)
- `HIL_FINDINGS.md` — main Claude (live RTU at 192.168.6.21)

These findings are NOT deploy-blockers in the same sense as the 11 failures
above — the daemon WILL install, start, and listen on the right ports
without fixing any of them. They are runtime correctness, chemical safety,
and field-tech workflow defects that make a successfully-deployed RTU
misbehave the first time a user adds a sensor or actuator.
**Several of them are more dangerous than the deploy failures.**

### Tier 2 — CRITICAL (chemical safety / data corruption)

#### T2-C1 Actuator `safe_state` is silently dropped — every actuator forced OFF on PROFINET disconnect

- **Source:** `CODE_REVIEW.md` (CRITICAL); confirmed by `ARCHAEOLOGY.md` D5/D6
- **Files:** `src/actuators/actuator_manager.c:232-266` (`apply_safe_state`),
  `src/actuators/actuator_manager.c:867-881` (`actuator_manager_reload` config conversion),
  `src/actuators/actuator_manager.h:59-80` (`actuator_config_t` definition)
- **Defect:** The `actuators.safe_state` TEXT column (DB default `'hold'`,
  valid values `off`/`on`/`hold`) is correctly persisted by the dialog and
  loaded into `db_actuator_t`, but **never copied** into `actuator_config_t`
  — that struct does not even have a `safe_state` field. `apply_safe_state()`
  unconditionally drives every ON actuator to OFF.
- **Impact:** A chlorine dosing pump configured `safe_state="on"`
  (fail-open) is silently driven CLOSED during a controller outage. A valve
  mid-stroke configured `safe_state="hold"` is dropped to closed.
  **Chemical safety hazard.** No diagnostic indication that the configured
  safe state was ignored.
- **Fix:** Add `safe_state_t safe_state;` to `actuator_config_t`; copy in
  `actuator_manager_reload()`; branch in `apply_safe_state()` on
  `OFF`/`ON`/`HOLD`.

#### T2-C2 `web_poll` driver returns `RESULT_OK` with stale-or-zero value on every fetch failure

- **Source:** `CODE_REVIEW.md` (CRITICAL)
- **File:** `src/sensors/drivers/driver_web_poll.c:164-187`
- **Defect:** `cache_on_error=true` is the default; `last_value` starts at 0
  from `memset`. A web_poll sensor whose URL is unreachable on the first
  read reports `value=0.0` with quality GOOD to the controller and never
  raises a diagnosis alarm. The cyclic update path writes `0.0` to the
  PROFINET input buffer with IOPS=GOOD. Subsequent failures keep masking.
- **Impact:** A failing web_poll sensor is reported to SCADA as a healthy
  sensor stuck at 0.0. For a level/pressure/pH input feeding a control
  loop, this is a worst-case silent fault — **the loop reacts to a
  fabricated value with full confidence**.
- **Fix:** Return `RESULT_ERROR` on fetch failure; let the cyclic path's
  existing IOPS=BAD propagation do its job.

#### T2-C3 DHT22 and HX711 drivers call `sched_setscheduler(0, ...)` — affects entire RTU process

- **Source:** `CODE_REVIEW.md` (CRITICAL)
- **Files:** `src/sensors/drivers/driver_dht22.c:122-123, 168-169`,
  `src/sensors/drivers/driver_hx711.c:113-114, 136-137`
- **Defect:** PID `0` in `sched_setscheduler` means "the calling process",
  not "this thread". Elevates the **entire RTU process** (TUI, alarm
  manager, PROFINET cyclic worker, watchdog, db writer) to `SCHED_FIFO 50`
  for ~25 ms per DHT22 read. On exit, drops everything to `SCHED_OTHER 0`,
  clearing any priority an init script set.
- **Impact:** PROFINET 1 ms cyclic timing is corrupted whenever a DHT22 or
  HX711 read runs. Catastrophic for cycle-overrun avoidance. The post-read
  demotion also breaks any operator real-time tuning.
- **Fix:** Replace with `pthread_setschedparam(pthread_self(), SCHED_FIFO,
  &sp)` and matching restore via the saved policy/param from
  `pthread_getschedparam`. Per-thread, not per-process.

#### T2-C4 Actuator interlock TOCTOU race — two actuators in same group can both physically activate

- **Source:** `CODE_REVIEW.md` (CRITICAL)
- **File:** `src/drivers/digital/relay_output.c:236-272` (`output_set`)
- **Defect:** `check_interlock_available` and `register_interlock_active`
  each take and release `g_interlock_mutex` independently. Between the
  check and the register, the mutex is not held. Two threads can both pass
  the check and both write to GPIO before either registers — relay GPIOs
  physically asserted simultaneously.
- **Impact:** Safety-critical mutual exclusion is broken. Two pumps
  interlocked because they share a discharge line can both run
  simultaneously → pressure surge. Two solenoids on opposing valves can
  both open → flow short-circuit. **The interlock gives a false sense of
  safety.**
- **Fix:** Combine check + register into a single critical section under
  `g_interlock_mutex`. Targeted helper `try_acquire_interlock()`.

#### T2-C5 Active-low actuators get a transient ON pulse during sysfs export

- **Source:** `CODE_REVIEW.md` (CRITICAL)
- **File:** `src/drivers/digital/relay_output.c:39-81` (`gpio_set_output`),
  `src/drivers/digital/relay_output.c:183` (`output_create`)
- **Defect:** Linux gpiolib sets a freshly-configured output line to logical
  0 when `direction=out` is written via the deprecated sysfs interface. For
  an `active_low` actuator, logical 0 = electrically asserted = relay
  closed = pump ON. Window between writing `direction` and the intended
  value is hundreds of microseconds to a few milliseconds.
- **Impact:** Every active_low relay/solenoid/pump has a transient ON pulse
  during `output_create`. For a chemical dosing pump that takes any pulse
  as a dose, **a single drop is delivered on every reload** — and given
  T2-T1 (TUI never reloads the actuator manager), every save/edit/delete
  triggers one.
- **Fix:** Use `direction=high` or `direction=low` (sysfs accepts these to
  set initial value atomically with direction), or migrate `relay_output.c`
  to the `gpio_hal.c` libgpiod abstraction which already does this
  correctly.

#### T2-C6 TCS34725 color register byte order swapped — every reading is wrong

- **Source:** `CODE_REVIEW.md` (CRITICAL)
- **File:** `src/sensors/drivers/driver_tcs34725.c:119-124`
- **Defect:** TCS34725 sends 16-bit color words little-endian; the driver
  reads them via `i2c_read_word` which (per `hw_interface.c:75`) returns
  big-endian. Every clear/red/green/blue reading is byte-swapped.
- **Impact:** Lux and color-temperature calculations all use wrong inputs.
  TCS34725 sensors are unusable as deployed.
- **Fix:** Swap bytes in the driver after `i2c_read_word`, or read raw
  bytes and assemble little-endian explicitly.

### Tier 2 — TUI workflow defects (every "add a sensor" path is broken)

#### T2-T1 Actuator add/edit/delete never reloads the actuator manager

- **Source:** `UX_AUDIT.md` (CRITICAL)
- **Files:** `src/tui/pages/page_actuators.c:334-419`,
  `src/tui/dialogs/dialog_actuator.c` (zero `actuator_manager_reload()`
  calls anywhere in `src/tui/`)
- **Defect:** After Add (`page_actuators.c:348`), Edit (`:389`), or Delete
  (`:409`), only `load_actuators()` runs, which refreshes the **display
  list** from the DB. The running `g_actuator_mgr` keeps old GPIO claims,
  old PWM channels, old instances. SPACE-toggling a newly-added actuator
  silently does nothing. **Deleting an actuator may leave its relay
  energized indefinitely** because the GPIO line is never released.
- **Fix:** Call `actuator_manager_reload(&g_actuator_mgr)` after every add,
  edit, and delete. Move the reload into `dialog_actuator_show()` itself
  post-save so the contract is intrinsic, not page-dependent.

#### T2-T2 Editing a GPIO sensor is impossible — self-conflict on save

- **Source:** `UX_AUDIT.md` (CRITICAL)
- **File:** `src/tui/dialogs/dialog_sensor.c:222-228`
- **Defect:** `check_gpio_conflict()` calls
  `db_actuator_gpio_conflict_check(db, gpio_pin, chip, 0, &conflict)` with
  a hard-coded `0` instead of the `exclude_sensor_id`. The block at lines
  224-228 literally acknowledges the bug in a comment ("`We can't easily
  check this without knowing the conflicting sensor ID`") and does nothing.
  Every edit of a DHT22, Float Switch, or any GPIO-interface sensor finds
  itself in the conflict check and refuses to save.
- **Fix:** Pass `exclude_sensor_id` through. The actuator dialog already
  does this correctly at `dialog_actuator.c:130`.

#### T2-T3 Sensor types `web_poll`, `calculated`, `static` are silently dropped on save

- **Source:** `UX_AUDIT.md` (CRITICAL); confirmed by `REALITY_CHECK.md` H2;
  confirmed by `ARCHAEOLOGY.md` D3
- **File:** `src/tui/dialogs/dialog_sensor.c:42` (type list),
  `:264-293` (`save_sensor`), `:446-475` (edit save)
- **Defect:** The dialog advertises five sensor types in the picker, but
  the save function only handles `physical` and `adc`. Picking `web_poll`,
  `calculated`, or `static` creates a `db_module` row with NO companion
  sub-record. Dialog returns success and the row appears in the list, but
  the sensor never reports a value.
- **Fix (immediate):** Remove the three unsupported types from
  `sensor_types[]` until they are wired end-to-end.
- **Fix (long-term):** Implement the missing branches in `save_sensor` and
  the edit save block. The data model already exists.

#### T2-T4 Sensor save creates phantom rows when sub-record creation fails

- **Source:** `UX_AUDIT.md` (CRITICAL)
- **File:** `src/tui/dialogs/dialog_sensor.c:261-293, 459, 474`
- **Defect:** `db_module_create()` is checked, but the subsequent
  `db_physical_sensor_create()` and `db_adc_sensor_create()` return values
  are ignored. On disk-full or NOT NULL violation, the parent `modules`
  row remains with no child row. Dialog returns success.
- **Fix:** Check every return; on failure roll back via `db_module_delete()`
  and surface an actionable error.

#### T2-T5 Default sensor slot is 1, which collides with the reserved CPU temp sensor

- **Source:** `UX_AUDIT.md` (CRITICAL)
- **File:** `src/tui/dialogs/dialog_sensor.c:65, 134`
- **Defect:** `init_form()` defaults `slot = 1`. Slot 1 is reserved for the
  CPU temp sensor (`sensor_manager.c:421`). New tech opens Add Sensor,
  types a name, hits Save → cryptic "Failed to save" with no indication
  the slot is the issue.
- **Fix:** Default to next free slot via `find_next_slot()`; range 2-246;
  surface UNIQUE-constraint failure as
  `"Slot N is already used by sensor 'foo'"`.

#### T2-T6 Sensor slot range is 1-63, but PROFINET allows 2-246

- **Source:** `UX_AUDIT.md` (CRITICAL)
- **File:** `src/tui/dialogs/dialog_sensor.c:134, 135`
- **Defect:** Hard-coded `1-63` range — a tech who knows the spec can't
  add sensors at slots 64-246. A site with 60 actuators is locked out.
- **Fix:** Change to `2-246`; reuse `SENSOR_SLOT_MIN/MAX` from the wizard.

#### T2-T7 Actuator delete confirmation accepts any keypress and has no Enter handler

- **Source:** `UX_AUDIT.md` (CRITICAL)
- **File:** `src/tui/dialogs/dialog_actuator.c:692-732`
- **Defect:** Hand-rolled `getch()` loop. Y/y → delete, N/n/Esc → cancel,
  any other key → loop. **Enter does nothing.** No default selection, no
  item-name double-confirmation. Combined with T2-T1 (delete leaves relay
  energized), a misclick can leave a process valve in an unsafe state.
- **Fix:** Replace with `dialog_confirm()` which defaults to "No". Consider
  typed-name confirmation for actuator deletes specifically.

#### T2-T8 Sensor delete leaks: actuator manager and on-disk sub-records not cleaned up

- **Source:** `UX_AUDIT.md` (CRITICAL)
- **Files:** `src/tui/dialogs/dialog_sensor.c:499-517`,
  `src/tui/pages/page_sensors.c:243-255`
- **Defect:** `dialog_sensor_delete` calls only `db_module_delete()` — no
  `sensor_manager_reload_sensors()`. The page-level callback happens to
  forward to the manager via `tui_notify_sensor_changed()`, but only
  because the page is doing the dialog's job. Any caller from another
  page leaks running sensor instances and held GPIO/I2C/1-Wire handles.
- **Fix:** Move the reload call into `dialog_sensor_delete` itself.

### Tier 2 — Architecture / dispatch divergences

#### T2-A1 Three coexisting sensor frameworks; two are dead

- **Source:** `ARCHAEOLOGY.md` D1 (Blocker); confirmed by `REALITY_CHECK.md`
  H1, H4, H5
- **Files:** `src/sensors/sensor_api.h:260-288` (Gen 1, ~700 LOC, no impl),
  `src/sensors/analog/analog_sensor.{c,h}` (Gen 1 backend, dead),
  `src/drivers/adc/adc_driver.h:81-117` (Gen 1 ADC, no `.c` file at all),
  `src/sensors/drivers/driver_common.{c,h}` (Gen 2 ops registry, ~330 LOC,
  no callers), `src/sensors/sensor_instance.c:146-217` (Gen 3 string-
  dispatch ladder, the **only** live framework)
- **Defect:** Two-thirds of the sensor framework code is unreachable. Each
  "live" driver carries TWO public surfaces — the `_init/_read/_close`
  functions used at runtime AND a `driver_ops_t` table referenced only by
  the dead Gen-2 registry.
- **Fix:** Strangler-Fig rip-out of Gen 1 and Gen 2 once Gen 3 is proven
  complete. Defer until after the fresh deploy.

#### T2-A2 Actuator `gpio_chip` column is silently dropped at runtime

- **Source:** `ARCHAEOLOGY.md` D5
- **Files:** `src/actuators/actuator_manager.c:868-881`,
  `src/actuators/actuator_manager.h:59-80`
- **Defect:** Same shape as T2-C1 (safe_state). The `actuators.gpio_chip`
  column is loaded into `db_actuator_t`, but `actuator_config_t` has no
  `gpio_chip` field. Any non-`gpiochip0` value the operator sets via the
  TUI is **silently ignored**. Combined with HIL-4 (the schema default
  `'gpiochip0'` lands on `gpy7` on Odroid-XU4), every Odroid-XU4 actuator
  is wrong by default AND cannot be fixed via the TUI.
- **Fix:** Add `char gpio_chip[32];` to `actuator_config_t`; copy in
  `actuator_manager_reload()`; pass to `gpio_hal` instead of the hardcoded
  default.

#### T2-A3 PUMP and VALVE actuator types silently degrade to RELAY on save

- **Source:** `REALITY_CHECK.md` M1, M2
- **Files:** `src/db/db_actuators.c:10-18, 29-35`, `include/constants.h:297-300`
- **Defect:** The `actuator_type_t` enum has `PUMP=4` and `VALVE=5`, but
  the string conversion functions have no cases. Inserting PUMP or VALVE
  binds the literal `"unknown"` into `actuators.type`. Round-tripping via
  `string_to_actuator_type("unknown")` returns RELAY. The wizard at
  `dialog_io_wizard.c:1816-1819` sets these enum values, so this is
  reachable from the user.
- **Fix:** Add `_PUMP_STR` and `_VALVE_STR` to `constants.h`; add the cases
  to both conversion functions; back-fill any pre-existing rows.

#### T2-A4 `bool used[17]` indexed up to 246 — buffer overrun

- **Source:** `REALITY_CHECK.md` M14; confirmed by `ARCHAEOLOGY.md` D10
- **File:** `src/tui/dialogs/dialog_io_wizard.c:386-426` (`find_next_slot`),
  line 394 specifically
- **Defect:** `bool used[17]` is sized for 17 entries but the loop
  iterates slots up to 246. Out-of-bounds writes trash adjacent stack
  memory if any persisted module/actuator slot is ≥ 17.
- **Impact:** Stack corruption on any RTU with more than 17 sensors or any
  sensor at slot ≥ 17.
- **Fix:** Size `used[]` to `[247]`, or use a bitset.

#### T2-A5 DHT22 driver is BCM2835-only — does not work on Odroid-XU4

- **Source:** `ARCHAEOLOGY.md` D12
- **File:** `src/sensors/drivers/driver_dht22.c:20-81`
- **Defect:** mmaps `/dev/gpiomem` with hardcoded BCM2835 register offsets.
  RPi-specific. Will not work on the documented Odroid-XU4 target.
  UNVERIFIED whether anyone has run DHT22 on the actual hardware.
- **Fix:** Replace with `gpio_hal` (libgpiod-based) bit-bang. Will still
  need real-time scheduling — see T2-C3.

#### T2-A6 Float-switch GPIO conflict check is case-mismatched

- **Source:** `REALITY_CHECK.md` M3
- **File:** `src/db/db_modules.c` (the
  `IN ('DHT22','DHT11','FLOAT_SWITCH','GPIO')` query)
- **Defect:** SQL filter checks for `'FLOAT_SWITCH'` exact match, but the
  dialog stores `'Float Switch'` (mixed case, with space). Conflict check
  returns no rows for float switches; two can be configured on the same
  GPIO pin without warning.
- **Fix:** Canonicalize hardware_type strings at the dialog write path.
  Same root cause as HIL-3.

#### T2-A7 `driver_pump.h` and `driver_solenoid.h` declare APIs with no implementation

- **Source:** `REALITY_CHECK.md` H3
- **Files:** `src/sensors/drivers/driver_pump.h:16-21`,
  `src/sensors/drivers/driver_solenoid.h`
- **Defect:** Pure dead-letter declarations. Anything that ever links them
  in will fail with unresolved symbols. Unrelated to the actuator code
  that actually controls pumps/solenoids (those go through
  `actuator_manager.c` + `relay_output.c`).
- **Fix:** Delete the headers and any references. Misleading dead code.

#### T2-A8 `relay_output.c` writes to deprecated `/sys/class/gpio/export`

- **Source:** `REALITY_CHECK.md` M15
- **File:** `src/drivers/digital/relay_output.c`
- **Defect:** Bypasses the `gpio_hal` libgpiod abstraction it `#include`s.
  The deprecated sysfs interface is gone in Linux 6.8+ on some configs.
  The Odroid-XU4 6.6.113 and Le Potato 6.18.15 kernels may or may not
  still expose it.
- **Fix:** Migrate to `gpio_hal_set_output()`. Same fix point resolves
  T2-C5 (active-low transient pulse).

#### T2-A9 `modules.module_ident` is always `GSDML_MOD_SENSOR_GENERIC`

- **Source:** `ARCHAEOLOGY.md` D11
- **Defect:** Both TUI write paths hardcode `GSDML_MOD_SENSOR_GENERIC`.
  Runtime ignores it and recomputes via fuzzy substring matching in
  `gsdml_sensor_module_from_string()`. Two sources of truth, both
  unreliable.
- **Fix:** Stop persisting a hardcoded ident; derive from
  `gsdml_sensor_module_from_string()` at write time and store the canonical
  ident. Then drop the runtime fuzzy match.

### Tier 2 — Counts

| Severity | Count | Top concerns |
|---|---|---|
| CRITICAL (chemical/data) | 6 | T2-C1..C6 |
| TUI workflow | 8 | T2-T1..T8 |
| Architecture / dispatch | 9 | T2-A1..A9 |
| **Tier 2 total** | **23** | |

Reality-checker also flagged **5 HALLUCINATION + 16 MISMATCH + 8 UNVERIFIED**
(UNVERIFIEDs gated on the blocked SOURCES.md). Code-reviewer reported
**6 CRITICAL + 11 HIGH + 9 MEDIUM + 3 LOW**. UX-designer reported
**8 critical + 12 high + 9 medium + 6 low**. Archaeologist reported
**14 canonical-model divergences + 6 dead files + 5 dispatch-table mismatches**.

The full per-finding evidence (file:line, code excerpts, reasoning) lives in
the four agent reports under `docs/verification/`.

### Tier 2 — Headline ranking for the field deploy

If we are about to ship this RTU to a water-treatment plant where actuators
control chemical dosing pumps and process valves, the items that **matter
more than the deploy script bugs** are:

1. **T2-C1** — chlorine pump configured fail-open will fail closed. Fix
   before any deploy where actuators control safety-critical chemical
   dosing.
2. **T2-C2** — silently fabricated zero from a failed web_poll sensor will
   feed a control loop with confidence. Fix before any deploy with web_poll
   sensors in a control loop.
3. **T2-C4** — interlock TOCTOU race breaks safety-critical mutual
   exclusion. Fix before any deploy that depends on actuator interlocks
   for safety.
4. **T2-T1** — TUI delete of an energized actuator leaves the relay
   energized. Fix before any deploy where field techs manage actuators
   in production.
5. **T2-A2** — `gpio_chip` ignored at runtime; combined with HIL-4 (schema
   default `gpiochip0` = `gpy7` on Odroid-XU4), every Odroid-XU4 actuator
   wired to header pins is misrouted.
6. **T2-C3** — DHT22/HX711 break PROFINET cyclic timing. Fix before any
   deploy with DHT22 or HX711 sensors.
7. **T2-C5** — active-low actuators get a single-drop dose pulse on every
   manager reload.
8. **T2-A4** — `bool used[17]` indexed to 246 = stack corruption with > 17
   sensors.
9. **T2-T2** — GPIO sensor edits are impossible.
10. **T2-T3** — silently broken sensor types in the picker. Hide them
    immediately.

The deploy CAN proceed without the Tier 2 fixes — but the operator must
know that **every actuator on the deployed RTU is in one of these failure
modes** until T2-C1, T2-C4, T2-C5, T2-A2, T2-T1, T2-T7 are fixed.
**Recommend gating deploy on T2-C1 and T2-T1 at minimum** (the two with
the most direct chemical-safety blast radius).

---

## Tier 4 — Security findings (training-range vulnerability hunt)

These are vulnerabilities **intentionally documented for the training range**.
Per `Water-treat/.claude-memory/feedback_preserve_training_vulns.md`, none of
these should be fixed as a side effect of any other work. They exist so the
range has more material for blue-team / detection / IR exercises.

### T4-S1 — Modbus TCP gateway one-shot deadlock (VALIDATED, single-packet unauth DoS)

`Water-controller/src/modbus/modbus_tcp.c` `server_thread_func()` re-locks
`ctx->lock` (a non-recursive `PTHREAD_MUTEX_NORMAL`) without releasing
between (D) and (E) in the request-handling branch. Result: a single
successful Modbus TCP request from any unauthenticated client deadlocks
the gateway thread permanently. Recovery requires restarting the controller
process. CVSS 3.1 7.5. Validated against `192.168.6.13:1502` on 2026-04-08.
PoC + writeup: `docs/verification/VULN_FINDING_D_POC.md`,
`docs/verification/poc-vuln-modbus-gateway-deadlock.py`,
`docs/verification/poc-vuln-modbus-gateway-deadlock.pcap`.

### T4-S2 — Modbus FC 0x10 partial-body uninit stack read (CODE-REVIEW ONLY, masked by T4-S1)

`Water-controller/src/modbus/modbus_gateway.c` `handle_server_request()`
FC 0x10 case reads `request->data[5 + i*2]` without checking the wire
request actually carried that many bytes. Underlying vulnerable code is
unambiguously present on inspection but cannot be observed in isolation
on a live target without first patching T4-S1 (which fires on the very
first request and prevents follow-up reads). CWE-457 + CWE-787.
Documented in `docs/verification/VULN_HUNT_HANDOFF.md` Finding B.

### T4-S3 — `historian.c:534` `system("mkdir -p %s", data_dir)` latent RCE primitive (NOT exploitable today)

`Water-controller/src/historian/historian.c:534` builds a shell command
via `snprintf` and passes it to `system()`. Today `data_dir` is hard-coded
NULL via designated-init in main.c, so the runtime fallback path is used
and the bug is unreachable. ANY future change that wires a config-file
value or IPC command into `historian->config.database_path` instantly
makes this an unauth RCE. Documented in
`docs/verification/VULN_HUNT_HANDOFF.md` Finding A.

### T4-S4 — `dap_count` underflow → masked OOB read in PROFINET cyclic frame handler (NOT exploitable today)

`Water-controller/src/profinet/profinet_controller.c` cyclic frame
handler. `dap_count = iodata_count` then `dap_count--` per sensor slot;
if `iodata_count == 0` and `slot_count > 0` underflows to 0xFFFF. Today
the downstream check `offset + GSDML_INPUT_DATA_SIZE <= data_length`
catches the wraparound via `int` promotion. A future refactor that
changes type widths could weaponize. Documented in
`docs/verification/VULN_HUNT_HANDOFF.md` Finding C.

### T4-S5 — Stored attacker-controlled `description` in `register_map_auto_generate` (CODE-REVIEW ONLY, no current sink)

`Water-controller/src/modbus/register_map.c` ~318/338. The `description`
is built via `snprintf("%.45s Sensor %d", dev->station_name, ...)`
where `dev->station_name` is the unauthenticated RTU registration
attacker-controlled value. Currently bounded and only used for human
output, but propagates one trust boundary. Documented as Finding E in
the handoff doc.

---

## Verification artifacts

Full agent reports under `docs/verification/`:

- `HIL_FINDINGS.md` — 15 findings from live RTU recon (192.168.6.21)
- `ARCHAEOLOGY.md` — sensor/actuator cross-module map, divergence list,
  dead-code inventory
- `REALITY_CHECK.md` — hallucination scan (5 HALLUCINATION + 16 MISMATCH +
  8 UNVERIFIED + ~82 VERIFIED)
- `CODE_REVIEW.md` — cyclic-path correctness review (6 CRITICAL + 11 HIGH
  + 9 MEDIUM + 3 LOW)
- `UX_AUDIT.md` — TUI sensor/actuator dialog audit (8 CRITICAL + 12 HIGH
  + 9 MEDIUM + 6 LOW)
- `DEPLOY_FAILURES.md` (under `docs/verification/`) — earlier draft of the
  deploy-failure list, predates the operator's hands-on findings on
  192.168.7.173 and is now superseded by THIS file (repo root)

Pending agent reports:

- `SOURCES.md` — research agent blocked on WebFetch permission. Can be
  unblocked or filed as Gaps-only stub. Lower priority than the Tier 2
  fixes above.
- Resilience map and test strategy — gated on the operator deciding which
  Tier 2 items to fix first.
