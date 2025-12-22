# Cross-System Development Guidelines Addendum

## PROFINET Data Exchange Standards

**Document ID:** WT-GUIDE-002  
**Applies To:** Water-Treat (RTU), Water-Controller  
**Authority:** WT-SPEC-001 (PROFINET Data Format Specification)  

---

## Preamble

This addendum establishes enforceable cross-system standards for PROFINET data exchange between Water-Treat RTU and Water-Controller. These guidelines supplement the system-specific DEVELOPMENT_GUIDELINES.md in each repository.

**Core Principle:** The two codebases are coupled at the wire protocol level. Changes to data format, byte ordering, or quality semantics in one system REQUIRE coordinated changes in the other. Unilateral changes will cause silent data corruption.

---

## Part 1: Normative Requirements

### 1.1 Sensor Data Format (RTU → Controller)

Per [WT-SPEC-001] Section 5.1, sensor data SHALL be 5 bytes:

```
┌─────────────────────────────────────────────────────────────────┐
│  CANONICAL SENSOR DATA FORMAT                                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Offset  │ Size │ Content              │ Encoding               │
│  ────────┼──────┼──────────────────────┼──────────────────────  │
│  0       │ 4    │ Sensor Value         │ IEEE 754 Float32       │
│          │      │                      │ Big-endian (network)   │
│  4       │ 1    │ Quality Indicator    │ Per Section 1.2        │
│                                                                  │
│  Total: 5 bytes per sensor submodule                            │
│                                                                  │
│  NORMATIVE REFERENCES:                                           │
│    [IEC-61158-6] Section 4.10.3.3 - Big-endian requirement      │
│    [IEEE-754] Section 3.4 - Binary32 interchange format         │
│    [GSDML-SPEC] Table 18 - Float32 data type                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 Quality Byte Encoding

| Value | Constant | C Enum | Python Enum | Meaning |
|-------|----------|--------|-------------|---------|
| 0x00 | QUALITY_GOOD | `QUALITY_GOOD` | `DataQuality.GOOD` | Valid measurement |
| 0x40 | QUALITY_UNCERTAIN | `QUALITY_UNCERTAIN` | `DataQuality.UNCERTAIN` | Degraded/stale |
| 0x80 | QUALITY_BAD | `QUALITY_BAD` | `DataQuality.BAD` | Sensor failure |
| 0xC0 | QUALITY_NOT_CONNECTED | `QUALITY_NOT_CONNECTED` | `DataQuality.NOT_CONNECTED` | No communication |

**Encoding Rationale:**
- Bit 7 (0x80): BAD flag
- Bit 6 (0x40): UNCERTAIN flag  
- Bits 0-5: Reserved for future substatus codes
- Aligns with OPC UA StatusCode major categories for interoperability

### 1.3 Protocol-Level Quality (IOPS)

IOPS is handled by the PROFINET stack and is SEPARATE from application quality:

| Mechanism | Scope | Set By | Meaning |
|-----------|-------|--------|---------|
| IOPS | Per-subslot | p-net stack via API | Communication health |
| Quality byte | Per-sensor | Application logic | Measurement quality |

**IOPS SHALL be GOOD (0x80) when:**
- RTU is operational
- PROFINET AR is established
- Submodule is producing data

**IOPS SHALL be BAD (0x00) when:**
- RTU initialization incomplete
- Hardware fault affecting submodule
- AR termination in progress

---

## Part 2: Water-Treat (RTU) Implementation Standards

### 2.1 Required Type Definitions

**File:** `include/common.h`

```c
/**
 * @brief Data quality indicators per WT-SPEC-001 Section 5.2
 * 
 * Encoding aligned with OPC UA StatusCode for interoperability.
 * Bit 7 = BAD, Bit 6 = UNCERTAIN, Bits 0-5 = Reserved.
 * 
 * Reference: [IEC-62541-4] OPC UA Part 4: Services
 */
