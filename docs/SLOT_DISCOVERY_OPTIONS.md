# Slot Discovery Options -- Controller/RTU Interop Analysis

**Principle:** The RTU defines its own module layout. The controller must discover it, not dictate it.

This document evaluates three approaches for the controller to learn the RTU's current slot configuration before (or during) a PROFINET Connect. Each section lists pros, cons, and **specific file changes** in both codebases.

---

## Current State

- RTU plugs modules dynamically from SQLite database at startup (`profinet_manager.c:371-410`)
- Fresh RTU has only DAP (slot 0) + 1 CPU temp sensor (slot 1, runtime-only)
- Controller's brute-force pcap requests 16 application slots -- RTU has at most 2
- p-net v0.2.0 rejects with `"Faulty ARBlockReq" / "Error in Parameter BlockLength"` **before** module matching fires
- GSDML defines module **types** (the menu), not which slots are populated at runtime

### Existing Infrastructure (RTU)

| Component | File | What Exists |
|-----------|------|-------------|
| HTTP server | `health_check.c:596-716` | `/health`, `/metrics`, `/config`, `/ready`, `/live`, LED endpoints |
| Registration API | `rtu_registration.c:234-252` | HTTP POST with `sensor_count`, `actuator_count` (no per-slot detail) |
| PROFINET read cb | `profinet_callbacks.c:271-307` | Handles I&M0 (0x8000) only; default case returns NULL/0 |
| PROFINET write cb | `profinet_callbacks.c:309-440` | Handles 0xF840-0xF845 (user sync, config sync, enrollment) |
| Record indices | `config_sync.h` | 0xF841-0xF843 defined (all write-only, controller->RTU) |
| Module plug loop | `profinet_manager.c:930-961` | Plugs from `g_pn.slots[]` array |
| Slot data struct | `profinet_manager.c:37-51` | `profinet_slot_t` with ident, sizes, direction |
| mDNS discovery | `controller_discovery.c` | `_profinet-controller._tcp` service discovery |

---

## Option 1: DAP-only Connect + Acyclic Record Read

### How It Works

```
Controller                          RTU
    |                                |
    |--- DCP Identify ------------->|  (already works)
    |<-- DCP Identify Response -----|
    |                                |
    |--- Connect(DAP only) -------->|  ExpectedSubmoduleBlockReq = slot 0 only
    |<-- Connect OK + ModuleDiff ---|  AR established
    |                                |
    |--- Record Read(0xF844) ------>|  Acyclic read: "give me your slot map"
    |<-- Record Read Response ------|  Binary slot map returned
    |                                |
    |--- Release ------------------->|  Tear down DAP-only AR
    |<-- Release Response ----------|
    |                                |
    |--- Connect(full layout) ----->|  ExpectedSubmoduleBlockReq = actual slots
    |<-- Connect OK ----------------|  Full AR established
    |                                |
    |=== Cyclic Data Exchange ======|
```

### RTU File Changes

| File | Change | Detail |
|------|--------|--------|
| `src/profinet/profinet_callbacks.c:282-303` | Add `case 0xF844` in `profinet_read_callback()` | Serialize `g_pn.slots[]` into a binary response buffer. Format: 2-byte count + per-slot records (2-byte slot, 2-byte subslot, 4-byte module_ident, 4-byte submodule_ident, 1-byte direction, 2-byte input_size, 2-byte output_size = 15 bytes each). |
| `src/profinet/profinet_manager.c` | Add `profinet_manager_get_slot_map()` public function | Returns pointer to static buffer with serialized slot data. Accesses `g_pn.slots[]` and `g_pn.slot_count` under mutex. |
| `src/profinet/profinet_manager.h` | Add function prototype | `result_t profinet_manager_get_slot_map(uint8_t *buf, uint16_t *len);` |
| `src/profinet/config_sync.h` | Define `0xF844` record index and struct | `#define RTU_SLOT_MAP_PROFINET_INDEX 0xF844` plus `slot_map_entry_t` packed struct. |

**Estimated RTU diff:** ~80 lines across 4 files.

### Controller File Changes

| File | Change | Detail |
|------|--------|--------|
| Connection manager | Two-phase connect logic | Phase 1: Connect with DAP-only ExpectedSubmoduleBlockReq. Phase 2: Record Read 0xF844, parse, reconnect with full layout. |
| Record read handler | Parse 0xF844 response | Deserialize binary slot map into internal module table. |
| ExpectedSubmodule builder | Dynamic construction | Build ExpectedSubmoduleBlockReq from parsed slot map instead of hardcoded 16-slot layout. |
| Fallback logic | Handle record read failure | If 0xF844 read fails or returns 0 modules, fall back to Option 2 or 3. |

