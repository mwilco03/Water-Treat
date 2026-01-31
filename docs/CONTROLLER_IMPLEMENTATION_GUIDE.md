# Controller Implementation Guide -- RTU Slot Discovery & Connection

This document is a specification for the **controller team**. It defines the RTU's API contract and the connection pattern the controller must implement to establish a PROFINET AR with dynamic slot configurations.

**Principle:** The RTU owns its module layout. The controller discovers it, never dictates it.

---

## Phase 1: HTTP Slot Discovery (Implement First)

### Overview

After DCP identifies an RTU and returns its IP address, the controller issues an HTTP GET to learn the RTU's current slot configuration, then builds the PROFINET Connect Request to match.

```
Controller                              RTU (port 9081)
    |                                      |
    |--- DCP Identify Request ----------->|
    |<-- DCP Identify Response (IP) ------|
    |                                      |
    |--- GET /api/v1/gsdml -------------->|  (once, cache locally)
    |<-- 200 OK + XML --------------------|
    |                                      |
    |--- GET /api/v1/slots -------------->|  (every connect)
    |<-- 200 OK + JSON -------------------|
    |                                      |
    |   Build ExpectedSubmoduleBlockReq    |
    |   from slot JSON                     |
    |                                      |
    |--- PROFINET Connect (correct) ----->|
    |<-- Connect Response (OK) -----------|
    |                                      |
    |=== Cyclic IO Data Exchange =========|
```

### RTU HTTP Endpoints

The RTU runs an HTTP server on port **9081** (configurable via `[health] http_port` in `/etc/water-treat.conf`). All responses are `Content-Type: application/json` unless noted.

#### `GET /api/v1/slots`

Returns the RTU's current PROFINET slot configuration.

**Response (200 OK):**

```json
{
  "station_name": "rtu-4b64",
  "vendor_id": "0x0493",
  "device_id": "0x0001",
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

**Field Reference:**

| Field | Type | Description |
|-------|------|-------------|
| `station_name` | string | PROFINET station name (matches DCP response) |
| `vendor_id` | hex string | PROFINET vendor ID (`"0x0493"`) |
| `device_id` | hex string | PROFINET device ID (`"0x0001"`) |
| `gsdml_version` | string | GSDML schema version (`"V2.4"`) |
| `slot_count` | integer | Number of slots including DAP |
| `slots[]` | array | Ordered slot entries |
| `slots[].slot` | integer | Slot number (0 = DAP, 1-246 = application) |
| `slots[].subslot` | integer | Subslot number (typically 1 for data submodule) |
| `slots[].module_ident` | hex string | Module identifier per GSDML (see table below) |
| `slots[].submodule_ident` | hex string | Submodule identifier (module_ident + 1 for data) |
| `slots[].type` | string | Human-readable module type name |
| `slots[].direction` | string | `"input"` (sensor), `"output"` (actuator), or `"none"` (DAP) |
| `slots[].input_size` | integer | Input data length in bytes (device -> controller) |
| `slots[].output_size` | integer | Output data length in bytes (controller -> device) |

**Error Responses:**

| Code | Meaning | Controller Action |
|------|---------|-------------------|
| 503 | RTU not ready (PROFINET stack still initializing) | Retry after 1-2 seconds |
| Connection refused | HTTP server not running | Fall back to Option 1 (DAP-only connect) or retry |

#### `GET /api/v1/gsdml`

Returns the raw GSDML XML file. Cache this -- it only changes on firmware update.

**Response (200 OK):**

```
Content-Type: application/xml
Content-Length: 32083

<?xml version="1.0" encoding="utf-8"?>
<!-- GSDML Device Description File ... -->
<ISO15745Profile xmlns="http://www.profibus.com/GSDML/2003/11/DeviceProfile" ...>
  ...
