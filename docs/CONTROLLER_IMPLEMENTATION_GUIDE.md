# Controller PROFINET Connection Implementation

You are implementing the PROFINET IO Controller that connects to Water Treatment RTU devices. This document is your complete specification. Follow it exactly.

## Your Goal

Establish a PROFINET IO connection to an RTU whose slot configuration is **dynamic and unknown at compile time**. The RTU decides what modules it has. You must discover them before connecting.

## Network Facts

- RTU is a PROFINET IO Device running p-net v0.2.0 (rt-labs, unmodified)
- RTU Vendor ID: `0x0493`, Device ID: `0x0001`
- RTU station name format: `rtu-XXXX` where XXXX = last 4 hex chars of MAC (lowercase)
- RTU HTTP API port: **9081** (configurable, assume 9081 unless told otherwise)
- GSDML file: `GSDML-V2.4-WaterTreat-RTU-20241222.xml` (available via HTTP, see below)
- PROFINET conformance class: B
- Supported RT class: RT_CLASS_1
- SendClock: 32, ReductionRatio: 1,2,4,8,16,32,64,128,256,512
- MinDeviceInterval: 32 (1ms at 31.25us base)
- API number: 0 (always)

## Discovery Priority (Slot Configuration)

You must discover the RTU's application modules before building a Connect Request. Try these sources in order. Use the first one that succeeds.

```
1. GSDML file      -- parse locally if you have the file (standard PROFINET)
2. Cached config   -- from a previous successful connection to this station_name
3. HTTP query      -- GET http://{rtu_ip}:9081/slots (non-standard fallback)
4. DAP-only + Read -- PROFINET Record Read 0xF844 after DAP-only connect
```

**DAP (slot 0) is always known.** It is defined in the GSDML. You never need to discover it. All discovery methods return only application modules (slots 1-246).

## Connection State Machine

Implement this exact state machine. Every transition is described.

```
[IDLE]
  |
  v
[DCP_DISCOVERY] ----------> send DCP Identify multicast
  |
  | DCP Identify Response received (got RTU IP + station_name)
  v
[RESOLVE_SLOTS] ----------> try sources 1-4 in order:
  |                           1. local GSDML parse
  |                           2. cached config for this station_name
  |                           3. HTTP GET /slots
  |                           4. DAP-only connect + Record Read 0xF844
  |
  | Got application slot list (may be empty)
  v
[BUILD_CONNECT] ----------> construct Connect Request from:
  |                           - DAP (always, from GSDML)
  |                           - application slots (from discovery)
  v
[CONNECT] -----------------> Send PROFINET Connect Request
  |
  | Connect Response received
  v
[PARSE_RESPONSE] ----------> Check for errors and ModuleDiffBlock
  |                         |
  | No errors               | ModuleDiffBlock present
  v                         v
[CONNECTED]           [ADAPT_IO_MAP] -> adjust cyclic map
  |                         |
  v                         v
[CYCLIC_EXCHANGE] <---------+
```

---

## Step 1: DCP Discovery

You already have this working. After DCP Identify Response, you have:
- `rtu_ip`: IPv4 address of the RTU (e.g., `192.168.6.14`)
- `station_name`: e.g., `rtu-4b64`
- `vendor_id`: `0x0493`
- `device_id`: `0x0001`

Proceed to Step 2.

---

## Step 2: Slot Discovery

Try each source in order. Use the first one that returns a valid slot list.

### Source 1: Local GSDML Parse (Standard)

If you have the GSDML file (`GSDML-V2.4-WaterTreat-RTU-20241222.xml`), parse the `UseableModules` section (lines 78-92) to know what module types exist. However, the GSDML defines the **menu of possible types**, not which slots are populated. For a fresh/unconfigured RTU, use this combined with cached knowledge or skip to source 3.

### Source 2: Cached Config

If you have previously connected to this `station_name` and stored its slot configuration, use that. This avoids any network round-trip.

### Source 3: HTTP Query (Fallback)

```
GET /slots HTTP/1.1
Host: {rtu_ip}:9081
Accept: application/json
```

**Success Response (HTTP 200):**

```json
{
  "slot_count": 2,
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
    }
  ]
}
```

**Parse rules:**
- `slot_count` is the number of application modules. DAP is NOT included.
- `slots` array is ordered by slot number
- `module_ident` and `submodule_ident` are **integers** (not hex strings). 16 = 0x10 = pH, 64 = 0x40 = Temperature, 256 = 0x100 = Pump, etc.
- `direction` is `"input"` (sensor) or `"output"` (actuator)
- `data_size` is the IO data length: 5 for sensors (Float32 BE + Quality), 4 for actuators (Cmd + Duty + Reserved*2)
- No device-level metadata. Get `vendor_id`, `station_name`, etc. from DCP or `/config`.
- Six fields per slot only. No `type`, `name`, or `module_type` strings. Derive type from `module_ident`.

**Mapping `data_size` to IOCR construction:**
- If `direction == "input"`: `input_size = data_size`, `output_size = 0`
- If `direction == "output"`: `input_size = 0`, `output_size = data_size`

**Error Handling:**

| HTTP Status | Meaning | Action |
|-------------|---------|--------|
| 200 | Success | Parse JSON, go to Step 3 |
| 200 + empty slots | No modules configured | Only DAP connect (valid for Phase 1 testing) |
| 503 | PROFINET subsystem unavailable | Wait 2 seconds, retry up to 5 times, then try source 4 |
| Connection refused | HTTP server not running | Try source 4 (DAP fallback) |
| Timeout (>5s) | Network issue | Try source 4 (DAP fallback) |

