# Alarm Response Procedures

**Version:** 1.0
**Last Updated:** 2025-12-22
**Document Status:** DRAFT - Requires site-specific customization
**Document Owner:** [Assign Owner]

---

## 1. Alarm System Overview

The Water Treatment RTU generates alarms when process conditions exceed configured thresholds. Alarms are:
- Transmitted to the PROFINET controller via alarm frames
- Displayed on the local TUI (Alarms screen - F6)
- Indicated via LED status (ISA-101 compliant)

**Current Implementation:** Basic threshold alarms. Full ISA-18.2 state machine (UNACK/ACK states) is implemented in Water-Controller; RTU provides alarm conditions only.

---

## 2. Alarm Severity Levels

| Severity | LED Indication | Response Time | Description |
|----------|----------------|---------------|-------------|
| EMERGENCY | Red fast blink | Immediate | Safety hazard, automatic action may be required |
| HIGH | Red solid | < 5 minutes | Abnormal condition requiring prompt operator action |
| MEDIUM | Yellow solid | < 1 hour | Abnormal condition, action required within shift |
| LOW | Yellow blink | Informational | Status change, no immediate action required |

**Reference:** ISA-18.2 Alarm Management standard, `CONTROLLER_SPEC.md`

---

## 3. Alarm Conditions by Sensor Type

### 3.1 pH Sensor Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| pH-HH | pH High-High | > 9.0 | HIGH | Over-dosing caustic/base, sensor drift, calibration error | 1. Check base dosing pump status 2. Verify pH probe 3. Manual sample verification |
| pH-H | pH High | > 8.5 | MEDIUM | Base dosing imbalance, setpoint issue | Monitor trend, adjust setpoint if needed |
| pH-L | pH Low | < 6.5 | MEDIUM | Acid dosing imbalance, setpoint issue | Monitor trend, adjust setpoint if needed |
| pH-LL | pH Low-Low | < 6.0 | HIGH | Over-dosing acid, sensor drift, calibration error | 1. Check acid dosing pump status 2. Verify pH probe 3. Manual sample verification |
| pH-FAULT | Sensor Fault | IOPS=BAD | HIGH | Sensor failure, wiring issue, ADC fault | 1. Check wiring 2. Check sensor 3. Replace if needed |

**Default Thresholds:** Per `CONTROLLER_SPEC.md` - Drinking water: Low 6.5, High 8.5

### 3.2 Level Sensor Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| LVL-HH | Level High-High | > 95% | **EMERGENCY** | Overflow imminent, outlet blocked, pump failure | 1. **STOP INFLOW IMMEDIATELY** 2. Open drain/overflow 3. Start transfer pump |
| LVL-H | Level High | > 90% | HIGH | Tank filling faster than draining | Reduce inflow or increase outflow |
| LVL-L | Level Low | < 15% | MEDIUM | Tank draining, leak, demand spike | Increase inflow, check for leaks |
| LVL-LL | Level Low-Low | < 10% | HIGH | Pump cavitation risk, supply issue | 1. **STOP PUMPS** drawing from tank 2. Increase inflow 3. Check supply |
| LVL-FAULT | Sensor Fault | IOPS=BAD | HIGH | Ultrasonic sensor obstruction, wiring | 1. Check sensor face for debris 2. Check wiring 3. Replace if needed |

### 3.3 Flow Sensor Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| FLOW-H | Flow High | > [site setpoint] | MEDIUM | Valve stuck open, demand spike | Verify valve positions, check for leaks |
| FLOW-L | Flow Low | < [site setpoint] | MEDIUM | Partial blockage, air lock, pump issue | Check for obstructions, verify pump operation |
| FLOW-ZERO | No Flow | 0 when pump ON | HIGH | Pump failure, complete blockage, closed valve | 1. Check pump operation 2. Check valve positions 3. Check for blockage |
| FLOW-FAULT | Sensor Fault | IOPS=BAD | MEDIUM | Sensor failure, wiring | Check wiring, replace sensor |

