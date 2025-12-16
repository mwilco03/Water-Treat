# Source Architecture Reference

Technical documentation for the Water Treatment RTU firmware implementation.

## Overview

This document describes the software architecture of the Water Treatment RTU, a PROFINET I/O Device designed for industrial water quality monitoring and process control.

## Architectural Principles

### Design Goals
1. **Industrial Protocol Compliance**: Full PROFINET I/O Device implementation
2. **Sensor Abstraction**: Hardware-agnostic interface for diverse sensor types
3. **Local Intelligence**: Autonomous operation capability during network outages
4. **Operational Visibility**: Real-time diagnostics via TUI and logging

### Layer Structure

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│              (main.c, TUI, Configuration)               │
├─────────────────────────────────────────────────────────┤
│                    Service Layer                         │
│    (Sensor Manager, Actuator Manager, Alarm Manager)    │
├─────────────────────────────────────────────────────────┤
│                   Protocol Layer                         │
│           (PROFINET Manager, Data Logger)               │
├─────────────────────────────────────────────────────────┤
│                   Abstraction Layer                      │
│        (Sensor Instance, GPIO HAL, Relay Output)        │
├─────────────────────────────────────────────────────────┤
│                    Driver Layer                          │
│   (ADS1115, MCP3008, DS18B20, DHT22, BME280, etc.)     │
├─────────────────────────────────────────────────────────┤
│                   Platform Layer                         │
│          (Board Detection, libgpiod, sysfs)             │
└─────────────────────────────────────────────────────────┘
```

## Source Organization

### Core Systems (`src/`)

| File | Description |
|------|-------------|
| `main.c` | Application entry, subsystem orchestration, signal handling |
| `config/config.c` | JSON configuration parsing and persistence |
| `db/database.c` | SQLite connection pool and schema management |
| `db/db_modules.c` | Module configuration CRUD operations |
| `db/db_events.c` | Event log storage and retrieval |
| `db/db_alarms.c` | Alarm state persistence |
| `utils/logger.c` | Syslog-compatible logging with level control |
| `platform/board_detect.c` | Runtime SBC platform identification |

### Sensor Subsystem (`src/sensors/`)

#### Unified Sensor Architecture (New)

| File | Description |
|------|-------------|
| `analog/analog_sensor.c` | Generic analog sensor driver (pH, TDS, turbidity) |
| `sensor_instance.c` | Sensor lifecycle management, calibration storage |
| `sensor_manager.c` | Multi-threaded polling, PROFINET data exchange |
| `formula_evaluator.c` | Calculated sensors via TinyExpr expressions |
| `hardware/hw_interface.c` | Hardware abstraction interface |

#### Hardware Drivers (Legacy)

| File | Interface | Description |
|------|-----------|-------------|
| `drivers/driver_ads1115.c` | I2C | 16-bit ADC, 4-channel differential |
| `drivers/driver_mcp3008.c` | SPI | 10-bit ADC, 8-channel single-ended |
| `drivers/driver_ds18b20.c` | 1-Wire | Digital temperature sensor |
| `drivers/driver_dht22.c` | GPIO | Temperature and humidity sensor |
| `drivers/driver_bme280.c` | I2C | Environmental sensor (T/H/P) |
| `drivers/driver_tcs34725.c` | I2C | RGB color sensor |
| `drivers/driver_jsn_sr04t.c` | GPIO | Waterproof ultrasonic distance |
| `drivers/driver_hx711.c` | GPIO | 24-bit load cell amplifier |
| `drivers/driver_float_switch.c` | GPIO | Digital level switch |
| `drivers/driver_web_poll.c` | HTTP | External REST API data source |

#### Deprecated Drivers
These are scheduled for migration to the unified architecture:
- `driver_ph.c` → Use `analog_sensor.c` with `SENSOR_TYPE_PH`
- `driver_tds.c` → Use `analog_sensor.c` with `SENSOR_TYPE_TDS`
- `driver_turbidity.c` → Use `analog_sensor.c` with `SENSOR_TYPE_TURBIDITY`
- `driver_pump.c` → Use `relay_output.c`
- `driver_solenoid.c` → Use `relay_output.c`

### Actuator Subsystem (`src/actuators/`)

| File | Description |
|------|-------------|
| `actuator_manager.c` | PROFINET output-to-GPIO bridge, degraded mode handling |

The actuator manager receives 4-byte output data from the PROFINET controller:
```c
typedef struct {
    uint8_t command;      // 0x00=Off, 0x01=On, 0x02=PWM
    uint8_t pwm_duty;     // 0-255 duty cycle
    uint8_t reserved[2];
} actuator_output_data_t;
```

### Driver Abstractions (`src/drivers/`)

| File | Description |
|------|-------------|
| `digital/relay_output.c` | Unified relay/GPIO output control |
| `bus/gpio_hal.c` | GPIO hardware abstraction (libgpiod or sysfs fallback) |

### PROFINET Integration (`src/profinet/`)

| File | Description |
|------|-------------|
| `profinet_manager.c` | p-net stack initialization, cyclic data handling |
| `profinet_callbacks.c` | Connection, alarm, and I/O callbacks |

#### PROFINET I/O Mapping

**Inputs (Sensor → PLC)**
- Each sensor provides a 4-byte IEEE 754 float value
- Up to 8 input modules supported (slots 1-8)
- Data updated at configured poll interval

**Outputs (PLC → Actuator)**
- Each actuator receives a 4-byte control structure
- Up to 7 output modules supported (slots 9-15)
- Commands processed immediately upon receipt

### Alarm System (`src/alarms/`)

| File | Description |
|------|-------------|
| `alarm_manager.c` | Threshold monitoring, alarm state machine |

Alarm priorities:
- `ALARM_PRIORITY_LOW` (1): Informational, logged only
- `ALARM_PRIORITY_MEDIUM` (2): Warning, TUI notification
- `ALARM_PRIORITY_HIGH` (3): Critical, PROFINET diagnostic alarm
- `ALARM_PRIORITY_CRITICAL` (4): Safety-critical, triggers safe state

### Data Logging (`src/logging/`)

| File | Description |
|------|-------------|
| `data_logger.c` | Local SQLite + remote HTTP with store & forward |

Store & forward behavior:
1. Log entries written to local SQLite immediately
2. Background thread attempts remote HTTP POST
3. On network failure, entries queued locally
4. On reconnection, queue flushed automatically
5. Entries older than `max_queue_age_seconds` are dropped

### Terminal User Interface (`src/tui/`)

| Directory | Contents |
|-----------|----------|
| `tui_common.c` | ncurses initialization, color schemes |
| `tui_main.c` | Main loop, page switching, input handling |
| `widgets/` | Reusable UI components (dialogs, lists) |
| `dialogs/` | Modal dialogs for configuration |
| `pages/` | Full-screen pages (F1-F7) |

#### TUI Pages

| Page | File | Function |
|------|------|----------|
| F1 | `page_system.c` | System configuration, device info |
| F2 | `page_sensors.c` | Sensor CRUD, calibration |
| F3 | `page_network.c` | PROFINET network settings |
| F4 | `page_modbus.c` | Protocol status display |
| F5 | `page_status.c` | Live sensor readings |
| F6 | `page_alarms.c` | Alarm configuration, active alarms |
| F7 | `page_logging.c` | Data logging settings |

## Build Configuration

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `HAVE_PNET` | Auto | Enable PROFINET support |
| `HAVE_TINYEXPR` | Auto | Enable calculated sensors |
| `HAVE_CJSON` | Auto | Enable JSON configuration |
| `HAVE_GPIOD` | Auto | Use libgpiod (vs sysfs fallback) |
| `HAVE_SYSTEMD` | Auto | Enable sd_notify integration |
| `HAVE_CURL` | Auto | Enable remote HTTP logging |

### Compiler Flags

Debug build:
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

Release build:
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
```

