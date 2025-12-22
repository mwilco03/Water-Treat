# Commissioning Checklist

**Version:** 1.0
**Document Status:** Template - Complete for each installation

---

## Project Information

| Field | Value |
|-------|-------|
| Project Name | |
| RTU Station Name | |
| Physical Location | |
| Installation Date | |
| Commissioning Engineer | |
| Firmware Version | |

---

## 1. Pre-Commissioning Verification

### 1.1 Hardware Installation Checklist

| Item | Verified | Initials | Notes |
|------|----------|----------|-------|
| SBC physically mounted and secured | [ ] | | |
| Enclosure properly grounded | [ ] | | |
| Power supply connected (5V DC, adequate amperage) | [ ] | | |
| Power supply voltage verified (4.9-5.2V) | [ ] | | |
| Ethernet cable connected (CAT5e or better) | [ ] | | |
| Ethernet link light active | [ ] | | |
| All sensor wiring complete | [ ] | | |
| All actuator wiring complete | [ ] | | |
| Proper wire terminations (no exposed copper) | [ ] | | |
| Cable strain relief installed | [ ] | | |
| Enclosure sealed (IP rating appropriate) | [ ] | | |
| Ambient temperature within operating range | [ ] | | |
| Humidity within operating range | [ ] | | |

### 1.2 Wiring Verification

**Reference:** Wiring diagrams in `OPERATOR.md`

| Connection | Terminal | Expected | Verified | Initials |
|------------|----------|----------|----------|----------|
| I2C SDA | [Pin] | Pull-up to 3.3V | [ ] | |
| I2C SCL | [Pin] | Pull-up to 3.3V | [ ] | |
| 1-Wire Data | GPIO [X] | 4.7kΩ pull-up | [ ] | |
| Sensor 1: [Name] | [Terminal] | [Description] | [ ] | |
| Sensor 2: [Name] | [Terminal] | [Description] | [ ] | |
| Actuator 1: [Name] | GPIO [X] | [Description] | [ ] | |
| Actuator 2: [Name] | GPIO [X] | [Description] | [ ] | |
| [Add rows as needed] | | | | |

### 1.3 Electrical Safety Verification

| Item | Verified | Initials | Notes |
|------|----------|----------|-------|
| Correct polarity on all connections | [ ] | | |
| No short circuits | [ ] | | |
| Isolation between low and high voltage | [ ] | | |
| E-STOP wired correctly (if applicable) | [ ] | | |
| Actuator power can be de-energized independently | [ ] | | |

---

## 2. Software Installation Verification

### 2.1 Operating System

| Item | Expected | Actual | Pass |
|------|----------|--------|------|
| OS Distribution | [e.g., Raspberry Pi OS Lite] | | [ ] |
| OS Version | [e.g., Bookworm] | | [ ] |
| Kernel Version | 6.1+ | | [ ] |
| Architecture | arm64/armhf | | [ ] |

**Verification Commands:**
```bash
cat /etc/os-release
uname -r
uname -m
```

### 2.2 Dependencies

| Package | Required | Installed | Pass |
|---------|----------|-----------|------|
| libncurses | Yes | | [ ] |
| libsqlite3 | Yes | | [ ] |
| libcurl | Yes | | [ ] |
| libcjson | Yes | | [ ] |
| libgpiod | Yes | | [ ] |
| p-net library | Yes | | [ ] |

**Verification:**
```bash
dpkg -l | grep -E "ncurses|sqlite|curl|cjson|gpiod"
ls /usr/local/lib/libpnet*
```

### 2.3 Firmware Installation

| Item | Expected | Actual | Pass |
|------|----------|--------|------|
| Firmware Version | v[X.Y.Z] | | [ ] |
| Installation Path | /usr/local/bin/profinet-monitor | | [ ] |
| Configuration Path | /etc/profinet-monitor/ | | [ ] |
| Database Path | /var/lib/profinet-monitor/ | | [ ] |
| Log Path | /var/log/profinet-monitor/ | | [ ] |

**Verification:**
```bash
profinet-monitor --version
ls -la /etc/profinet-monitor/
ls -la /var/lib/profinet-monitor/
```

### 2.4 Service Configuration

| Item | Expected | Actual | Pass |
|------|----------|--------|------|
| Service installed | Yes | | [ ] |
| Service enabled | Yes | | [ ] |
| Service running | Yes | | [ ] |
| Service auto-restart on failure | Yes | | [ ] |

**Verification:**
```bash
systemctl status profinet-monitor
systemctl is-enabled profinet-monitor
```

---

## 3. Network Configuration

### 3.1 Network Settings

| Parameter | Configured Value | Verified | Pass |
|-----------|------------------|----------|------|
| IP Address | | [ ] | [ ] |
| Subnet Mask | | [ ] | [ ] |
| Gateway | | [ ] | [ ] |
| DNS Server(s) | | [ ] | [ ] |
| PROFINET Station Name | | [ ] | [ ] |
| Hostname | | [ ] | [ ] |

