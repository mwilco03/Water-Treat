# Source Code Reference

This repository contains the complete structure and core files for PROFINET Monitor.

## Complete Implementation

The full implementation of all source files was developed in a Claude.ai conversation.
The stub files in this repository provide the structure, and the complete implementations
include:

### Hardware Drivers (src/sensors/drivers/)
- **driver_ads1115.c** - ADS1115 16-bit ADC (I2C)
- **driver_mcp3008.c** - MCP3008 10-bit ADC (SPI)
- **driver_ds18b20.c** - DS18B20 Temperature (1-Wire)
- **driver_dht22.c** - DHT22 Temp/Humidity (GPIO)
- **driver_bme280.c** - BME280/BMP280 Environmental (I2C)
- **driver_tcs34725.c** - TCS34725 RGB Color (I2C)
- **driver_jsn_sr04t.c** - JSN-SR04T Ultrasonic Distance (GPIO)
- **driver_hx711.c** - HX711 Load Cell Amplifier (GPIO)
- **driver_ph.c** - pH Sensor (ADC)
- **driver_tds.c** - TDS Sensor (ADC)
- **driver_turbidity.c** - Turbidity Sensor (ADC)
- **driver_float_switch.c** - Float Switch (GPIO)
- **driver_solenoid.c** - Solenoid Valve (GPIO)
- **driver_pump.c** - Water Pump (GPIO/PWM)
- **driver_web_poll.c** - HTTP/REST API Polling

### Core Systems
- **sensor_manager.c** - Multi-threaded sensor polling
- **sensor_instance.c** - Sensor instance management with calibration
- **formula_evaluator.c** - TinyExpr-based calculated sensors
- **data_logger.c** - Local SQLite + remote HTTP logging
- **alarm_manager.c** - Threshold monitoring with notifications

### TUI Pages
- **page_system.c** - System configuration
- **page_sensors.c** - Sensor management with CRUD dialogs
- **page_network.c** - Network settings
- **page_modbus.c** - Modbus gateway configuration
- **page_status.c** - Live sensor status display
- **page_alarms.c** - Alarm rules and active alarms
- **page_logging.c** - Data logging configuration

### PROFINET Integration
- **profinet_manager.c** - p-net stack management
- **profinet_callbacks.c** - PROFINET event handlers

## Building

The core files (main.c, config, database, logger) are complete and functional.
For the full implementation of sensor drivers and TUI, refer to the conversation
transcript or implement based on the headers and documentation.

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Dependencies

```bash
sudo apt install -y build-essential cmake libncurses5-dev libsqlite3-dev \
    libcurl4-openssl-dev libcjson-dev
```

Optional:
- p-net library for PROFINET support
- tinyexpr for calculated sensors