### 3.4 Temperature Sensor Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| TEMP-H | Temperature High | > [site setpoint] | MEDIUM | Process heat, ambient conditions | Monitor, adjust if needed |
| TEMP-L | Temperature Low | < [site setpoint] | MEDIUM | Cooling, ambient conditions | Monitor, adjust if needed |
| TEMP-FAULT | Sensor Fault | IOPS=BAD | MEDIUM | DS18B20 failure, wiring | Check 1-Wire connection, replace sensor |

### 3.5 Turbidity Sensor Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| TURB-H | Turbidity High | > 4.0 NTU | HIGH | Filter breakthrough, coagulation issue | 1. Check filter status 2. Adjust coagulant dosing 3. Consider divert |
| TURB-HH | Turbidity Very High | > 10.0 NTU | **EMERGENCY** | Major process upset | 1. **DIVERT FLOW** 2. Investigate source 3. Check all treatment stages |
| TURB-FAULT | Sensor Fault | IOPS=BAD | HIGH | Sensor fouling, calibration needed | 1. Clean sensor 2. Recalibrate 3. Replace if needed |

### 3.6 TDS Sensor Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| TDS-H | TDS High | > 500 ppm | HIGH | Source water quality, treatment issue | Check source, treatment stages |
| TDS-FAULT | Sensor Fault | IOPS=BAD | MEDIUM | Sensor failure, calibration | Check sensor, recalibrate |

### 3.7 Actuator Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| ACT-FAIL | Actuator Failure | Feedback mismatch | HIGH | Relay failure, contactor issue | Check actuator, wiring, contactor |
| ACT-OVERRUN | Runtime Exceeded | > max_on_time | HIGH | Control loop issue, stuck command | 1. Check control logic 2. Verify actuator operation |

### 3.8 Communication Alarms

| Alarm ID | Condition | Threshold | Severity | Possible Causes | Operator Response |
|----------|-----------|-----------|----------|-----------------|-------------------|
| PNET-DISC | PROFINET Disconnected | Connection lost | HIGH | Network cable, controller down, switch issue | 1. Check cables 2. Check controller 3. Check network switch |
| PNET-TIMEOUT | Controller Timeout | No commands | HIGH | Controller issue, network congestion | Check controller status and logs |
| SENSOR-TIMEOUT | Sensor Read Timeout | No reading | MEDIUM | Sensor fault, bus issue | Check sensor wiring, I2C/1-Wire bus |

---

## 4. Alarm Viewing and Acknowledgment

### 4.1 Viewing Alarms on TUI

1. Press **F6** to navigate to Alarms screen
2. Active alarms are displayed with:
   - Alarm name
   - Current value
   - Threshold exceeded
   - Severity indicator (color-coded)
   - Time of occurrence

### 4.2 Acknowledging Alarms via TUI

1. Navigate to Alarms screen (F6)
2. Use **↑/↓** arrow keys to select alarm
3. Press **'A'** to acknowledge selected alarm
4. Alarm moves to ACKNOWLEDGED state

**Note:** Acknowledgment indicates operator awareness, not resolution.

### 4.3 Acknowledgment via Controller

The Water-Controller provides full alarm management including:
- Remote acknowledgment via HMI
- Alarm shelving (temporary suppression)
- Alarm history and reporting

Refer to Water-Controller documentation for HMI procedures.

---

## 5. Alarm Response Matrix

### 5.1 Emergency Alarms - Immediate Response Required

| Alarm | Automatic Action | Operator Action | Notification |
|-------|------------------|-----------------|--------------|
| LVL-HH | [None - implement locally?] | Stop inflow, open drain | Supervisor |
| TURB-HH | [None - implement locally?] | Divert flow | Supervisor |

### 5.2 High Severity Alarms - Response Within 5 Minutes

