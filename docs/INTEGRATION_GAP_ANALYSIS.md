# Water-Controller and Water-Treat Integration Gap Analysis

This document provides a comprehensive comparison between the **Water-Controller** (PROFINET IO Controller) and **Water-Treat** (PROFINET IO Device/RTU) codebases to identify integration gaps and missing functionality.

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Data Format Compatibility](#data-format-compatibility)
3. [Slot Numbering Conventions](#slot-numbering-conventions)
4. [PROFINET Protocol Alignment](#profinet-protocol-alignment)
5. [Alarm System Comparison](#alarm-system-comparison)
6. [Missing Features - Controller](#missing-features---controller)
7. [Missing Features - RTU](#missing-features---rtu)
8. [Integration Requirements](#integration-requirements)
9. [Recommendations](#recommendations)

---

## Architecture Overview

### Water-Controller (PROFINET IO Controller)
- **Role**: Central coordinator managing multiple RTU devices
- **Location**: `/home/user/Water-Controller`
- **Primary Functions**:
  - RTU device discovery and registration
  - Cyclic data exchange with multiple RTUs
  - PID control loops spanning multiple RTUs
  - Alarm management and ISA-18.2 compliance
  - Historian data logging
  - Modbus gateway for SCADA integration
  - Multi-RTU coordination (failover, load balancing, cascade control)

### Water-Treat (PROFINET IO Device/RTU)
- **Role**: Field device with sensors and actuators
- **Location**: `/home/user/Water-Treat`
- **Primary Functions**:
  - Sensor reading (physical, ADC, calculated, web-polled)
  - Actuator control (relay, PWM, latching, momentary)
  - Local PROFINET cyclic data provider
  - Standalone/degraded mode operation
  - LED status indicators
  - Local alarm rules
  - Terminal UI for local configuration

---

## Data Format Compatibility

### Sensor Data (RTU → Controller)

| Aspect | Water-Treat (RTU) | Water-Controller | Status |
|--------|-------------------|------------------|--------|
| Data Size | 4 bytes per sensor | 4 bytes per sensor | ✅ MATCH |
| Data Type | IEEE 754 float | IEEE 754 float | ✅ MATCH |
| Byte Order | Host order → p-net handles | Network order (ntohl) | ⚠️ CHECK |
| IOPS | 0x00=BAD, 0x80=GOOD | 0x00=BAD, 0x80=GOOD | ✅ MATCH |

**RTU Code** (`profinet_manager.h:37-38`):
```c
result_t profinet_manager_update_input_float(int slot, int subslot, float value);
```

**Controller Code** (`cyclic_exchange.c:179-216`):
```c
wtc_result_t get_slot_input_float(profinet_ar_t *ar, int slot, float *value, iops_t *status) {
    // Reads 4 bytes, applies ntohl for network byte order
    uint32_t int_val;
    memcpy(&int_val, ar->iocr[i].data_buffer + offset, 4);
    int_val = ntohl(int_val);
    memcpy(value, &int_val, 4);
}
```

### Actuator Data (Controller → RTU)

| Aspect | Water-Controller | Water-Treat (RTU) | Status |
|--------|------------------|-------------------|--------|
| Data Size | 4 bytes per actuator | 4 bytes per actuator | ✅ MATCH |
| Command byte | 0=OFF, 1=ON, 2=PWM | 0=OFF, 1=ON, 2=PWM | ✅ MATCH |
| PWM byte | 0-100 duty cycle | 0-100 duty cycle | ✅ MATCH |
| Reserved | 2 bytes | 2 bytes | ✅ MATCH |
| Packing | `__attribute__((packed))` | `__attribute__((packed))` | ✅ MATCH |

**Controller Code** (`types.h:229-233`):
```c
typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t pwm_duty;
    uint8_t reserved[2];
} actuator_output_t;
```

**RTU Code** (`actuator_manager.h:44-48`):
```c
typedef struct {
    uint8_t command;
    uint8_t pwm_duty;
    uint8_t reserved[2];
} __attribute__((packed)) actuator_output_data_t;
```

**⚠️ ISSUE**: Structure names differ (`actuator_output_t` vs `actuator_output_data_t`) but are binary compatible.

---

## Slot Numbering Conventions

### Controller Expectations (`cyclic_exchange.c`)

| Slot Range | Type | Data Size |
|------------|------|-----------|
| 1-8 | Sensors (input) | 4 bytes each = 32 bytes |
| 9-16 | Actuators (output) | 4 bytes each = 32 bytes |

**Controller Code** (`cyclic_exchange.c:191-196`):
```c
/* Calculate offset - slots 1-8 are sensors */
if (slot < 1 || slot > 8) {
    return WTC_ERROR_INVALID_PARAM;
}
size_t offset = (slot - 1) * 4;  /* 4 bytes per float */
```

**Controller Code** (`cyclic_exchange.c:230-235`):
```c
/* Calculate offset - slots 9-16 are actuators */
if (slot < 9 || slot > 16) {
    return WTC_ERROR_INVALID_PARAM;
}
size_t offset = (slot - 9) * 4;  /* 4 bytes per actuator */
```

### RTU Implementation (`sensor_instance.h`, `actuator_manager.h`)

| Aspect | RTU Implementation | Status |
|--------|-------------------|--------|
| Sensor slots | Configurable via `profinet_slot` field | ⚠️ CONFIGURABLE |
| Actuator slots | Configurable via `profinet_slot` field | ⚠️ CONFIGURABLE |
| Slot 0 | DAP (Device Access Point) | ✅ STANDARD |

**⚠️ GAP**: RTU allows flexible slot assignment, but Controller hardcodes slots 1-8 for sensors, 9-16 for actuators. RTU configuration **MUST** follow this convention.

---

## PROFINET Protocol Alignment

### Frame Types Supported

| Frame Type | Controller | RTU (via p-net) | Status |
|------------|------------|-----------------|--------|
| RT Class 1 (cyclic) | ✅ Yes | ✅ Yes | ✅ MATCH |
| RT Class 3 (IRT) | Defined | Not implemented | ❌ GAP |
| DCP Discovery | ✅ Yes | ✅ Via p-net | ✅ MATCH |
| DCP Set (IP config) | ✅ Yes | ✅ Via p-net | ✅ MATCH |
| Alarms | ✅ High/Low | ✅ Via p-net | ✅ MATCH |
| PTCP Sync | Defined | Not used | ❌ GAP |

### Connection State Mapping

| Controller State | RTU State | Mapping |
|-----------------|-----------|---------|
| `PROFINET_STATE_OFFLINE` | `PROFINET_STATE_IDLE` | ⚠️ DIFFERENT NAME |
| `PROFINET_STATE_DISCOVERY` | N/A (passive) | RTU doesn't discover |
| `PROFINET_STATE_CONNECTING` | `PROFINET_STATE_CONNECTING` | ✅ MATCH |
| `PROFINET_STATE_CONNECTED` | `PROFINET_STATE_CONNECTED` | ✅ MATCH |
| `PROFINET_STATE_RUNNING` | `PROFINET_STATE_READY` | ⚠️ DIFFERENT NAME |
| `PROFINET_STATE_ERROR` | `PROFINET_STATE_ERROR` | ✅ MATCH |
| `PROFINET_STATE_DISCONNECT` | N/A | Controller-only |

---

## Alarm System Comparison

### Controller Alarm System (`alarm_manager.h`)

**Features**:
- ISA-18.2 compliant states (ACTIVE_UNACK, ACTIVE_ACK, CLEARED, CLEARED_UNACK)
- Alarm shelving with audit trail
- Alarm suppression by slot
- Alarm flood detection
- History storage with PostgreSQL
- Rule-based alarm generation
- Multiple severity levels (LOW, MEDIUM, HIGH, EMERGENCY)

**Alarm Conditions**:
```c
typedef enum {
    ALARM_CONDITION_HIGH = 0,
    ALARM_CONDITION_LOW,
    ALARM_CONDITION_HIGH_HIGH,
    ALARM_CONDITION_LOW_LOW,
    ALARM_CONDITION_RATE_OF_CHANGE,
    ALARM_CONDITION_DEVIATION,
    ALARM_CONDITION_BAD_QUALITY,
} alarm_condition_t;
```

### RTU Alarm System (`alarm_manager.h` in Water-Treat)

**Features**:
- Basic severity levels (CRITICAL, HIGH, MEDIUM, LOW)
- Local alarm rules per sensor
- LED status integration
- Simple high/low threshold checks

**⚠️ GAP**: RTU alarm system is simpler:
- No ISA-18.2 state machine
- No shelving/suppression
- No flood detection
- No rate-of-change or deviation conditions
- No alarm history persistence

### Severity Mapping

| Controller Severity | RTU Severity | Status |
|--------------------|--------------|--------|
| EMERGENCY (4) | CRITICAL | ✅ EQUIVALENT |
| HIGH (3) | HIGH | ✅ MATCH |
| MEDIUM (2) | MEDIUM | ✅ MATCH |
| LOW (1) | LOW | ✅ MATCH |

---

## Missing Features - Controller

### 1. **Web-Polled Sensor Support** ❌
RTU supports `SENSOR_INSTANCE_WEB_POLL` for HTTP/JSON data sources. Controller has no equivalent remote data ingestion.

### 2. **Calculated/Formula Sensors** ❌
RTU supports `SENSOR_INSTANCE_CALCULATED` with formula evaluation (`formula_evaluator.h`). Controller doesn't process calculated sensors.

**RTU Formula Support**:
```c
typedef struct {
    char formula[MAX_CONFIG_VALUE_LEN];
    int input_slots[8];
    int input_count;
    formula_evaluator_t formula_eval;
} sensor_instance_t;
```

### 3. **ADC Driver Integration** ❌
RTU has built-in ADC drivers (ADS1115, MCP3008). Controller assumes pre-processed values.

### 4. **LED Status Feedback** ❌
RTU has WS2812B LED status system. Controller doesn't track or configure RTU LEDs.

### 5. **Direct Hardware Abstraction** ❌
RTU has HAL layer for GPIO, I2C, SPI, 1-Wire. Controller relies on PROFINET abstraction.

### 6. **TUI for Local Config** ❌
RTU has ncurses-based TUI for on-device configuration. Controller only has web UI.

### 7. **Standalone Operation Mode** ❌
Controller doesn't have graceful degradation when no RTUs are connected.

---

## Missing Features - RTU

### 1. **PID Control Loops** ❌
Controller has full PID implementation (`pid_loop.c`):
```c
typedef struct {
    float kp, ki, kd;
    float setpoint;
    float output_min, output_max;
    float deadband;
    float integral_limit;
    float derivative_filter;
    pid_mode_t mode;
} pid_loop_t;
```
RTU has no local PID - relies on controller.

### 2. **Interlock Logic** ❌
Controller has interlock manager (`interlock_manager.c`). RTU has no conditional interlocks.

### 3. **Sequence Engine** ❌
Controller has `sequence_engine.c` for automated procedures. RTU has no sequencing.

### 4. **Historian** ❌
Controller has time-series data storage with compression:
```c
typedef enum {
    COMPRESSION_NONE = 0,
    COMPRESSION_SWINGING_DOOR,
    COMPRESSION_BOXCAR,
    COMPRESSION_DEADBAND,
} compression_t;
```
RTU only logs to local SQLite for config, no trend history.

### 5. **Modbus Gateway** ❌
Controller has Modbus TCP/RTU gateway for SCADA. RTU has no Modbus support.

### 6. **Multi-Device Coordination** ❌
Controller has:
- `coordination.c` - Multi-RTU management
- `cascade_control.c` - Cascaded PID loops
- `load_balance.c` - Actuator load distribution
- `failover.c` - Hot standby failover

RTU operates independently with no peer awareness.

### 7. **User Authentication** ❌
Controller has user management (`user_t` with roles):
```c
typedef enum {
    USER_ROLE_VIEWER = 0,
    USER_ROLE_OPERATOR,
    USER_ROLE_ENGINEER,
    USER_ROLE_ADMIN,
} user_role_t;
```
RTU has no authentication.

### 8. **Full ISA-18.2 Alarm Handling** ❌
Controller has full ISA-18.2 compliance. RTU has simplified alarms.

---

## Integration Requirements

### Required RTU Configuration

For proper integration with Water-Controller, RTUs **MUST** be configured with:

1. **Slot Assignment**:
   - Slot 0: DAP (Device Access Point)
   - Slots 1-8: Sensors only (4 bytes each, IEEE 754 float)
   - Slots 9-16: Actuators only (4 bytes each, packed command struct)

2. **Station Name**: Unique name matching controller registry entry

3. **PROFINET Identity**:
   - Vendor ID: As configured in GSDML
   - Device ID: As configured in GSDML

4. **Network Configuration**:
   - Interface bound to same network as controller
   - IP address in same subnet

### API Endpoints for Integration

**Controller REST API** (port 8080 - Controller plane):
```
GET  /api/v1/rtus                    - List registered RTUs
POST /api/v1/rtus                    - Add RTU configuration
GET  /api/v1/rtus/{station}/sensors  - Get sensor values
POST /api/v1/rtus/{station}/actuators/{slot} - Control actuator
GET  /api/v1/alarms/active           - Get active alarms
POST /api/v1/alarms/{id}/acknowledge - Acknowledge alarm
GET  /api/v1/pid-loops               - List PID loops
POST /api/v1/pid-loops               - Create PID loop
```

**RTU REST API** (port 9081 - RTU plane):
```
GET  /sensors                        - List sensors
GET  /actuators                      - List actuators
POST /actuators/{slot}/set           - Manual actuator control
GET  /health                         - Health status
GET  /config                         - Export configuration
```

> **Port Allocation**: 8xxx = Controller plane, 9xxx = RTU plane.
> See [DR-001-port-allocation.md](decisions/DR-001-port-allocation.md) for rationale.

---

## Recommendations

### Immediate Actions

1. **Verify Byte Order Handling**
   - Test that float values transmitted from RTU are correctly interpreted by Controller
   - p-net library may handle byte order differently than expected

2. **Enforce Slot Convention**
   - Add validation in RTU to warn if sensors are assigned to slots 9-16
   - Add validation in RTU to warn if actuators are assigned to slots 1-8

3. **Alarm Severity Sync**
   - Map RTU CRITICAL → Controller EMERGENCY
   - Ensure PROFINET alarm codes match

### Future Enhancements

#### For RTU (Water-Treat)

1. **Add Local PID Option** (LOW PRIORITY)
   - Optional local PID for degraded mode operation
   - Parameters synced from controller when connected

2. **ISA-18.2 Alarm States** (MEDIUM PRIORITY)
   - Implement full state machine
   - Report to controller via PROFINET alarms

3. **User Authentication** (MEDIUM PRIORITY)
   - Add basic auth for REST API
   - Role-based access for TUI

#### For Controller (Water-Controller)

1. **LED Configuration API** (LOW PRIORITY)
   - REST endpoint to configure RTU LED colors
   - Sync LED patterns with alarm states

2. **Calculated Sensor Support** (MEDIUM PRIORITY)
   - Define formulas at controller level
   - Calculate values from multiple RTU inputs

3. **Web Polling Integration** (LOW PRIORITY)
   - Add controller-level web data sources
   - Virtual sensors from external APIs

---

## Version Compatibility Matrix

| Controller Version | RTU Version | Compatibility |
|-------------------|-------------|---------------|
| 1.0.x | 1.0.x | ✅ Full (with slot convention) |
| 1.0.x | <1.0 | ⚠️ May work, untested |
| Future | Future | TBD |

---

## Summary

The Water-Controller and Water-Treat codebases are **architecturally aligned** with **binary-compatible data formats**. The primary integration requirements are:

1. ✅ Actuator command structure matches
2. ✅ Sensor float format matches
3. ⚠️ Slot numbering must follow convention (1-8 sensors, 9-16 actuators)
4. ⚠️ Byte order handling needs verification
5. ❌ RTU lacks advanced control (PID, interlocks, sequences)
6. ❌ Controller lacks hardware abstraction for local sensors

The division of responsibility is correct:
- **RTU**: Data acquisition, actuator driving, local safety
- **Controller**: Coordination, control logic, historian, SCADA integration

Both codebases are production-ready for their respective roles.