**Data source note:** This endpoint reads from the RTU's SQLite database (`db_module_list()`), not from p-net runtime state. It returns the configured intent, available even before `pnet_init()` completes.

### GSDML Fetch (Do Once, Cache)

```
GET /gsdml HTTP/1.1
Host: {rtu_ip}:9081
Accept: application/xml
```

Response is the raw GSDML XML file (~32KB, Content-Type: application/xml). Cache it keyed by `station_name`. Only refetch on firmware version change. This file defines the **menu** of possible module types.

---

### Source 4: DAP-Only Fallback (When HTTP Fails)

If HTTP is unreachable, discover slots through PROFINET itself.

### 4.1: Connect with DAP Only

Build a Connect Request containing ONLY the DAP at slot 0. The RTU always has DAP plugged so this will succeed (once the DREP encoding issue is fixed -- see Known Issues at the end).

**ExpectedSubmoduleBlockReq for DAP-only connect contains exactly 1 API entry with 1 slot and 3 submodules:**

```
NumberOfAPIs: 1
API: 0
  NumberOfSlots: 1
  Slot 0:
    ModuleIdentNumber: 0x00000001
    NumberOfSubmodules: 3
      Subslot 0x0001, SubmoduleIdentNumber 0x00000001, DataDescription: NO_IO
      Subslot 0x8000, SubmoduleIdentNumber 0x00000100, DataDescription: NO_IO
      Subslot 0x8001, SubmoduleIdentNumber 0x00000200, DataDescription: NO_IO
```

**IOCRBlockReq for DAP-only:**
- Input CR: header only, no application IO data bytes, no IOPS/IOCS from app modules
- Output CR: header only

No application data flows in a DAP-only AR.

### 4.2: Record Read Index 0xF844

After the DAP-only AR is established, issue an acyclic Record Read:

```
API:     0
Slot:    0
Subslot: 1
Index:   0xF844
```

**Response format (big-endian packed binary):**

```
Bytes 0-1:   uint16_t slot_count    (number of APPLICATION slots, NOT counting DAP)

Repeated slot_count times (15 bytes each):
  Bytes 0-1:   uint16_t slot_number
  Bytes 2-3:   uint16_t subslot_number
  Bytes 4-7:   uint32_t module_ident
  Bytes 8-11:  uint32_t submodule_ident
  Byte  12:    uint8_t  direction       (0=none, 1=input, 2=output)
  Bytes 13-14: uint16_t data_size       (input_size for sensors, output_size for actuators)
```

**Example: RTU with 1 temperature sensor:**
```
Hex: 00 01 00 01 00 01 00 00 00 40 00 00 00 41 01 00 05
     ^^^^^ count=1
           ^^^^^ slot=1
                 ^^^^^ subslot=1
                       ^^^^^^^^^^^ module=0x00000040
                                   ^^^^^^^^^^^ submod=0x00000041
                                               ^^ dir=1 (input)
                                                  ^^^^^ size=5
```

### 4.3: Release the DAP-Only AR

Send a Release Request to cleanly tear down the AR.

**CRITICAL: Wait 2 seconds after Release before attempting the second Connect.** p-net v0.2.0's AR state machine needs time to fully reset. Without this delay, the second Connect may be silently dropped.

### 4.4: Continue to Step 3

You now have the slot data. Build the full Connect Request as described in Step 3.

---

## Step 3: Build the Connect Request

You now have the RTU's application slot list from one of the four discovery sources. DAP (slot 0) is always included from your GSDML knowledge -- it is never part of the discovery response. Build the PROFINET Connect Request.

### ARBlockReq

| Field | Value |
|-------|-------|
| ARType | 0x0001 (IO Controller AR) |
| ARUUID | Generate a fresh UUID for each connection attempt |
| CMInitiatorMacAddress | Your controller's MAC address |
| CMInitiatorObjectUUID | Your controller's object UUID |
| CMInitiatorStationNameLength | Length of your station name |
| CMInitiatorStationName | Your controller's station name |

### IOCRBlockReq (Input CR -- RTU to Controller)

This carries sensor data FROM the RTU TO you.

**Calculate frame contents:**

```
For the Input CR you build two lists:

IODataObjects = []    (data the RTU provides)
IOCSObjects   = []    (consumer status you provide for output modules)

frame_offset = 0

For each slot where direction == "input" (ordered by slot number):
    IODataObjects.append({
        slot:        slot.slot,
        subslot:     slot.subslot,
        frame_offset: frame_offset
    })
    frame_offset += slot.data_size     # sensor data bytes (5 for all sensors)
    frame_offset += 1                  # IOPS byte (RTU's provider status)

For each slot where direction == "output" (ordered by slot number):
    IOCSObjects.append({
        slot:        slot.slot,
        subslot:     slot.subslot,
        frame_offset: frame_offset
    })
    frame_offset += 1                  # IOCS byte (RTU's consumer status for commands you send)

input_cr_data_length = frame_offset
```

### IOCRBlockReq (Output CR -- Controller to RTU)

This carries actuator commands FROM you TO the RTU.

