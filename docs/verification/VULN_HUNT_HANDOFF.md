# Vulnerability Hunt — Handoff for Continued Investigation

**Status**: First-pass complete, three findings logged, no validated unauth RCE yet
**Date**: 2026-04-08
**Handoff from**: Wave 8 first pass (main Claude session)
**Handoff to**: Whoever picks this up next (likely a security-focused agent or session)

---

## Why this exists

The user explicitly said:

> "if you can find another vulnerability especially rce it would be a SIGNIFICANT win"
> "but it would need to be validated"

The **only currently functional vulnerability** in the training range is the L2 race / spoofing chain documented in `/opt/poc-vuln-full-chain.sh` on `192.168.6.13`. The user wants more — especially a validated unauthenticated RCE — to enrich the training material.

This file captures what was already searched, what's still unsearched, what was found, what next steps are most likely to yield a real RCE, and the operational facts a continuation agent needs to not repeat work.

---

## CRITICAL — What you must NOT break while hunting

The L2 race vuln must remain intact. Concretely, do **not** propose fixes to:

1. `water-controller/src/profinet/ar_manager.c` (the vulnerable RT frame handler — function `ar_handle_rt_frame()` at lines ~1215-1269 — accepts frames by FrameID alone, no source MAC validation)
2. `water-controller/src/profinet/profinet_controller.c` (the receive thread that calls `ar_handle_rt_frame` without passing the source MAC)
3. `water-controller/web/api/app/api/v1/rtu.py` (the unauthenticated RTU registration endpoint — VULN-3)
4. RTU station name derivation from interface MAC (`Water-treat/src/config/config.c:218-264`, function `detect_station_id`) — the rogue device PoC depends on this to override the MAC-derived default via the config file
5. RTU's `/var/lib/water-treat/pnet/` NV data format and persistence

You can FIND new bugs in any of those areas, but do NOT propose fixes that close the existing race. Document new findings as additive, not as replacement.

The shared-memory feedback memory at `Water-treat/.claude-memory/feedback_preserve_training_vulns.md` has the full rationale and the full list of vulns to preserve.

---

## Source code locations

**Water-treat (RTU device side)**:
`/mnt/cephfs/shared/projects/Water-treat/`
Or relative: `.` (this is where you are)

**Water-controller (PROFINET IO Controller side, Python web API, Modbus gateway, IPC server)**:
`/mnt/cephfs/shared/projects/Water-controller/`
Or relative from Water-treat: `../Water-controller/`
**Note:** lowercase `c` in `controller` on the Ceph mount, even though the GitHub repo is `Water-Controller` (uppercase C). On the controller box at `192.168.6.13` it lives at `/opt/water-controller/`.

**Live deployments**:
- `192.168.6.13` (`sadmin@`) — controller host, Docker (controller + API + Postgres), all PoC scripts in `/opt/poc-*`, root shell via `echo H2OhYeah! | sudo -S`
- `192.168.6.21` (`rtu@` or `root@` via key `/opt/water.creds`) — Odroid-XU4 RTU, water-treat installed at `/usr/local/bin/water-treat`, source tree at `/opt/water-treat/`
- `192.168.7.173` (`rtu@`) — Le Potato (aarch64), the user's fresh-deploy target, source tree at `/home/rtu/Water-Treat/`, build dir at `/home/rtu/Water-Treat/build/`. **All source-side wave fixes are validated here.**

All three accept password `H2OhYeah!`.

**MEMORY references**: the controller box has Claude memory at `/root/.claude/projects/-opt/memory/MEMORY.md` documenting the prior PROFINET RPC bug-hunting work, the working full-chain PoC details, and discovered RTU/controller behavior. Read it.

---

## What was already searched in the first pass

These surfaces were audited and judged clean (no exploitable bug found). **Don't redo these.**

