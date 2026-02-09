# RTU Team: Connection Support Instructions

**From**: p-net analysis (pcap-verified, code-traced)
**Date**: 2026-01-31
**Scope**: Water-treat repo (`mwilco03/Water-treat`)

---

## Current State

The RTU PROFINET implementation is structurally sound. The p-net library
handles connection acceptance, block parsing, and validation correctly.
The database-driven slot system works as designed. The primary connection
failures originate from the controller's wire encoding (documented separately).

This document covers:
1. Items the RTU team must verify/maintain for connection success
2. The `/slots` HTTP endpoint (fallback mechanism, lowest priority)
3. GSDML consistency checks
4. Operational concerns

---

## Section 1: Verify Existing Implementation

### 1.1 DAP Submodule Idents Must Match GSDML

**File**: `src/profinet/profinet_manager.c`
**Lines**: 885-919

Current code:
```c
885  pnet_plug_module(g_pn.pnet, 0, 0, GSDML_MOD_DAP);
894  pnet_plug_submodule(g_pn.pnet, 0, 0, 1,
895                       GSDML_MOD_DAP, GSDML_SUBMOD_DAP,
896                       PNET_DIR_NO_IO, 0, 0);
906  pnet_plug_submodule(g_pn.pnet, 0, 0, 0x8000,
907                       GSDML_MOD_DAP, GSDML_SUBMOD_DAP_INTERFACE,
908                       PNET_DIR_NO_IO, 0, 0);
917  pnet_plug_submodule(g_pn.pnet, 0, 0, 0x8001,
918                       GSDML_MOD_DAP, GSDML_SUBMOD_DAP_PORT,
919                       PNET_DIR_NO_IO, 0, 0);
```

**Verify these constants match the GSDML**:

| Constant | Expected Value | GSDML Source |
|----------|---------------|--------------|
| `GSDML_MOD_DAP` | 0x00000001 | Line 52: DeviceAccessPointItem |
| `GSDML_SUBMOD_DAP` | 0x00000001 | Line 95: VirtualSubmoduleItem "VSM_DAP" |
| `GSDML_SUBMOD_DAP_INTERFACE` | 0x00000100 | Line 116: InterfaceSubmoduleItem |
| `GSDML_SUBMOD_DAP_PORT` | 0x00000200 | Line 130: PortSubmoduleItem |

**Subslot numbers**:
- DAP identity: subslot 1 (0x0001) at line 894
- Interface: subslot 0x8000 at line 906
- Port: subslot 0x8001 at line 917

These are standard PROFINET subslot assignments. Confirm the defines match.

**GSDML note**: Line 105 of the GSDML had a VirtualSubmoduleItem "VSM_Port1"
with `SubmoduleIdentNumber="0x00008000"`. This was the subslot number, not a
valid submodule ident. **FIXED**: Changed to `0x00000200` to match the
PortSubmoduleItem in SystemDefinedSubmoduleList (line 130). The code at
profinet_manager.c:917 correctly uses `GSDML_SUBMOD_DAP_PORT = 0x00000200`.

---

### 1.2 Module Plugging from Database

**File**: `src/profinet/profinet_manager.c`
**Lines**: 930-953

```c
930  for (int i = 0; i < g_pn.slot_count; i++) {
931      profinet_slot_t *slot = &g_pn.slots[i];
933      int ret = pnet_plug_module(g_pn.pnet, 0, slot->slot, slot->module_ident);
939      pnet_submodule_dir_t direction = is_actuator_module(slot->module_ident)
940                                           ? PNET_DIR_OUTPUT
941                                           : PNET_DIR_INPUT;
943      ret = pnet_plug_submodule(g_pn.pnet, 0, slot->slot, slot->subslot,
944                                slot->module_ident, slot->submodule_ident,
945                                direction,
946                                slot->input_size, slot->output_size);
```

**Status**: This is correct. Modules are loaded from the `modules` SQLite table
via `db_module_list()` (`db_modules.c:176-227`), ordered by slot number.

**Empty database behavior**: If no modules are in the database, only DAP is
plugged. The controller will see a ModuleDiffBlock with MODULE_STATE_NO_MODULE
for any application slots it requests. This is correct PROFINET behavior.