```
IODataObjects = []    (commands you send)
IOCSObjects   = []    (consumer status you send for input modules)

frame_offset = 0

For each slot where direction == "output" (ordered by slot number):
    IODataObjects.append({
        slot:        slot.slot,
        subslot:     slot.subslot,
        frame_offset: frame_offset
    })
    frame_offset += slot.data_size     # actuator command bytes (4 for all actuators)
    frame_offset += 1                  # IOPS byte (your provider status)

For each slot where direction == "input" (ordered by slot number):
    IOCSObjects.append({
        slot:        slot.slot,
        subslot:     slot.subslot,
        frame_offset: frame_offset
    })
    frame_offset += 1                  # IOCS byte (your consumer status for sensor data)

output_cr_data_length = frame_offset
```

### ExpectedSubmoduleBlockReq

One block listing ALL modules. DAP comes from the GSDML (always the same). Application slots come from your discovery result. This MUST match exactly what the RTU has plugged.

**Structure:**

```
NumberOfAPIs: 1
API: 0

  NumberOfSlots: 1 + {number of application slots from discovery}

  Slot 0 (DAP -- ALWAYS include this, every RTU has it):
    ModuleIdentNumber: 0x00000001
    NumberOfSubmodules: 3
      Subslot 0x0001: SubmoduleIdentNumber 0x00000001
        SubmoduleProperties: 0x0000 (no IO)
      Subslot 0x8000: SubmoduleIdentNumber 0x00000100
        SubmoduleProperties: 0x0000 (no IO)
      Subslot 0x8001: SubmoduleIdentNumber 0x00000200
        SubmoduleProperties: 0x0000 (no IO)

  For each application slot (from discovery result):
    SlotNumber: {slot.slot}
    ModuleIdentNumber: {slot.module_ident}
    NumberOfSubmodules: 1
      SubslotNumber: {slot.subslot}
      SubmoduleIdentNumber: {slot.submodule_ident}
        If direction == "input":
          SubmoduleProperties: 0x0001 (input only)
          InputDataDescription:
            SubmoduleDataLength: {slot.data_size}
            LengthIOPS: 1
            LengthIOCS: 1
        If direction == "output":
          SubmoduleProperties: 0x0002 (output only)
          OutputDataDescription:
            SubmoduleDataLength: {slot.data_size}
            LengthIOPS: 1
            LengthIOCS: 1
```

### DREP (Data Representation)

**CRITICAL BUG TO FIX:** Your DREP declaration and your actual encoding MUST match.

DREP is 3 bytes in the RPC header:
```
Byte 0: Integer representation   (0x00 = big-endian, 0x10 = little-endian)
Byte 1: Character representation (0x00 = ASCII)
Byte 2: Floating-point           (0x00 = IEEE)
```

Rules:
- If DREP byte 0 = `0x10` (little-endian): ALL uint16 and uint32 fields in the entire RPC stub body MUST be little-endian. That means BlockType, BlockLength, ModuleIdentNumber, SubmoduleIdentNumber, SlotNumber, SubslotNumber, ARType, ARUUID fields, FrameID, every 16-bit and 32-bit value.
- If DREP byte 0 = `0x00` (big-endian): ALL uint16 and uint32 fields MUST be big-endian.

The RTU's p-net reads your DREP field and interprets every multi-byte field accordingly. If you declare LE but encode BE, p-net will read BlockLength as a swapped value, see a nonsensical length, and immediately reject with `"Faulty ARBlockReq" / "Error in Parameter BlockLength"`.

**Recommendation:** Use DREP=`0x10 0x00 0x00` (little-endian) and encode ALL multi-byte stub fields in little-endian byte order. This is the most common choice for x86-based controllers.

---

## Step 4: Send Connect and Parse Response

Send the DCE/RPC Connect Request (opnum 0, PNIO Device interface UUID `dea00001-6c97-11d1-8271-00a02442df7d`). Parse the Connect Response.

### Success (no error block)

AR is established. Proceed to Step 5 (cyclic exchange).

### Error in Response

If the response contains a PNIO error status, the fields tell you exactly what went wrong:

| Error Pattern | Meaning | Fix |
|---------------|---------|-----|
| ErrorDecode=0x80, ErrorCode1=0x.. containing "BlockLength" | DREP mismatch | Fix your byte order encoding to match DREP declaration |
| ErrorDecode=0x80, ErrorCode1 referencing "ARBlockReq" | ARBlockReq structure invalid | Check ARType, ARUUID format, station name encoding |
| ErrorDecode=0x80, ErrorCode1 referencing "ExpectedSubmodule" | Module ident mismatch | Slots don't match RTU -- re-query /slots |
| ErrorCode=0xCF | Application error in write callback | Record write payload was malformed |

### ModuleDiffBlock in Response

Even on a successful connect, the response MAY contain a ModuleDiffBlock. This is **normal PROFINET behavior** per IEC 61158-6. Parse it. Do NOT treat it as an error.

The ModuleDiffBlock lists differences between what you expected and what the RTU has:

```
ModuleDiffBlock:
  NumberOfAPIs: 1
  API: 0
    NumberOfModules: N

    For each module:
      SlotNumber: X
      ModuleIdentNumber: Y
      ModuleState:
        0x0000 = PROPER_MODULE (matches what you expected)
        0x0002 = WRONG_MODULE (different module than you expected)
        The slot being absent means NO_MODULE

      NumberOfSubmodules: M
      For each submodule:
        SubslotNumber: Z
        SubmoduleIdentNumber: W
        SubmoduleState:
          0x0000 = PROPER_SUBMODULE (matches)
          0x0002 = WRONG_SUBMODULE (different)
          0x0004 = NO_SUBMODULE (empty)
          0x0007 = SUBSTITUTE (RTU provides zero/default data)
```

**Controller action per diff entry:**