### Pros

1. **Pure PROFINET** -- no dependency on HTTP or any other protocol
2. Works on **isolated L2 networks** without IP routing (PROFINET is Ethernet-level)
3. **Standard mechanism** -- acyclic Record Read is a first-class PROFINET operation
4. Single communication path -- everything through the AR
5. GSDML already provides module type definitions the controller imports

### Cons

1. **Two connect/disconnect cycles** add startup latency
2. **p-net v0.2.0 stability risk** -- we've seen p-net go silent after ~9 rapid Connect attempts in the pcap. A disconnect+reconnect cycle may trigger the same stuck state.
3. **DAP-only AR may timeout** -- p-net starts a watchdog when AR is established. If the Record Read doesn't complete fast enough, the AR could be torn down internally.
4. **More complex controller state machine** -- must handle partial-AR (DAP-only) vs full-AR as distinct states
5. p-net v0.2.0 doesn't have a documented `pnet_close()` -- clean disconnect between phase 1 and phase 2 relies on the controller sending a proper Release, and p-net handling it cleanly
6. **The current "Faulty ARBlockReq" error must be fixed first** -- if p-net rejects at the RPC level due to DREP/encoding issues, even DAP-only Connect will fail

### Risk Assessment

The core risk is p-net v0.2.0's behavior under rapid connect/disconnect. The 228-packet pcap shows p-net stops responding after 9 connections. A two-phase approach adds one more disconnect/reconnect cycle per RTU startup. If p-net's AR state machine doesn't clean up between phases, the RTU needs a full stack restart.

---

## Option 2: HTTP/API Query Before Connect

### How It Works

```
Controller                          RTU
    |                                |
    |--- DCP Identify ------------->|  (already works, gives RTU IP)
    |<-- DCP Identify Response -----|
    |                                |
    |--- HTTP GET /api/v1/slots --->|  JSON query over TCP
    |<-- 200 OK + JSON slot map ----|
    |                                |
    |--- Connect(full layout) ----->|  ExpectedSubmoduleBlockReq = actual slots
    |<-- Connect OK ----------------|  AR established first try
    |                                |
    |=== Cyclic Data Exchange ======|
```

### RTU File Changes

| File | Change | Detail |
|------|--------|--------|
| `src/health/health_check.c:684-698` | Add `/api/v1/slots` route in `handle_http_request()` | New `else if` branch that calls `slots_to_json()` and returns the result. Insert before the 404 handler. Also update the 404 endpoint list. |
| `src/health/health_check.c` (new function) | Add `slots_to_json()` | Builds JSON: `{"station_name":"rtu-XXXX","slot_count":N,"slots":[{"slot":1,"subslot":1,"module_ident":"0x00000040","submodule_ident":"0x00000041","direction":"input","input_size":5,"output_size":0}, ...]}`. Reads from `g_pn.slots[]` via a manager accessor. |
| `src/profinet/profinet_manager.c` | Add `profinet_manager_get_slots()` accessor | Public function returning `const profinet_slot_t*` array and count. Provides read-only access to `g_pn.slots[]` under mutex. |
| `src/profinet/profinet_manager.h` | Add accessor prototype and public slot struct | `typedef struct { int slot; int subslot; uint32_t module_ident; uint32_t submodule_ident; size_t input_size; size_t output_size; } profinet_slot_info_t;` + `int profinet_manager_get_slots(profinet_slot_info_t *out, int max);` |

**Optional enhancement:**

| File | Change | Detail |
|------|--------|--------|
| `src/profinet/rtu_registration.c:234-252` | Extend `build_registration_json()` | Add `"slots"` array to registration POST payload so controller gets slot info at registration time, not just counts. |

**Estimated RTU diff:** ~100 lines across 3-4 files.

### Controller File Changes

| File | Change | Detail |
|------|--------|--------|
| RTU discovery/connect | HTTP GET after DCP | After DCP gives RTU IP, issue `GET http://{rtu_ip}:{port}/api/v1/slots` before calling Connect. |
| JSON parser | Parse slot array | Extract slot entries from JSON response. Map `module_ident` values to GSDML module types. |
| ExpectedSubmodule builder | Dynamic construction | Build ExpectedSubmoduleBlockReq from parsed JSON slots. |
| Registration handler | Parse extended registration | If RTU sends slots in registration POST, cache them -- skip separate GET. |
| Fallback | Handle HTTP failure | If HTTP unreachable, fall back to Option 1 or 3. |

