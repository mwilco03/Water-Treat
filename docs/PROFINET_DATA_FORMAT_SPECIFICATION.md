# PROFINET Cyclic Data Format Specification

## Document Control

| Property | Value |
|----------|-------|
| Document ID | WT-SPEC-001 |
| Version | 1.0 |
| Status | APPROVED |
| Applies To | Water-Treat (RTU), Water-Controller |
| Effective Date | 2024-12-22 |

---

## 1. Purpose and Scope

This specification defines the authoritative format for cyclic I/O data exchanged between Water-Treat RTU (PROFINET I/O Device) and Water-Controller (PROFINET IO Controller). This document resolves prior inconsistencies in documentation and establishes enforceable standards with traceable compliance to IEC/PI specifications.

**Affected Systems:**
- Water-Treat: PROFINET I/O Device, sensor/actuator data producer
- Water-Controller: PROFINET IO Controller, data consumer, HMI, historian

**Supersedes:**
- Any prior documentation stating "4 bytes" or "5 bytes" without this specification's authority
- Informal references in README.md, OPERATOR.md, or code comments

---

## 2. Normative References

The following standards and specifications are normative for this document:

| Reference ID | Document | Version | Authority |
|--------------|----------|---------|-----------|
| [IEC-61158-5] | IEC 61158-5-10 Industrial communication networks - Fieldbus specifications - Part 5-10: Application layer service definition - Type 10 elements | Ed 4.0 (2023) | IEC |
| [IEC-61158-6] | IEC 61158-6-10 Industrial communication networks - Fieldbus specifications - Part 6-10: Application layer protocol specification - Type 10 elements | Ed 4.0 (2023) | IEC |
| [IEC-61784-2] | IEC 61784-2-3 Industrial communication networks - Profiles - Part 2-3: Additional fieldbus profiles for real-time networks based on ISO/IEC/IEEE 8802-3 - CPF 3 | Ed 5.0 (2023) | IEC |
| [GSDML-SPEC] | GSDML Specification for PROFINET V2.44 | V2.44 | PI (PROFIBUS & PROFINET International) |
| [PI-PROFINET] | PROFINET System Description - Technology and Application | 2018 Update | PI |
| [PI-PROFIDRIVE] | PROFIdrive - Profile Drive Technology V4.2 | V4.2 | PI |
| [IEEE-754] | IEEE 754-2019 Standard for Floating-Point Arithmetic | 2019 | IEEE |

---

## 3. Definitions

| Term | Definition |
|------|------------|
| **Cyclic Data** | Process data exchanged periodically between IO Controller and IO Device within an IOCR (IO Communication Relationship) |
| **IOPS** | IO Provider Status - protocol-level status byte indicating validity of produced data (per [IEC-61158-6] Section 5.2.6) |
| **IOCS** | IO Consumer Status - protocol-level status byte indicating consumer acknowledgment |
| **Application Quality** | Measurement-level quality indicator embedded in user data, distinct from protocol-level IOPS |
| **Submodule** | Smallest addressable unit in PROFINET device model containing I/O data (per [PI-PROFINET] Section 2.2) |

---

## 4. Design Decision Record

### 4.1 Decision Statement

**DECISION:** Sensor input data SHALL use a 5-byte format consisting of a 4-byte IEEE 754 Float32 value followed by a 1-byte application-level quality indicator.

### 4.2 Rationale