| ModuleState | SubmoduleState | What to Do |
|-------------|---------------|------------|
| PROPER | PROPER | Normal. Use this slot in your cyclic IO map as planned. |
| NO_MODULE | any | Remove this slot from your cyclic IO map entirely. Do not read/write. Recalculate frame offsets. |
| WRONG_MODULE | any | Log warning. Remove from IO map or adapt to actual module. |
| PROPER | SUBSTITUTE | RTU sends substitute data (zeros) for this submodule. Accept it. Do not treat as error. |
| PROPER | NO_SUBMODULE | Remove this subslot from IO map. |

After parsing the diff, rebuild your cyclic IO frame offset map to skip any removed slots. Then proceed to cyclic exchange with only the active slots.

---

## Step 5: Cyclic Data Exchange

### Reading Sensor Data (Input CR frames)

For each `direction == "input"` slot that is active in your IO map, at the offset calculated in Step 3:

**Sensor data format (5 bytes per sensor submodule):**

```
Byte 0:   Float32 byte 3 (MSB)  \
Byte 1:   Float32 byte 2         |  IEEE 754, big-endian
Byte 2:   Float32 byte 1         |  This is the sensor reading as a float
Byte 3:   Float32 byte 0 (LSB)  /
Byte 4:   Quality byte
            0x00 = Good (reading is valid and accurate)
            0x40 = Uncertain (reading present but may be inaccurate)
            0x80 = Bad (reading is invalid, do not use)
            0xC0 = Not Connected (sensor hardware not detected)
```

Immediately after the 5 data bytes is 1 IOPS byte:
```
Byte 5:   IOPS (provider status from RTU)
            0x80 = GOOD (RTU is actively providing valid data)
            0x00 = BAD (RTU is not providing valid data for this submodule)
```

**Example: Temperature = 23.5 C, quality good, IOPS good:**
```
Hex: 41 BC 00 00 00 80
     ^^^^^^^^^^^ float 23.5 in big-endian IEEE 754
                 ^^ quality 0x00 = Good
                    ^^ IOPS 0x80 = Good
```

**Example: pH = 7.2, quality good:**
```
Hex: 40 E6 66 66 00 80
     ^^^^^^^^^^^ float 7.2 in big-endian IEEE 754
                 ^^ quality 0x00 = Good
                    ^^ IOPS 0x80 = Good
```

**Example: Sensor disconnected:**
```
Hex: 00 00 00 00 C0 00
     ^^^^^^^^^^^ float 0.0 (meaningless)
                 ^^ quality 0xC0 = Not Connected
                    ^^ IOPS 0x00 = Bad
```

### Writing Actuator Commands (Output CR frames)

For each `direction == "output"` slot that is active in your IO map, at the offset calculated in Step 3:

**Actuator command format (4 bytes per actuator submodule):**

```
Byte 0: Command
          0x00 = OFF (deactivate actuator)
          0x01 = ON (activate at full power or binary open)
          0x02 = PWM (activate at duty cycle specified in byte 1)
Byte 1: Duty cycle (0-100, percentage, only used when command = 0x02)
Byte 2: Reserved (always write 0x00)
Byte 3: Reserved (always write 0x00)
```

Immediately after the 4 data bytes is 1 IOPS byte:
```
Byte 4:   IOPS (your provider status)
            0x80 = GOOD (you are actively controlling this actuator)
            0x00 = BAD (you are not controlling, RTU should go to safe state)
```

**Example: Turn pump ON at full speed:**
```
Hex: 01 00 00 00 80
     ^^ cmd=ON
        ^^ duty=0 (ignored for ON)
           ^^^^^ reserved
                 ^^ IOPS=Good
```

**Example: Pump PWM at 75% duty:**
```
Hex: 02 4B 00 00 80
     ^^ cmd=PWM
        ^^ duty=75 (0x4B)
           ^^^^^ reserved
                 ^^ IOPS=Good
```

**Example: Valve OPEN:**
```
Hex: 01 00 00 00 80
```

**Example: Everything OFF (safe state):**
```
Hex: 00 00 00 00 80
```

### IOCS Bytes You Must Send

In the Output CR, after all output IODataObjects and their IOPS bytes, you must send IOCS bytes for each input submodule (this tells the RTU you are consuming its sensor data):

```
For each input slot:
  IOCS = 0x80   (Good -- you are consuming this sensor data)
  IOCS = 0x00   (Bad -- you are not consuming, tell RTU you don't need it)
```

In the Input CR, the RTU sends IOCS bytes for each output submodule (this tells you the RTU received your commands):

```
For each output slot:
  IOCS from RTU = 0x80   (Good -- RTU received and is acting on your command)
  IOCS from RTU = 0x00   (Bad -- RTU did not receive or cannot act on command)
```

---

## Complete Module Identifier Table

These are ALL the module types the RTU supports. The GSDML defines this complete set. Any application slot on the RTU will be one of these.

### Sensor Modules (direction = "input", data_size = 5)

| module_ident (dec) | module_ident (hex) | submodule_ident | What it measures |
|------|-------------|-----------------|------------------|
| 16 | `0x00000010` | 17 | pH sensor, 0-14 scale |
| 32 | `0x00000020` | 33 | Total dissolved solids, ppm |
| 48 | `0x00000030` | 49 | Water turbidity, NTU |
| 64 | `0x00000040` | 65 | Temperature, degrees Celsius |
| 80 | `0x00000050` | 81 | Flow rate, liters/minute |
| 96 | `0x00000060` | 97 | Tank level, percentage |
| 112 | `0x00000070` | 113 | Generic analog, raw float |