### Water-treat RTU
- `src/profinet/profinet_callbacks.c` — write_record dispatcher and handler, lines 440-502. Clean dispatch. Read handler clean.
- `src/profinet/config_sync.c` — all three handlers (`config_sync_process_device`, `config_sync_process_sensors`, `config_sync_process_actuators`). Length-check-before-cast, count caps (max 246), CRC verification, sized memcpy, forced null termination, slot range validation. **Defensively well-written.**
- `src/auth/user_sync.c` — `user_sync_process_packet` and `process_user_entry`. Same defensive pattern. SAFE_STRNCPY usage, role validation with default fallback, fail-safe denial.
- `src/health/health_check.c` — HTTP request parser (`handle_http_request` at line 788). `recv` bounds match buffer (`request[1024]`, recv reads `sizeof-1`). `sscanf("%15s %255s", method, path)` matches buffer sizes (`method[16]`, `path[256]`). No stack overflow.
- `serve_gsdml_file()` — paths are hardcoded constants, no user input.
- `src/` whole-tree grep for `system|popen|exec*|fork|strcpy|strcat|sprintf` — **zero unsafe primitive calls in the entire RTU codebase.** Unusually well-hardened.

### Water-controller
- `src/db/database.c` — 12 `PQexec` calls, 8 of which are built via `snprintf` into a query buffer. **All 8 use `%d` integer interpolation only** (LIMIT clauses, INTERVAL '%d days'). Other queries use `PQexecParams` parameterized. No string SQL injection.
- `src/modbus/modbus_tcp.c` — `tcp_recv_frame()` bounds checks correct: header receive is fixed 7 bytes, length validated against `MODBUS_MAX_PDU_LEN+1`, second recv reads exactly `pdu_len` bytes into `buffer[7..]`. `MODBUS_TCP_MAX_ADU_LEN=260`, `MODBUS_TCP_HEADER_LEN=7`, `MODBUS_MAX_PDU_LEN=253` — `7+253=260` — buffer exactly sized. The PDU receive path is safe.
- `src/profinet/profinet_controller.c` receive thread at line ~313 — has bounds checks. `dap_count` integer underflow exists (Finding C below) but the downstream `offset + GSDML_INPUT_DATA_SIZE <= data_length` check uses int promotion which catches the wraparound case. Latent but not exploitable for memory corruption.
- `src/profinet/profinet_controller.c` `profinet_controller_read_record` at line ~1370 — manual offset arithmetic but the `data_offset >= resp_len` check is correctly placed before the `memcpy(&record_len_be, response + data_offset - 4, 4)` so the read is in-bounds. `record_len` from network is clamped against `resp_len - data_offset` and against `*len`. Safe.
- `src/ipc/ipc_server.c` — uses `snprintf` only, no `sprintf/strcpy/strcat/system/popen`. IPC commands are typed `shm_command_t` structs over POSIX shared memory. No string parsing on the IPC path, no command injection surface.
- `web/api/` (Python FastAPI) — grep for unsafe primitives: **zero** `yaml.load`, `pickle.loads`, `marshal.loads`, `__import__` from user input, `cursor.execute(f"...")`, `cursor.execute(...%...)`, `eval(`, `exec(`, `render_template_string`, `subprocess shell=True`, `open(request.X)`, `FileResponse(request.X)`. The only `compile` calls are `re.compile` with hardcoded regex patterns. The Python web API is unusually well-defended.

---

## Findings (3) — first pass

### Finding A — `historian.c:534` latent RCE primitive (CRITICAL if armed, NOT exploitable today)

**File**: `../Water-controller/src/historian/historian.c:531-540`

```c
const char *data_dir = historian->config.database_path;
if (!data_dir) {
    data_dir = "/var/lib/water-controller/historian";
}
...
char mkdir_cmd[300];
snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", data_dir);
system(mkdir_cmd);
```

**Why it's a finding**: `system()` with a snprintf-built command string is the textbook command-injection primitive. If `data_dir` ever contains a `;`, `` ` ``, `$()`, or newline, it executes as a shell command — and the historian process runs as root inside the controller container.

