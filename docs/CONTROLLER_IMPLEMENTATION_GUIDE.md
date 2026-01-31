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

## Connection State Machine

Implement this exact state machine. Every transition is described.

```
[IDLE]
  |
  v
[DCP_DISCOVERY] ----> send DCP Identify multicast
  |
  | DCP Identify Response received (got RTU IP + station_name)
  v
[HTTP_QUERY] -------> GET http://{rtu_ip}:9081/api/v1/slots
  |                         |
  | 200 OK (got slots JSON) | Connection refused / timeout / 503
  v                         v
[BUILD_CONNECT]       [DAP_FALLBACK] ---> Connect with DAP-only
  |                         |
  |                         | Connect OK
  |                         v
  |                   [RECORD_READ] ---> Read index 0xF844
  |                         |
  |                         | Got slot map
  |                         v
  |                   [RELEASE] ------> Release DAP-only AR
  |                         |
  |                         | Release confirmed, wait 2 seconds
  |                         v
  |                   [BUILD_CONNECT] (now with correct slots)
  |                         |
  v                         v
[CONNECT] -----------> Send PROFINET Connect Request
  |
  | Connect Response received
  v
[PARSE_RESPONSE] ----> Check for errors and ModuleDiffBlock
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

## Step 2: HTTP Slot Query (Primary Path)

### Request

```
GET /api/v1/slots HTTP/1.1
Host: {rtu_ip}:9081
Accept: application/json
```

### Success Response (HTTP 200)

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

**Parse rules:**
- `slot_count` is the total count including DAP (slot 0)
- `slots` array is ordered by slot number
- Slot 0 is always DAP, always present
- `module_ident` and `submodule_ident` are hex strings -- parse to uint32
- `direction` is one of: `"none"`, `"input"`, `"output"`
- `input_size` is how many bytes the RTU sends to you per cycle (sensor data)
- `output_size` is how many bytes you send to the RTU per cycle (actuator commands)

### Error Handling

| HTTP Status | Meaning | Action |
|-------------|---------|--------|
| 200 | Success | Parse JSON, go to Step 3 |
| 503 | RTU PROFINET stack still starting | Wait 2 seconds, retry up to 5 times |
| Connection refused | HTTP server not running | Go to Step 2B (DAP fallback) |
| Timeout (>5s) | Network issue | Go to Step 2B (DAP fallback) |
| Any other error | Unexpected | Go to Step 2B (DAP fallback) |

### GSDML Fetch (Do Once, Cache)

```
GET /api/v1/gsdml HTTP/1.1
Host: {rtu_ip}:9081
Accept: application/xml
```

Response is the raw GSDML XML file (~32KB, Content-Type: application/xml). Cache it keyed by `station_name`. Only refetch on firmware version change. This file defines the **menu** of possible module types. The `/api/v1/slots` response tells you which ones are actually plugged right now.

---

## Step 2B: DAP-Only Fallback (When HTTP Fails)

If HTTP is unreachable, discover slots through PROFINET itself.

### 2B.1: Connect with DAP Only

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

### 2B.2: Record Read Index 0xF844

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

### 2B.3: Release the DAP-Only AR

Send a Release Request to cleanly tear down the AR.

**CRITICAL: Wait 2 seconds after Release before attempting the second Connect.** p-net v0.2.0's AR state machine needs time to fully reset. Without this delay, the second Connect may be silently dropped.

### 2B.4: Continue to Step 3

You now have the slot data. Build the full Connect Request as described in Step 3.

---

## Step 3: Build the Connect Request

You now have the RTU's slot configuration from either HTTP (Step 2) or Record Read (Step 2B). Build the PROFINET Connect Request.

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
    frame_offset += slot.input_size    # sensor data bytes
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
    frame_offset += slot.output_size   # actuator command bytes
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

One block listing ALL modules the RTU reported. This MUST match exactly what the RTU has plugged.

**Structure:**

```
NumberOfAPIs: 1
API: 0

  NumberOfSlots: {number of unique slot numbers from RTU response}

  Slot 0 (DAP -- ALWAYS include this, every RTU has it):
    ModuleIdentNumber: 0x00000001
    NumberOfSubmodules: 3
      Subslot 0x0001: SubmoduleIdentNumber 0x00000001
        SubmoduleProperties: 0x0000 (no IO)
      Subslot 0x8000: SubmoduleIdentNumber 0x00000100
        SubmoduleProperties: 0x0000 (no IO)
      Subslot 0x8001: SubmoduleIdentNumber 0x00000200
        SubmoduleProperties: 0x0000 (no IO)

  For each application slot (from slots array, slot > 0):
    SlotNumber: {slot.slot}
    ModuleIdentNumber: {slot.module_ident}
    NumberOfSubmodules: 1
      SubslotNumber: {slot.subslot}
      SubmoduleIdentNumber: {slot.submodule_ident}
        If direction == "input":
          SubmoduleProperties: 0x0001 (input only)
          InputDataDescription:
            SubmoduleDataLength: {slot.input_size}
            LengthIOPS: 1
            LengthIOCS: 1
        If direction == "output":
          SubmoduleProperties: 0x0002 (output only)
          OutputDataDescription:
            SubmoduleDataLength: {slot.output_size}
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
| ErrorDecode=0x80, ErrorCode1 referencing "ExpectedSubmodule" | Module ident mismatch | Slots don't match RTU -- re-query /api/v1/slots |
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