Pattern: `submodule_ident` is always `module_ident + 1`.

All sensor modules have identical wire format: 5 bytes (Float32 BE + Quality). `data_size` = 5.

### Actuator Modules (direction = "output", data_size = 4)

| module_ident (dec) | module_ident (hex) | submodule_ident | What it controls |
|------|-------------|-----------------|------------------|
| 256 | `0x00000100` | 257 | Peristaltic pump, ON/OFF/PWM |
| 272 | `0x00000110` | 273 | Solenoid valve, OPEN/CLOSE |
| 288 | `0x00000120` | 289 | Generic digital output |

Pattern: `submodule_ident` is always `module_ident + 1`.

All actuator modules have identical wire format: 4 bytes (Cmd + Duty + Reserved + Reserved). `data_size` = 4.

### How to determine type from ident

The JSON API does not return type strings. Derive the type from `module_ident`:
- `module_ident < 256` → sensor (direction will be `"input"`)
- `module_ident >= 256` → actuator (direction will be `"output"`)
- Specific type within category: see hex table above

### DAP (always slot 0, always present, no IO data)

| subslot | submodule_ident | Purpose |
|---------|-----------------|---------|
| `0x0001` | `0x00000001` | Device Access Point |
| `0x8000` | `0x00000100` | PROFINET Interface (SubslotNumber 32768) |
| `0x8001` | `0x00000200` | Ethernet Port (SubslotNumber 32769) |

Module ident for DAP is always `0x00000001`. Direction is NO_IO. No data bytes.

---

## PROFINET Record Indices (Acyclic Read/Write After AR)

Once an AR is established, you can issue acyclic Record Read/Write to these vendor-specific indices. All use API=0.

| Index | Read/Write | Slot | Subslot | Purpose | Payload Size |
|-------|-----------|------|---------|---------|-------------|
| `0x8000` | Read | 0 | 1 | I&M0 device identification | Standard I&M0 |
| `0xF840` | Write | 0 | 1 | User credential sync (DJB2 hashed) | Variable |
| `0xF841` | Write | 0 | 1 | Device configuration | 52 bytes |
| `0xF842` | Write | 0 | 1 | Sensor configuration | Variable (header + 42 bytes/sensor) |
| `0xF843` | Write | 0 | 1 | Actuator configuration | Variable (header + 22 bytes/actuator) |
| `0xF844` | Read | 0 | 1 | Slot map (binary, see Source 4.2) | 2 + 15*N bytes |
| `0xF845` | Write | 0 | 1 | Enrollment/binding | 80 bytes |

---

## Worked Example: RTU With 1 Temperature Sensor (Minimal)

This is what a fresh, unconfigured RTU looks like. It auto-creates one CPU temperature sensor at slot 1.

### Discovery Result (from any source)

From HTTP `/slots`:
```json
{
  "slot_count": 1,
  "slots": [
    {"slot": 1, "subslot": 1, "module_ident": 64, "submodule_ident": 65, "direction": "input", "data_size": 5}
  ]
}
```

Note: `module_ident` 64 = 0x40 = Temperature sensor. DAP is not in the response.

### ExpectedSubmoduleBlockReq You Build

DAP comes from GSDML knowledge (always the same). Slot 1 comes from discovery.

```
API 0:
  Slot 0, Module 0x00000001 (DAP -- from GSDML, always include):
    Subslot 0x0001, Submod 0x00000001, NO_IO
    Subslot 0x8000, Submod 0x00000100, NO_IO
    Subslot 0x8001, Submod 0x00000200, NO_IO
  Slot 1, Module 0x00000040 (Temperature -- from discovery):
    Subslot 0x0001, Submod 0x00000041, INPUT, DataLength=5, LengthIOPS=1, LengthIOCS=1
```

### Input IOCR You Build (RTU -> Controller)

```
IODataObjects:
  Slot 1, Subslot 1, FrameOffset 0

Total frame layout at runtime:
  Offset 0-4:  Temperature data (5 bytes: Float32 BE + Quality)
  Offset 5:    IOPS for Slot 1 (1 byte, set by RTU)
  ---
  Total: 6 bytes of frame payload
```

No IOCSObjects in the Input CR (there are no output modules).

### Output IOCR You Build (Controller -> RTU)

```
IOCSObjects:
  Slot 1, Subslot 1, FrameOffset 0

Total frame layout at runtime:
  Offset 0:    IOCS for Slot 1 (1 byte, you set to 0x80 = consuming)
  ---
  Total: 1 byte of frame payload
```

No IODataObjects in the Output CR (there are no output modules).

### What You Receive Each Cycle (Input CR)

```
Hex: 41 BC 00 00 00 80
     [  23.5 C  ] [Q] [IOPS]
```

### What You Send Each Cycle (Output CR)

```
Hex: 80
     [IOCS = Good, I'm consuming the temperature data]
```

---

## Worked Example: RTU With 2 Sensors + 1 Pump (Typical)

### Discovery Result

From HTTP `/slots`:
```json
{
  "slot_count": 3,
  "slots": [
    {"slot": 1, "subslot": 1, "module_ident": 16,  "submodule_ident": 17,  "direction": "input",  "data_size": 5},
    {"slot": 2, "subslot": 1, "module_ident": 64,  "submodule_ident": 65,  "direction": "input",  "data_size": 5},
    {"slot": 3, "subslot": 1, "module_ident": 256, "submodule_ident": 257, "direction": "output", "data_size": 4}
  ]
}
```