PROFINET defines two distinct quality mechanisms serving different purposes:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    PROFINET QUALITY ARCHITECTURE                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  PROTOCOL LAYER (Mandatory per [IEC-61158-6])                               │
│  ─────────────────────────────────────────────                              │
│  IOPS/IOCS: 1 byte per subslot, handled by PROFINET stack                   │
│                                                                              │
│  Semantics: "Is the communication channel functioning?"                      │
│             "Is the submodule producing/consuming data?"                     │
│                                                                              │
│  Values per [IEC-61158-6] Table 586:                                        │
│    0x80 = GOOD (data valid, provider running)                               │
│    0x00 = BAD (data invalid or provider stopped)                            │
│                                                                              │
│  ───────────────────────────────────────────────────────────────────────    │
│                                                                              │
│  APPLICATION LAYER (Optional, manufacturer-defined)                          │
│  ─────────────────────────────────────────────────                          │
│  Embedded in user data per [GSDML-SPEC] Section 8.12                        │
│                                                                              │
│  Semantics: "What is the quality of this specific measurement?"              │
│             "What is the sensor's diagnostic state?"                         │
│                                                                              │
│  Precedent: [PI-PROFIDRIVE] Status Word (ZSW1) embeds operational           │
│             status in cyclic data independent of IOPS                        │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Key Distinction:**

| Scenario | IOPS | Application Quality | Interpretation |
|----------|------|---------------------|----------------|
| Normal operation | GOOD | GOOD | Sensor healthy, data valid |
| Sensor calibration drift | GOOD | UNCERTAIN | Communication OK, measurement degraded |
| Sensor hardware failure | GOOD | BAD | RTU online, sensor failed |
| RTU communication loss | BAD | N/A | Cannot read data at all |

These are NOT redundant - they convey orthogonal information.

### 4.3 Standards Compliance

| Requirement | Specification Reference | Compliance |
|-------------|------------------------|------------|
| User data content is manufacturer-defined | [IEC-61158-5] Section 5.2.2 | ✓ COMPLIANT |
| OctetString type supports arbitrary length | [GSDML-SPEC] Table 18 | ✓ COMPLIANT |
| Multiple DataItem elements per submodule | [GSDML-SPEC] Section 8.12 | ✓ COMPLIANT |
| Embedded status in cyclic data | [PI-PROFIDRIVE] Section 6.3 (precedent) | ✓ COMPLIANT |
| Big-endian byte order for multi-byte values | [IEC-61158-6] Section 4.10.3.3 | ✓ COMPLIANT |

### 4.4 Precedent

The [PI-PROFIDRIVE] profile, deployed on millions of certified devices, embeds application-level status in every cyclic telegram:

```
PROFIdrive Standard Telegram 1:
  Bytes 0-1:  STW1 (Control Word) - 16 status/control bits
  Bytes 2-3:  ZSW1 (Status Word) - includes fault, warning, ready bits
  Bytes 4-7:  NSOLL_A (Speed setpoint)
  Bytes 8-11: NIST_A (Actual speed)
```

This establishes clear precedent that application-level status embedded in user data is standard practice, not a proprietary extension.

---

## 5. Data Format Specification

### 5.1 Sensor Input Data Format

Each sensor submodule SHALL produce 5 bytes of input data:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  SENSOR INPUT DATA FORMAT (5 bytes)                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Byte:    │  0   │  1   │  2   │  3   │  4       │                          │
│           ├──────┴──────┴──────┴──────┼──────────┤                          │
│  Content: │      Float32 Value        │ Quality  │                          │
│           │      (Big-Endian)         │  Byte    │                          │
│           └───────────────────────────┴──────────┘                          │
│                                                                              │
│  Float32 Value (Bytes 0-3):                                                  │
│    - IEEE 754 single-precision floating-point [IEEE-754]                    │
│    - Big-endian byte order (MSB first) per [IEC-61158-6]                    │
│    - Contains sensor measurement in engineering units                        │
│                                                                              │
│  Quality Byte (Byte 4):                                                      │
│    - Application-level quality indicator                                     │
│    - Encoding per Section 5.2                                                │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 5.2 Quality Byte Encoding

The quality byte SHALL use the following encoding, aligned with OPC UA StatusCode major categories for interoperability:

| Value | Constant | Meaning | Visual Indication |
|-------|----------|---------|-------------------|
| 0x00 | QUALITY_GOOD | Fresh, valid measurement | Normal display |
| 0x40 | QUALITY_UNCERTAIN | Measurement may be degraded (stale, calibration drift, range limit) | Yellow background |
| 0x80 | QUALITY_BAD | Sensor failure, measurement invalid | Red background, "X" indicator |
| 0xC0 | QUALITY_NOT_CONNECTED | No communication with sensor | Grey/disabled, "?" indicator |

**Bit Layout:**

```
  Bit 7   Bit 6   Bits 5-0
┌───────┬───────┬──────────┐
│   B   │   U   │ Reserved │
└───────┴───────┴──────────┘

B=0, U=0: GOOD          (0x00)
B=0, U=1: UNCERTAIN     (0x40)
B=1, U=0: BAD           (0x80)
B=1, U=1: NOT_CONNECTED (0xC0)
```

### 5.3 Actuator Output Data Format

Each actuator submodule SHALL consume 2 bytes of output data:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  ACTUATOR OUTPUT DATA FORMAT (2 bytes)                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Byte:    │  0        │  1        │                                         │
│           ├───────────┼───────────┤                                         │
│  Content: │  Command  │  Reserved │                                         │
│           └───────────┴───────────┘                                         │
│                                                                              │
│  Command Byte (Byte 0):                                                      │
│    0x00 = OFF                                                                │
│    0x01 = ON                                                                 │
│    0x02-0xFF = Reserved for future use (PWM, speed control)                 │
│                                                                              │
│  Reserved (Byte 1):                                                          │
│    SHALL be set to 0x00 by controller                                        │
│    SHALL be ignored by RTU                                                   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 5.4 IOPS Handling

Independent of application quality, the PROFINET stack SHALL set IOPS per [IEC-61158-6]:

| RTU State | IOPS Value | Meaning |
|-----------|------------|---------|
| Normal operation | 0x80 (GOOD) | Submodule data valid |
| Initialization incomplete | 0x00 (BAD) | Data not yet available |
| Hardware fault affecting all sensors | 0x00 (BAD) | Submodule inoperative |

**Relationship to Application Quality:**

```
IOPS is set at submodule level by the PROFINET stack.
Application quality is set per-sensor by application logic.

Valid combinations:
  IOPS=GOOD, Quality=GOOD        → Normal operation
  IOPS=GOOD, Quality=UNCERTAIN   → Sensor degraded, RTU healthy
  IOPS=GOOD, Quality=BAD         → Sensor failed, RTU reports failure
  IOPS=BAD,  Quality=N/A         → RTU/submodule fault, data invalid
```

---

## 6. GSDML Requirements

### 6.1 Sensor Submodule Declaration

The GSDML file SHALL declare sensor submodules with 5-byte input data using one of these valid constructs:

**Option A: Single OctetString (Recommended)**

```xml
<SubmoduleItem ID="sensor_pH" SubmoduleIdentNumber="0x00000010">
    <IOData>
        <Input>
            <DataItem DataType="OctetString" Length="5" TextId="IDT_SensorData_pH">
                <!-- Bytes 0-3: Float32 value (big-endian), Byte 4: Quality -->
            </DataItem>
        </Input>
    </IOData>
    <!-- ... -->
</SubmoduleItem>
```

**Option B: Separate DataItems (More Semantic)**

```xml
<SubmoduleItem ID="sensor_pH" SubmoduleIdentNumber="0x00000010">
    <IOData>
        <Input>
            <DataItem DataType="Float32" TextId="IDT_pH_Value"/>
            <DataItem DataType="Unsigned8" TextId="IDT_pH_Quality"/>
        </Input>
    </IOData>
    <!-- ... -->
</SubmoduleItem>
```

### 6.2 Text Definitions

```xml
<Text TextId="IDT_SensorData_pH" Value="pH Sensor Data (Float32 + Quality)"/>
<Text TextId="IDT_pH_Value" Value="pH Measurement Value"/>
<Text TextId="IDT_pH_Quality" Value="pH Data Quality (0=Good, 64=Uncertain, 128=Bad, 192=NotConnected)"/>
```