### JSON Response Format

```json
{
  "station_name": "rtu-4b64",
  "gsdml_version": "V2.4",
  "slot_count": 2,
  "slots": [
    {
      "slot": 0,
      "subslot": 1,
      "module_ident": "0x00000001",
      "submodule_ident": "0x00000001",
      "type": "DAP",
      "direction": "none",
      "input_size": 0,
      "output_size": 0
    },
    {
      "slot": 1,
      "subslot": 1,
      "module_ident": "0x00000040",
      "submodule_ident": "0x00000041",
      "type": "Temperature",
      "direction": "input",
      "input_size": 5,
      "output_size": 0
    }
  ]
}
```

### Pros

1. **Single Connect attempt** -- no disconnect/reconnect cycle, no p-net stability risk
2. **Human-readable** -- JSON payload is easy to debug with curl, browser, or any HTTP tool
3. **RTU already has an HTTP server** -- `health_check.c` has a working socket server with routing (just add one endpoint)
4. **Registration API already sends counts** -- `rtu_registration.c:244-245` already sends `sensor_count` and `actuator_count`, extending to per-slot detail is natural
5. **DCP gives RTU's IP** -- controller already knows where to HTTP GET after DCP Identify
6. **Fast** -- HTTP roundtrip is typically <100ms on a local network
7. **Doesn't stress p-net** -- slot query happens outside the PROFINET stack entirely
8. **Debugging** -- `curl http://rtu-ip:8080/api/v1/slots` from any laptop on the network

### Cons

1. **Second protocol dependency** -- HTTP alongside PROFINET means two things that must work
2. **HTTP server must be running** -- if health_check HTTP thread hasn't started yet (startup ordering), no slot data available
3. **Port/firewall** -- health check HTTP port (configurable, default varies) must be reachable from controller
4. **Not pure PROFINET** -- some industrial environments restrict IP traffic on PROFINET networks
5. **Race condition** -- RTU HTTP server may start before PROFINET modules are loaded from database (slots may be incomplete). Need to ensure `/api/v1/slots` is only served after `profinet_manager_start()` completes.
6. **If HTTP is down but PROFINET L2 is up** -- slot discovery fails. Need fallback.

### Risk Assessment

Low risk. The HTTP server in health_check.c is battle-tested (already serves `/health`, `/config`, etc.). The main concern is startup ordering: ensure modules are loaded from DB before the endpoint becomes available. A simple "not ready" 503 response until `profinet_manager_start()` completes handles this.

---

## Option 3: ModuleDiffBlock Tolerance

### How It Works

```
Controller                          RTU
    |                                |
    |--- DCP Identify ------------->|  (already works)
    |<-- DCP Identify Response -----|
    |                                |
    |--- Connect(any layout) ------>|  Controller sends its best guess
    |<-- Connect OK + ModuleDiff ---|  p-net reports actual vs expected diff
    |                                |
    |   Controller parses diff,      |
    |   adapts IO map accordingly    |
    |                                |
    |=== Cyclic Data Exchange ======|  Only for modules that actually exist
```

The PROFINET specification (IEC 61158-6) defines **ModuleDiffBlock** as part of the Connect Response. When the controller's ExpectedSubmoduleBlockReq doesn't match the device's actually-plugged modules, the device includes a ModuleDiffBlock listing:
- Modules present in device but not expected by controller
- Modules expected by controller but not present in device
- Modules with different properties (wrong submodule, wrong data size)

The controller then adjusts its runtime IO map to only exchange cyclic data with modules that actually exist.

### RTU File Changes

| File | Change | Detail |
|------|--------|--------|
| `src/profinet/profinet_callbacks.c:446-469` | **Potentially none** | `exp_module_callback` and `exp_submodule_callback` already return 0 (accept). p-net v0.2.0 generates ModuleDiffBlock internally when plugged modules don't match expected modules. The callbacks don't need to change for this to work. |
| `src/profinet/profinet_manager.c` | **No changes** | Modules are already plugged correctly from the database. p-net handles the diff calculation. |