**Why it's NOT exploitable today**: traced every assignment to `historian->config.database_path` across the entire repo. The struct is initialized **exactly once**, in `main.c:684`:

```c
historian_config_t hist_config = {
    .max_tags = WTC_MAX_HISTORIAN_TAGS,
    .buffer_size = 1000,
    .default_sample_rate_ms = 1000,
    .default_deadband = 0.1f,
    .retention_days = 365,
};
```

The `database_path` field is OMITTED, so it defaults to NULL via designated-initializer rules, and the runtime fallback path `/var/lib/water-controller/historian` is always used. There is no config file load and no API endpoint that writes to this field today.

**What turns this into a real RCE**:
1. Adding a config file option that loads `historian_database_path` from the controller's config and assigns it to `hist_config.database_path` in main.c. Common, expected, would be a "small refactor" PR.
2. OR: adding an IPC command (`SHM_CMD_SET_HISTORIAN_PATH`) that lets the Python API change it at runtime.
3. OR: a future change to `historian_init` that reads the config from elsewhere.

**Why this is great training material**: the training range can either ship the code as-is (latent, "find the loaded gun" exercise) or arm it with a one-line config wiring diff (instant unauth RCE through the Python API → IPC → controller). The diff to arm it is small enough that it could plausibly land in a real PR review without anyone noticing.

**Validation status**: NOT validated (not currently exploitable). To validate post-arming, see "Next steps for the agent" section.

---

### Finding B — Modbus FC 0x10 partial-body uninitialized stack read (info disclosure + register write chaos, NOT directly RCE)

**File**: `../Water-controller/src/modbus/modbus_gateway.c`, function `handle_server_request()`, case `MODBUS_FC_WRITE_MULTIPLE_REGISTERS`

```c
case MODBUS_FC_WRITE_MULTIPLE_REGISTERS: {
    if (quantity > MODBUS_MAX_WRITE_REGISTERS) {
        return MODBUS_EX_ILLEGAL_DATA_VALUE;
    }
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t value = modbus_get_uint16_be(&request->data[5 + i * 2]);
        ...
        write_register_value(gw, mapping, value);
    }
}
```

**Why it's a finding**: the handler trusts `quantity` (bytes 2-3 of the PDU body) and reads `request->data[5..5+quantity*2-1]` without verifying the request actually contained that many bytes of register data. A standard Modbus FC 0x10 PDU has a `byte_count` byte at offset 4 — the handler ignores it.

`request` is a `modbus_pdu_t` declared on the stack of `handle_client_request`. `tcp_recv_frame()` only fills the bytes that arrived from the wire (`pdu->data_len = pdu_len - 1`). The unfilled tail of `pdu->data[252]` is whatever was on the stack before — possibly leftover data from the previous request handled on the same thread.

**Effects**:
1. **Uninitialized stack read**: leaks prior request data from the same thread's stack frame. Could leak partial heap addresses if the previous request handler put pointers in that area.
2. **Garbage register writes**: 100+ register writes with leftover stack values. Each `write_register_value()` call dispatches to `write_register_value()` which can update PROFINET actuator outputs, set PID setpoints, or write to downstream Modbus devices. Causes **denial of service / control surface chaos** for any deployment that has actuators or PID loops mapped into the Modbus register space.

**Why it's NOT directly RCE**: the writes are constrained to Modbus register types (uint16). They don't write into stack pointers, return addresses, or function pointers. Each value is dispatched through `write_register_value()` which maps to typed actuator/PID/Modbus-client write paths. No direct memory corruption.

**Validation plan** (untested but mechanical):

```python
import socket, struct
TARGET = "192.168.6.13"; PORT = 1502
s = socket.create_connection((TARGET, PORT))
# MBAP: trans_id=1, proto=0, length=7 (unit + FC + start + qty), unit=1
# PDU:  FC=0x10, start=0x0000, qty=0x0064 (100), <no byte_count, no data>
pdu = struct.pack(">HHHBBHH", 1, 0, 7, 1, 0x10, 0x0000, 0x0064)
s.send(pdu)
resp = s.recv(256)
print(resp.hex())
# Expect: either 100-register-write success response (which proves the bug
# wrote 100 garbage values to mapped registers), or an exception response
# (which means the controller did detect the partial body somewhere I didn't
# see and rejected it).
```