**Action required**: Ensure the database is populated before PROFINET init.
The controller team needs at least one successful connection to validate
Phase 1 (DAP-only), which requires no database entries.

---

### 1.3 Station Name: Do Not Accept DCP Set-Name

**File**: `src/profinet/profinet_manager.c`
**Lines**: 185-243

The NV storage cleanup at lines 185-243 (`clear_pnet_nv_station()`) deletes
all `pf_*` files before `pnet_init()` to enforce the configured station name.
This prevents a controller's DCP Set-Name from overwriting the RTU's identity.

**Status**: Correct. The RTU generates its own name (`rtu-XXXX` from MAC) and
enforces it on every boot. No changes needed.

**Design agreement**: The controller team has been instructed to NOT use
DCP Set-Name. The RTU's station name is discovered via DCP Identify Response
and used as-is.

---

### 1.4 IP Address: RTU Owns Its IP

**File**: `src/profinet/profinet_manager.c`
**Lines**: 608-660

The RTU reads its IP from the system interface via `getifaddrs()`. Whether the
IP comes from DHCP or static configuration is a system-level concern, not a
PROFINET concern.

```c
612      if (getifaddrs(&ifaddr) != 0) { ... }
619      for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
621          if (strcmp(ifa->ifa_name, iface) != 0) continue;
622          if (ifa->ifa_addr->sa_family != AF_INET) continue;
627          in_addr_to_pnet_ip(addr->sin_addr, &cfg->if_cfg.ip_cfg.ip_addr);
```

**Status**: Correct. The RTU reports its current IP. The controller discovers
it via DCP and connects to it.

**Design agreement**: The controller team has been instructed to NOT use
DCP Set to assign IP addresses.

---

### 1.5 Rapid Reconnect Stability

The pcap analysis showed the RTU stops responding after approximately 9
connection attempts (session exhaustion at frame 58). This is because p-net
has a limited number of AR (Application Relationship) slots, controlled by
`PNET_MAX_AR` in the p-net build configuration.

**File**: `p-net/pnet_options.h.in` (in the p-net library, not Water-treat)

When a failed connect request consumes an AR slot but doesn't cleanly release
it, subsequent requests find no available slots and are silently dropped.

**Mitigation**: p-net cleans up stale sessions on a timer, but rapid-fire
requests (as in the brute force pcap) can exhaust slots faster than cleanup.

**Action**: No RTU code change needed. The controller team has been instructed
to use a single correct wire format rather than rapid strategy cycling. If
session exhaustion is still observed after the controller fixes, consider
increasing `PNET_MAX_AR` in the p-net build or adding explicit session timeout
handling.

---

## Section 2: HTTP Endpoints (Implemented)

### Status

Both endpoints are **implemented** in `src/health/health_check.c`:
- `/slots` — `slots_to_json()` at line 615
- `/gsdml` — `serve_gsdml_file()` at line 660
- Route handler at line 811, after `/config`, before 404

### Context

These are NON-STANDARD extensions. Standard PROFINET uses GSDML files (on disk)
and ModuleDiffBlock/Record Read for module discovery. These HTTP endpoints exist
as fallbacks when the controller doesn't have the GSDML file locally and
standard discovery mechanisms have failed.

The controller team's fallback chain (both documents agree on this order):
1. Local GSDML file on disk (standard, offline)
2. **GET /gsdml from RTU HTTP** — fetch the standard device description
3. Cached config from a previous successful connection
4. **GET /slots from RTU HTTP** — proprietary slot list
5. DAP-only connect + Record Read 0xF844 (standard PROFINET)

### 2.1 `/gsdml` — Device Description (Fallback #2)

Serves the raw GSDML XML file. This is architecturally preferred over
`/slots` because the GSDML IS the standard device description — only
the transport (HTTP vs. file) is non-standard. The controller can cache it
locally, making future connections use fallback #1.

```
GET /gsdml
Content-Type: application/xml
```

**Implementation details:**
- Streams the GSDML file in 4KB chunks (file exceeds 8KB response buffer)
- Search path: `/opt/water-treat/gsd/` (installed) then `gsd/` (development)
- Returns HTTP 200 with raw XML on success
- Returns HTTP 404 with JSON error if GSDML file not found