| Alarm | Automatic Action | Operator Action | Notification |
|-------|------------------|-----------------|--------------|
| pH-HH/LL | None | Check dosing, verify sensor | Log entry |
| LVL-LL | None | Stop outflow pumps | Log entry |
| PNET-DISC | Safe state activated | Check network | Supervisor if > 15 min |

### 5.3 Medium Severity Alarms - Response Within 1 Hour

| Alarm | Automatic Action | Operator Action | Notification |
|-------|------------------|-----------------|--------------|
| pH-H/L | None | Monitor, adjust if needed | None |
| FLOW-H/L | None | Investigate cause | None |

---

## 6. Alarm Shelving

**Purpose:** Temporarily suppress known nuisance alarms during maintenance or known conditions.

### 6.1 Shelving Procedure (via Controller HMI)

1. Navigate to alarm in HMI
2. Select "Shelve" action
3. Enter shelve duration (max [site-defined])
4. Enter reason for shelving
5. Confirm with operator credentials

**Audit Trail:** All shelving actions are logged with operator ID, time, duration, and reason.

### 6.2 Shelving Restrictions

- Maximum shelve duration: [Site-defined, typically 8-24 hours]
- Some alarms may not be shelvable (safety-critical)
- Shelved alarms appear with special indicator

---

## 7. Alarm Flood Management

An **alarm flood** is when multiple alarms occur in rapid succession, potentially overwhelming the operator.

### 7.1 Flood Detection

- Threshold: > 10 alarms per minute (configurable)
- Detection implemented in Water-Controller

### 7.2 Flood Response

1. Focus on EMERGENCY and HIGH alarms first
2. Acknowledge informational alarms after critical alarms addressed
3. Investigate root cause of alarm cascade
4. Document in shift log

---

## 8. Alarm Testing

### 8.1 Commissioning Tests

All alarms must be tested during commissioning per `COMMISSIONING.md`.

### 8.2 Periodic Testing

| Test | Frequency | Method |
|------|-----------|--------|
| Communication alarms | Annually | Disconnect Ethernet cable |
| Sensor fault alarms | Annually | Disconnect sensor |
| Threshold alarms | Per site policy | Adjust threshold or simulate condition |

### 8.3 Test Documentation

Record all alarm tests with:
- Date and time
- Alarm tested
- Test method
- Expected result
- Actual result
- Pass/Fail

---

## 9. Escalation Procedures

| Condition | Escalation Action | Contact |
|-----------|------------------|---------|
| EMERGENCY alarm unacknowledged > 5 min | Notify Shift Supervisor | [Phone] |
| HIGH alarm unacknowledged > 15 min | Notify Shift Supervisor | [Phone] |
| Multiple EMERGENCY alarms | Notify Plant Manager | [Phone] |
| PROFINET disconnect > 30 min | Notify IT/Controls | [Phone] |
| Recurring alarm (> 3x in 1 hour) | Create maintenance work order | [System] |

---

## 10. Alarm Rationalization

This section documents the basis for alarm thresholds.

| Alarm | Threshold | Basis |
|-------|-----------|-------|
| pH-HH | 9.0 | [Regulatory limit / equipment protection / process requirement] |
| pH-H | 8.5 | Drinking water standard per EPA |
| pH-L | 6.5 | Drinking water standard per EPA |
| TURB-H | 4.0 NTU | Drinking water standard |
| [Continue for all alarms] | | |

---

## 11. Change History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-12-22 | Documentation Audit | Initial draft from code analysis |

---

## 12. Site-Specific Customization

**REQUIRED BEFORE USE:** The following items must be customized for each installation:

- [ ] Threshold values for all alarms
- [ ] Escalation contacts and phone numbers
- [ ] Maximum shelve duration
- [ ] Site-specific alarm IDs if different from template
- [ ] Any additional site-specific alarms
- [ ] Regulatory references for threshold basis

---

**NOTICE:** This document requires site-specific customization before operational use. Generic thresholds are provided as examples based on common water treatment standards.