If the response is success (FC `0x10`, echoes start+qty), the bug is confirmed and reachable. Then read back the registers with FC `0x03` (Read Holding Registers) starting at the same start_addr to see what got written.

**Severity**: high info-disclosure + high availability impact, no integrity-on-RCE.

**Status**: NOT validated. Validation is ~15 minutes of work and is the **single most actionable next step** for the continuation agent. See Next Steps.

---

### Finding D — Modbus TCP gateway one-shot deadlock (VALIDATED, single-packet unauth DoS)

**File**: `../Water-controller/src/modbus/modbus_tcp.c`, function `server_thread_func()` lines ~255-289
**CWE**: CWE-667 (improper locking) -> CWE-400 (denial of service)
**CVSS 3.1**: 7.5 — `AV:N/AC:L/PR:N/UI:N/S:U/C:N/I:N/A:H`
**Status**: VALIDATED end-to-end against `192.168.6.13:1502` on 2026-04-08
**PoC**: `docs/verification/poc-vuln-modbus-gateway-deadlock.py`
**Pcap**: `docs/verification/poc-vuln-modbus-gateway-deadlock.pcap`
**Writeup**: `docs/verification/VULN_FINDING_D_POC.md`

**TL;DR**: After successfully handling a Modbus request, the per-connection inner loop re-acquires `ctx->lock` once for bookkeeping, then **falls through** to a SECOND `pthread_mutex_lock(&ctx->lock)` call without an intervening unlock. The mutex is `PTHREAD_MUTEX_NORMAL` (default) — re-locking by the same thread deadlocks on glibc. Result: **the Modbus TCP gateway can serve exactly one successful request from any client before its thread permanently self-deadlocks**. Single 12-byte FC 0x03 from any unauthenticated network client kills the entire Modbus integration surface. Only recovery is restarting the controller process.

Vulnerable sequence (annotated):

```c
pthread_mutex_lock(&ctx->lock);                       // outer
for (i = 0; i < MAX; i++) {
    if (active[i] && FD_ISSET(...)) {
        pthread_mutex_unlock(&ctx->lock);             // (A) released
        recv(MSG_PEEK);
        if (disconnect) {
            pthread_mutex_lock(&ctx->lock);           // (B)
            ...
            pthread_mutex_unlock(&ctx->lock);         // (C) released - OK
        } else {
            handle_client_request(...);
            pthread_mutex_lock(&ctx->lock);           // (D) re-acquire
            clients[i].last_activity_ms = ...;
            // <<< NO UNLOCK HERE >>>
        }
        pthread_mutex_lock(&ctx->lock);               // (E) DEADLOCK after request path
    }
}
```

**Validation evidence** (live target):
- Request 1 (FC 0x03 read 1 holding reg at addr 0): clean reply in 0.00s.
- Request 2 (identical, fresh TCP connection): timed out at 10.00s.
- Process inspection: thread `30957` sitting in `futex_wait_queue`.
- `ss` shows new connections pile up in `CLOSE-WAIT` state with non-zero `Recv-Q`.

**Why this matters for the training range**: this is a single-packet, unauthenticated, network-reachable, easily-discoverable bug that disables the entire Modbus integration on the controller. It's a great purple-team finding (defenders should detect it via Modbus health monitoring + the unique CLOSE-WAIT signature) AND a great red-team finding (one packet from any L2/L3-reachable host disables SCADA polling for as long as the operator doesn't notice). **This is the validated unauth vuln from the continuation pass — not RCE, but very real availability impact.**