### 6.3 GSDML Validation

The GSDML file SHALL pass validation using the PI GSDML Checker tool with zero errors.

---

## 7. Implementation Requirements

### 7.1 Water-Treat (RTU) Requirements

| ID | Requirement | Verification |
|----|-------------|--------------|
| RTU-001 | RTU SHALL produce 5 bytes of input data per sensor submodule | Unit test, Wireshark capture |
| RTU-002 | Bytes 0-3 SHALL contain IEEE 754 Float32 in big-endian order | Unit test with known values |
| RTU-003 | Byte 4 SHALL contain quality code per Section 5.2 | Unit test per quality state |
| RTU-004 | RTU SHALL set IOPS via `pnet_input_set_data_and_iops()` | Code review, integration test |
| RTU-005 | Quality SHALL reflect actual sensor state, not default to GOOD | Fault injection test |
| RTU-006 | GSDML SHALL declare 5-byte input per Section 6.1 | GSDML Checker validation |

### 7.2 Water-Controller Requirements

| ID | Requirement | Verification |
|----|-------------|--------------|
| CTL-001 | Controller SHALL expect 5 bytes of input data per sensor | Unit test, integration test |
| CTL-002 | Controller SHALL parse bytes 0-3 as big-endian Float32 | Unit test with known values |
| CTL-003 | Controller SHALL extract and propagate quality from byte 4 | Unit test per quality state |
| CTL-004 | Controller SHALL read IOPS via p-net API | Code review |
| CTL-005 | HMI SHALL visually distinguish quality states per Section 5.2 | UI test |
| CTL-006 | Historian SHALL store quality alongside value | Database query verification |
| CTL-007 | Alarms SHALL NOT trigger on BAD quality data values | Alarm logic test |

### 7.3 Cross-System Compliance Matrix

| Check | Water-Treat | Water-Controller | Status |
|-------|-------------|------------------|--------|
| Input data size | `slot->input_size = 5` | `expected_size = 5` | VERIFY |
| Byte order | `htonl()` for float | `ntohl()` for float | VERIFY |
| Quality byte offset | Byte 4 | Byte 4 | VERIFY |
| Quality encoding | Per Section 5.2 | Per Section 5.2 | VERIFY |
| GSDML match | Declares 5 bytes | Imports same GSDML | VERIFY |

---

## 8. Migration Path

### 8.1 From 4-Byte Format

Systems currently using 4-byte format (Float32 only) SHALL migrate as follows:

1. **Update Water-Treat RTU:**
   - Modify `profinet_manager.c` to produce 5-byte data
   - Implement quality derivation in sensor read path
   - Update GSDML to declare 5 bytes

2. **Update Water-Controller:**
   - Modify data parsing to expect 5 bytes
   - Add quality extraction logic
   - Update HMI to display quality indicators
   - Update historian schema if needed

3. **Deployment Order:**
   - Controller MUST be updated before or simultaneously with RTU
   - Deploying new RTU against old Controller will cause data misalignment

### 8.2 Backwards Compatibility

This specification does NOT provide backwards compatibility with 4-byte format. Both systems must be updated together.

---

## 9. Compliance Verification

### 9.1 Required Tests

| Test ID | Description | Pass Criteria |
|---------|-------------|---------------|
| COMP-001 | Wireshark capture of cyclic frame | 5 bytes visible in input data area |
| COMP-002 | Float value encoding | Known float values decode correctly |
| COMP-003 | Quality propagation end-to-end | All 4 quality states display correctly in HMI |
| COMP-004 | IOPS independent of quality | IOPS=GOOD when quality=BAD for sensor fault |
| COMP-005 | GSDML validation | PI GSDML Checker returns zero errors |
| COMP-006 | Interoperability | RTU connects to third-party controller (e.g., Siemens S7) |

### 9.2 Certification Readiness

While this system is not currently seeking PI certification, adherence to this specification ensures:

- GSDML conforms to [GSDML-SPEC] V2.4x
- Data encoding conforms to [IEC-61158-6]
- Quality model aligns with established profiles ([PI-PROFIDRIVE])
- System could pursue certification with minimal modification

---

## 10. Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2024-12-22 | System Architect | Initial specification resolving 4-byte vs 5-byte ambiguity |

---

## Appendix A: Code Examples

### A.1 RTU: Producing Sensor Data (C)

```c
#include <arpa/inet.h>  // for htonl

typedef enum {
    QUALITY_GOOD          = 0x00,
    QUALITY_UNCERTAIN     = 0x40,
    QUALITY_BAD           = 0x80,
    QUALITY_NOT_CONNECTED = 0xC0,
} data_quality_t;

typedef struct {
    float value;
    data_quality_t quality;
} sensor_reading_t;

/**
 * @brief Pack sensor reading into PROFINET cyclic data buffer.
 * 
 * @param[in]  reading  Sensor reading with value and quality
 * @param[out] buffer   5-byte output buffer for PROFINET data
 * 
 * @note Buffer must be at least 5 bytes.
 * @note Float is converted to big-endian per IEC 61158-6.
 */
void pack_sensor_data(const sensor_reading_t *reading, uint8_t *buffer)
{
    // Convert float to big-endian
    uint32_t float_bits;
    memcpy(&float_bits, &reading->value, sizeof(float_bits));
    float_bits = htonl(float_bits);
    
    // Pack into buffer: bytes 0-3 = float, byte 4 = quality
    memcpy(buffer, &float_bits, 4);
    buffer[4] = (uint8_t)reading->quality;
}
```

### A.2 Controller: Parsing Sensor Data (C)

```c
#include <arpa/inet.h>  // for ntohl

/**
 * @brief Unpack sensor reading from PROFINET cyclic data buffer.
 * 
 * @param[in]  buffer   5-byte input buffer from PROFINET
 * @param[out] reading  Parsed sensor reading
 * 
 * @return 0 on success, -1 if buffer is NULL
 */
int unpack_sensor_data(const uint8_t *buffer, sensor_reading_t *reading)
{
    if (buffer == NULL || reading == NULL) {
        return -1;
    }
    
    // Extract big-endian float from bytes 0-3
    uint32_t float_bits;
    memcpy(&float_bits, buffer, 4);
    float_bits = ntohl(float_bits);
    memcpy(&reading->value, &float_bits, sizeof(float));
    
    // Extract quality from byte 4
    reading->quality = (data_quality_t)buffer[4];
    
    return 0;
}
```

### A.3 Controller: Quality-Aware Processing (Python)

```python
from enum import IntEnum
from dataclasses import dataclass
from datetime import datetime

class DataQuality(IntEnum):
    GOOD = 0x00
    UNCERTAIN = 0x40
    BAD = 0x80
    NOT_CONNECTED = 0xC0

@dataclass
class QualifiedSensorReading:
    """Sensor reading with quality metadata."""
    value: float
    quality: DataQuality
    timestamp: datetime
    source_rtu: str
    slot: int

def should_trigger_alarm(reading: QualifiedSensorReading, 
                         high_limit: float) -> bool:
    """
    Determine if reading should trigger high alarm.
    
    Per ISA-18.2: Do not alarm on bad quality data.
    """
    if reading.quality in (DataQuality.BAD, DataQuality.NOT_CONNECTED):
        # Bad quality - suppress value-based alarms
        # A separate "sensor fault" alarm may be raised instead
        return False
    
    return reading.value > high_limit
```

---

## Appendix B: GSDML Template