Ident decoding: 16=0x10=pH, 64=0x40=Temperature, 256=0x100=Pump.

### ExpectedSubmoduleBlockReq

```
API 0:
  Slot 0, Module 0x00000001 (DAP -- from GSDML):
    Subslot 0x0001, Submod 0x00000001, NO_IO
    Subslot 0x8000, Submod 0x00000100, NO_IO
    Subslot 0x8001, Submod 0x00000200, NO_IO
  Slot 1, Module 0x00000010 (pH -- from discovery):
    Subslot 0x0001, Submod 0x00000011, INPUT, DataLength=5, LengthIOPS=1, LengthIOCS=1
  Slot 2, Module 0x00000040 (Temperature -- from discovery):
    Subslot 0x0001, Submod 0x00000041, INPUT, DataLength=5, LengthIOPS=1, LengthIOCS=1
  Slot 3, Module 0x00000100 (Pump -- from discovery):
    Subslot 0x0001, Submod 0x00000101, OUTPUT, DataLength=4, LengthIOPS=1, LengthIOCS=1
```

### Input IOCR (RTU -> Controller) Frame Layout

```
IODataObjects:
  Slot 1, Subslot 1, FrameOffset 0    (pH, 5 bytes data + 1 IOPS)
  Slot 2, Subslot 1, FrameOffset 6    (Temp, 5 bytes data + 1 IOPS)

IOCSObjects:
  Slot 3, Subslot 1, FrameOffset 12   (Pump IOCS from RTU, 1 byte)

Frame at runtime:
  Offset 0-4:   pH data (Float32 BE + Quality)
  Offset 5:     pH IOPS
  Offset 6-10:  Temp data (Float32 BE + Quality)
  Offset 11:    Temp IOPS
  Offset 12:    Pump IOCS (RTU confirms it received your pump command)
  Total: 13 bytes
```

### Output IOCR (Controller -> RTU) Frame Layout

```
IODataObjects:
  Slot 3, Subslot 1, FrameOffset 0    (Pump, 4 bytes data + 1 IOPS)

IOCSObjects:
  Slot 1, Subslot 1, FrameOffset 5    (pH IOCS, 1 byte)
  Slot 2, Subslot 1, FrameOffset 6    (Temp IOCS, 1 byte)

Frame at runtime:
  Offset 0-3:   Pump command (Cmd + Duty + Reserved + Reserved)
  Offset 4:     Pump IOPS (0x80 = you are actively controlling)
  Offset 5:     pH IOCS (0x80 = you consumed pH data)
  Offset 6:     Temp IOCS (0x80 = you consumed Temp data)
  Total: 7 bytes
```

### Full Cycle Exchange Example

**You receive (Input CR):**
```
Hex: 40 E6 66 66 00 80 41 BC 00 00 00 80 80
     [  pH=7.2   ] Q  IO [  T=23.5  ] Q  IO IO
                    PS                  PS CS
```

Decoding:
- Bytes 0-4: pH = 7.2, Quality = 0x00 (Good)
- Byte 5: IOPS = 0x80 (Good)
- Bytes 6-10: Temp = 23.5 C, Quality = 0x00 (Good)
- Byte 11: IOPS = 0x80 (Good)
- Byte 12: Pump IOCS = 0x80 (RTU received your pump command)

**You send (Output CR):**
```
Hex: 01 00 00 00 80 80 80
     [Pump ON   ] IO IO IO
                   PS CS CS
```

Decoding:
- Bytes 0-3: Pump command = ON, duty = 0, reserved
- Byte 4: Pump IOPS = 0x80 (you are actively controlling)
- Byte 5: pH IOCS = 0x80 (you consumed pH reading)
- Byte 6: Temp IOCS = 0x80 (you consumed Temp reading)

---

## Known Issues That Block Connection Today

### 1. DREP Encoding Mismatch (MUST FIX FIRST)

Your current Connect Requests declare `DREP=0x10 0x00 0x00` (little-endian) but encode multi-byte fields in big-endian. p-net reads DREP and byte-swaps all fields accordingly, so it sees garbage values for BlockLength and rejects with `"Faulty ARBlockReq" / "Error in Parameter BlockLength"`.

**This is why every Connect attempt in the 228-packet pcap failed. Fix this before anything else.**

### 2. Rapid Reconnect Instability

p-net v0.2.0 stops responding after approximately 9 rapid Connect attempts. Its internal AR state machine gets stuck. If you are retrying, wait at least 2 seconds between attempts.

### 3. Remove Hardcoded 16-Slot Layout

The current controller sends ExpectedSubmoduleBlockReq with 16 hardcoded slots (8 GenericAI + 7 GenericDO + DAP). Replace this entirely with dynamic construction from the HTTP response or record read.

---

## Implementation Checklist

### Prerequisites (Do First)

- [ ] Fix DREP encoding to match declaration (all LE or all BE throughout stub)
- [ ] Remove hardcoded 16-slot layout; replace with dynamic construction
- [ ] Import GSDML and hardcode DAP config: slot 0, module 0x00000001, submodules 0x00000001/0x00000100/0x00000200

### Phase 1: Slot Discovery Chain

- [ ] Source 1: Parse local GSDML `UseableModules` for module type menu
- [ ] Source 2: Cache slot config per `station_name` after each successful connection
- [ ] Source 3: HTTP GET `http://{rtu_ip}:9081/slots`, timeout 5 seconds
  - [ ] Handle HTTP 503: wait 2s, retry, max 5 retries
  - [ ] Handle connection refused / timeout: try source 4
  - [ ] Parse JSON: `module_ident` and `submodule_ident` are integers (not hex strings)
  - [ ] `data_size` = input bytes for sensors, output bytes for actuators
  - [ ] DAP is NOT in the response -- you always add it yourself from GSDML