**Note on Finding B**: this deadlock masks the FC 0x10 partial-body uninit stack read (Finding B) on a live target. Finding B is still in the source — unchanged — but cannot be observed in isolation without first patching this deadlock. See `VULN_FINDING_D_POC.md` "Why the original Finding B could not be observed in isolation" for details.

---

### Finding E — `register_map_auto_generate` `description` is built from attacker-controlled `dev->station_name` via unauth RTU registration (code-review only)

**File**: `../Water-controller/src/modbus/register_map.c` lines ~318 and ~338
**Status**: code-review only, NOT validated
**Severity**: Low/Info (no memory corruption, no obvious downstream sink)

```c
strncpy(reg.rtu_station, dev->station_name, sizeof(reg.rtu_station) - 1);
reg.rtu_station[sizeof(reg.rtu_station) - 1] = '\0';
snprintf(reg.description, sizeof(reg.description), "%.45s Sensor %d",
         dev->station_name, s + 1);
```

`dev->station_name` is the RTU station name learned via the existing unauth RTU-registration endpoint (VULN-3). Currently the use is constrained: `strncpy` and `snprintf("%.45s", ...)` are both bounded, so there's no buffer overflow. The `description` field is in-memory and used (as far as I traced) only for human-readable output. **However**, if a future change adds the description to a SQL query or HTTP response that's later rendered without escaping, this becomes a stored-XSS / SQL-injection sink fed from an unauthenticated source.

**What turns this into a real bug**: any change that pipes `register_mapping_t.description` into a downstream string sink without sanitizing. Worth flagging in the training range as "unauth-influenced data inside the control surface, propagating one boundary".

---

### Finding C — `dap_count` integer underflow (defensive depth, not exploitable today)

**File**: `../Water-controller/src/profinet/profinet_controller.c` cyclic frame handler in the receive thread (around line 380-410)

```c
uint16_t dap_count = ar->iocr[j].iodata_count;
for (int s = 0; s < ar->slot_count; s++) {
    if (ar->slot_info[s].type == SLOT_TYPE_SENSOR)
        dap_count--;
}
uint16_t offset = dap_count;  /* Skip DAP IOPS bytes */
```

**Why it's a finding**: if `iodata_count == 0` and `slot_count > 0` with at least one sensor slot, `dap_count` underflows to ~65535. `offset` then becomes the underflow value.

**Why it's NOT exploitable today**: the downstream check `if (offset + GSDML_INPUT_DATA_SIZE <= ar->iocr[j].data_length)` uses `int` promotion (because both operands are smaller than `int`), so `65535 + 5 = 65540` which is greater than any plausible `data_length` for an Ethernet-bound IOCR. The check correctly rejects.

**Why it's worth a flag**: the safety here depends on the downstream check using the right promotion rules. A future refactor that, e.g., changes `data_length` to `size_t` or moves the check to a function that takes `uint16_t` parameters could weaponize this into an OOB read or wild memcpy.

**Validation status**: NOT validated as exploitable. Worth keeping in the training notes as a defensive-depth lesson.

---

## Next steps for the continuation agent

Ordered by RCE-yield-per-hour-of-investigation:

### 1. Validate Finding B (~15 min, high probability of real PoC)

Run the Python snippet above against `192.168.6.13:1502` (Modbus TCP). Capture:
- The raw response bytes
- A follow-up FC 0x03 read of the same register range to see what got written
- The controller's journal lines (`docker logs wtc-controller`) during the attack
- A diff of any actuator state changes via `curl http://192.168.6.13:8000/api/v1/rtus/<station>/sensors`

Document as `docs/verification/VULN_FINDING_B_POC.md` with the writeup template:
```
## Vuln-B PoC: Modbus FC 0x10 partial-body uninit stack read

Target: 192.168.6.13:1502 (Modbus TCP, no auth)
File: water-controller/src/modbus/modbus_gateway.c handle_server_request() FC 0x10
Type: CWE-457 (uninit memory read) + CWE-787 (out-of-bounds write to control surface)
Validated: <YES|NO>
PoC script: docs/verification/poc-vuln-modbus-fc10-partial.py
Wire trace: docs/verification/poc-vuln-modbus-fc10-partial.pcap
```