</ISO15745Profile>
```

The GSDML defines the **module type menu** -- all module types the RTU supports. The `/api/v1/slots` endpoint tells you which of those types are **actually plugged** right now.

#### Existing Endpoints (Reference)

| Endpoint | Purpose |
|----------|---------|
| `GET /health` | System health status JSON |
| `GET /config` | Full RTU configuration JSON |
| `GET /ready` | Readiness probe (`{"ready": true/false}`) |
| `GET /metrics` | Prometheus-format metrics |

---

### Module Identifier Reference

These values are defined in the shared GSDML and in `gsdml_modules.h`.

#### Sensor Input Modules

| Type | module_ident | submodule_ident | Input Size | Format |
|------|-------------|-----------------|------------|--------|
| pH Sensor | `0x00000010` | `0x00000011` | 5 bytes | Float32 BE + Quality |
| TDS Sensor | `0x00000020` | `0x00000021` | 5 bytes | Float32 BE + Quality |
| Turbidity Sensor | `0x00000030` | `0x00000031` | 5 bytes | Float32 BE + Quality |
| Temperature Sensor | `0x00000040` | `0x00000041` | 5 bytes | Float32 BE + Quality |
| Flow Sensor | `0x00000050` | `0x00000051` | 5 bytes | Float32 BE + Quality |
| Level Sensor | `0x00000060` | `0x00000061` | 5 bytes | Float32 BE + Quality |
| Generic Analog Input | `0x00000070` | `0x00000071` | 5 bytes | Float32 BE + Quality |

**Sensor data format (5 bytes):**
```
Offset 0-3: IEEE 754 Float32, big-endian (sensor value)
Offset 4:   Quality byte
              0x00 = Good
              0x40 = Uncertain
              0x80 = Bad
              0xC0 = Not Connected
```

#### Actuator Output Modules

| Type | module_ident | submodule_ident | Output Size | Format |
|------|-------------|-----------------|-------------|--------|
| Pump Control | `0x00000100` | `0x00000101` | 4 bytes | Cmd + Duty + Reserved |
| Valve Control | `0x00000110` | `0x00000111` | 4 bytes | Cmd + Duty + Reserved |
| Generic Digital Output | `0x00000120` | `0x00000121` | 4 bytes | Cmd + Duty + Reserved |

**Actuator data format (4 bytes):**
```
Offset 0: Command byte (0=OFF, 1=ON, 2=AUTO)
Offset 1: Duty cycle (0-100, for PWM-capable actuators)
Offset 2-3: Reserved (write 0x0000)
```

#### DAP (Always Present)

| Type | module_ident | submodule_ident | Slot | Subslot |
|------|-------------|-----------------|------|---------|
| DAP | `0x00000001` | `0x00000001` | 0 | 1 |
| DAP Interface | `0x00000001` | `0x00000100` | 0 | 0x8000 |
| DAP Port | `0x00000001` | `0x00000200` | 0 | 0x8001 |

DAP is always at slot 0. Every RTU has it. Controller MUST include slot 0 in ExpectedSubmoduleBlockReq.

---

### Controller Implementation Steps

#### Step 1: After DCP Identify

DCP gives you the RTU's IP address. You already have this working.

#### Step 2: HTTP GET /api/v1/slots

```python
# Pseudocode
rtu_ip = dcp_response.ip_address
rtu_port = 9081  # default, could be discovered

response = http_get(f"http://{rtu_ip}:{rtu_port}/api/v1/slots")

if response.status == 503:
    # RTU still starting up, retry
    sleep(2)
    retry()
elif response.status != 200:
    # HTTP failed, fall back to DAP-only connect
    use_dap_only_fallback()
else:
    slot_config = json_parse(response.body)
```

#### Step 3: Build ExpectedSubmoduleBlockReq

For each slot in the JSON response, create an `ExpectedSubmoduleBlockReq` entry:

```python
# Pseudocode
expected_submodules = []

for slot_entry in slot_config["slots"]:
    entry = ExpectedSubmoduleEntry(
        slot_number = slot_entry["slot"],
        subslot_number = slot_entry["subslot"],
        module_ident = parse_hex(slot_entry["module_ident"]),
        submodule_ident = parse_hex(slot_entry["submodule_ident"]),
    )

    if slot_entry["direction"] == "input":
        entry.data_description = InputDataDescription(
            length = slot_entry["input_size"],
            iops_length = 1,
            iocs_length = 1,
        )
    elif slot_entry["direction"] == "output":
        entry.data_description = OutputDataDescription(
            length = slot_entry["output_size"],
            iops_length = 1,
            iocs_length = 1,
        )

    expected_submodules.append(entry)
```

#### Step 4: Build IOCRBlockReq

The `IOCRBlockReq` (IO Communication Relationship) must list all the IO data items. Calculate from the slot JSON:

```python
total_input_length = sum(s["input_size"] for s in slots if s["direction"] == "input")
total_output_length = sum(s["output_size"] for s in slots if s["direction"] == "output")