typedef enum {
    QUALITY_GOOD          = 0x00,  /**< Fresh, valid measurement */
    QUALITY_UNCERTAIN     = 0x40,  /**< Stale, degraded, or at limits */
    QUALITY_BAD           = 0x80,  /**< Sensor failure, invalid data */
    QUALITY_NOT_CONNECTED = 0xC0,  /**< No communication with sensor */
} data_quality_t;

/**
 * @brief Sensor reading with quality metadata.
 * 
 * This structure represents a single sensor measurement with
 * associated quality information for propagation through the
 * system per WT-SPEC-001.
 */
typedef struct {
    float value;              /**< Measured value in engineering units */
    data_quality_t quality;   /**< Quality indicator */
    uint64_t timestamp_us;    /**< Measurement timestamp (microseconds since boot) */
} sensor_reading_t;
```

### 2.2 PROFINET Data Packing

**File:** `src/profinet/profinet_manager.c`

```c
/**
 * @brief Pack sensor reading into PROFINET cyclic data buffer.
 * 
 * Converts sensor reading to wire format per WT-SPEC-001:
 *   - Float32 in big-endian (network byte order)
 *   - Quality byte appended
 * 
 * @param[in]  reading  Source sensor reading
 * @param[out] buffer   Destination buffer (must be >= 5 bytes)
 * @param[in]  size     Buffer size for bounds checking
 * 
 * @return Number of bytes written (5), or -1 on error
 * 
 * @pre reading != NULL
 * @pre buffer != NULL
 * @pre size >= 5
 * 
 * @note Thread safety: SAFE (no shared state)
 */
int pack_sensor_to_profinet(const sensor_reading_t *reading,
                            uint8_t *buffer,
                            size_t size)
{
    if (reading == NULL || buffer == NULL || size < 5) {
        return -1;
    }
    
    /* Convert float to big-endian per [IEC-61158-6] */
    uint32_t float_bits;
    memcpy(&float_bits, &reading->value, sizeof(float_bits));
    float_bits = htonl(float_bits);
    
    /* Pack: bytes 0-3 = float, byte 4 = quality */
    memcpy(buffer, &float_bits, 4);
    buffer[4] = (uint8_t)reading->quality;
    
    return 5;
}
```

### 2.3 Quality Derivation

Quality SHALL be derived from actual sensor state, not defaulted:

```c
/**
 * @brief Derive quality indicator from sensor diagnostic state.
 * 
 * Quality derivation rules per WT-SPEC-001:
 *   1. Hardware fault → BAD
 *   2. Communication timeout → NOT_CONNECTED
 *   3. Value at range limit → UNCERTAIN
 *   4. Stale (age > threshold) → UNCERTAIN
 *   5. Otherwise → GOOD
 * 
 * @param[in] sensor  Sensor instance with diagnostic state
 * @return Appropriate quality indicator
 */
data_quality_t derive_sensor_quality(const sensor_instance_t *sensor)
{
    if (sensor == NULL) {
        return QUALITY_NOT_CONNECTED;
    }
    
    /* Check hardware fault flags */
    if (sensor->fault_flags & SENSOR_FAULT_HARDWARE) {
        return QUALITY_BAD;
    }
    
    /* Check communication state */
    if (sensor->fault_flags & SENSOR_FAULT_COMM_TIMEOUT) {
        return QUALITY_NOT_CONNECTED;
    }
    
    /* Check for range limiting */
    if (sensor->value <= sensor->range_min || 
        sensor->value >= sensor->range_max) {
        return QUALITY_UNCERTAIN;
    }
    
    /* Check staleness */
    uint64_t now = get_monotonic_time_us();
    uint64_t age_ms = (now - sensor->last_update_us) / 1000;
    if (age_ms > sensor->stale_threshold_ms) {
        return QUALITY_UNCERTAIN;
    }
    
    return QUALITY_GOOD;
}
```

### 2.4 GSDML Requirements

**File:** `gsd/GSDML-V2.4-WaterTreat-RTU-*.xml`

Each sensor submodule MUST declare 5 bytes of input data:

```xml
<!-- CORRECT: 5 bytes per WT-SPEC-001 -->
<VirtualSubmoduleItem ID="sensor_pH" SubmoduleIdentNumber="0x00000010">
    <IOData>
        <Input>
            <DataItem DataType="Float32" TextId="IDT_pH_Value"/>
            <DataItem DataType="Unsigned8" TextId="IDT_pH_Quality"/>
        </Input>
    </IOData>
