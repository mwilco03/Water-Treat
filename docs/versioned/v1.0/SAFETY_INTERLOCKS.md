# Safety Interlocks Documentation

**Version:** 1.0
**Last Updated:** 2025-12-22
**Document Status:** DRAFT - Requires review and completion
**Document Owner:** [Assign Owner]

---

## 1. Overview

This document describes all safety-critical behaviors implemented in the Water Treatment RTU firmware. It is a controlled document and must be updated whenever safety-related functionality changes.

**IMPORTANT:** This document is derived from code analysis and existing documentation. All values and behaviors must be verified against actual implementation before operational use.

---

## 2. Definitions

| Term | Definition |
|------|------------|
| Safe State | The state an actuator assumes when a fault condition occurs |
| Interlock | A condition that prevents or forces an actuator state regardless of controller commands |
| Fault | Any condition that triggers safe state behavior |
| IOPS | Input/Output Provider Status - PROFINET data quality indicator |
| IOCS | Input/Output Consumer Status - PROFINET consumer acknowledgment |

---

## 3. Actuator Safe States

### 3.1 Default Safe State Configuration

| Actuator Type | Default Safe State | Configurable | GPIO State | Rationale |
|---------------|-------------------|--------------|------------|-----------|
| Pump (ACTUATOR_TYPE_PUMP) | OFF (de-energize) | Yes | LOW | Prevent dry running, overflow, uncontrolled dosing |
| Valve (ACTUATOR_TYPE_VALVE) | CLOSED (de-energize) | Yes | LOW | Prevent uncontrolled flow |
| Relay (ACTUATOR_TYPE_RELAY) | OFF (de-energize) | Yes | LOW | Fail-safe default |
| PWM (ACTUATOR_TYPE_PWM) | 0% duty | Yes | LOW | Stop variable-speed output |

**Source:** `src/actuators/actuator_manager.c`, `CONTROLLER_INTEGRATION_NOTES.md`

### 3.2 Safe State Configuration

Safe states are configurable per-actuator in the RTU configuration database:

```sql
-- From SQLite schema
CREATE TABLE actuators (
    ...
    safe_state INTEGER DEFAULT 0,  -- 0=OFF, 1=ON
    active_low INTEGER DEFAULT 0,  -- Invert GPIO logic
    ...
);
```

**Configuration via TUI:**
1. Navigate to Actuators screen (F8)
2. Select actuator
3. Press 'E' to edit
4. Set "Safe State" field (OFF or ON)

---

## 4. Safe State Triggers

Safe state is activated when ANY of the following conditions occur:

### 4.1 PROFINET Connection Loss

| Parameter | Value | Source |
|-----------|-------|--------|
| Detection Method | No valid PROFINET frame received | p-net stack |
| Default Timeout | 500 ms (configurable via p-net) | `src/profinet/profinet_device.c` |
| Action | All actuators → configured safe state | `actuator_set_safe_state()` |
| Recovery | Automatic when connection restored | Automatic |
| Operator Notification | LED status changes, TUI indicator | ISA-101 compliant |

**Sequence of Events:**
1. p-net stack detects missing cyclic data frames
2. Connection state transitions to OFFLINE
3. `profinet_device_on_disconnect()` callback invoked
4. `actuator_manager_set_all_safe_state()` called
5. All actuators driven to configured safe state
6. LED status changes to indicate fault (red per ISA-101)
7. When connection restored, actuators respond to controller commands

### 4.2 Controller Watchdog Timeout

**Note:** Per `CONTROLLER_INTEGRATION_NOTES.md`, controller watchdog is the responsibility of the Water-Controller, not the RTU. The RTU relies on PROFINET connection loss detection.

| Behavior | Description |
|----------|-------------|
| Controller-side | Controller monitors RTU response and can stop sending commands |
| RTU-side | Treats absence of PROFINET frames as connection loss |

### 4.3 Sensor Fault on Interlock Input

**Current Implementation Status:** Basic sensor fault detection implemented. Full interlock engine is a future enhancement (see `CLEANUP_AND_ROADMAP.md`).

| Trigger | Detection | Action |
|---------|-----------|--------|
| Sensor IOPS = BAD (0x00) | Sensor driver timeout | Sensor value marked invalid |
| Sensor read timeout | No response from hardware | IOPS set to BAD |
| Out-of-range reading | Value outside configured limits | IOPS set to UNCERTAIN (0x40) |

**Quality Byte Values (per GSDML):**
- `0x00` = Good
- `0x40` = Uncertain
- `0x80` = Bad
- `0xC0` = Not Connected

---

## 5. Implemented Interlocks

**Current Status:** The RTU currently delegates all control logic and interlocks to the Water-Controller. Local safety interlocks are planned but not yet implemented (see `INTEGRATION_GAP_ANALYSIS.md` Section 5.3).

### 5.1 Planned Local Interlocks (Not Yet Implemented)

The following interlocks are recommended for local implementation to protect against controller communication failures:

#### 5.1.1 Low Level Pump Cutoff