```xml
<?xml version="1.0" encoding="utf-8"?>
<ISO15745Profile xmlns="http://www.profibus.com/GSDML/2003/11/DeviceProfile"
                 xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
                 xsi:schemaLocation="http://www.profibus.com/GSDML/2003/11/DeviceProfile
                                     GSDML-V2.4-Schema.xsd">
    
    <ProfileHeader>
        <ProfileIdentification>PROFINET Device Profile</ProfileIdentification>
        <ProfileRevision>1.00</ProfileRevision>
        <ProfileName>Water-Treat RTU</ProfileName>
    </ProfileHeader>
    
    <ProfileBody>
        <DeviceIdentity VendorID="0xFFFF" DeviceID="0x0001">
            <InfoText TextId="IDT_DeviceInfo"/>
            <VendorName Value="Water-Treat"/>
        </DeviceIdentity>
        
        <ApplicationProcess>
            <DeviceAccessPointList>
                <DeviceAccessPointItem ID="DAP_1" PhysicalSlots="0..8">
                    <!-- DAP configuration -->
                </DeviceAccessPointItem>
            </DeviceAccessPointList>
            
            <ModuleList>
                <!-- Sensor Module: 5 bytes input (Float32 + Quality) -->
                <ModuleItem ID="MOD_Sensor_pH" ModuleIdentNumber="0x00000010">
                    <ModuleInfo>
                        <Name TextId="IDT_MOD_Sensor_pH"/>
                        <InfoText TextId="IDT_MOD_Sensor_pH_Info"/>
                    </ModuleInfo>
                    <VirtualSubmoduleList>
                        <VirtualSubmoduleItem ID="SUB_Sensor_pH" 
                                              SubmoduleIdentNumber="0x00000001"
                                              MayIssueProcessAlarm="true">
                            <IOData>
                                <Input>
                                    <DataItem DataType="Float32" 
                                              TextId="IDT_pH_Value"/>
                                    <DataItem DataType="Unsigned8" 
                                              TextId="IDT_pH_Quality"/>
                                </Input>
                            </IOData>
                        </VirtualSubmoduleItem>
                    </VirtualSubmoduleList>
                </ModuleItem>
                
                <!-- Actuator Module: 2 bytes output -->
                <ModuleItem ID="MOD_Actuator_Pump" ModuleIdentNumber="0x00000020">
                    <ModuleInfo>
                        <Name TextId="IDT_MOD_Actuator_Pump"/>
                    </ModuleInfo>
                    <VirtualSubmoduleList>
                        <VirtualSubmoduleItem ID="SUB_Actuator_Pump"
                                              SubmoduleIdentNumber="0x00000001">
                            <IOData>
                                <Output>
                                    <DataItem DataType="Unsigned8" 
                                              TextId="IDT_Pump_Command"/>
                                    <DataItem DataType="Unsigned8" 
                                              TextId="IDT_Reserved"/>
                                </Output>
                            </IOData>
                        </VirtualSubmoduleItem>
                    </VirtualSubmoduleList>
                </ModuleItem>
            </ModuleList>
        </ApplicationProcess>
        
        <ExternalTextList>
            <PrimaryLanguage>
                <Text TextId="IDT_DeviceInfo" Value="Water Treatment RTU"/>
                <Text TextId="IDT_MOD_Sensor_pH" Value="pH Sensor Module"/>
                <Text TextId="IDT_MOD_Sensor_pH_Info" 
                      Value="pH sensor with quality indication (5 bytes: Float32 + Quality)"/>
                <Text TextId="IDT_pH_Value" Value="pH Value (Float32, big-endian)"/>
                <Text TextId="IDT_pH_Quality" 
                      Value="Quality (0x00=Good, 0x40=Uncertain, 0x80=Bad, 0xC0=NotConnected)"/>
                <Text TextId="IDT_MOD_Actuator_Pump" Value="Pump Actuator Module"/>
                <Text TextId="IDT_Pump_Command" Value="Command (0=Off, 1=On)"/>
                <Text TextId="IDT_Reserved" Value="Reserved (set to 0)"/>
            </PrimaryLanguage>
        </ExternalTextList>
    </ProfileBody>
</ISO15745Profile>
```

---

*This specification is authoritative for the Water-Treat/Water-Controller system. All implementations SHALL conform to these requirements.*