</VirtualSubmoduleItem>

<!-- PROHIBITED: Old 4-byte format -->
<!-- <DataItem DataType="Float32" TextId="IDT_pH"/> -->
```

### 2.5 Compliance Checklist (RTU)

```
WATER-TREAT PRE-COMMIT VERIFICATION
═══════════════════════════════════════════════════════════════════

DATA FORMAT COMPLIANCE
  [ ] data_quality_t enum defined in common.h with correct values
  [ ] sensor_reading_t struct includes quality field
  [ ] pack_sensor_to_profinet() produces exactly 5 bytes
  [ ] Float conversion uses htonl() for big-endian
  [ ] Quality byte placed at offset 4

QUALITY DERIVATION
  [ ] derive_sensor_quality() implemented (not stub)
  [ ] All quality states reachable (GOOD, UNCERTAIN, BAD, NOT_CONNECTED)
  [ ] Quality reflects actual sensor state (not hardcoded GOOD)
  [ ] Fault injection tests verify quality transitions

GSDML ALIGNMENT
  [ ] GSDML declares 5 bytes input per sensor submodule
  [ ] GSDML validated with PI Checker (zero errors)
  [ ] GSDML version date updated
  [ ] GSDML file name matches convention

PROFINET STACK INTEGRATION
  [ ] slot->input_size = 5 (not 4)
  [ ] pnet_input_set_data_and_iops() called correctly
  [ ] IOPS set to GOOD when submodule operational

═══════════════════════════════════════════════════════════════════
```

---

## Part 3: Water-Controller Implementation Standards

### 3.1 Required Type Definitions

**File:** `include/profinet/data_types.h` (C controller core)

```c
/**
 * @brief Data quality indicators per WT-SPEC-001 Section 5.2
 * 
 * MUST match Water-Treat RTU encoding exactly.
 */
typedef enum {
    QUALITY_GOOD          = 0x00,
    QUALITY_UNCERTAIN     = 0x40,
    QUALITY_BAD           = 0x80,
    QUALITY_NOT_CONNECTED = 0xC0,
} data_quality_t;

typedef struct {
    float value;
    data_quality_t quality;
    uint64_t timestamp_us;
    uint16_t source_slot;
    char source_rtu[64];
} qualified_sensor_value_t;
```

**File:** `src/api/models/sensor.py` (Python API)

```python
from enum import IntEnum
from dataclasses import dataclass
from datetime import datetime

class DataQuality(IntEnum):
    """
    Data quality indicators per WT-SPEC-001 Section 5.2.
    
    MUST match Water-Treat RTU encoding exactly.
    """
    GOOD = 0x00
    UNCERTAIN = 0x40
    BAD = 0x80
    NOT_CONNECTED = 0xC0

@dataclass(frozen=True)
class QualifiedSensorValue:
    """Immutable sensor value with quality metadata."""
    value: float
    quality: DataQuality
    timestamp: datetime
    source_rtu: str
    slot: int
    
    @property
    def is_usable(self) -> bool:
        """True if value can be used for control/alarming."""
        return self.quality in (DataQuality.GOOD, DataQuality.UNCERTAIN)
```

### 3.2 PROFINET Data Unpacking

**File:** `src/profinet/profinet_manager.c`

```c
/**
 * @brief Unpack sensor data from PROFINET cyclic buffer.
 * 
 * Parses wire format per WT-SPEC-001:
 *   - Bytes 0-3: Float32, big-endian
 *   - Byte 4: Quality indicator
 * 
 * @param[in]  buffer   Source buffer from PROFINET (>= 5 bytes)
 * @param[in]  size     Buffer size for validation
 * @param[out] value    Parsed sensor value
 * 
 * @return 0 on success, -1 on error
 */