**Critical caveat:** The current failure (`"Faulty ARBlockReq" / "Error in Parameter BlockLength"`) happens **before p-net reaches the module matching stage**. The rejection is at the RPC/ARBlockReq parsing level. ModuleDiffBlock is generated **after** ARBlockReq is accepted and module comparison runs. So **this option alone cannot fix the current connection failure.** The DREP/encoding/BlockLength issues in the controller's Connect Request must be fixed first.

If the RPC-level issue is resolved, p-net should:
1. Accept the ARBlockReq
2. Compare ExpectedSubmoduleBlockReq against plugged modules
3. Generate ModuleDiffBlock in the response
4. Establish AR with only matching modules active

**Estimated RTU diff:** 0 lines (p-net handles it). Possibly add logging in callbacks to confirm diff behavior.

### Controller File Changes

| File | Change | Detail |
|------|--------|--------|
| Connect response parser | Parse ModuleDiffBlock | After receiving Connect Response, look for ModuleDiffBlock (BlockType=0x7FFE or similar per spec). Parse each `ModuleDiffBlockEntry` to understand which slots matched, which are substitute, which are wrong. |
| IO map builder | Dynamic IO map | Build cyclic data exchange map from ModuleDiffBlock rather than from the original ExpectedSubmoduleBlockReq. Slots marked "no module" or "wrong module" get zero IO data. |
| Runtime adapter | Handle substitute data | For slots where RTU has no module but controller expected one, handle substitute/zero data gracefully. Don't treat missing IO as a fatal error. |
| Error handling | Don't reject on diff | Current behavior likely treats any non-zero diff as connection failure. Must change to: parse diff, adapt, continue. |

### Pros

1. **Standard PROFINET mechanism** -- defined in IEC 61158-6, implemented by all certified devices
2. **Single Connect attempt** -- no extra round-trips
3. **No extra protocols** -- pure PROFINET, no HTTP dependency
4. **Handles dynamic changes** -- if RTU adds/removes a sensor, next Connect automatically gets the right diff
5. **All real controllers implement this** -- Siemens S7-1500, Beckhoff, etc. all parse ModuleDiffBlock
6. **Future-proof** -- any configuration mismatch is handled gracefully, not just the current one
7. **Should be implemented regardless** -- makes the system robust to any configuration drift

### Cons

1. **Cannot fix the current failure alone** -- p-net rejects at ARBlockReq BEFORE reaching module matching. The DREP/encoding/BlockLength issues in the Connect Request must be resolved first.
2. **Controller must implement ModuleDiffBlock parser** -- this is a non-trivial PROFINET structure with nested entries and multiple diff types
3. **p-net v0.2.0 behavior uncertain** -- needs testing to confirm it actually generates ModuleDiffBlock correctly (vs just rejecting)
4. **Controller runtime IO map becomes dynamic** -- can't pre-allocate fixed buffers for known slots; must adapt based on diff
5. **Substitute data handling** -- controller must handle "this slot is empty" gracefully in its cyclic processing loop
6. **Debugging is harder** -- ModuleDiffBlock is a binary PROFINET structure, not human-readable like JSON

### Risk Assessment

High value but **blocked by the ARBlockReq-level rejection**. Once the DREP/encoding issues are fixed, this should "just work" on the RTU side since p-net handles it internally. The controller side requires significant work to parse and adapt to ModuleDiffBlock.

---

## Side-by-Side Comparison

| Factor | Option 1 (DAP+Read) | Option 2 (HTTP) | Option 3 (ModuleDiff) |
|--------|---------------------|-----------------|----------------------|
| **Connect attempts** | 2 (DAP, then full) | 1 | 1 |
| **Extra protocols** | None (pure PROFINET) | HTTP | None (pure PROFINET) |
| **RTU code changes** | ~80 lines / 4 files | ~100 lines / 3-4 files | ~0 lines |
| **Controller complexity** | Medium (two-phase) | Low (HTTP GET + JSON) | High (ModuleDiffBlock parser) |
| **Works with current bug** | No (needs DREP fix) | **Yes** (bypasses p-net entirely) | No (needs DREP fix) |
| **p-net stability risk** | High (rapid reconnect) | None | None |
| **Debuggability** | Medium (binary record) | **High** (curl/browser) | Low (binary PROFINET) |
| **Startup latency** | Higher (2 connects) | **Low** (~100ms HTTP) | Low (1 connect) |
| **Works on isolated L2** | **Yes** | Only with IP | **Yes** |
| **PROFINET spec compliant** | Yes (acyclic read) | N/A (out-of-band) | **Yes** (IEC 61158-6) |
| **Handles future changes** | Per-connect | Per-connect | **Automatic** |
| **Production readiness** | Needs testing | **Ready infrastructure** | Needs DREP fix first |