**Controller-side usage:**
1. Fetch GSDML via HTTP
2. Save to local cache (e.g., `/var/cache/water-controller/gsdml/`)
3. Parse as standard GSDML — same code path as local file
4. Build ExpectedSubmoduleBlockReq from parsed module catalog
5. Next connection uses cached file (fallback #1), no HTTP needed

### 2.2 `/slots` — Slot Configuration (Fallback #4)

```
GET /slots
Content-Type: application/json
```

### API Contract (agreed between both teams)

This is the single source of truth for the `/slots` contract.
Both the controller and RTU implementations must conform to this spec.

**Design decisions and rationale:**

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Path | `/slots` | Versioned path separates API from health endpoints. Matches controller's `/api/v1/` convention. |
| Ident encoding | Integer (decimal) | JSON-native. No string parsing needed. DB stores integers. C compares work: `json_val == 0x10` since 0x10 == 16. Hex is for documentation only. |
| DAP included | No | DAP is always present with fixed config defined in GSDML. Controller already knows DAP. Including it creates a redundant source of truth that could contradict GSDML. |
| Data source | Database (`db_module_list()`) | Available before `pnet_init()` completes. Represents configured intent, not just runtime state. Answers "what should be plugged?" not "what is the stack doing right now?" |
| Audience | Machine-to-machine | This endpoint is consumed by the controller C code, not by operators. Operators use the controller's HMI, which presents RTU data through its own API. |

### Response format

**Response when modules are loaded** (HTTP 200):
```json
{
  "slot_count": 3,
  "slots": [
    {
      "slot": 1,
      "subslot": 1,
      "module_ident": 16,
      "submodule_ident": 17,
      "direction": "input",
      "data_size": 5
    },
    {
      "slot": 2,
      "subslot": 1,
      "module_ident": 256,
      "submodule_ident": 257,
      "direction": "output",
      "data_size": 4
    },
    {
      "slot": 5,
      "subslot": 1,
      "module_ident": 64,
      "submodule_ident": 65,
      "direction": "input",
      "data_size": 5
    }
  ]
}
```

**Field definitions:**

| Field | Type | Description |
|-------|------|-------------|
| `slot` | integer | Slot number (1-246). DAP at slot 0 is never included. |
| `subslot` | integer | Subslot number (always 1 for application modules) |
| `module_ident` | integer | Module identifier per GSDML. E.g., 16 = pH (0x10), 256 = Pump (0x100) |
| `submodule_ident` | integer | Submodule identifier. Convention: module_ident + 1 |
| `direction` | string | `"input"` (sensor) or `"output"` (actuator) |
| `data_size` | integer | Bytes per cyclic update. 5 for sensors, 4 for actuators. |

**Module ident reference (decimal / hex):**

| Module | Decimal | Hex | Submodule | Direction | Data Size |
|--------|---------|-----|-----------|-----------|-----------|
| pH | 16 | 0x00000010 | 17 | input | 5 |
| TDS | 32 | 0x00000020 | 33 | input | 5 |
| Turbidity | 48 | 0x00000030 | 49 | input | 5 |
| Temperature | 64 | 0x00000040 | 65 | input | 5 |
| Flow | 80 | 0x00000050 | 81 | input | 5 |
| Level | 96 | 0x00000060 | 97 | input | 5 |
| Generic AI | 112 | 0x00000070 | 113 | input | 5 |
| Pump | 256 | 0x00000100 | 257 | output | 4 |
| Valve | 272 | 0x00000110 | 273 | output | 4 |
| Generic DO | 288 | 0x00000120 | 289 | output | 4 |

**Response when database is empty** (HTTP 200, no application modules):
```json
{
  "slot_count": 0,
  "slots": []
}
```

**Response when PROFINET subsystem not initialized** (HTTP 503):
```json
{
  "error": "PROFINET subsystem not initialized"
}
```

**Response when HTTP server is starting** (HTTP 503):
The controller must handle TCP connection refused (server not yet listening)
the same as HTTP 503 — retry later or fall through to next discovery method.

### Implementation (DONE)

Implemented in `src/health/health_check.c`:
- `slots_to_json()` at line 615
- Data source: `db_module_list(g_health.db, ...)` — reads from database,
  available before `pnet_init()` completes
- Direction: `(module_ident & 0x100) != 0` → actuator ("output"), else sensor ("input")
- Data size: 5 bytes (`GSDML_SENSOR_INPUT_SIZE`) for sensors,
  4 bytes (`GSDML_ACTUATOR_OUTPUT_SIZE`) for actuators

### DAP is NOT included

The response lists only application modules (slots 1-246).
DAP (slot 0) is always present and has a fixed configuration that both
teams know from the GSDML. Including it would create a redundant source
of truth — if the GSDML says one thing and the HTTP response says another,
the controller doesn't know which to trust. Single source of truth for
DAP is the GSDML.

### Operator visibility note

This endpoint is machine-to-machine. Operators should NOT interact with
RTU HTTP directly. The controller's HMI (Next.js + FastAPI at
`web/api/app/api/v1/`) presents RTU slot data through its own API after
fetching from the RTU. This separation means:
- Operators see slot data through the controller's unified interface
- The RTU's HTTP is a low-level machine interface, not an operator console
- No confusion about "which system am I talking to?"

