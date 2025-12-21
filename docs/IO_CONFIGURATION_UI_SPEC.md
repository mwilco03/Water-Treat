# I/O Configuration UI Specification

## Core Philosophy

**User's mental model**: "I plugged a wire into pin X, help me tell the system what it is"

**NOT**: "Select protocol, then address, then pin, then type, then units..."

---

## Navigation Contract

**ESC always goes back exactly one step. Always. No exceptions.**

- Screen 1 → ESC → Return to main menu
- Screen 2 → ESC → Screen 1
- Screen 3 → ESC → Screen 2
- etc.

---

## The Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 1: The Fundamental Split                                            │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Configure New I/O Point                              ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  What does this connection do?                                          ║│
│  ║                                                                         ║│
│  ║     ┌─────────────────────────────────────────────────────────────────┐ ║│
│  ║     │  [1] INPUT  - Reads the physical world                         │ ║│
│  ║     │              Temperature, pressure, flow, level                │ ║│
│  ║     │              (sensors, probes, meters)                         │ ║│
│  ║     └─────────────────────────────────────────────────────────────────┘ ║│
│  ║                                                                         ║│
│  ║     ┌─────────────────────────────────────────────────────────────────┐ ║│
│  ║     │  [2] OUTPUT - Changes the physical world                       │ ║│
│  ║     │              Pumps, valves, relays, solenoids                  │ ║│
│  ║     │              (actuators, switches)                             │ ║│
│  ║     └─────────────────────────────────────────────────────────────────┘ ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [1/2] Select  │  [ESC] Cancel                                          ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: None (instant navigation)                                   │
│  ESC: Return to previous screen (sensors/actuators page)                    │
│  Enter/1/2: Proceed to Screen 2                                             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## INPUT Path (Sensors)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 2A: How Is It Connected?                                            │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    INPUT: Connection Type                               ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  How is your sensor connected?                                          ║│
│  ║                                                                         ║│
│  ║     [S] SCAN - Let me look for connected devices                        ║│
│  ║                I'll check I2C buses and 1-Wire for you                  ║│
│  ║                                                                         ║│
│  ║     [G] GPIO PIN - Direct wire to a specific pin                        ║│
│  ║                    Flow meters, float switches, pulse counters          ║│
│  ║                                                                         ║│
│  ║     [A] ADC CHANNEL - Analog voltage reading via ADC                    ║│
│  ║                       pH probes, pressure transducers, 4-20mA           ║│
│  ║                       (requires I2C ADC like ADS1115)                   ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [S/G/A] Select  │  [ESC] Back to Input/Output                          ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: None                                                        │
│  ESC: Back to Screen 1                                                      │
│  S: Proceed to Screen 3A (Scan)                                             │
│  G: Proceed to Screen 3B (GPIO Picker)                                      │
│  A: Proceed to Screen 3C (ADC Channel Picker)                               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