# Add IOPS/IOCS bytes (1 per submodule per direction)
input_submodule_count = len([s for s in slots if s["direction"] == "input"])
output_submodule_count = len([s for s in slots if s["direction"] == "output"])
```

#### Step 5: Connect

Send the PROFINET Connect Request with the dynamically-built ExpectedSubmoduleBlockReq. The slot layout now matches exactly what the RTU has plugged.

**Important DREP note:** The RTU (p-net v0.2.0) expects response encoding per the DREP field. Ensure your Connect Request encodes fields consistent with the declared DREP byte order. If DREP=0x10 (little-endian), all multi-byte fields in the stub must actually be little-endian.

---

### Controller Implementation Checklist

- [ ] After DCP: Extract RTU IP from DCP Identify Response
- [ ] HTTP client: `GET http://{rtu_ip}:9081/api/v1/slots`
- [ ] Handle 503 (retry) and connection-refused (fallback)
- [ ] JSON parser: Extract `slots` array
- [ ] Map `module_ident` hex strings to uint32 values
- [ ] Build `ExpectedSubmoduleBlockReq` from slot array
- [ ] Build `IOCRBlockReq` with correct total IO lengths
- [ ] Build `ARBlockReq` with correct parameters
- [ ] Send Connect Request
- [ ] Verify: Connect Response should have no errors

---

## Phase 2: ModuleDiffBlock Tolerance (Implement for Robustness)

Even with HTTP-based slot discovery, the controller should handle `ModuleDiffBlock` in the Connect Response gracefully. This handles edge cases where:

- RTU modules changed between the HTTP query and the Connect Request
- HTTP returned stale data (race condition)
- Manual override scenarios

### What p-net Sends

If the controller's `ExpectedSubmoduleBlockReq` doesn't exactly match the RTU's plugged modules, p-net includes a `ModuleDiffBlock` in the Connect Response. This is standard PROFINET behavior per IEC 61158-6.

The diff block lists:
- Slots where the expected module doesn't match the plugged module
- Slots where no module is plugged (substitute)
- Submodules with wrong properties

### Controller Implementation

```python
# Pseudocode
connect_response = send_connect_request(expected_submodules)

if connect_response.has_module_diff_block():
    diff = connect_response.module_diff_block

    for diff_entry in diff.entries:
        if diff_entry.state == MODULE_STATE_SUBSTITUTE:
            # RTU has no module in this slot
            # Remove from cyclic IO map, don't send/expect data
            io_map.disable_slot(diff_entry.slot)

        elif diff_entry.state == MODULE_STATE_WRONG:
            # Different module than expected
            # Adapt IO map to actual module's data size
            io_map.update_slot(diff_entry.slot, diff_entry.actual_module)

    # Continue with adapted IO map
    start_cyclic_exchange(io_map)
else:
    # Perfect match, proceed normally
    start_cyclic_exchange(expected_io_map)
```

### Controller Checklist

- [ ] Parse `ModuleDiffBlock` (BlockType per IEC 61158-6) from Connect Response
- [ ] Handle `MODULE_STATE_SUBSTITUTE` (slot empty on RTU)
- [ ] Handle `MODULE_STATE_WRONG` (different module than expected)
- [ ] Adapt cyclic IO map at runtime based on diff
- [ ] Do NOT treat ModuleDiffBlock as a connection error
- [ ] Log diffs for debugging

---

## Phase 3: DAP-only Fallback (Implement If Needed)

If HTTP is unavailable (server down, isolated L2 network, firewall), the controller can discover slots via PROFINET acyclic read.

### Flow

```
Controller                              RTU
    |                                      |
    |--- Connect(DAP only) -------------->|  Slot 0 only in ExpectedSubmoduleBlockReq
    |<-- Connect OK + ModuleDiff ---------|  AR established (DAP-only)
    |                                      |
    |--- Record Read(0xF844, slot 0) ---->|  Acyclic read: slot map
    |<-- Record Read Response ------------|  Binary slot map
    |                                      |
    |--- Release ------------------------->|  Tear down DAP-only AR
    |<-- Release OK ----------------------|
    |                                      |
    |--- Connect(full layout) ----------->|  Correct ExpectedSubmoduleBlockReq
    |<-- Connect OK ----------------------|
    |                                      |
    |=== Cyclic IO Data Exchange =========|
```

