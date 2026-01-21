# Controller Integration Notes

This document describes changes made to the Water-Treat RTU that affect integration with the Water-Controller.

## Changes Summary

### New I/O Configuration Wizard

The RTU now uses a **progressive disclosure wizard** for configuring sensors and actuators. This change improves user experience but does **not** affect the PROFINET interface or data formats.

**What Changed (RTU Side Only)**:
- New `dialog_io_wizard.c` replaces the old 15-field sensor form
- Wizard scans I2C/1-Wire buses automatically before asking questions
- User picks from discovered devices instead of entering technical details
- GPIO pin conflicts are shown immediately with clear alternatives
- PROFINET slots are auto-assigned (sensors: 1-8, actuators: 9-16)

**What Did NOT Change (Controller Side)**:
- PROFINET slot structure (DAP=0, sensors=1-8, actuators=9-16)
- Data format (4-byte IEEE 754 float for sensors)
- Command format (4-byte: command, pwm_duty, reserved[2] for actuators)
- Station name format (`rtu-XXXX`)
- Vendor ID (0x0493) and Device ID (0x0001)

---

## Nomenclature Alignment

The wizard enforces consistent naming conventions that align with HMI expectations.

### Sensor/Actuator Naming Convention

**Format**: `lowercase_with_underscores`

**Examples**:
| RTU Name | HMI Display | Type |
|----------|-------------|------|
| `intake_temp` | Intake Temperature | Sensor |
| `clearwell_level` | Clearwell Level | Sensor |
| `ph_probe` | pH Probe | Sensor |
| `chlorine_pump` | Chlorine Pump | Actuator |
| `backwash_valve` | Backwash Valve | Actuator |

**Validation Rules** (enforced by wizard):
1. Minimum 3 characters
2. Only lowercase letters, numbers, underscores
3. Must be unique across all sensors and actuators
4. No spaces, hyphens, or special characters

### Sensor Type Mapping

The wizard maps user-friendly names to technical configurations:

| User Selection | module_type | driver | unit |
|----------------|-------------|--------|------|
| pH Probe | adc | ADS1115 | pH |
| Pressure Transducer | adc | ADS1115 | bar |
| TDS Sensor | adc | ADS1115 | ppm |
| Turbidity Sensor | adc | ADS1115 | NTU |
| ORP Sensor | adc | ADS1115 | mV |
| Flow Meter | physical | gpio | pulses/s |
| Float Switch | physical | gpio | (binary) |
| DS18B20 Temperature | physical | 1wire | °C |
| BME280 | physical | i2c | °C |

### Actuator Type Mapping

| User Selection | actuator_type enum | PROFINET Command |
|----------------|-------------------|------------------|
| Pump | ACTUATOR_TYPE_PUMP | 0x00=OFF, 0x01=ON |
| Valve/Solenoid | ACTUATOR_TYPE_VALVE | 0x00=OFF, 0x01=ON |
| Generic Relay | ACTUATOR_TYPE_RELAY | 0x00=OFF, 0x01=ON |
| PWM Variable | ACTUATOR_TYPE_PWM | 0x02=PWM, duty in byte 1 |

---

## PROFINET Slot Assignment

The wizard auto-assigns slots to avoid conflicts:

### Sensor Slots (1-8)
- Slots are assigned sequentially starting from 1
- If slot 1 is in use, slot 2 is tried, etc.
- Maximum 8 sensors can be configured

### Actuator Slots (9-16)
- Slots are assigned sequentially starting from 9
- If slot 9 is in use, slot 10 is tried, etc.
- Maximum 8 actuators can be configured

**Controller must**:
1. Not assume specific slots are used
2. Query RTU database or GSD file for active slots
3. Handle sparse slot assignments (e.g., only slots 1, 3, 5 configured)

---

## Data Quality Indicators

Sensor readings include IOPS (Input/Output Provider Status):
- `0x80 (PNET_IOXS_GOOD)` - Valid data
- `0x00 (PNET_IOXS_BAD)` - Sensor fault/timeout/disconnected

**Controller should**:
1. Check IOPS before using sensor values
2. Display "FAULT" or "---" on HMI for bad status
3. Suppress alarms during bad status (optional)

---

## Safe State Behavior

All actuators default to `SAFE_STATE_OFF` (de-energize on fault):
- PROFINET connection lost → actuator turns OFF
- Controller can override via PROFINET parameter write (not implemented yet)