### Sensor Modules (direction = "input", input_size = 5, output_size = 0)

| type string in JSON | module_ident | submodule_ident | What it measures |
|---------------------|-------------|-----------------|------------------|
| `"pH"` | `0x00000010` | `0x00000011` | pH sensor, 0-14 scale |
| `"TDS"` | `0x00000020` | `0x00000021` | Total dissolved solids, ppm |
| `"Turbidity"` | `0x00000030` | `0x00000031` | Water turbidity, NTU |
| `"Temperature"` | `0x00000040` | `0x00000041` | Temperature, degrees Celsius |
| `"Flow"` | `0x00000050` | `0x00000051` | Flow rate, liters/minute |
| `"Level"` | `0x00000060` | `0x00000061` | Tank level, percentage |
| `"Generic Analog Input"` | `0x00000070` | `0x00000071` | Generic analog, raw float |

Pattern: `submodule_ident` is always `module_ident + 1`.

All sensor modules have identical wire format: 5 bytes (Float32 BE + Quality).

### Actuator Modules (direction = "output", input_size = 0, output_size = 4)

| type string in JSON | module_ident | submodule_ident | What it controls |
|---------------------|-------------|-----------------|------------------|
| `"Pump"` | `0x00000100` | `0x00000101` | Peristaltic pump, ON/OFF/PWM |
| `"Valve"` | `0x00000110` | `0x00000111` | Solenoid valve, OPEN/CLOSE |
| `"Generic Digital Output"` | `0x00000120` | `0x00000121` | Generic digital output |

Pattern: `submodule_ident` is always `module_ident + 1`.

All actuator modules have identical wire format: 4 bytes (Cmd + Duty + Reserved + Reserved).

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
| `0xF844` | Read | 0 | 1 | Slot map (binary, see Step 2B.2) | 2 + 15*N bytes |
| `0xF845` | Write | 0 | 1 | Enrollment/binding | 80 bytes |

---

## Worked Example: RTU With 1 Temperature Sensor (Minimal)

This is what a fresh, unconfigured RTU looks like. It auto-creates one CPU temperature sensor at slot 1.

### HTTP Response from /api/v1/slots

```json
{
  "station_name": "rtu-4b64",
  "vendor_id": "0x0493",
  "device_id": "0x0001",
  "gsdml_version": "V2.4",
  "slot_count": 2,
  "slots": [
    {
      "slot": 0, "subslot": 1,
      "module_ident": "0x00000001", "submodule_ident": "0x00000001",
      "type": "DAP", "direction": "none",
      "input_size": 0, "output_size": 0
    },
    {
      "slot": 1, "subslot": 1,
      "module_ident": "0x00000040", "submodule_ident": "0x00000041",
      "type": "Temperature", "direction": "input",
      "input_size": 5, "output_size": 0
    }
  ]
}
```

### ExpectedSubmoduleBlockReq You Build