**Verification:**
```bash
ip addr show eth0
ip route
cat /etc/hostname
```

### 3.2 Controller Connectivity

| Test | Command | Expected Result | Actual | Pass |
|------|---------|-----------------|--------|------|
| Ping controller | `ping [controller IP]` | Response < 10ms | | [ ] |
| Ping gateway | `ping [gateway]` | Response | | [ ] |
| DNS resolution | `nslookup [hostname]` | Resolves | | [ ] |

### 3.3 PROFINET Configuration

| Parameter | Value | Verified | Pass |
|-----------|-------|----------|------|
| Station Name | [rtu-xxx] | [ ] | [ ] |
| Vendor ID | 0x0493 | [ ] | [ ] |
| Device ID | 0x0001 | [ ] | [ ] |
| Network Interface | eth0 | [ ] | [ ] |

---

## 4. Sensor Configuration and Calibration

### 4.1 Sensor Discovery

Run sensor discovery and verify all sensors detected:

```bash
sudo profinet-monitor --scan
```

| Sensor | Type | Address/Pin | Discovered | Pass |
|--------|------|-------------|------------|------|
| [Name] | [Type] | [I2C/GPIO/1W] | [ ] | [ ] |
| [Name] | [Type] | [I2C/GPIO/1W] | [ ] | [ ] |
| [Add rows] | | | | |

### 4.2 Sensor Calibration

**Calibrate each analog sensor against known reference:**

#### Sensor: [Name]

| Calibration Point | Reference Value | Measured Value | Error | Within Spec | Pass |
|-------------------|-----------------|----------------|-------|-------------|------|
| Zero/Low | | | | ±[X]% | [ ] |
| Mid-range | | | | ±[X]% | [ ] |
| Span/High | | | | ±[X]% | [ ] |

**Calibration Date:** ________________
**Calibrated By:** ________________
**Reference Standard:** ________________

[Repeat for each sensor requiring calibration]

### 4.3 Sensor Configuration in Database

Verify each sensor is correctly configured:

| Sensor Name | Slot | Type | Unit | Poll Rate | Verified |
|-------------|------|------|------|-----------|----------|
| [Name] | [1-8] | [Type] | [Unit] | [ms] | [ ] |
| [Add rows] | | | | | |

---

## 5. Actuator Configuration and Testing

### 5.1 Actuator Configuration

| Actuator Name | Slot | GPIO | Type | Safe State | Active Low | Verified |
|---------------|------|------|------|------------|------------|----------|
| [Name] | [9-15] | [X] | [Pump/Valve/Relay] | [OFF/ON] | [Yes/No] | [ ] |
| [Add rows] | | | | | | |

### 5.2 Actuator Function Testing

**WARNING:** Ensure process is in safe condition before testing actuators.

#### Actuator: [Name]

| Test | Command | Expected | Observed | Pass |
|------|---------|----------|----------|------|
| OFF command | Send 0x00 | De-energized, confirmed OFF | | [ ] |
| ON command | Send 0x01 | Energized, confirmed ON | | [ ] |
| PWM 50% (if applicable) | Send 0x02, duty=50 | 50% duty cycle | | [ ] |
| Return to OFF | Send 0x00 | De-energized | | [ ] |

[Repeat for each actuator]

---

## 6. PROFINET Communication Verification

### 6.1 Connection Establishment

| Step | Expected | Observed | Pass |
|------|----------|----------|------|
| RTU visible in DCP discovery | Yes | | [ ] |
| Controller connects to RTU | AR established | | [ ] |
| Connection state = RUN | Yes | | [ ] |
| LED status = Green | Yes | | [ ] |

### 6.2 Cyclic Data Exchange

| Test | Expected | Observed | Pass |
|------|----------|----------|------|
| Sensor values appear in controller | All configured sensors | | [ ] |
| Data updates at cycle rate | [X] ms | | [ ] |
| Data quality = GOOD (0x00) | Yes for all sensors | | [ ] |
| Actuator commands reach RTU | Commands executed | | [ ] |
| Command latency | < 100 ms | | [ ] |

### 6.3 Data Quality Verification

| Sensor | Slot | Value Correct | Quality = GOOD | Pass |
|--------|------|---------------|----------------|------|
| [Name] | [X] | | [ ] | [ ] |
| [Add rows] | | | | |

---

## 7. Alarm System Testing

### 7.1 Threshold Alarm Testing

| Alarm | Test Method | Alarm Raised | Alarm Cleared | Pass |
|-------|-------------|--------------|---------------|------|
| [Sensor]-High | Lower threshold temporarily | [ ] | [ ] | [ ] |
| [Sensor]-Low | Raise threshold temporarily | [ ] | [ ] | [ ] |
| [Add rows] | | | | |

### 7.2 Communication Alarm Testing

| Alarm | Test Method | Alarm Raised | Pass |
|-------|-------------|--------------|------|
| PROFINET Disconnect | Disconnect Ethernet | [ ] | [ ] |
| Sensor Timeout | Disconnect sensor | [ ] | [ ] |