### DAP-only ExpectedSubmoduleBlockReq

For the phase-1 connect, include ONLY slot 0:

| Slot | Subslot | Module Ident | Submodule Ident |
|------|---------|-------------|-----------------|
| 0 | 0x0001 | 0x00000001 | 0x00000001 |
| 0 | 0x8000 | 0x00000001 | 0x00000100 |
| 0 | 0x8001 | 0x00000001 | 0x00000200 |

### Record Read 0xF844 Response Format

Binary packed, big-endian:

```
Offset 0-1:  slot_count (uint16, number of application slots)

Per slot (15 bytes each, repeated slot_count times):
  Offset 0-1:   slot_number (uint16)
  Offset 2-3:   subslot_number (uint16)
  Offset 4-7:   module_ident (uint32)
  Offset 8-11:  submodule_ident (uint32)
  Offset 12:    direction (uint8: 0=none, 1=input, 2=output)
  Offset 13-14: data_size (uint16: input_size for sensors, output_size for actuators)
```

**Example (1 temperature sensor):**
```
00 01                         -- slot_count = 1
00 01 00 01                   -- slot 1, subslot 1
00 00 00 40 00 00 00 41       -- module 0x40, submodule 0x41
01                            -- direction = input
00 05                         -- data_size = 5 bytes
```

### Controller Checklist

- [ ] Build DAP-only ExpectedSubmoduleBlockReq (3 submodules at slot 0)
- [ ] Send Connect, handle ModuleDiffBlock (non-fatal)
- [ ] Issue `pnet_record_read()` or equivalent with API=0, slot=0, subslot=1, index=0xF844
- [ ] Parse binary slot map (BE, 2-byte count + 15-byte entries)
- [ ] Send Release to tear down DAP-only AR
- [ ] Wait for clean disconnect (p-net needs time to reset AR state)
- [ ] Build full ExpectedSubmoduleBlockReq from parsed data
- [ ] Send second Connect with correct layout

### Risk Note

p-net v0.2.0 has shown instability under rapid connect/disconnect (goes silent after ~9 attempts in testing). Insert a delay (1-2 seconds) between Release and the second Connect to allow AR state cleanup.

---

## Complete Fallback Chain

```python
# Pseudocode - Controller connection logic

def connect_to_rtu(rtu_ip):
    # Phase 1: Try HTTP slot discovery
    slots = http_get_slots(rtu_ip, port=9081)

    if slots is None:
        # Phase 3 fallback: DAP-only connect + record read
        slots = dap_only_discover(rtu_ip)

    if slots is None:
        # Last resort: connect with GSDML defaults, rely on ModuleDiff
        slots = gsdml_default_slots()

    # Build and send Connect
    expected = build_expected_submodule_block(slots)
    response = profinet_connect(rtu_ip, expected)

    # Phase 2: Handle ModuleDiffBlock regardless of discovery method
    if response.has_module_diff():
        adapt_io_map(response.module_diff_block)

    return response
```

---

## PROFINET Record Index Allocation

These indices are allocated in the RTU codebase. The controller uses them for acyclic read/write after AR is established.

| Index | Direction | Purpose | Packet Size |
|-------|-----------|---------|-------------|
| 0x8000 | Read | I&M0 (Identification & Maintenance) | Standard |
| 0xF840 | Write | User sync (DJB2 hashed credentials) | Variable |
| 0xF841 | Write | Device configuration | 52 bytes |
| 0xF842 | Write | Sensor configuration | Variable |
| 0xF843 | Write | Actuator configuration | Variable |
| **0xF844** | **Read** | **Slot map (Phase 3)** | **2 + 15*N bytes** |
| 0xF845 | Write | Enrollment/binding | 80 bytes |

---

## Known Issues to Fix Before Connect Will Succeed

1. **DREP byte-order mismatch**: Controller Connect Requests declare DREP=0x10 (little-endian) but encode multi-byte fields in big-endian. p-net reads DREP and interprets accordingly, causing BlockLength parse failures. **Fix: encode consistent with declared DREP, or declare DREP=0x00 (big-endian) and encode BE.**

2. **Block count**: With HTTP slot discovery, the controller should generate exactly the right number of blocks. No more 16-slot hardcoded layout.

3. **Rapid reconnect**: If using Phase 3 (DAP-only fallback), add a delay between Release and second Connect. p-net v0.2.0's AR state machine needs time to reset.