- [ ] Source 4: DAP-only connect + Record Read 0xF844
  - [ ] Build DAP-only ExpectedSubmoduleBlockReq (1 slot, 3 submodules)
  - [ ] Build minimal IOCRBlockReq (no app data)
  - [ ] Send Connect, accept ModuleDiffBlock
  - [ ] Issue Record Read: API=0, Slot=0, Subslot=1, Index=0xF844
  - [ ] Parse binary response: uint16 BE count, then 15-byte BE entries
  - [ ] Send Release, wait 2 seconds for p-net AR reset

### Phase 2: Build and Send Connect

- [ ] Combine DAP (from GSDML) + application slots (from discovery) into full layout
- [ ] Build ExpectedSubmoduleBlockReq:
  - [ ] Slot 0 DAP with 3 submodules (0x0001, 0x8000, 0x8001), all NO_IO
  - [ ] Each app slot with 1 submodule, DataDescription per direction, DataLength = data_size
- [ ] Build Input IOCRBlockReq:
  - [ ] IODataObjects for each input submodule with cumulative FrameOffset
  - [ ] IOCSObjects for each output submodule after input data
  - [ ] Calculate total data length
- [ ] Build Output IOCRBlockReq:
  - [ ] IODataObjects for each output submodule with cumulative FrameOffset
  - [ ] IOCSObjects for each input submodule after output data
  - [ ] Calculate total data length
- [ ] Build ARBlockReq with fresh ARUUID
- [ ] Encode ALL stub fields consistent with DREP declaration
- [ ] Send Connect Request
- [ ] Parse Connect Response for error blocks
- [ ] On success: cache this slot config for source 2 next time
- [ ] Optional: HTTP GET `/gsdml` once, cache the XML by station_name

### Phase 3: ModuleDiff Tolerance (Implement for Robustness)

- [ ] After every Connect Response: check for ModuleDiffBlock
- [ ] If present: parse each module/submodule entry
- [ ] For NO_MODULE: remove slot from cyclic IO map
- [ ] For WRONG_MODULE: log, remove or adapt
- [ ] For SUBSTITUTE: accept, do not error
- [ ] Recalculate frame offsets after removing slots
- [ ] Enter cyclic exchange with adjusted IO map

### Phase 4: Connection Resilience (Critical for Production)

This phase implements the supervision, monitoring, and recovery patterns
required for reliable controller-RTU communication.  All mechanisms below
are wire-compliant PROFINET unless explicitly marked as application-layer
additions.

#### 4.1: IOPS/IOCS Monitoring (Standard PROFINET)

The RTU now correctly propagates sensor quality to the IOPS byte of each
input submodule.  The controller MUST monitor IOPS in every Input CR frame:

```
For each input slot at its frame offset:
  Read IOPS byte (immediately after data + quality bytes)

  If IOPS == 0x80 (GOOD):
    Sensor data is trustworthy.  Use the value + quality byte.

  If IOPS == 0x00 (BAD):
    The RTU is reporting that this sensor has failed.
    - Do NOT use the data value for control decisions
    - Check the quality byte (byte 4) for specific reason:
        0x80 = BAD (sensor read failures)
        0xC0 = NOT_CONNECTED (hardware not detected)
    - Fall back to last known good value or alarm
```

**Cross-protocol parallel:** This is equivalent to OPC UA StatusCode on
each monitored item, or EtherNet/IP connection status per produced tag.

- [ ] On every Input CR: check IOPS for each input slot
- [ ] If IOPS transitions GOOD→BAD: mark data unreliable, raise alarm
- [ ] If IOPS transitions BAD→GOOD: resume using data

#### 4.2: Diagnosis Alarm Handling (Standard PROFINET)

The RTU sends standard channel diagnosis alarms (IEC 61158-6-10) when a
sensor transitions to/from a fault state.  This provides immediate
notification rather than waiting for the next cyclic frame.

**Alarm types the RTU sends:**

| Type | USI | Meaning |
|------|-----|---------|
| 0x0001 | 0x8000 | Diagnosis appears (sensor fault) |
| 0x0002 | 0x8000 | Diagnosis disappears (sensor recovered) |

**Diagnosis payload (6 bytes):**
```
Bytes 0-1: Channel number (0x0000 = whole submodule)
Bytes 2-3: Channel properties (0x0000 = input, accumulative)
Bytes 4-5: Channel error type:
           0x001F = Sensor not available (QUALITY_NOT_CONNECTED)
           0x0008 = Data transmission impossible (QUALITY_BAD)
           0x0000 = No error (cleared)
```

**Controller action:**
- On alarm type 0x0001: Mark slot as faulted, raise operator alarm
- On alarm type 0x0002: Clear fault, resume normal operation
- Always ACK the alarm via `pnet_alarm_send_ack()`

**Cross-protocol parallel:** OPC UA StatusChangeNotification, EtherNet/IP
device-level ring fault detection.

- [ ] Register alarm indication callback
- [ ] Parse USI 0x8000 diagnosis payload (6 bytes: channel + props + error)
- [ ] Map alarm to slot and track fault state
- [ ] Send alarm ACK after processing
- [ ] Log all alarm transitions for audit trail

#### 4.3: Data Status Monitoring (Standard PROFINET)