### 2. DCP frame parser (untouched, high yield)

The controller listens for DCP responses on a raw socket. DCP is what the controller uses to discover RTUs by station name. The DCP frame parser lives in `../Water-controller/src/profinet/dcp.c` (or similar — confirm location). Likely places for bugs:

- **DCP option/sub-option TLV parsing**: a malformed TLV with a length field larger than the remaining frame causes OOB read.
- **DCP station-name parsing**: attacker-controlled station name length field with no upper bound check.
- **Vendor-specific block parsing**: many DCP implementations forget to validate the inner block length.

The receive thread (already located at `profinet_controller.c:313`) calls `dcp_process_frame(ctrl->dcp, buffer, len)`. Read `dcp_process_frame()` and trace every length field that comes from the wire. The function name to grep for is `dcp_process_frame` and `dcp_parse_*`.

**Why this is high yield**: DCP is processed before any AR is established, so it runs at the absolute earliest point in the controller's lifecycle. No auth, no session, no state. A bug here is unauth RCE on the controller from any L2-connected attacker. This pairs perfectly with the existing race vuln (which uses the SAME L2 attack vector but only achieves data injection).

### 3. `register_map.c` loader (22KB, untouched, possible config-file parser bugs)

`../Water-controller/src/modbus/register_map.c` is 22KB. It loads register-mapping config from somewhere (likely a JSON or CSV file). If the loader uses `sscanf("%s", ...)` without bounds, or an integer field in the config gets promoted/clamped wrong, that's a write primitive into the global register map. Combined with the FC 0x10 partial-body bug (Finding B), this could escalate.

Grep for `register_map_load`, `parse_mapping`, `fopen` in this file. If the loader reads from a file at startup, check whether the file path is configurable (i.e., attacker-influenceable through config volume mount).

### 4. `handle_user_sync_command` station_name handling (IPC)

`../Water-controller/src/ipc/ipc_server.c:735`:
```c
if (cmd->command_type == SHM_CMD_USER_SYNC && cmd->user_sync_cmd.station_name[0]) {
    ...
}
```

The `station_name` field comes through SHM from the Python API. If it's used in any string-format operation (snprintf for logs, log4c-like formatters, journal field formatting), check whether the format string is constant. The classic Python→C IPC RCE pattern is "API blindly forwards untrusted user input to an IPC field that the C side passes to a logger that uses sprintf-style formatting."

### 5. Path-traversal → file-based RCE chain (VULN-5 already documented)

`../Water-controller/web/api/app/api/v1/discover.py:1091-1151` is the `fetch_gsdml` handler. The PoC writes attacker-XML to `/var/cache/water-controller/gsdml/<station_name>.xml` with traversal in `station_name`. The `.xml` extension is appended unconditionally.

**Open question**: is there any installed software that processes `*.xml` files automatically (cron job, systemd timer, ImageMagick policy, Java/Tomcat, XSLT processor)? If yes, file-write becomes RCE. The Python container is FastAPI on uvicorn — likely no. The controller container is C — also unlikely. But the **host** (`192.168.6.13`) might have something.

Check `crontab -l` as root, `/etc/cron.*/`, `systemctl list-timers`, and `dpkg -l | grep -E 'imagemagick|xsltproc|libxml2-utils|java'`.

### 6. PostgreSQL `COPY ... FROM PROGRAM` chain (low yield, requires auth or chained injection)

The `database.c` queries are clean of SQL injection. But check the **Python API's database layer** (`web/api/app/db/` or similar). If any Python ORM call uses raw SQL with f-strings, a SQL injection there can be escalated to RCE via `COPY ... FROM PROGRAM` IF the postgres user has the right privileges. Check what role the API connects as (likely in a docker-compose env file or `web/api/app/core/config.py`).