### Screen 3A: Discovery Scan

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3A: Scanning Hardware                                               │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Scanning for Devices...                              ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  Checking I2C bus 1...  [################....] 80%                      ║│
│  ║                                                                         ║│
│  ║  Found so far:                                                          ║│
│  ║    • I2C 0x48: ADS1115 (4-channel ADC) - has 4 analog inputs            ║│
│  ║    • I2C 0x76: BME280 (Temp/Humidity/Pressure)                          ║│
│  ║                                                                         ║│
│  ║  Checking 1-Wire bus...                                                 ║│
│  ║    • 28-00000abc1234: DS18B20 Temperature (currently 23.5°C)            ║│
│  ║    • 28-00000def5678: DS18B20 Temperature (currently 24.1°C)            ║│
│  ║                                                                         ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: hw_discover_all() - scans I2C buses 0-7, 1-Wire bus         │
│  ESC: Cancel scan, return to Screen 2A                                      │
│  Completion: Auto-advance to Screen 3A-Results                              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Screen 3A-Results: Pick Discovered Device

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3A-Results: Select Device                                           │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Found 5 Devices                                      ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  Select the device you want to configure:                               ║│
│  ║                                                                         ║│
│  ║   ► I2C 0x48: ADS1115 ADC Channel 0         [available]                 ║│
│  ║     I2C 0x48: ADS1115 ADC Channel 1         [available]                 ║│
│  ║     I2C 0x48: ADS1115 ADC Channel 2         [available]                 ║│
│  ║     I2C 0x48: ADS1115 ADC Channel 3         [available]                 ║│
│  ║     I2C 0x76: BME280 (Temp/Humid/Press)     [available]                 ║│
│  ║     1-Wire: DS18B20 (28-00000abc1234)       23.5°C [available]          ║│
│  ║     1-Wire: DS18B20 (28-00000def5678)       24.1°C [in use: tank_temp]  ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [R] Rescan  │  [ESC] Back         ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: Check database for existing sensors using these addresses   │
│  ESC: Back to Screen 2A (connection type)                                   │
│  Enter: If available, proceed to Screen 4 (Name it)                         │
│         If in use, show conflict dialog                                     │
│  R: Re-run discovery scan                                                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

### Screen 3B: GPIO Pin Picker (Direct Wire)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3B: Select GPIO Pin                                                 │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    GPIO Input Pin Selection                             ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  Board: Raspberry Pi 4                                                  ║│
│  ║  Which GPIO pin is your sensor connected to?                            ║│
│  ║                                                                         ║│
│  ║  Suggested Input Pins:                                                  ║│
│  ║   ► GPIO 17 (Pin 11)    [available]                                     ║│
│  ║     GPIO 27 (Pin 13)    [available]                                     ║│
│  ║     GPIO 22 (Pin 15)    [in use: flow_meter_1]                          ║│
│  ║     GPIO  5 (Pin 29)    [available]                                     ║│
│  ║     GPIO  6 (Pin 31)    [available]                                     ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [M] Manual entry  │  [ESC] Back   ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: board_detect() for pin list, DB check for conflicts         │
│  ESC: Back to Screen 2A                                                     │
│  Enter: Proceed to Screen 3B-Type (what kind of GPIO input?)                │
│  M: Allow manual pin number entry                                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Screen 3B-Type: GPIO Input Type

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3B-Type: What Kind of Input?                                        │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    GPIO 17: Input Type                                  ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  What's connected to GPIO 17?                                           ║│
│  ║                                                                         ║│
│  ║   ► Flow Meter / Pulse Counter                                          ║│
│  ║     Counts pulses per second (e.g., hall effect flow sensor)            ║│
│  ║                                                                         ║│
│  ║     Float Switch / Level Sensor                                         ║│
│  ║     Simple on/off state (high = triggered, low = normal)                ║│
│  ║                                                                         ║│
│  ║     DHT22 Temperature/Humidity                                          ║│
│  ║     Single-wire digital temp/humidity sensor                            ║│
│  ║                                                                         ║│
│  ║     Generic Digital Input                                               ║│
│  ║     Any on/off signal                                                   ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [ESC] Back to pin selection       ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: None                                                        │
│  ESC: Back to Screen 3B (pin picker)                                        │
│  Enter: Proceed to Screen 4 (Name it)                                       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

### Screen 3C: ADC Channel Picker

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3C: ADC Channel Selection                                           │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Select ADC Channel                                   ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  Scanning for ADC devices...                                            ║│
│  ║                                                                         ║│
│  ║  Found: ADS1115 at I2C address 0x48                                     ║│
│  ║                                                                         ║│
│  ║  Available channels:                                                    ║│
│  ║   ► Channel 0 (A0)    [available]                                       ║│
│  ║     Channel 1 (A1)    [in use: ph_probe]                                ║│
│  ║     Channel 2 (A2)    [available]                                       ║│
│  ║     Channel 3 (A3)    [available]                                       ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [ESC] Back                        ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: hw_discover_i2c_all(), filter for ADC types                 │
│  ESC: Back to Screen 2A                                                     │
│  Enter: Proceed to Screen 3C-Type (what analog sensor?)                     │
│                                                                             │
│  If no ADC found:                                                           │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║  No ADC devices found on I2C bus.                                       ║│
│  ║                                                                         ║│
│  ║  ADC chips like ADS1115 convert analog voltages to digital readings.    ║│
│  ║  Connect an ADC to the I2C bus (SDA/SCL pins) and try again.            ║│
│  ║                                                                         ║│
│  ║  [R] Rescan  │  [ESC] Back                                              ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Screen 3C-Type: Analog Sensor Type

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3C-Type: What Analog Sensor?                                        │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    ADS1115 Channel 0: Sensor Type                       ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  What's connected to this ADC channel?                                  ║│
│  ║                                                                         ║│
│  ║   ► pH Probe                                                            ║│
│  ║     Measures acidity/alkalinity (0-14 pH)                               ║│
│  ║                                                                         ║│
│  ║     Pressure Transducer                                                 ║│
│  ║     4-20mA or 0-5V pressure sensor                                      ║│
│  ║                                                                         ║│
│  ║     TDS Sensor (Total Dissolved Solids)                                 ║│
│  ║     Measures water purity in ppm                                        ║│
│  ║                                                                         ║│
│  ║     Turbidity Sensor                                                    ║│
│  ║     Measures water clarity                                              ║│
│  ║                                                                         ║│
│  ║     Generic 0-5V Analog                                                 ║│
│  ║     Custom voltage-to-value mapping                                     ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [ESC] Back to channel selection   ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: None                                                        │
│  ESC: Back to Screen 3C (channel picker)                                    │
│  Enter: Proceed to Screen 4 (Name it)                                       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Screen 4: Name It (Universal for all sensor types)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 4: Give It a Name                                                   │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Name Your Sensor                                     ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  You're adding: DS18B20 Temperature Sensor                              ║│
│  ║  Connected via: 1-Wire (28-00000abc1234)                                ║│
│  ║  Current reading: 23.5°C                                                ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║                                                                         ║│
│  ║  Name: [intake_tank_temp________________]                               ║│
│  ║                                                                         ║│
│  ║  Use lowercase with underscores. Examples:                              ║│
│  ║    intake_pressure, clearwell_level, chlorine_flow                      ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [Enter] Continue  │  [ESC] Back to device selection                    ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: Validate name (unique, valid characters)                    │
│  ESC: Back to previous screen (device/pin/channel selection)                │
│  Enter: If valid, proceed to Screen 5 (Confirm)                             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Screen 5: Confirm and Save

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 5: Confirm Configuration                                            │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Confirm New Sensor                                   ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  Ready to add this sensor:                                              ║│
│  ║                                                                         ║│
│  ║    Name:       intake_tank_temp                                         ║│
│  ║    Type:       DS18B20 Temperature                                      ║│
│  ║    Connection: 1-Wire (28-00000abc1234)                                 ║│
│  ║    Unit:       °C                                                       ║│
│  ║    Poll Rate:  1000 ms                                                  ║│
│  ║    PROFINET:   Slot 1, Subslot 1                                        ║│
│  ║                                                                         ║│
│  ║  Current reading: 23.5°C ✓                                              ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║                                                                         ║│
│  ║     [ Save ]        [ Edit Advanced ]        [ Cancel ]                 ║│
│  ║                                                                         ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: Auto-assign next available PROFINET slot                    │
│  ESC: Back to Screen 4 (naming)                                             │
│  Save: Write to database, return to sensor list with success message        │
│  Edit Advanced: Show full form for power users (slot, poll rate, etc.)      │
│  Cancel: Return to Screen 1                                                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## OUTPUT Path (Actuators)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 2B: Output Type                                                     │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    OUTPUT: What Are You Controlling?                    ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  What kind of output is this?                                           ║│
│  ║                                                                         ║│
│  ║   ► Relay / Digital Output                                              ║│
│  ║     Simple on/off control (pumps, valves, lights)                       ║│
│  ║                                                                         ║│
│  ║     PWM / Variable Speed                                                ║│
│  ║     Proportional control (VFD pumps, dimming)                           ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [ESC] Back to Input/Output        ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: None                                                        │
│  ESC: Back to Screen 1                                                      │
│  Enter: Proceed to Screen 3D (GPIO output picker)                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Screen 3D: Output Pin Selection

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3D: Select Output Pin                                               │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    GPIO Output Pin Selection                            ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  Board: Raspberry Pi 4                                                  ║│
│  ║  Which GPIO pin controls your relay/actuator?                           ║│
│  ║                                                                         ║│
│  ║  Suggested Relay Pins:                                                  ║│
│  ║   ► GPIO 17 (Pin 11) - Relay 1    [available]                           ║│
│  ║     GPIO 27 (Pin 13) - Relay 2    [available]                           ║│
│  ║     GPIO 22 (Pin 15) - Relay 3    [in use: chlorine_pump]               ║│
│  ║     GPIO 23 (Pin 16) - Relay 4    [available]                           ║│
│  ║                                                                         ║│
│  ║  PWM Capable:                                                           ║│
│  ║     GPIO 18 (Pin 12) - PWM0       [available]                           ║│
│  ║     GPIO 19 (Pin 35) - PWM1       [available]                           ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [M] Manual  │  [ESC] Back         ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: board_detect() for pin list, filter by output capability    │
│  ESC: Back to Screen 2B                                                     │
│  Enter: Proceed to Screen 3D-Type                                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Screen 3D-Type: What Does It Control?

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 3D-Type: What Does This Control?                                    │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    GPIO 17: Output Type                                 ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  What is GPIO 17 controlling?                                           ║│
│  ║                                                                         ║│
│  ║   ► Pump                                                                ║│
│  ║     Chemical dosing, transfer, circulation                              ║│
│  ║                                                                         ║│
│  ║     Valve / Solenoid                                                    ║│
│  ║     Flow control, isolation, diversion                                  ║│
│  ║                                                                         ║│
│  ║     Generic Relay                                                       ║│
│  ║     Any on/off controlled device                                        ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Navigate  │  [Enter] Select  │  [ESC] Back to pin selection       ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: None                                                        │
│  ESC: Back to Screen 3D                                                     │
│  Enter: Proceed to Screen 4B (Name it)                                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Screen 4B: Name the Actuator

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 4B: Name Your Actuator                                              │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Name Your Actuator                                   ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  You're adding: Pump on GPIO 17                                         ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║                                                                         ║│
│  ║  Name: [chlorine_pump____________________]                              ║│
│  ║                                                                         ║│
│  ║  Use lowercase with underscores. Examples:                              ║│
│  ║    intake_valve, backwash_pump, effluent_solenoid                       ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [Enter] Continue  │  [ESC] Back                                        ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: Validate name                                               │
│  ESC: Back to Screen 3D-Type                                                │
│  Enter: Proceed to Screen 5B (Confirm)                                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Screen 5B: Confirm Actuator

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  SCREEN 5B: Confirm Actuator                                                │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    Confirm New Actuator                                 ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  Ready to add this actuator:                                            ║│
│  ║                                                                         ║│
│  ║    Name:       chlorine_pump                                            ║│
│  ║    Type:       Pump (Relay)                                             ║│
│  ║    GPIO Pin:   17                                                       ║│
│  ║    Safe State: OFF (de-energize on fault)                               ║│
│  ║    PROFINET:   Slot 9, Subslot 1                                        ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║                                                                         ║│
│  ║     [ Save ]        [ Edit Advanced ]        [ Cancel ]                 ║│
│  ║                                                                         ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
│  System action: Auto-assign PROFINET output slot (9-15)                     │
│  ESC: Back to Screen 4B (naming)                                            │
│  Save: Write to database, return to actuator list                           │
│  Edit Advanced: Show full form (active-low, max on time, etc.)              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Conflict Handling (Graceful Degradation)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  CONFLICT DIALOG                                                            │
│  ─────────────────────────────────────────────────────────────────────────  │
│                                                                             │
│  ╔═════════════════════════════════════════════════════════════════════════╗│
│  ║                    ⚠ Pin Already In Use                                 ║│
│  ╠═════════════════════════════════════════════════════════════════════════╣│
│  ║                                                                         ║│
│  ║  GPIO 22 is currently used by:                                          ║│
│  ║    flow_meter_1 (Flow Sensor)                                           ║│
│  ║                                                                         ║│
│  ║  What would you like to do?                                             ║│
│  ║                                                                         ║│
│  ║   ► Choose a different pin                                              ║│
│  ║     Replace the existing sensor (delete flow_meter_1)                   ║│
│  ║                                                                         ║│
│  ║  ─────────────────────────────────────────────────────────────────────  ║│
│  ║  [↑↓] Select  │  [Enter] Confirm  │  [ESC] Cancel                       ║│
│  ╚═════════════════════════════════════════════════════════════════════════╝│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Summary: What We've Achieved

| Old Flow | New Flow |
|----------|----------|
| 15 fields upfront | 4-5 screens, 1-2 choices each |
| User picks protocol | System scans and discovers |
| User enters I2C address | System shows found devices |
| User picks slot number | System auto-assigns |
| Confusing type names | Plain English descriptions |
| ESC = lose everything | ESC = back one step |

## Implementation Priority

1. **Screen 1**: Input/Output split (trivial)
2. **Screen 2A/2B**: Connection type (simple menu)
3. **Screen 3A**: Discovery scan (hw_discover.c already exists!)
4. **Screen 3B/3C/3D**: Pin/channel pickers (board_detect.c already exists!)
5. **Screen 4**: Naming (single text field)
6. **Screen 5**: Confirmation with smart defaults

## Data Flow

```
User Choice           → System Inference
─────────────────────────────────────────────────────────
INPUT                 → Looking for sensors
  → SCAN              → hw_discover_all()
    → DS18B20 found   → type=physical, interface=1wire, driver=ds18b20
  → GPIO PIN          → interface=gpio
    → Flow Meter      → driver=flow_sensor, unit=pulses/s
  → ADC CHANNEL       → type=adc, interface=i2c
    → pH Probe        → driver=ph, unit=pH, range=0-14

OUTPUT                → Looking for actuators
  → RELAY             → type=relay
    → Pump            → driver=pump, safe_state=OFF
```

The system derives the technical details from the user's plain-language choices.