---

## Recommendation: Layered Approach

Based on the analysis, the options aren't mutually exclusive. They serve different purposes:

### Implement Now: Option 2 (HTTP/API)

**Rationale:** Works today, even with the DREP bug unfixed. The HTTP server exists, the endpoint is ~60 lines of new code, and the controller just needs a GET before Connect. This unblocks controller development immediately.

Specific immediate files:
- **RTU:** `health_check.c` (add route + JSON builder), `profinet_manager.h/.c` (add slot accessor)
- **Controller:** HTTP client after DCP, JSON parse, dynamic ExpectedSubmoduleBlockReq builder

### Implement Next: Option 3 (ModuleDiff)

**Rationale:** Should exist regardless. Once the DREP/encoding issue is fixed, ModuleDiff tolerance makes the system resilient to any configuration mismatch. Zero RTU code changes -- all work is controller-side parsing.

Specific files:
- **RTU:** None (p-net handles it)
- **Controller:** ModuleDiffBlock parser, dynamic IO map

### Implement If Needed: Option 1 (DAP+Read)

**Rationale:** Only necessary if HTTP isn't available (isolated L2 network, HTTP server disabled). Adds the 0xF844 vendor-specific record. Could serve as a fallback for Option 2.

Specific files:
- **RTU:** `profinet_callbacks.c` (read handler), `config_sync.h` (record def), `profinet_manager.c/.h` (slot map serializer)
- **Controller:** Two-phase connect logic

### Connection Flow with All Three

```
Controller                          RTU
    |                                |
    |--- DCP Identify ------------->|
    |<-- DCP Response (IP=x.x.x.x)-|
    |                                |
    |--- HTTP GET /api/v1/slots --->|  Option 2 (primary)
    |<-- 200 OK + JSON             -|
    |                                |
    |   Build ExpectedSubmoduleBlockReq from JSON
    |                                |
    |--- Connect(correct layout) -->|  Single attempt with correct slots
    |<-- Connect OK + ModuleDiff ---|  Option 3 (safety net)
    |                                |
    |   Verify: diff should be empty |
    |   If not: adapt IO map         |
    |                                |
    |=== Cyclic Data Exchange ======|
```

If HTTP fails:
```
    |--- HTTP GET /api/v1/slots --->|  TIMEOUT / connection refused
    |                                |
    |--- Connect(DAP only) -------->|  Option 1 (fallback)
    |<-- Connect OK + ModuleDiff ---|
    |--- Record Read(0xF844) ------>|
    |<-- Slot map ------------------|
    |--- Release ------------------->|
    |--- Connect(full layout) ----->|
    |<-- Connect OK ----------------|
```

---

## Appendix: Key File Quick Reference

### RTU Codebase

| File | Lines | Purpose |
|------|-------|---------|
| `src/profinet/profinet_manager.c` | 1549 | p-net lifecycle, slot management, module plugging |
| `src/profinet/profinet_callbacks.c` | 697 | All p-net callbacks (read/write/connect/module) |
| `src/profinet/config_sync.h` | 184 | Record index definitions (0xF841-0xF843) |
| `src/profinet/rtu_registration.c` | ~300 | HTTP registration + enrollment processing |
| `src/profinet/rtu_registration.h` | 304 | Registration types, token handling, API |
| `src/health/health_check.c` | ~800 | HTTP server, health endpoints, config export |
| `include/gsdml_modules.h` | 202 | Module ident constants and mapping functions |
| `gsd/GSDML-V2.4-WaterTreat-RTU-20241222.xml` | 557 | Device description (module type menu) |

### Record Index Allocation

| Index | Direction | Purpose | Status |
|-------|-----------|---------|--------|
| 0x8000 | Read | I&M0 (standard) | Implemented |
| 0xF840 | Write | User sync (credentials) | Implemented |
| 0xF841 | Write | Device config | Implemented |
| 0xF842 | Write | Sensor config | Implemented |
| 0xF843 | Write | Actuator config | Implemented |
| 0xF844 | **Read** | **Slot map (proposed)** | **Option 1** |
| 0xF845 | Write | Enrollment/binding | Implemented |