### 7. WaterTreat-side acyclic record write — was clean on first pass, but recheck after Wave 2 changes

I made TUI dialog changes in Wave 2 that touched `dialog_sensor.c` and `dialog_actuator.c`. These are local TUI surfaces, not network-reachable. **Don't re-search them.** But: I added a `db_actuator_gpio_conflict_check` parameter. Make sure the new parameter doesn't introduce a wild dereference or off-by-one anywhere it's called from.

### 8. Look at `controller_discovery.c` on the RTU side

`Water-treat/src/profinet/controller_discovery.c` — discovers controllers via DCP. Same family of bugs as the controller-side DCP parser. Lower priority than the controller-side hunt because the RTU has fewer attackers (it accepts frames from a network, but the network in a real deployment is more trusted than the controller's network). But still worth a look.

---

## Methodology notes

### What worked

- **Grep for unsafe primitives first** (`system|popen|exec|fork|strcpy|strcat|sprintf|gets|sscanf %s`) — fastest way to find loaded guns. Hit on `historian.c:534`.
- **Read length-check-then-cast patterns** in network parsers — config_sync, user_sync, modbus_tcp all do this correctly. Modbus FC 0x10 specifically does NOT, which is why Finding B exists.
- **Trace `*.config.X` field assignments end-to-end** before flagging a bug as exploitable — saved me from claiming Finding A is reachable when it isn't.

### What didn't work / dead ends

- Searching the Python API for unsafe primitives — it's well-defended and modern FastAPI patterns make injection hard. Don't sink more time here unless you find a very specific lead.
- The HTTP request parser in `health_check.c` — I expected to find a buffer overflow but `sscanf("%15s %255s")` matches the buffer sizes exactly. Not a bug.
- `database.c` SQL injection — every snprintf+PQexec uses `%d` interpolation only.

### Tools useful for the next pass

- **`gdb`** on the controller: `docker exec -it wtc-controller gdb -p $(pidof wtc_controller)` lets you set breakpoints in C handlers and observe stack contents during PoC validation.
- **`strace`** on the controller process: see exact `recv()` boundaries and `system()` calls if any.
- **`tcpdump -i any -w`** on `192.168.6.13` while running PoCs: capture wire traffic for documentation.
- **`docker logs -f wtc-controller`** and **`docker logs -f wtc-api`**: real-time log diff during PoC.
- **`PGUSER`, `PGDATABASE` env vars on the API container**: tell you what privileges the SQL injection chain would need.

### Reminders

- The current race vuln depends on starting the rogue device BEFORE restarting the controller. Don't break that ordering invariant in any code change.
- The controller binds raw sockets and runs as root inside the container. Privesc inside the container = host filesystem access if any host paths are mounted (check `docker inspect wtc-controller` for volume mounts).
- All findings must be **validated end-to-end with a working PoC** before claiming "this is a vuln". The user explicitly stated: "but it would need to be validated".

---

## Where to put new findings

- **Validated PoCs**: `docs/verification/VULN_FINDING_<letter>_POC.md` + accompanying script in `docs/verification/poc-*.py`
- **Code-review-only findings (no PoC yet)**: append to this file as new entries under `## Findings`
- **Updates to the preserve-list**: edit `Water-treat/.claude-memory/feedback_preserve_training_vulns.md` to add any new vuln so future fixes don't accidentally close it
- **Cross-reference**: add a one-line summary of every validated finding to `Water-treat/DEPLOY_FAILURES.md` Tier 4 (create the section if it doesn't exist) so the deployment punch-list reflects the security posture

---

## Final note on scope

The user is explicitly **not** in defensive-fix mode for these findings. They want the vulns documented and either left in the codebase (if they're already exploitable, like the race vuln) or trivially-armable (like Finding A) so that the training range has more material. **Do not propose remediation patches in this file.** Findings only, with PoCs.

If you find a bug that is genuinely high-impact and the user might not want it in the training range, surface it to the user and ask before either documenting it OR proposing a fix.