---

## Section 3: GSDML Consistency

### File: `gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml`

### Items to verify

1. **PhysicalSlots** (line 54): `"0..246"` — 247 total slots.
   This matches the Modbus parity requirement (247 registers).

2. **MinDeviceInterval** (line 55): `"32"` — 32ms minimum.
   The controller's TIMING_CONSERVATIVE profile uses 256ms, which is
   a multiple of 32ms. Verified compatible.

3. **DAP Module Ident**: Must match `GSDML_MOD_DAP` constant.
   Standard p-net uses 0x00000001 for DAP. Verify this matches.

4. **VirtualSubmoduleItem "VSM_Port1"** (line 105):
   **FIXED**: Was `SubmoduleIdentNumber="0x00008000"` (the subslot number,
   not a submodule ident). Changed to `0x00000200` to match the
   PortSubmoduleItem in SystemDefinedSubmoduleList (line 130).
   This prevents GSDML parsers from seeing a phantom submodule with
   ident 0x00008000 that doesn't correspond to any plugged submodule.

5. **Module UsableModules** definitions: Verify each sensor/actuator
   module type listed in the GSDML has a corresponding entry in the
   database schema and in `profinet_manager.c`'s `is_actuator_module()`.

---

## Section 4: Operational Notes

### Database population

The modules database drives everything. If empty, only DAP is plugged.
This is valid for Phase 1 testing (DAP-only connect).

For full operation, modules must be inserted before PROFINET init:

```sql
INSERT INTO modules (slot, subslot, name, module_type, module_ident,
                     submodule_ident, status)
VALUES (1, 1, 'pH Sensor', 'sensor', 16, 17, 'active');
```

Module idents are decimal in SQLite:
- 16 = 0x00000010 (pH)
- 17 = 0x00000011 (pH submodule)
- 256 = 0x00000100 (Pump)
- 257 = 0x00000101 (Pump submodule)

### Health check port

The HTTP server port is configurable via `[health] http_port` in
`/etc/water-treat/water-treat.conf`. Default varies by deployment.
The controller team uses port 9081 for RTU probing
(`web/api/app/api/v1/discover.py` probe-ip endpoint).

Ensure the RTU's health check port matches what the controller expects.

### p-net version

The RTU uses p-net v0.2.0. The analysis in this document was performed
against the p-net source in `mwilco03/p-net`, which appears consistent
with v0.2.0 behavior. Core RPC/NDR/block parsing logic is stable across
minor versions.

### Sensor data format

Sensors write 5 bytes per submodule (Float32 BE + quality byte).
Actuators read 4 bytes per submodule (command + duty + reserved).

The cyclic data update happens in `profinet_manager.c` via:
```c
pnet_input_set_data_and_iops()   // Sensor → Controller
pnet_output_get_data_and_iops()  // Controller → Actuator
```

These are called from the periodic handler. Ensure the data sizes passed
to `pnet_plug_submodule()` (input_size / output_size) match exactly what
the cyclic handler writes/reads.

---

## Verification Checklist

### Before first connection attempt

- [x] DAP submodule idents match GSDML (0x00000001, 0x00000100, 0x00000200)
      Verified: gsdml_modules.h constants match GSDML XML lines 95, 116, 130.
      Code at profinet_manager.c:885-919 uses correct constants and subslots.