int unpack_sensor_from_profinet(const uint8_t *buffer,
                                 size_t size,
                                 qualified_sensor_value_t *value)
{
    if (buffer == NULL || value == NULL || size < 5) {
        return -1;
    }
    
    /* Extract big-endian float from bytes 0-3 */
    uint32_t float_bits;
    memcpy(&float_bits, buffer, 4);
    float_bits = ntohl(float_bits);
    memcpy(&value->value, &float_bits, sizeof(float));
    
    /* Extract quality from byte 4 */
    value->quality = (data_quality_t)buffer[4];
    
    /* Validate quality code */
    switch (value->quality) {
        case QUALITY_GOOD:
        case QUALITY_UNCERTAIN:
        case QUALITY_BAD:
        case QUALITY_NOT_CONNECTED:
            break;
        default:
            /* Unknown quality code - treat as BAD */
            log_warn("Unknown quality code 0x%02X, treating as BAD", 
                     buffer[4]);
            value->quality = QUALITY_BAD;
            break;
    }
    
    value->timestamp_us = get_monotonic_time_us();
    
    return 0;
}
```

### 3.3 Quality Propagation Requirements

Quality MUST be propagated through all system layers:

```
┌─────────────────────────────────────────────────────────────────┐
│                    QUALITY PROPAGATION PATH                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  PROFINET Frame                                                  │
│       │                                                          │
│       ▼                                                          │
│  C Controller (unpack_sensor_from_profinet)                      │
│       │                                                          │
│       ▼                                                          │
│  Shared Memory (include quality in struct)                       │
│       │                                                          │
│       ├──────────────────┬──────────────────┐                   │
│       ▼                  ▼                  ▼                   │
│  Python API         Historian           Alarm Manager           │
│  (REST/WS)          (PostgreSQL)        (ISA-18.2)              │
│       │                  │                  │                   │
│       ▼                  ▼                  ▼                   │
│  JSON Response     quality column      suppress on BAD          │
│  includes quality  in historian_data   quality data             │
│       │                                                          │
│       ▼                                                          │
│  React HMI (visual indication per quality)                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.4 HMI Quality Visualization

**File:** `src/components/SensorDisplay.tsx`

```typescript
interface SensorDisplayProps {
    value: number;
    quality: 'GOOD' | 'UNCERTAIN' | 'BAD' | 'NOT_CONNECTED';
    units: string;
    label: string;
}

/**
 * Visual indication per WT-SPEC-001 Section 5.2:
 *   GOOD: Normal display
 *   UNCERTAIN: Yellow background
 *   BAD: Red background, "X" indicator
 *   NOT_CONNECTED: Grey/disabled, "?" indicator
 */
const qualityStyles: Record<string, React.CSSProperties> = {
    GOOD: {},
    UNCERTAIN: { backgroundColor: '#FFF3CD', borderColor: '#FFC107' },
    BAD: { backgroundColor: '#F8D7DA', borderColor: '#DC3545', color: '#721C24' },
    NOT_CONNECTED: { backgroundColor: '#E2E3E5', color: '#6C757D' },
};

const qualityIndicators: Record<string, string> = {
    GOOD: '',
    UNCERTAIN: '⚠',
    BAD: '✕',
    NOT_CONNECTED: '?',
};

export function SensorDisplay({ value, quality, units, label }: SensorDisplayProps) {
    const displayValue = quality === 'BAD' || quality === 'NOT_CONNECTED'
        ? '---'
        : value.toFixed(2);
    
    return (
        <div style={qualityStyles[quality]} className="sensor-display">
            <span className="sensor-label">{label}</span>
            <span className="sensor-value">
                {qualityIndicators[quality]} {displayValue} {units}
            </span>
        </div>
    );
}
```

### 3.5 Historian Quality Storage

**File:** `docker/init.sql`