**Controller should**:
1. Expect actuators to go OFF on connection loss
2. Implement watchdog to detect connection health
3. Consider "graceful shutdown" sequence before disconnect

---

## Migration Guide

### If Upgrading RTU Firmware

1. Existing sensor/actuator configurations are preserved
2. New sensors/actuators will use wizard flow
3. Old `dialog_sensor_add()` still accessible via "Edit Advanced" button

### If Updating Controller

No changes required. The PROFINET interface is unchanged.

---

## Testing Checklist

- [ ] Sensor created via wizard appears in PROFINET cyclic data
- [ ] Actuator created via wizard responds to PROFINET commands
- [ ] Slot assignment matches expected range (1-8 sensors, 9-16 actuators)
- [ ] Name validation rejects invalid characters
- [ ] GPIO conflict detection works across sensors and actuators
- [ ] DS18B20 1-Wire sensors discovered and configured correctly
- [ ] ADS1115 ADC channels discovered and assigned correctly
- [ ] ESC key navigates back one step at every screen

---

## DCP Protocol Behavior (Authoritative)

**Decision Date**: 2026-01-21
**Status**: FINAL - Aligned with IEC 61158 PROFINET specification

### DCP Overview

DCP (Discovery and Configuration Protocol) operates at Layer 2 (multicast `01:0E:CF:00:00:00`). The RTU implements DCP as a PROFINET I/O Device per IEC 61158.

### DCP Operations Supported

| Operation | RTU Behavior | Notes |
|-----------|--------------|-------|
| **DCP Identify** | Always responds | Returns station_name, vendor_id, device_id, MAC, IP |
| **DCP Get** | Always responds | Read any DCP parameter |
| **DCP Set IP** | Conditional | See DHCP behavior below |
| **DCP Set Name** | Accepted | Station name can be changed via DCP |

### DCP Set IP Behavior

**DHCP Mode** (`network.dhcp_enabled = true`):
- DCP Set IP requests are **accepted by p-net stack** (cannot be blocked at application layer)
- IP change is **temporary** (not persisted to config file)
- On RTU reboot, DHCP will reassign IP address
- **Note**: p-net handles DCP internally; application cannot intercept or log DCP Set events

**Static Mode** (`network.dhcp_enabled = false`):
- DCP Set IP requests are **accepted and applied**
- IP change **may be persisted** if RTU implements NV callback (currently: not persisted)
- Commissioning tools can use DCP to configure RTU IP address

### Device Identity (Authoritative)

Per PROFINET specification (IEC 61158-6):

| Identifier | Value | Scope | Uniqueness |
|------------|-------|-------|------------|
| `vendor_id` | `0x0493` | Global | Assigned by PI to Water Treatment Systems |
| `device_id` | `0x0001` | Per vendor | Identifies "Water-Treat RTU" product line |
| `station_name` | User-configured | Per network | **MUST be unique** on L2 broadcast domain |
| `MAC address` | Hardware | Global | Unique per physical device |

**Key Points**:
- `vendor_id` + `device_id` identify the **product type** (like a model number)
- Multiple RTUs of the same model share `0x0493:0x0001`
- `station_name` differentiates individual devices: `rtu-tank-1`, `rtu-pump-station`
- Controller discovers RTUs by `station_name`, not by `device_id`

### Station Name Requirements

Per PROFINET specification:
- Lowercase only
- DNS-compatible characters: `a-z`, `0-9`, `-`
- Maximum 63 characters
- No underscores (use hyphens)
- Must be unique within L2 broadcast domain

**Examples**:
```
rtu-tank-1          ✓ Valid
rtu-pump-station    ✓ Valid
RTU-Tank-1          ✗ Invalid (uppercase)
rtu_tank_1          ✗ Invalid (underscore)
```

---

## PROFINET Behavior Decisions (Authoritative)

**Decision Date**: 2026-01-21
**Status**: FINAL - Code-verified, Controller-acknowledged