```
API 0:
  Slot 0, Module 0x00000001 (DAP):
    Subslot 0x0001, Submod 0x00000001, NO_IO
    Subslot 0x8000, Submod 0x00000100, NO_IO
    Subslot 0x8001, Submod 0x00000200, NO_IO
  Slot 1, Module 0x00000040 (Temperature):
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

### HTTP Response

```json
{
  "station_name": "rtu-eeff",
  "vendor_id": "0x0493",
  "device_id": "0x0001",
  "gsdml_version": "V2.4",
  "slot_count": 4,
  "slots": [
    {"slot": 0, "subslot": 1, "module_ident": "0x00000001", "submodule_ident": "0x00000001",
     "type": "DAP", "direction": "none", "input_size": 0, "output_size": 0},
    {"slot": 1, "subslot": 1, "module_ident": "0x00000010", "submodule_ident": "0x00000011",
     "type": "pH", "direction": "input", "input_size": 5, "output_size": 0},
    {"slot": 2, "subslot": 1, "module_ident": "0x00000040", "submodule_ident": "0x00000041",
     "type": "Temperature", "direction": "input", "input_size": 5, "output_size": 0},
    {"slot": 3, "subslot": 1, "module_ident": "0x00000100", "submodule_ident": "0x00000101",
     "type": "Pump", "direction": "output", "input_size": 0, "output_size": 4}
  ]
}
```

### ExpectedSubmoduleBlockReq

```
API 0:
  Slot 0, Module 0x00000001 (DAP):
    Subslot 0x0001, Submod 0x00000001, NO_IO
    Subslot 0x8000, Submod 0x00000100, NO_IO
    Subslot 0x8001, Submod 0x00000200, NO_IO
  Slot 1, Module 0x00000010 (pH):
    Subslot 0x0001, Submod 0x00000011, INPUT, DataLength=5, LengthIOPS=1, LengthIOCS=1
  Slot 2, Module 0x00000040 (Temperature):
    Subslot 0x0001, Submod 0x00000041, INPUT, DataLength=5, LengthIOPS=1, LengthIOCS=1
  Slot 3, Module 0x00000100 (Pump):
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

### Phase 1: HTTP Slot Discovery (Do First -- Unblocks Development)

- [ ] Fix DREP encoding to match declaration (all LE or all BE throughout stub)
- [ ] After DCP Identify: extract RTU IP address from DCP response
- [ ] HTTP GET `http://{rtu_ip}:9081/api/v1/slots`, timeout 5 seconds
- [ ] Handle HTTP 503: wait 2s, retry, max 5 retries
- [ ] Handle connection refused / timeout: go to Phase 3 DAP fallback
- [ ] Parse JSON response body
- [ ] Extract `slots` array, iterate each entry
- [ ] For each slot: parse `module_ident` and `submodule_ident` hex strings to uint32 values
- [ ] Validate: slot 0 must be DAP (module_ident 0x00000001)
- [ ] Build ExpectedSubmoduleBlockReq:
  - [ ] Slot 0 DAP with 3 submodules (0x0001, 0x8000, 0x8001), all NO_IO
  - [ ] Each app slot with 1 submodule, correct DataDescription per direction
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
- [ ] On success: enter cyclic exchange
- [ ] Optional: HTTP GET `/api/v1/gsdml` once, cache the XML by station_name

### Phase 2: ModuleDiff Tolerance (Implement for Robustness)

- [ ] After every Connect Response: check for ModuleDiffBlock
- [ ] If present: parse each module/submodule entry
- [ ] For NO_MODULE: remove slot from cyclic IO map
- [ ] For WRONG_MODULE: log, remove or adapt
- [ ] For SUBSTITUTE: accept, do not error
- [ ] Recalculate frame offsets after removing slots
- [ ] Enter cyclic exchange with adjusted IO map

### Phase 3: DAP Fallback (When HTTP Unavailable)

- [ ] Build DAP-only ExpectedSubmoduleBlockReq (1 slot, 3 submodules)
- [ ] Build minimal IOCRBlockReq (no app data)
- [ ] Send Connect
- [ ] Accept ModuleDiffBlock (expected: many missing modules)
- [ ] Issue Record Read: API=0, Slot=0, Subslot=1, Index=0xF844
- [ ] Parse binary response: uint16 BE count, then 15-byte BE entries
- [ ] Map parsed entries to slot config (same as HTTP JSON fields)
- [ ] Send Release
- [ ] Wait 2 seconds (p-net AR reset time)
- [ ] Build full ExpectedSubmoduleBlockReq from parsed data
- [ ] Build full IOCRBlockReqs from parsed data
- [ ] Send second Connect
- [ ] Parse response (check errors + ModuleDiff)
- [ ] Enter cyclic exchange