The APDU status in each cyclic frame contains a `ProviderState (RUN)` bit
(bit 4, 0x10) and a `DataValid` bit (bit 2, 0x04).  The RTU now monitors
these and acts on transitions:

```
Controller Output CR → data_status byte:
  Bit 4 (0x10): RUN    — 1=controller is actively sending, 0=STOP mode
  Bit 2 (0x04): Valid  — 1=data valid, 0=data invalid
```

**Controller-side requirements:**

When the controller enters STOP mode (e.g., PLC program stopped):
- Set ProviderState=0 in the Output CR APDU status
- The RTU will detect RUN→STOP and enter safe state for actuators

When the controller resumes:
- Set ProviderState=1
- The RTU will detect STOP→RUN and resume normal operation

**Cross-protocol parallel:**
- EtherNet/IP: production inhibit timer expiry
- OPC UA: session keepalive timeout
- Modbus TCP: TCP keepalive + application-level timeout

- [ ] Set ProviderState=RUN (bit 4) in Output CR when actively controlling
- [ ] Set ProviderState=STOP when PLC program is stopped
- [ ] Monitor RTU's ProviderState in Input CR for RTU-side stops

#### 4.4: Connection Liveness / Watchdog (Application Layer)

The RTU implements application-level liveness supervision that runs
independently of p-net's DataHoldTimer.  This catches scenarios where the
PROFINET AR is technically established but no meaningful output data flows.

**Configuring the watchdog via Record Write 0xF841:**

The `watchdog_ms` field in the device config packet controls how long the
RTU waits before declaring the connection stale:

```
Device config packet (Record Write index 0xF841):
  ...
  Bytes 8-11: uint32_t watchdog_ms (network byte order)
              0 = use RTU default (5000ms)
              Recommended: 3× your cyclic send interval
  ...
```

**RTU behavior on liveness timeout:**
1. Logs warning with diagnostic context (cycle count, output poll count)
2. Notifies actuator manager → enters degraded mode
3. After 30 seconds in degraded mode → applies safe state (all actuators OFF)

**Recovery:** When the controller resumes sending valid output data, the RTU
automatically exits degraded mode and resumes normal actuator control.

**Cross-protocol parallel:**
- EtherNet/IP: `O→T RPI × multiplier` = production timeout
- OPC UA: `RevisedSessionTimeout / 3` = keepalive interval
- Modbus TCP: application-level "no response" counter

- [ ] Set `watchdog_ms` in device config to 3× your cyclic send interval
- [ ] Monitor actuator IOCS in Input CR for confirmation of command receipt
- [ ] Implement reconnect-on-timeout: if liveness timeout + Release, wait 2s, reconnect

#### 4.5: Reconnection Strategy

When the PROFINET connection is lost (ABORT, DataHoldTimer expiry, or
the controller decides to release), implement this reconnection pattern:

```
[CONNECTION_LOST]
  |
  v
[WAIT 2 SECONDS] -----> Required for p-net v0.2.0 AR state reset
  |
  v
[DCP_RE_IDENTIFY] -----> Verify RTU is still reachable via DCP
  |
  | Success                    | Failure
  v                            v
[RESOLVE_SLOTS]         [EXPONENTIAL_BACKOFF]
  |                            |
  | Got slots                  | Retry: 2s, 4s, 8s, 16s, max 60s
  v                            |
[CONNECT]                      +-----> [DCP_RE_IDENTIFY]
  |
  | Success
  v
[CYCLIC_EXCHANGE]
```

**Reconnection rules:**
1. Always wait 2 seconds after Release/ABORT before reconnecting
2. Re-verify slot configuration (may have changed during downtime)
3. Use cached config first (source 2), HTTP fallback (source 3) on mismatch
4. Exponential backoff on repeated failures: 2s, 4s, 8s, 16s, cap at 60s
5. Log every reconnection attempt with attempt count and reason
6. Reset backoff on successful connection

**Cross-protocol parallel:**
- TCP: exponential backoff with jitter (RFC 6298)
- OPC UA: session reactivation with channel renewal
- EtherNet/IP: Forward Open retry with timeout multiplier

- [ ] Implement 2-second wait after any connection termination
- [ ] Re-run DCP Identify before reconnect (verify RTU reachable)
- [ ] Re-resolve slots (cached → HTTP → DAP fallback)
- [ ] Implement exponential backoff: 2s, 4s, 8s, 16s, cap 60s
- [ ] Log reconnection attempts with attempt number and reason
- [ ] Reset backoff counter on successful connection

#### 4.6: Quality Propagation Summary

The complete data quality path from RTU sensor to controller application:

```
Sensor Hardware
  → driver_xxx_read()        returns RESULT_OK or RESULT_ERROR
  → sensor_instance_read()   updates consecutive_failures, timestamp
  → determine_quality()      computes GOOD/UNCERTAIN/BAD/NOT_CONNECTED
  → sensor_manager worker    sends to PROFINET:
      ├── Quality byte (cyclic data byte 4):    0x00/0x40/0x80/0xC0
      ├── IOPS byte (after data):               0x80 (GOOD) or 0x00 (BAD)
      └── Diagnosis alarm (on transitions):     0x0001 (appears) / 0x0002 (disappears)
  → Controller receives all three signals
```

**Controller should use this priority for quality assessment:**
1. **IOPS byte** — definitive "is the RTU providing valid data for this slot?"
2. **Quality byte** — fine-grained reason (uncertain vs bad vs disconnected)
3. **Diagnosis alarm** — immediate async notification of state change

All three are standard PROFINET mechanisms.  No proprietary extensions.