```sql
-- Quality stored as integer per WT-SPEC-001 encoding
CREATE TABLE historian_data (
    time        TIMESTAMPTZ NOT NULL,
    rtu_name    TEXT NOT NULL,
    slot        INTEGER NOT NULL,
    value       DOUBLE PRECISION,
    quality     SMALLINT NOT NULL DEFAULT 0,  -- 0=GOOD, 64=UNCERTAIN, 128=BAD, 192=NOT_CONNECTED
    
    PRIMARY KEY (time, rtu_name, slot)
);

-- Index for quality-filtered queries
CREATE INDEX idx_historian_quality ON historian_data (quality) WHERE quality != 0;

-- Quality values reference
COMMENT ON COLUMN historian_data.quality IS 
    'Data quality per WT-SPEC-001: 0=GOOD, 64=UNCERTAIN, 128=BAD, 192=NOT_CONNECTED';
```

### 3.6 Alarm Suppression on Bad Quality

**File:** `src/alarms/alarm_manager.py`

```python
def evaluate_limit_alarm(
    reading: QualifiedSensorValue,
    alarm_config: AlarmConfiguration,
) -> Optional[AlarmEvent]:
    """
    Evaluate reading against alarm limits.
    
    Per ISA-18.2 and WT-SPEC-001: Suppress value-based alarms
    when data quality is BAD or NOT_CONNECTED. A separate
    "sensor fault" alarm may be raised instead.
    """
    # Do not alarm on unusable data
    if not reading.is_usable:
        logger.debug(
            "Suppressing alarm evaluation for %s/%d: quality=%s",
            reading.source_rtu, reading.slot, reading.quality.name
        )
        return None
    
    # Normal alarm evaluation
    if reading.value >= alarm_config.high_high_limit:
        return AlarmEvent(
            priority=AlarmPriority.CRITICAL,
            type=AlarmType.HIGH_HIGH,
            # ...
        )
    # ... etc
```

### 3.7 Compliance Checklist (Controller)

```
WATER-CONTROLLER PRE-COMMIT VERIFICATION
═══════════════════════════════════════════════════════════════════

DATA FORMAT COMPLIANCE
  [ ] data_quality_t enum matches RTU values exactly
  [ ] DataQuality Python enum matches RTU values exactly
  [ ] unpack_sensor_from_profinet() expects exactly 5 bytes
  [ ] Float conversion uses ntohl() for big-endian
  [ ] Quality extracted from byte 4

QUALITY PROPAGATION
  [ ] Shared memory struct includes quality field
  [ ] API response JSON includes quality field
  [ ] WebSocket messages include quality field
  [ ] Historian stores quality in database

HMI QUALITY DISPLAY
  [ ] All 4 quality states have distinct visual indication
  [ ] BAD/NOT_CONNECTED show placeholder instead of stale value
  [ ] Quality indicator visible without hovering

ALARM INTEGRATION
  [ ] Alarm evaluation checks quality before comparing values
  [ ] BAD quality suppresses value-based alarms
  [ ] Separate "sensor fault" alarm exists for BAD quality sensors

HISTORIAN
  [ ] quality column exists in historian_data table
  [ ] quality values match WT-SPEC-001 encoding
  [ ] Trend queries can filter by quality

═══════════════════════════════════════════════════════════════════
```

---

## Part 4: Cross-System Compliance Verification

### 4.1 Pre-Release Cross-Check

Before releasing either system, verify cross-compatibility:

```
CROSS-SYSTEM COMPLIANCE GATE
═══════════════════════════════════════════════════════════════════

BYTE-LEVEL ALIGNMENT
  [ ] Water-Treat input_size = 5
  [ ] Water-Controller expected input size = 5
  [ ] Both use htonl/ntohl for float conversion
  [ ] Quality byte at identical offset (4)

QUALITY ENCODING
  [ ] QUALITY_GOOD value identical (0x00)
  [ ] QUALITY_UNCERTAIN value identical (0x40)
  [ ] QUALITY_BAD value identical (0x80)
  [ ] QUALITY_NOT_CONNECTED value identical (0xC0)

GSDML CONSISTENCY
  [ ] Controller imports RTU's GSDML file
  [ ] GSDML declares 5-byte input
  [ ] Module/submodule IDs match expected configuration

INTEGRATION TESTS
  [ ] End-to-end test: RTU → Controller → HMI displays value
  [ ] Quality propagation test: All 4 states verified visually
  [ ] Wireshark capture confirms 5-byte payload

DEPLOYMENT COORDINATION
  [ ] Both systems updated in same release window
  [ ] Rollback plan documented for both systems
  [ ] Version compatibility matrix updated

═══════════════════════════════════════════════════════════════════
```

