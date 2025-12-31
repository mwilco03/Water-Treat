# Water Treatment RTU - Operator Manual

## Table of Contents
1. [Quick Start Guide](#quick-start-guide)
2. [Hardware Setup](#hardware-setup)
3. [Sensor Wiring Diagrams](#sensor-wiring-diagrams)
4. [First-Time Configuration](#first-time-configuration)
5. [Calibration Procedures](#calibration-procedures)
6. [TUI Navigation Guide](#tui-navigation-guide)
7. [Status Indicators](#status-indicators)
8. [Troubleshooting](#troubleshooting)
9. [Maintenance Procedures](#maintenance-procedures)

---

## Quick Start Guide

### Prerequisites
- Raspberry Pi 4B (or compatible SBC) with Raspbian/Debian
- Ethernet connection to PROFINET controller network
- Sensors connected per wiring diagrams below
- Power supply (5V 3A recommended)

### First Boot Checklist
```
□ Connect Ethernet cable
□ Connect sensors (do NOT power on yet)
□ Insert SD card with installed software
□ Power on the RTU
□ Wait for green LED (system ready)
□ Access TUI via SSH or local terminal
□ Run first-time setup wizard
□ Verify sensor readings
□ Connect to PROFINET controller
```

### Default Credentials

**SSH Access:**
- Username: `pi` / Password: `raspberry` (change immediately!)

**TUI Login:**
- Username: `admin` / Password: `H2OhYeah!`
- After 3 failed attempts, a password hint is displayed
- See README.md for instructions on changing default credentials

---

## Hardware Setup

### Supported Single Board Computers

| Board | GPIO Pins | I2C | SPI | 1-Wire | PWM | Notes |
|-------|-----------|-----|-----|--------|-----|-------|
| Raspberry Pi 5 | 26 | Yes | Yes | Yes | 2ch | Recommended, uses gpiochip4 |
| Raspberry Pi 4B | 26 | Yes | Yes | Yes | 2ch | Well tested |
| Raspberry Pi 3B+ | 26 | Yes | Yes | Yes | 2ch | Supported |
| Orange Pi Zero 3 | 26 | Yes | Yes | Yes | 2ch | Good alternative (H618) |
| Orange Pi 2W | 26 | Yes | Yes | Yes | 2ch | Compact (H618) |
| ODROID-XU4 | 30 | Yes | Yes | Yes | 1ch | High performance |
| ODROID-C4 | 40 | Yes | Yes | Yes | 2ch | S905X3 SoC |
| ODROID-N2+ | 40 | Yes | Yes | Yes | 2ch | S922X SoC |
| Luckfox Lyra | 20 | Yes | Yes | Limited | 1ch | Compact option (RK3506) |

### Pin Header Reference (40-pin Raspberry Pi)

```
                    3V3  (1) (2)  5V
                  GPIO2  (3) (4)  5V       ← I2C SDA
                  GPIO3  (5) (6)  GND      ← I2C SCL
                  GPIO4  (7) (8)  GPIO14   ← 1-Wire default
                    GND  (9) (10) GPIO15
                 GPIO17 (11) (12) GPIO18   ← PWM0
                 GPIO27 (13) (14) GND
                 GPIO22 (15) (16) GPIO23
                    3V3 (17) (18) GPIO24
                 GPIO10 (19) (20) GND      ← SPI MOSI
                  GPIO9 (21) (22) GPIO25   ← SPI MISO
                 GPIO11 (23) (24) GPIO8    ← SPI SCLK / CE0
                    GND (25) (26) GPIO7    ← SPI CE1
                  GPIO0 (27) (28) GPIO1
                  GPIO5 (29) (30) GND
                  GPIO6 (31) (32) GPIO12   ← PWM0 alt
                 GPIO13 (33) (34) GND      ← PWM1
                 GPIO19 (35) (36) GPIO16
                 GPIO26 (37) (38) GPIO20
                    GND (39) (40) GPIO21
```

### Default Pin Assignments (Raspberry Pi)

| Function | GPIO | Physical Pin | Notes |
|----------|------|--------------|-------|
| Relay 1 | 17 | 11 | Pump control |
| Relay 2 | 27 | 13 | Valve 1 |
| Relay 3 | 22 | 15 | Valve 2 |
| Relay 4 | 23 | 16 | Spare |
| Status LED | 24 | 18 | Green = OK |
| Error LED | 25 | 22 | Red = Fault |
| 1-Wire | 4 | 7 | DS18B20 temp |
| I2C SDA | 2 | 3 | ADC/sensors |
| I2C SCL | 3 | 5 | ADC/sensors |
| PWM 0 | 18 | 12 | Variable pump |
| PWM 1 | 13 | 33 | Variable valve |

### Orange Pi Zero 3 / 2W Pin Assignments (H618)

| Function | GPIO | Pin Name | Physical Pin | Notes |
|----------|------|----------|--------------|-------|
| Relay 1 | 73 | PI9 | 7 | Pump control |
| Relay 2 | 74 | PI10 | 11 | Valve 1 |
| Relay 3 | 75 | PI11 | 13 | Valve 2 |
| Relay 4 | 76 | PI12 | 15 | Spare |
| I2C-2 SDA | 70 | PI6 | 3 | ADC/sensors |
| I2C-2 SCL | 69 | PI5 | 5 | ADC/sensors |
| 1-Wire | 65 | PI1 | 12 | DS18B20 temp |
| PWM 0 | 79 | PI15 | 32 | Variable pump |

**GPIO Chip:** `gpiochip0` (all pins)

### ODROID-C4 / N2+ Pin Assignments (Amlogic)

| Function | GPIO | Pin Name | Physical Pin | Notes |
|----------|------|----------|--------------|-------|
| Relay 1 | 481 | GPIOX.3 | 7 | Pump control |
| Relay 2 | 482 | GPIOX.4 | 11 | Valve 1 |
| Relay 3 | 483 | GPIOX.5 | 13 | Valve 2 |
| Relay 4 | 484 | GPIOX.6 | 15 | Spare |
| I2C-2 SDA | 493 | GPIOX.17 | 3 | ADC/sensors |
| I2C-2 SCL | 494 | GPIOX.18 | 5 | ADC/sensors |
| PWM 0 | 476 | GPIOX.0 | 12 | Variable pump |

**GPIO Chip:** `gpiochip1` (main GPIO banks)

### ODROID-XU4 Pin Assignments (Exynos)

| Function | GPIO | Pin Name | Physical Pin | Notes |
|----------|------|----------|--------------|-------|
| Relay 1 | 25 | GPX1.1 | 11 | Pump control |
| Relay 2 | 31 | GPX1.7 | 13 | Valve 1 |
| Relay 3 | 28 | GPX1.4 | 15 | Valve 2 |
| Relay 4 | 30 | GPX1.6 | 16 | Spare |
| I2C-1 SDA | - | I2C1_SDA | 3 | ADC/sensors |
| I2C-1 SCL | - | I2C1_SCL | 5 | ADC/sensors |

**GPIO Chip:** `gpiochip1` (GPX/GPB banks)

> **Note:** The firmware auto-detects your board type and uses the appropriate pin mappings.
> You can view detected pins in the Setup Wizard (Hardware Detection step) or by pressing 'P' in the Actuator dialog.

---

## Sensor Wiring Diagrams

### DS18B20 Temperature Sensor (1-Wire)

```
    DS18B20                 Raspberry Pi
    ┌─────┐
    │ GND ├─────────────────── GND (Pin 6)
    │ DQ  ├─────────────────── GPIO4 (Pin 7)
    │ VDD ├─────────────────── 3.3V (Pin 1)
    └─────┘
           │
           ├──[4.7kΩ]──── 3.3V (Pull-up resistor)
           │

    Note: Multiple DS18B20 can share the same bus
    Each has unique 64-bit address
```

### pH Sensor (via ADS1115 ADC)

```
    pH Probe          pH Amplifier Board        ADS1115         Raspberry Pi
    ┌─────┐           ┌───────────────┐        ┌───────┐
    │ BNC ├───────────┤ pH IN         │        │       │
    └─────┘           │               │        │       │
                      │ V+  ──────────┼────────┤ VDD   ├──── 3.3V (Pin 1)
                      │ GND ──────────┼────────┤ GND   ├──── GND (Pin 6)
                      │ Po  ──────────┼────────┤ A0    │
                      │ To  ──────────┼────────┤ A1    │     (Temp compensation)
                      └───────────────┘        │ SDA   ├──── GPIO2 (Pin 3)
                                               │ SCL   ├──── GPIO3 (Pin 5)
                                               │ ADDR  ├──── GND (0x48)
                                               └───────┘

    Calibration: Use pH 4.0, 7.0, 10.0 buffer solutions
```

### TDS Sensor (via ADS1115 ADC)

```
    TDS Probe         TDS Module               ADS1115         Raspberry Pi
    ┌─────┐          ┌──────────┐             ┌───────┐
    │  +  ├──────────┤ Probe +  │             │       │
    │  -  ├──────────┤ Probe -  │             │       │
    └─────┘          │ VCC ─────┼─────────────┤ VDD   ├──── 5V (Pin 2)
                     │ GND ─────┼─────────────┤ GND   ├──── GND
                     │ A   ─────┼─────────────┤ A2    │
                     └──────────┘             │ SDA   ├──── GPIO2 (Pin 3)
                                              │ SCL   ├──── GPIO3 (Pin 5)
                                              └───────┘

    Note: TDS module runs on 5V, ADS1115 tolerates 5V input
```

### Turbidity Sensor (via ADS1115 ADC)

```
    Turbidity Sensor                          ADS1115         Raspberry Pi
    ┌────────────────┐                       ┌───────┐
    │ VCC (Red)      ├───────────────────────┤ VDD   ├──── 5V (Pin 2)
    │ GND (Black)    ├───────────────────────┤ GND   ├──── GND
    │ Signal (Yellow)├───────────────────────┤ A3    │
    └────────────────┘                       │ SDA   ├──── GPIO2
                                             │ SCL   ├──── GPIO3
                                             └───────┘

    Output: ~4.5V (clear) to ~0V (opaque)
```

### Relay Module (4-Channel)

```
    Relay Module                              Raspberry Pi
    ┌──────────────┐
    │ VCC          ├──────────────────────── 5V (Pin 2)
    │ GND          ├──────────────────────── GND (Pin 6)
    │ IN1          ├──────────────────────── GPIO17 (Pin 11)
    │ IN2          ├──────────────────────── GPIO27 (Pin 13)
    │ IN3          ├──────────────────────── GPIO22 (Pin 15)
    │ IN4          ├──────────────────────── GPIO23 (Pin 16)
    └──────────────┘

    Active LOW: GPIO=0 turns relay ON
    Maximum: 10A @ 250VAC per channel

    ⚠️ WARNING: Isolate high voltage wiring from low voltage!
```

### Float Switch (Digital Input)

```
    Float Switch                              Raspberry Pi
    ┌────────────┐
    │ Common     ├────────────────────────── GND (Pin 6)
    │ NO         ├────────────────────────── GPIO5 (Pin 29)
    └────────────┘                              │
                                               ├──[10kΩ]── 3.3V (Pull-up)

    Open = Float up (tank full)
    Closed = Float down (tank low)
```

---

## First-Time Configuration

### Setup Wizard

On first boot, the setup wizard will guide you through:

1. **Network Configuration**
   - DHCP or Static IP
   - Network interface selection

2. **PROFINET Setup**
   - Station name (must match PLC config)
   - Device name for identification

3. **Sensor Discovery**
   - Auto-detect connected I2C devices
   - Auto-detect 1-Wire sensors
   - Manual GPIO assignment for analog sensors

4. **Actuator Setup**
   - Relay pin assignment
   - PWM channel configuration
   - Safe state selection (ON/OFF/HOLD)

5. **Calibration**
   - Guided calibration for pH, TDS, etc.

### Manual Configuration

Edit `/etc/profinet-monitor/profinet-monitor.conf`:

```ini
[system]
device_name = water-treatment-rtu-01
log_level = info

[network]
interface = eth0
dhcp_enabled = true

[profinet]
station_name = wt-rtu-01
vendor_id = 0x0493
device_id = 0x0001
enabled = true

[health]
enabled = true
http_enabled = true
# Port 9081 for RTU plane (8xxx = Controller, 9xxx = RTU)
http_port = 9081
```

---

## Calibration Procedures

### pH Sensor Calibration

**Required Materials:**
- pH 7.0 buffer solution (neutral)
- pH 4.0 buffer solution (acidic)
- Distilled water for rinsing
- Clean, dry container

**Procedure:**

1. Navigate to TUI → F2 (Sensors) → Select pH sensor → Press 'C' for calibrate

2. **Neutral Point (pH 7.0):**
   ```
   a. Rinse probe with distilled water
   b. Immerse in pH 7.0 buffer
   c. Wait 60 seconds for stabilization
   d. Press ENTER to record point
   ```

3. **Acid Point (pH 4.0):**
   ```
   a. Rinse probe with distilled water
   b. Immerse in pH 4.0 buffer
   c. Wait 60 seconds for stabilization
   d. Press ENTER to record point
   ```

4. Verify calibration with known solution

**Expected Accuracy:** ±0.1 pH after calibration

### TDS Calibration

**Required Materials:**
- 1000 ppm TDS calibration solution
- Distilled water (0 ppm reference)

**Procedure:**

1. Navigate to TUI → F2 (Sensors) → Select TDS sensor → Press 'C'

2. **Zero Point:**
   ```
   a. Immerse in distilled water
   b. Wait 30 seconds
   c. Press ENTER (should read ~0 ppm)
   ```

3. **Span Point:**
   ```
   a. Immerse in 1000 ppm solution
   b. Wait 30 seconds
   c. Enter "1000" as reference value
   d. Press ENTER to calibrate
   ```

### Temperature Sensor Verification

DS18B20 sensors are factory-calibrated. Verify with:

1. Ice water bath: Should read 0°C ±0.5°C
2. Boiling water: Should read 100°C ±0.5°C (at sea level)

---

## TUI Navigation Guide

### Key Reference

**Global Keys:**

| Key | Action |
|-----|--------|
| F1 | System Configuration |
| F2 | Sensor Management |
| F3 | Network Settings |
| F4 | PROFINET Status |
| F5 | Live Status Dashboard |
| F6 | Alarm Configuration |
| F7 | Logging Settings |
| F8 | Actuator Management |
| F10 | Exit Application |
| ↑↓ | Navigate list items |
| ENTER | Select / Edit |
| ESC | Cancel / Back |
| TAB | Next field |

**Common Actions:**

| Key | Action |
|-----|--------|
| 'A' / 'N' | Add new item |
| 'D' | Delete selected |
| 'E' | Edit selected |
| 'C' | Calibrate sensor |
| 'R' | Refresh data |

**System Page (F1):**

| Key | Action |
|-----|--------|
| Ctrl+S | Save configuration to file |
| 'E' | Export config backup (timestamped) |
| 'I' | Import config from backup |

Config backups are saved to `/var/backup/profinet-monitor/`.

**Actuator Dialog:**

| Key | Action |
|-----|--------|
| 'P' | Open GPIO pin selector (shows board defaults) |
| Ctrl+S | Save actuator |

### Screen Layout

```
┌─────────────────────────────────────────────────────────────────────┐
│ Water Treatment RTU v1.0.0                    [Connected] 14:32:05  │
├─────────────────────────────────────────────────────────────────────┤
│ F1:System  F2:Sensors  F3:Network  F4:PROFINET  F5:Status  F6:Alarms│
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│                        [Page Content Area]                          │
│                                                                     │
│                                                                     │
├─────────────────────────────────────────────────────────────────────┤
│ Status: OK │ Sensors: 8/8 │ Alarms: 0 │ PROFINET: Connected         │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Sensor Management Workflow

### Adding a New Sensor

1. **Open Sensor Management (F2)**
2. **Press 'A' to add new sensor**
3. **Fill in the form fields:**

| Field | Description | Example |
|-------|-------------|---------|
| Name | Descriptive sensor name | "Tank 1 pH" |
| Slot | PROFINET slot (1-63) | 5 |
| Subslot | PROFINET subslot (usually 1) | 1 |
| Type | physical, adc, web_poll, calculated, static | "adc" |
| Hardware | Sensor hardware type | "ADS1115" |
| Interface | Communication interface | "i2c" |
| Address | Device address/ID | "0x48" |
| Bus | I2C bus number | 1 |
| Channel | ADC channel (0-3) | 0 |
| Gain | ADC gain setting | 1 |
| Ref Voltage | Reference voltage | 3.3 |
| Unit | Engineering unit | "pH" |
| Min Value | Minimum expected value | 0.0 |
| Max Value | Maximum expected value | 14.0 |
| Poll Rate | Polling interval in ms | 1000 |

4. **Press Tab to reach Save button, then Enter**
5. **Sensor immediately begins polling** (no restart required)

### Editing an Existing Sensor

1. **Navigate to sensor with ↑↓ keys**
2. **Press 'E' to edit**
3. **Modify desired fields**
4. **Press Tab → Save → Enter**
5. **Changes take effect immediately**

### Deleting a Sensor

1. **Navigate to sensor with ↑↓ keys**
2. **Press 'D' to delete**
3. **Confirm deletion with 'Y'**
4. **Sensor removed from polling immediately**

### Sensor Types Explained

| Type | Use Case | Configuration |
|------|----------|---------------|
| **physical** | DS18B20, DHT22, flow sensors | Interface, address, bus |
| **adc** | pH, TDS, turbidity via ADC | ADC type, channel, gain, calibration |
| **web_poll** | Remote HTTP sensors | URL, method, JSON path |
| **calculated** | Derived values | Formula, input sensors |
| **static** | Fixed test values | Initial value, writable flag |

### Live Value Monitoring

- **View Sensor Details:** Select sensor, press Enter
- **Refresh Values:** Press 'R' to reload all sensor values
- **Status Colors:**
  - Green: Normal operation
  - Yellow: Warning threshold exceeded
  - Red: Error or alarm state

---

## Status Indicators

### LED Indicators (ISA-101 / IEC 60073 Colors)

If WS2812B LED strip is installed and configured:

| Color | Animation | Meaning |
|-------|-----------|---------|
| **GREEN** | Solid | Normal operation, system OK |
| **YELLOW** | Slow blink (1 Hz) | Warning condition |
| **RED** | Fast blink (4 Hz) | Alarm active |
| **RED** | Solid | Fault condition |
| **BLUE** | Solid | Manual mode active |
| **CYAN** | Fast blink | Communication/data exchange active |
| **MAGENTA** | Pulsing | Calibration in progress |
| **WHITE** | Pulsing | Initializing/standby |
| **OFF** | - | LED disabled or not applicable |

**LED Assignment (8-LED default):**
- LED 0: System status
- LED 1: PROFINET connection status
- LED 2-5: Sensor status (slots 1-4)
- LED 6-7: Actuator status

**Configuration (`/etc/profinet-monitor/profinet-monitor.conf`):**
```ini
[led]
enabled = true
led_count = 8
brightness = 64
backend = auto    ; auto, spi, rpi_ws281x
spi_device = /dev/spidev0.0
gpio_pin = 18     ; For rpi_ws281x on Pi
```

### PROFINET Connection States

| State | Description | Action |
|-------|-------------|--------|
| DISCONNECTED | No controller connection | Check network, verify station name |
| CONNECTING | Handshake in progress | Wait 10-30 seconds |
| CONNECTED | Normal operation | None required |
| ERROR | Communication fault | Check cables, restart if persistent |

### Health Check Endpoints

Access via web browser or curl:

| Endpoint | Purpose | Response |
|----------|---------|----------|
| `http://RTU_IP:9081/health` | Overall status | JSON with subsystem details |
| `http://RTU_IP:9081/metrics` | Prometheus metrics | Text format |
| `http://RTU_IP:9081/ready` | Kubernetes readiness | `{"ready": true/false}` |
| `http://RTU_IP:9081/live` | Kubernetes liveness | `{"alive": true}` |

> **Note:** Port 9081 is the default for RTU plane services (9xxx range).
> Controller plane services use 8xxx range. Override via `WT_HTTP_PORT` environment variable.

---

## Troubleshooting

### Decision Tree

```
Problem: No sensor readings
│
├─► Check physical connections
│   ├─► Loose wire? → Reseat connection
│   └─► Correct pins? → Verify wiring diagram
│
├─► Check I2C bus (for ADC sensors)
│   └─► Run: sudo i2cdetect -y 1
│       ├─► No devices? → Check SDA/SCL wiring, pull-ups
│       └─► Device shown? → Check sensor config in TUI
│
├─► Check 1-Wire (for DS18B20)
│   └─► Run: ls /sys/bus/w1/devices/
│       ├─► No 28-* folders? → Check wiring, 4.7kΩ pull-up
│       └─► Devices listed? → Verify sensor ID in config
│
└─► Check logs
    └─► Run: journalctl -u profinet-monitor -f
```

### Common Issues

#### Issue: "PROFINET not connecting"

**Symptoms:** Status shows DISCONNECTED, no I/O data exchange

**Solutions:**
1. Verify station name matches PLC configuration exactly (case-sensitive)
2. Check Ethernet cable and link LED
3. Ensure RTU is on same network segment as controller
4. Verify GSD file imported into PLC engineering tool
5. Check firewall: PROFINET uses UDP ports 34964, 49152-65535

```bash
# Test network connectivity
ping <controller_ip>

# Check PROFINET ports
sudo netstat -ulnp | grep pnet
```

#### Issue: "Sensor shows -999 or NaN"

**Symptoms:** Invalid reading displayed

**Solutions:**
1. Sensor disconnected → Check wiring
2. ADC not responding → Run `i2cdetect -y 1`
3. Calibration corrupt → Re-run calibration
4. Sensor damaged → Replace sensor

#### Issue: "Relay not switching"

**Symptoms:** Pump/valve doesn't respond

**Solutions:**
1. Check GPIO pin assignment in TUI
2. Verify relay module power (5V)
3. Test with `gpioset gpiochip0 17=1` (replace pin number)
4. Check relay module jumper (VCC-JD connected for opto-isolation)
5. Verify active-low setting matches your relay module

#### Issue: "TUI won't start"

**Symptoms:** Terminal shows garbled output or crashes

**Solutions:**
1. Ensure terminal is at least 80x24 characters
2. Set TERM variable: `export TERM=xterm-256color`
3. Run in SSH: `ssh -t user@rtu`
4. Check ncurses installed: `apt install libncurses5`

### Log Analysis

```bash
# View real-time logs
journalctl -u profinet-monitor -f

# View last 100 lines
journalctl -u profinet-monitor -n 100

# Search for errors
journalctl -u profinet-monitor | grep -i error

# Export logs for support
journalctl -u profinet-monitor --since "1 hour ago" > rtu_logs.txt
```

### Factory Reset

To reset all configuration to defaults:

```bash
# Stop service
sudo systemctl stop profinet-monitor

# Backup current config (optional)
cp /etc/profinet-monitor/profinet-monitor.conf ~/config_backup.conf

# Remove configuration
sudo rm /etc/profinet-monitor/profinet-monitor.conf
sudo rm /var/lib/profinet-monitor/data.db

# Restart - will use defaults and trigger setup wizard
sudo systemctl start profinet-monitor
```

---

## Maintenance Procedures

### Daily Checks
- [ ] Verify PROFINET connection status (green LED solid)
- [ ] Check for active alarms in TUI (F6)
- [ ] Verify sensor readings are within expected ranges

### Weekly Checks
- [ ] Review alarm history
- [ ] Check disk space: `df -h /var/lib/profinet-monitor`
- [ ] Verify remote logging is functioning (if enabled)

### Monthly Checks
- [ ] Clean pH probe with cleaning solution
- [ ] Verify calibration with known solutions
- [ ] Check all wiring connections for corrosion
- [ ] Review and rotate logs if needed
- [ ] Update software if new version available

### Sensor Probe Maintenance

| Sensor | Maintenance | Frequency |
|--------|-------------|-----------|
| pH | Store in storage solution, clean with HCl | Weekly |
| TDS | Rinse with distilled water after use | Daily |
| Turbidity | Clean optical window with soft cloth | Weekly |
| DS18B20 | Visual inspection for corrosion | Monthly |

### Software Updates

```bash
# Check current version
profinet-monitor --version

# Update from package manager
sudo apt update
sudo apt upgrade profinet-monitor

# Or update from source
cd /path/to/Water-Treat
git pull
mkdir -p build && cd build
cmake ..
make -j$(nproc)
sudo make install
sudo systemctl restart profinet-monitor
```

---

## Support Information

### Getting Help
1. Check this operator manual first
2. Review logs: `journalctl -u profinet-monitor`
3. Check health endpoint: `http://RTU_IP:9081/health`
4. Consult SOURCES.md for code documentation

### Collecting Diagnostic Information

Before requesting support, gather:

```bash
# System information
profinet-monitor --version
uname -a
cat /proc/device-tree/model

# Network configuration
ip addr show
ip route

# I2C devices
sudo i2cdetect -y 1

# 1-Wire devices
ls /sys/bus/w1/devices/

# Recent logs
journalctl -u profinet-monitor --since "1 hour ago"

# Health status
curl http://localhost:9081/health
```

---

## Appendix A: Error Codes

| Code | Meaning | Resolution |
|------|---------|------------|
| E001 | Database initialization failed | Check disk space, permissions |
| E002 | PROFINET stack failed | Check network interface |
| E003 | Sensor manager failed | Check I2C/GPIO permissions |
| E004 | Configuration parse error | Validate config file syntax |
| E005 | GPIO access denied | Run as root or add to gpio group |
| E006 | I2C bus error | Check wiring, pull-up resistors |
| E007 | Network interface not found | Verify interface name |
| E008 | Calibration data corrupt | Re-run calibration |

## Appendix B: PROFINET Slot Mapping

| Slot | Type | Data | Description |
|------|------|------|-------------|
| 1 | Input | 4 bytes (float) | Sensor 1 (e.g., pH) |
| 2 | Input | 4 bytes (float) | Sensor 2 (e.g., TDS) |
| 3 | Input | 4 bytes (float) | Sensor 3 (e.g., Turbidity) |
| 4 | Input | 4 bytes (float) | Sensor 4 (e.g., Temp 1) |
| 5 | Input | 4 bytes (float) | Sensor 5 (e.g., Temp 2) |
| 6 | Input | 4 bytes (float) | Sensor 6 (e.g., Level) |
| 7 | Input | 4 bytes (float) | Sensor 7 (e.g., Pressure) |
| 8 | Input | 4 bytes (float) | Sensor 8 (spare) |
| 9 | Output | 4 bytes | Actuator 1 (e.g., Feed Pump) |
| 10 | Output | 4 bytes | Actuator 2 (e.g., Acid Pump) |
| 11 | Output | 4 bytes | Actuator 3 (e.g., Base Pump) |
| 12 | Output | 4 bytes | Actuator 4 (e.g., Inlet Valve) |
| 13 | Output | 4 bytes | Actuator 5 (e.g., Outlet Valve) |
| 14 | Output | 4 bytes | Actuator 6 (e.g., Drain Valve) |
| 15 | Output | 4 bytes | Actuator 7 (spare) |

---

*Document Version: 1.0.0*
*Last Updated: 2024-12-17*
