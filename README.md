# Water Treatment RTU

A PROFINET I/O Device implementation for industrial water treatment monitoring and control. This codebase provides the firmware for a Remote Terminal Unit (RTU) that interfaces physical sensors and actuators with industrial control systems via the PROFINET protocol.

## System Architecture

This RTU operates as **SBC #2** in a two-tier SCADA architecture:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         SCADA Network                                    │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                    ┌────────────┴────────────┐
                    │      SBC #1             │
                    │   (Controller/PLC)      │
                    │  - OpenPLC Runtime      │
                    │  - HMI (React)          │
                    │  - Modbus Gateway       │
                    │  - PROFINET Controller  │
                    └────────────┬────────────┘
                                 │ PROFINET
                    ┌────────────┴────────────┐
                    │      SBC #2             │
                    │   (This Codebase)       │
                    │  - PROFINET I/O Device  │
                    │  - Sensor Drivers       │
                    │  - Actuator Control     │
                    │  - Local Autonomy       │
                    └────────────┬────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
         ┌────┴────┐       ┌─────┴─────┐      ┌────┴────┐
         │ Sensors │       │  Pumps    │      │ Valves  │
         │ pH/TDS  │       │  Motors   │      │ Relays  │
         └─────────┘       └───────────┘      └─────────┘
```

## Features

### PROFINET Integration
- Full PROFINET I/O Device stack using [p-net](https://github.com/rtlabs-com/p-net)
- GSD/GSDML device description files for PLC configuration
- Cyclic I/O data exchange (sensor inputs, actuator outputs)
- Supports standard PROFINET diagnostic and alarm services
- **Clear-text protocol** for network traffic analysis (training/educational use)

### Sensor Abstraction Layer
Multi-protocol sensor support with unified data interface:

| Interface | Supported Devices |
|-----------|-------------------|
| I2C | ADS1115 (16-bit ADC), BME280 (Environmental), TCS34725 (Color) |
| SPI | MCP3008 (10-bit ADC) |
| 1-Wire | DS18B20 (Temperature) |
| GPIO | DHT22 (Temp/Humidity), JSN-SR04T (Ultrasonic), HX711 (Load Cell), Float Switch |
| Analog | pH, TDS, Turbidity (via ADC) |
| Network | HTTP/REST polling for external data sources |

### Actuator Control
- GPIO-based relay control for pumps and solenoid valves
- PWM output support for variable-speed control
- PROFINET output-to-actuator bridge with safety watchdog

### Offline Autonomy
- **Last-State-Saved**: Maintains actuator states during controller disconnect
- **Degraded Mode Detection**: Automatic alarm generation on communication loss
- **Store & Forward Logging**: Queues data locally when remote unavailable, flushes on reconnect

### Local Management
- ncurses-based TUI for configuration and diagnostics
- SQLite database for configuration persistence and local data logging
- Optional remote HTTP logging with queue management

## Hardware Requirements

### Supported Platforms
- Raspberry Pi 3B/3B+/4B/5
- BeagleBone Black
- Other Linux SBCs with GPIO support

### Recommended Configuration
- 1GB+ RAM
- 8GB+ SD card
- Ethernet connection (PROFINET requires wired network)
- GPIO headers for sensor/actuator connections

## Building

### Dependencies

```bash
# Required packages
sudo apt install -y \
    build-essential \
    cmake \
    libncurses5-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libcjson-dev \
    libgpiod-dev

# Optional: p-net library for PROFINET support
# See: https://github.com/rtlabs-com/p-net