| Property | Recommended Value |
|----------|-------------------|
| Condition Sensor | Level sensor (configurable slot) |
| Threshold | < 10% (configurable) |
| Comparison | BELOW |
| Protected Actuator | All pumps |
| Action | FORCE_OFF |
| Bypass Capable | No |
| Reset Mode | AUTO (when level > threshold) |
| Rationale | Prevent pump cavitation and dry running |

#### 5.1.2 High Level Overflow Protection

| Property | Recommended Value |
|----------|-------------------|
| Condition Sensor | Level sensor (configurable slot) |
| Threshold | > 95% (configurable) |
| Comparison | ABOVE |
| Protected Actuator | Inlet valves, fill pumps |
| Action | FORCE_OFF |
| Bypass Capable | No |
| Reset Mode | AUTO (when level < threshold) |
| Rationale | Prevent tank overflow |

### 5.2 Controller-Implemented Interlocks

The Water-Controller implements full interlock management per `CONTROLLER_SPEC.md`:

```c
typedef struct {
    int interlock_id;
    char name[64];
    char condition_rtu[64];
    int condition_slot;
    interlock_condition_t condition;  // ABOVE, BELOW, EQUAL
    float threshold;
    char action_rtu[64];
    int action_slot;
    interlock_action_t action;  // FORCE_OFF, FORCE_ON, ALARM_ONLY
    bool tripped;
    uint64_t trip_time_ms;
} interlock_t;
```

---

## 6. Emergency Shutdown

### 6.1 Manual Emergency Stop

**Current Status:** Physical E-STOP is not integrated with RTU firmware. E-STOP should be wired to directly de-energize actuator power circuits independently of the RTU.

**Recommended Configuration:**
1. Wire physical E-STOP in series with actuator power
2. E-STOP state can be monitored via GPIO input for logging/status
3. RTU cannot override physical E-STOP

### 6.2 Software Emergency Shutdown

**Via TUI:**
1. Press 'Q' or F10 to exit TUI
2. systemd service stops
3. All GPIO outputs released (return to safe state per hardware design)

**Via PROFINET:**
1. Controller sends OFF command to all actuators
2. Controller can close PROFINET connection to trigger safe state

---

## 7. Recovery Procedures

### 7.1 After PROFINET Connection Loss

| Step | Action | Verification |
|------|--------|--------------|
| 1 | Verify network connectivity | `ping [controller IP]` |
| 2 | Check Ethernet cable connections | Visual inspection, link lights |
| 3 | Verify controller is running | Check controller status/logs |
| 4 | Check RTU service status | `systemctl status profinet-monitor` |
| 5 | When connection restored | LED status returns to normal (green) |
| 6 | Verify process state | Review sensor readings before resuming |
| 7 | Resume control from controller | Controller sends new commands |

**Actuators resume automatically** when PROFINET connection is restored. No manual acknowledgment required at RTU level.

### 7.2 After Sensor Fault

| Step | Action | Verification |
|------|--------|--------------|
| 1 | Identify faulted sensor | TUI Sensors screen, IOPS indicator |
| 2 | Investigate fault cause | Check wiring, sensor hardware |
| 3 | Repair or replace sensor | Follow sensor-specific procedures |
| 4 | Verify sensor reading | Check current value and IOPS = GOOD |
| 5 | Clear fault indication | Automatic when reading valid |
| 6 | Resume normal operation | Sensor data flows to controller |

---

## 8. LED Status Indicators

Per ISA-101 and `OPERATOR.md`:

| LED Color | Pattern | Meaning |
|-----------|---------|---------|
| Green | Solid | Normal operation, PROFINET connected |
| Green | Slow blink | Configuration mode |
| Yellow | Solid | Warning condition |
| Red | Solid | Fault condition, safe state active |
| Red | Fast blink | Critical alarm |
| White | Solid | System starting |

---

## 9. Testing Requirements

### 9.1 Pre-Commissioning Testing

All safety interlocks MUST be tested during commissioning using the checklist in `COMMISSIONING.md`.

### 9.2 Periodic Testing

| Test | Frequency | Procedure |
|------|-----------|-----------|
| PROFINET connection loss | Annually or after firmware update | Disconnect Ethernet, verify safe state |
| Sensor fault handling | Annually or after firmware update | Simulate sensor fault, verify behavior |
| E-STOP (if integrated) | Per site requirements | Activate E-STOP, verify all outputs de-energized |

### 9.3 Testing Documentation

Record all safety testing in site maintenance logs with:
- Date and time
- Tester name
- Test performed
- Expected result
- Actual result
- Pass/Fail
- Any corrective actions

---

## 10. Regulatory Considerations

This system may be subject to:
- State/provincial drinking water regulations
- EPA requirements (if applicable)
- OSHA process safety management (if applicable)
- Local environmental regulations

Consult with regulatory authorities regarding safety interlock documentation requirements.

---

## 11. Change History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-12-22 | Documentation Audit | Initial draft from code analysis |

---

## 12. Approval

| Role | Name | Signature | Date |
|------|------|-----------|------|
| Author | | | |
| Technical Review | | | |
| Safety Review | | | |
| Approval | | | |

---

**NOTICE:** This document requires completion and verification before operational use. Sections marked "[TBD]" or "Not Yet Implemented" indicate areas requiring further development or verification.