### 4.2 Wireshark Verification Procedure

```
PROCEDURE: Verify PROFINET Cyclic Data Format
═══════════════════════════════════════════════════════════════════

1. Capture Setup
   - Wireshark on PROFINET network interface
   - Filter: pn_rt (PROFINET Real-Time)
   - Identify Input IOCR frames from RTU

2. Frame Analysis
   - Locate cyclic data payload (after APDU header)
   - Identify submodule data by offset (from AR establishment)
   
3. Verify Sensor Data
   Expected structure per submodule:
   
   Offset  Bytes  Content
   ──────  ─────  ───────────────────────────
   0       4      Float32 value (big-endian)
   4       1      Quality byte
   5       1      IOPS (set by stack)
   
4. Verify Float Encoding
   - Inject known value (e.g., 7.0 pH)
   - Expected bytes: 0x40 0xE0 0x00 0x00 (7.0 in IEEE 754 BE)
   
5. Verify Quality States
   - Trigger each quality state on RTU
   - Confirm byte 4 changes: 0x00 → 0x40 → 0x80 → 0xC0

═══════════════════════════════════════════════════════════════════
```

---

## Part 5: Development Prompt Addendum

Add the following to system prompts for AI-assisted development:

```
PROFINET DATA EXCHANGE CONSTRAINTS (per WT-SPEC-001)
════════════════════════════════════════════════════════════════════

When working on Water-Treat RTU or Water-Controller code involving
PROFINET cyclic data:

DATA FORMAT:
- Sensor input: 5 bytes (4-byte Float32 big-endian + 1-byte quality)
- Actuator output: 2 bytes (1-byte command + 1-byte reserved)
- Byte order: Big-endian (network order) per IEC 61158-6
- Use htonl()/ntohl() for float byte swapping

QUALITY ENCODING:
- 0x00 = QUALITY_GOOD (valid measurement)
- 0x40 = QUALITY_UNCERTAIN (degraded/stale)
- 0x80 = QUALITY_BAD (sensor failure)
- 0xC0 = QUALITY_NOT_CONNECTED (no communication)

IOPS vs APPLICATION QUALITY:
- IOPS is protocol-level (handled by p-net stack)
- Quality byte is application-level (set by sensor logic)
- Both MUST be set independently - they are NOT redundant

CROSS-SYSTEM COUPLING:
- Changes to data format require coordinated updates to BOTH systems
- GSDML must match implementation (5 bytes per sensor)
- Controller must expect same byte layout as RTU produces

QUALITY PROPAGATION:
- Quality flows: RTU → Controller → Shared Memory → API → HMI
- Historian stores quality alongside value
- Alarms suppress on BAD/NOT_CONNECTED quality

COMPLIANCE REFERENCES:
- [IEC-61158-6] Section 4.10.3.3 (byte order)
- [GSDML-SPEC] Table 18 (data types)
- [PI-PROFIDRIVE] Section 6.3 (embedded status precedent)
- [WT-SPEC-001] (project specification)

════════════════════════════════════════════════════════════════════
```

---

## Appendix: Version Compatibility Matrix

| Water-Treat Version | Water-Controller Version | Data Format | Compatible |
|---------------------|--------------------------|-------------|------------|
| < 1.0.0 | < 1.0.0 | 4-byte (Float32 only) | Yes |
| ≥ 1.0.0 | < 1.0.0 | Mixed | **NO** |
| < 1.0.0 | ≥ 1.0.0 | Mixed | **NO** |
| ≥ 1.0.0 | ≥ 1.0.0 | 5-byte (Float32 + Quality) | Yes |

**Critical:** Version 1.0.0 introduces breaking wire format change. Both systems MUST be updated together.

---

*This addendum is authoritative for cross-system data exchange. Deviations require documented justification and updates to WT-SPEC-001.*