# Optional: tinyexpr for calculated sensors
# See: https://github.com/codeplea/tinyexpr
```

### Compile

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Install

```bash
sudo make install
```

## Configuration

### PROFINET Device Setup

Import the GSD file into your PLC engineering tool:
```
gsd/GSDML-V2.4-WaterTreat-RTU-20241216.xml
```

Device parameters:
- **Vendor ID**: 0x0493
- **Device ID**: 0x0001
- **Device Name**: water-treat-rtu

### I/O Module Assignment

| Slot | Module Type | Data Size | Description |
|------|-------------|-----------|-------------|
| 1-8  | Input | 4 bytes (float) | Sensor readings |
| 9-15 | Output | 4 bytes | Actuator commands |

Output data format:
```
Byte 0: Command (0x00=Off, 0x01=On, 0x02=PWM)
Byte 1: PWM Duty Cycle (0-255)
Byte 2-3: Reserved
```

## TUI Navigation

| Key | Function |
|-----|----------|
| F1 | System Configuration |
| F2 | Sensor Management |
| F3 | Network Settings |
| F4 | PROFINET Status |
| F5 | Live Status |
| F6 | Alarm Configuration |
| F7 | Logging Settings |
| F10 | Exit |

## Authentication

### Default Credentials

| Username | Password |
|----------|----------|
| `admin` | `H2OhYeah!` |

> **Note:** The password is a water pun (H2O + "Oh Yeah!"). After 3 failed login attempts, the TUI displays a hint.

### Changing Default Credentials

Edit [`src/auth/auth.c`](src/auth/auth.c) lines 16-18:

```c
#define DEFAULT_USERNAME    "admin"
#define DEFAULT_PASSWORD    "H2OhYeah!"
#define DEFAULT_SALT        "NaCl4Life"
```

Then rebuild:
```bash
cd build && make
```

### Authentication Modes

The RTU supports two authentication sources:

1. **Local Users** (always available)
   - Default admin account
   - Works when controller is offline
   - Stored in local SQLite database

2. **Controller-Synced Users** (when connected)
   - User list synced from controller via PROFINET acyclic read
   - Controller is the authority for user management
   - RTU caches synced users for offline fallback

```
┌─────────────────────────┐         ┌─────────────────────────┐
│  Controller (SBC #1)    │         │     RTU (SBC #2)        │
│  - Master user list     │ ──────► │  - Synced user cache    │
│  - HMI / Web interface  │ PROFINET│  - Local fallback users │
│  - User management UI   │ acyclic │  - Default admin always │
└─────────────────────────┘         └─────────────────────────┘
```

## Runtime

```bash
# Run with root privileges (required for GPIO access)
sudo ./profinet-monitor

# Or install systemd service for production deployment
sudo cp systemd/profinet-monitor.service /etc/systemd/system/
sudo systemctl enable profinet-monitor
sudo systemctl start profinet-monitor
```

## Project Structure

```
Water-Treat/
├── src/
│   ├── main.c                    # Application entry point
│   ├── auth/                     # Authentication system
│   │   ├── auth.h                # Auth API and structures
│   │   └── auth.c                # Login, sessions, password hashing
│   ├── config/                   # Configuration management
│   ├── db/                       # SQLite database layer
│   ├── sensors/                  # Sensor abstraction and drivers
│   │   ├── drivers/              # Hardware-specific drivers
│   │   └── analog/               # Unified analog sensor interface
│   ├── actuators/                # Actuator management
│   ├── profinet/                 # PROFINET stack integration
│   ├── alarms/                   # Alarm processing
│   ├── logging/                  # Data logging subsystem
│   ├── platform/                 # Platform detection
│   └── tui/                      # Terminal user interface
│       ├── dialogs/              # CRUD dialogs (alarms, actuators)
│       └── pages/                # TUI pages including login
├── include/                      # Public headers
├── gsd/                          # PROFINET GSD files
└── systemd/                      # Service configuration
```

## Integration Notes

### Connecting to SBC #1 (Controller)
1. Import the GSD file into your PROFINET controller configuration
2. Assign a device name matching the RTU configuration
3. Configure I/O modules per sensor/actuator requirements
4. Establish PROFINET connection

### Offline Operation
When communication with the controller is lost:
1. RTU continues operating with last-received actuator commands
2. Degraded mode alarm is raised
3. Sensor data continues logging locally
4. On reconnection, queued logs are transmitted automatically

## License

GPL v3

## Contributing

This project is part of an industrial control systems training environment. Contributions should maintain focus on educational value and realistic SCADA/ICS implementations.