- [x] Station name is set and NV storage is cleaned on boot
      Verified: clear_pnet_nv_station() at profinet_manager.c:185 deletes all pf_* files.
      bootstrap.sh:277 detect_station_name() generates rtu-XXXX from MAC.
      config_validate.c:32 validate_station_name() enforces IEC 61158-6.
- [x] Network interface has IPv4 address (DHCP or static)
      Verified: configure_pnet_ip() at profinet_manager.c:608 reads from getifaddrs().
- [x] Health check HTTP server is running and reachable
      Verified: main.c:480 health_check_init(), main.c:486 health_check_start().
      Default port 9081 from config_defaults.h:27 WT_HTTP_PORT_DEFAULT.
- [ ] `CAP_NET_RAW` capability is available for p-net
      Requires runtime verification on target hardware.
- [ ] p-net NV directory exists and is writable
      Requires runtime verification on target hardware.

### For Phase 1 (DAP-only connect)

- [x] Database can be empty (only DAP plugged)
      Verified: load_modules_from_db() returns RESULT_OK with count=0.
      profinet_manager.c:930 loop skips when slot_count==0. Only DAP is plugged.
- [ ] RTU responds to DCP Identify with correct station name and vendor/device ID
      Requires live connection test.
- [ ] RTU accepts Connect Request and returns Connect Response (not just error)
      Requires live connection test (blocked by controller DREP fix).

### For Phase 2+ (full connect)

- [x] Database populated with correct module idents
      Verified: db_module_list() returns modules ordered by slot.
      Module idents are integers matching GSDML (all 10 types verified).
- [x] All modules in database match GSDML module definitions
      Verified: All 7 sensor types (0x10-0x70) and 3 actuator types (0x100-0x120)
      have matching entries in gsdml_modules.h and GSDML XML.
- [x] `is_actuator_module()` correctly classifies all module idents
      Verified: (ident & 0x100) != 0 correctly separates sensors (0x10-0x70)
      from actuators (0x100-0x120). profinet_manager.c:157.
- [x] Input/output sizes match GSDML data lengths (5 bytes sensor, 4 bytes actuator)
      Verified: GSDML_SENSOR_INPUT_SIZE=5 (Float32+quality), GSDML_ACTUATOR_OUTPUT_SIZE=4
      (cmd+duty+reserved*2). load_modules_from_db() at profinet_manager.c:392-398.

### For HTTP endpoints

- [x] `GET /slots` returns correct JSON per contract in Section 2.2
- [x] Response uses integer idents (16, not "0x00000010")
- [x] DAP (slot 0) is NOT included in response
- [x] Returns HTTP 200 with `{"slot_count": 0, "slots": []}` when no modules configured
- [x] Returns HTTP 503 when database unavailable
- [x] Direction derived from `(module_ident & 0x100) != 0`
- [x] `GET /gsdml` returns raw XML with `Content-Type: application/xml`
- [x] `GET /gsdml` returns HTTP 404 when GSDML file missing
      Verified: serve_gsdml_file() at health_check.c:660 sends 404 JSON if fopen fails.
- [x] GSDML file exists at `/opt/water-treat/gsd/` (installed) or `gsd/` (dev)
      Verified: gsd/ directory in source tree, bootstrap.sh copies to INSTALL_DIR.
- [x] Port matches controller's expected RTU health port (9081)
      Verified: config_defaults.h:27 WT_HTTP_PORT_DEFAULT=9081. Consistent across
      bootstrap.sh:947, etc/water-treat.conf.example:48, etc/water-treat.env:23.

### GSDML consistency

- [x] VSM_Port1 SubmoduleIdentNumber fixed: was 0x00008000 (subslot number),
      changed to 0x00000200 (matching PortSubmoduleItem ident).
- [x] All 10 module types in GSDML match gsdml_modules.h constants
- [x] PhysicalSlots="0..246", MinDeviceInterval="32" verified
- [x] Record indices 0xF840-0xF845 all implemented in profinet_callbacks.c
      (0xF844 slot map read implemented: profinet_manager_build_slot_map()
      returns BE-packed binary with 2-byte count + 15 bytes per slot)
