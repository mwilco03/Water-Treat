# PROFINET 

A comprehensive PROFINET I/O device for Raspberry Pi with sensor integration, data logging, and alarm management.

## Features

- **PROFINET I/O Device**: Full PROFINET stack using p-net
- **Multi-Sensor Support**: ADS1115, MCP3008, DS18B20, DHT22, BME280, TCS34725, JSN-SR04T, HX711, pH, TDS, Turbidity
- **Data Logging**: Local SQLite + remote HTTP logging
- **Alarm System**: Configurable thresholds with notifications  
- **TUI Interface**: ncurses-based configuration

## Quick Start

```bash
sudo apt install -y build-essential cmake libncurses5-dev libsqlite3-dev libcurl4-openssl-dev libcjson-dev
mkdir build && cd build && cmake .. && make -j$(nproc)
sudo ./profinet-monitor
```

## TUI Keys

F1:System F2:Sensors F3:Network F4:Modbus F5:Status F6:Alarms F7:Logging F10:Quit

## License

MIT License