### 7.3 Alarm Acknowledgment

| Location | Acknowledgment Works | Pass |
|----------|---------------------|------|
| RTU TUI | [ ] | [ ] |
| Controller HMI | [ ] | [ ] |

---

## 8. Safe State Verification

**CRITICAL TEST - Ensure process is safe before conducting**

### 8.1 PROFINET Connection Loss Test

| Step | Action | Expected | Observed | Pass |
|------|--------|----------|----------|------|
| 1 | Record current actuator states | | | |
| 2 | Disconnect Ethernet cable | | | |
| 3 | Wait for timeout ([X] seconds) | LED turns red | | [ ] |
| 4 | Verify actuator 1 state | [Safe state] | | [ ] |
| 5 | Verify actuator 2 state | [Safe state] | | [ ] |
| 6 | [Verify all actuators] | | | |
| 7 | Reconnect Ethernet | Connection restored | | [ ] |
| 8 | Verify LED returns to green | Green | | [ ] |
| 9 | Verify actuators respond to commands | Commands executed | | [ ] |

### 8.2 Interlock Testing (if implemented)

| Interlock | Trigger Condition | Expected Action | Observed | Pass |
|-----------|-------------------|-----------------|----------|------|
| [Name] | [Condition] | [Action] | | [ ] |
| [Add rows] | | | | |

---

## 9. Backup and Documentation

### 9.1 Configuration Backup

| Item | Completed | Location | Pass |
|------|-----------|----------|------|
| Database backup created | [ ] | | [ ] |
| Configuration file backup | [ ] | | [ ] |
| Network configuration documented | [ ] | | [ ] |
| GSDML file archived | [ ] | | [ ] |

**Backup Command:**
```bash
sudo cp /var/lib/profinet-monitor/*.db /var/backup/profinet-monitor/
sudo cp /etc/profinet-monitor/* /var/backup/profinet-monitor/
```

### 9.2 Documentation Handoff

| Document | Provided | Location |
|----------|----------|----------|
| As-built wiring diagram | [ ] | |
| Sensor calibration records | [ ] | |
| Network configuration | [ ] | |
| OPERATOR.md (printed or accessible) | [ ] | |
| ALARM_RESPONSE.md | [ ] | |
| SAFETY_INTERLOCKS.md | [ ] | |
| This completed checklist | [ ] | |

---

## 10. Operator Training

| Topic | Completed | Trainee Initials |
|-------|-----------|------------------|
| TUI navigation | [ ] | |
| Viewing sensor values | [ ] | |
| Viewing alarms | [ ] | |
| Acknowledging alarms | [ ] | |
| Basic troubleshooting | [ ] | |
| Emergency procedures | [ ] | |
| Who to call for support | [ ] | |

---

## 11. Final Verification

### 11.1 Continuous Operation Test

Run system for minimum [X] hours under normal operation:

| Check | Time | Status | Initials |
|-------|------|--------|----------|
| Start of test | | | |
| 1 hour | | | |
| 4 hours | | | |
| 24 hours | | | |

**Criteria for success:**
- [ ] No unexpected alarms
- [ ] All sensor values within expected range
- [ ] Actuators respond correctly to commands
- [ ] PROFINET connection stable
- [ ] No service restarts

### 11.2 System Status Summary

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Hardware | [ ] Pass [ ] Fail | |
| Software | [ ] Pass [ ] Fail | |
| Network | [ ] Pass [ ] Fail | |
| Sensors | [ ] Pass [ ] Fail | |
| Actuators | [ ] Pass [ ] Fail | |
| PROFINET | [ ] Pass [ ] Fail | |
| Alarms | [ ] Pass [ ] Fail | |
| Safe State | [ ] Pass [ ] Fail | |

---

## 12. Sign-Off

### 12.1 Commissioning Complete

I certify that all tests have been completed satisfactorily and the system is ready for operational use.

| Role | Name | Signature | Date |
|------|------|-----------|------|
| Commissioning Engineer | | | |
| Site Operator | | | |
| Project Manager | | | |

### 12.2 Conditional Acceptance

If there are outstanding items:

| Role | Name | Signature | Date |
|------|------|-----------|------|
| Acceptance with conditions | | | |

**Conditions:**

---

## 13. Punch List / Outstanding Items

| Item # | Description | Severity | Assigned To | Due Date | Status |
|--------|-------------|----------|-------------|----------|--------|
| 1 | | | | | |
| 2 | | | | | |
| 3 | | | | | |

---

## 14. Notes and Observations

[Additional notes, recommendations, or observations from commissioning]

---

## 15. Appendices

### A. Wiring Diagram (As-Built)

[Attach or reference as-built wiring diagram]

### B. Calibration Certificates

[Attach calibration certificates for reference standards used]

### C. Network Diagram

[Attach network topology diagram showing RTU and controller connectivity]

---

**Document Control:**

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-12-22 | Documentation Audit | Initial template |