| ID | Question | RTU Behavior | Code Reference |
|----|----------|--------------|----------------|
| Q1 | AR watchdog timeout | **Wait** for PNET_EVENT_ABORT (passive) | `profinet_callbacks.c:99-100` |
| Q2 | Command to unknown slot | **Ignore** silently (no NACK) | `profinet_manager.c:85-92,105-136` |
| Q4 | Minimum cycle time | **1ms** (`PROFINET_TICK_INTERVAL_US=1000`) | `profinet_manager.c:24` |
| Q5 | Authority mode | **Stored, not enforced** | `config_sync.c:43,96` |
| Q6 | Token mismatch | **Reject packet**, AR continues | `rtu_registration.c:660-664` |
| Q7 | Firmware version | **I&M0 @ 0x8000** (standard PROFINET) | `profinet_callbacks.c:165-168` |
| Q8 | Diagnostic alarms | **Manual API only** (`profinet_manager_send_alarm()`) | `profinet_manager.c:934-962` |
| Q9 | Config CRC mismatch | **Reject packet** (PNIO 0xCF/0x81) | `config_sync.c:88-91` |
| Q10 | Max simultaneous AR | **1** (single-controller device) | `profinet_manager.c:48` |

### CRC16-CCITT Parameters (Confirmed)

All config sync, user sync, and enrollment packets use identical CRC:
- **Polynomial**: 0x1021
- **Initial value**: 0xFFFF
- **Final XOR**: None
- **Implementation**: `src/profinet/config_sync.c:19-31`, `src/auth/user_sync.c:108-121`

### Authority Mode (Q5 Detail)

Currently stored but NOT enforced:
- `AUTHORITY_AUTONOMOUS (0)`: Default - RTU operates independently
- `AUTHORITY_SUPERVISED (1)`: Intended for controller-directed operation

**Status**: Enum defined, value persisted, no behavioral difference implemented.
**Action**: Define SUPERVISED behavior if differentiation needed.

---

## Controller Team Alignment (2026-01-21)

**Status**: ✅ ALIGNED - Integration testing can proceed

### Controller Confirmed

| Item | Status |
|------|--------|
| Station name regex | `^[a-z0-9][a-z0-9-]{0,62}$` (no dots, no underscores) |
| CRC16-CCITT | Verified: poly=0x1021, init=0xFFFF (matches RTU) |
| Config models | Regenerated from schema |

### Controller Will Code For

| RTU Behavior | Controller Handling |
|--------------|---------------------|
| AR watchdog (passive) | Controller drives reconnect (3s, 6s, 12s backoff) |
| Invalid slot (ignored) | Controller validates slots before write |
| Firmware version | Read I&M0 @ 0x8000 |
| Max 1 AR | No dual-controller redundancy |
| Enrollment mismatch | Handle packet rejection, AR continues |

### Remaining TBD (Joint Decision Required)

| Item | Owner | Status |
|------|-------|--------|
| SUPERVISED mode behavior | Both | RTU stores, doesn't enforce. Define if needed. |
| Sensor fault → PROFINET alarm | Both | RTU has API, no auto-generation. Specify triggers. |

---

## Files Changed

| File | Change |
|------|--------|
| `src/tui/dialogs/dialog_io_wizard.h` | **NEW** - Progressive disclosure wizard header |
| `src/tui/dialogs/dialog_io_wizard.c` | **NEW** - Wizard implementation (~1400 lines) |
| `src/tui/pages/page_sensors.c` | Modified - Uses wizard for add |
| `src/tui/pages/page_actuators.c` | Modified - Uses wizard for add |
| `CMakeLists.txt` | Modified - Added new source file |
| `docs/IO_CONFIGURATION_UI_SPEC.md` | **NEW** - UI design specification |
| `docs/CONTROLLER_INTEGRATION_NOTES.md` | **NEW** - This document |

---

## Design Principles Applied

1. **Dynamic Discovery Over Static Configuration**
   - Wizard scans I2C/1-Wire before asking questions
   - Board detection provides GPIO pin mappings

2. **Loose Coupling**
   - Wizard doesn't know about specific drivers
   - Configuration stored in database, drivers loaded at runtime

3. **Graceful Degradation**
   - Conflicts shown with alternatives, not blocked
   - "Edit Advanced" fallback for power users

4. **Single Source of Truth**
   - User points at device, system derives technical config
   - No duplicate configuration needed

5. **Hardware Agnostic**
   - Board detection supports 15+ SBC variants
   - GPIO chip abstracted via libgpiod

6. **Informational Output**
   - Shows "Scanning..." with progress
   - Displays current readings during configuration
   - Shows board name and available pins