## Threading Model

| Thread | Responsibility |
|--------|---------------|
| Main | TUI event loop, signal handling |
| Sensor Poller | Periodic sensor sampling |
| PROFINET Stack | Cyclic I/O, alarms, diagnostics |
| Data Logger | Queue processing, HTTP transmission |
| Actuator Watchdog | Safety timeout monitoring |

## Safety Considerations

### Degraded Mode Operation
When PROFINET connection is lost:
1. `actuator_manager` receives disconnect callback
2. Current actuator states are preserved (last-state-saved)
3. Degraded mode alarm raised via `alarm_manager`
4. Local logging continues via store & forward
5. On reconnection, alarm cleared and normal operation resumes

### Watchdog Behavior
- Software watchdog monitors PROFINET cyclic data
- Timeout triggers safe state for actuators
- Configurable per-actuator safe state (ON/OFF)

## Dependencies

### Required
- `ncurses` - Terminal UI rendering
- `sqlite3` - Local database storage
- `pthread` - POSIX threading

### Optional
- `p-net` - PROFINET I/O Device stack
- `libcurl` - Remote HTTP logging
- `libcjson` - JSON parsing
- `libgpiod` - Modern GPIO interface
- `libsystemd` - Service integration
- `tinyexpr` - Expression evaluation

## File Naming Conventions

| Pattern | Purpose |
|---------|---------|
| `*_manager.c` | Subsystem coordinators |
| `*_instance.c` | Object lifecycle management |
| `driver_*.c` | Hardware-specific implementations |
| `page_*.c` | TUI full-screen pages |
| `dialog_*.c` | TUI modal dialogs |
| `db_*.c` | Database access layer |
