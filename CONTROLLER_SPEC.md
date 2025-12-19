# PROFINET Controller for Water Treatment RTU Network

## Project Overview

Create a PROFINET IO Controller (PLC/SCADA) that manages multiple Water Treatment RTU devices. Each RTU is a PROFINET IO Device exposing sensor inputs and actuator outputs. The controller reads sensor data, executes control logic, and writes actuator commands.

## Target RTU Specification

The controller must integrate with RTUs running the `Water-Treat` firmware with the following characteristics:

### PROFINET Device Identity
```
Vendor ID: Configurable (default 0x0001)
Device ID: Configurable (default 0x0001)
Station Name: Unique per RTU (e.g., "rtu-tank-1", "rtu-pump-station")
Product Name: "Water Treatment RTU"
Min Device Interval: 32 (1ms cycle time supported)
```

### Slot/Subslot Architecture

| Slot | Function | Direction | Data Size | Format |
|------|----------|-----------|-----------|--------|
| 0 | DAP (Device Access Point) | - | - | Standard PROFINET |
| 1-8 | Sensor Inputs | Input | 4 bytes | IEEE 754 float |
| 9-16 | Actuator Outputs | Output | 4 bytes | See below |

### Sensor Input Data Format (Slots 1-8)
```c
// Each sensor slot provides 4 bytes
typedef struct {
    float value;  // IEEE 754 single precision
} sensor_input_t;

// IOPS (Input/Output Provider Status)
// 0x80 = GOOD (valid data)
// 0x00 = BAD (sensor fault/disconnected)
```

### Actuator Output Data Format (Slots 9-16)
```c
typedef struct {
    uint8_t command;     // 0x00=OFF, 0x01=ON, 0x02=PWM
    uint8_t pwm_duty;    // 0-100 (percentage) when command=0x02
    uint8_t reserved[2];
} actuator_output_t;
```

### Supported Sensor Types
The controller should understand these measurement types for proper scaling, alarming, and display:

| Type | Unit | Typical Range | Alarm Thresholds |
|------|------|---------------|------------------|
| pH | pH | 0.0 - 14.0 | Low: 6.5, High: 8.5 |
| Temperature | °C | -20.0 - 100.0 | Process dependent |
| Turbidity | NTU | 0.0 - 1000.0 | High: 4.0 (drinking water) |
| TDS | ppm | 0.0 - 5000.0 | High: 500 (drinking water) |
| Dissolved Oxygen | mg/L | 0.0 - 20.0 | Low: 2.0 |
| Flow Rate | L/min | 0.0 - 10000.0 | Process dependent |
| Level | % | 0.0 - 100.0 | Low: 10%, High: 90% |
| Pressure | bar | 0.0 - 100.0 | Process dependent |
| Conductivity | µS/cm | 0.0 - 100000.0 | Process dependent |
| ORP | mV | -2000 - 2000 | Process dependent |
| Chlorine | ppm | 0.0 - 10.0 | Low: 0.2, High: 4.0 |

### Supported Actuator Types

| Type | Control Mode | Safety Behavior |
|------|--------------|-----------------|
| RELAY | ON/OFF | Configurable safe state |
| PWM | 0-100% duty | Configurable safe state |
| PUMP | ON/OFF or PWM | Max runtime limit, anti-short-cycle |
| VALVE | ON/OFF | Fail-open or fail-closed |
| LATCHING | Pulse to toggle | Maintains state |
| MOMENTARY | Pulse duration | Returns to off |

---

## Controller Architecture Requirements

### 1. PROFINET IO Controller Stack

Implement or integrate a PROFINET IO Controller using:
- **Recommended**: Siemens PROFINET stack, or open-source alternative
- Must support:
  - Device discovery (DCP - Discovery and Configuration Protocol)
  - AR (Application Relationship) establishment
  - Cyclic data exchange (RT Class 1, minimum 1ms)
  - Acyclic read/write (Record Data)
  - Alarm handling (diagnosis alarms from RTUs)
  - Device replacement without engineering (topology discovery)

### 2. RTU Registry & Discovery

```
Module: rtu_registry

Functions:
- rtu_registry_init(database)
- rtu_registry_discover_devices(interface, timeout_ms)
- rtu_registry_add_device(station_name, ip_address, config)
- rtu_registry_remove_device(station_name)
- rtu_registry_get_device(station_name) -> rtu_device_t*
- rtu_registry_list_devices() -> rtu_device_t[]
- rtu_registry_get_device_count() -> int
- rtu_registry_set_device_config(station_name, slot_config[])
- rtu_registry_save_topology()
- rtu_registry_load_topology()

Data Structures:
typedef struct {
    char station_name[64];
    char ip_address[16];
    uint16_t vendor_id;
    uint16_t device_id;
    profinet_state_t connection_state;
    uint64_t last_seen_ms;

    // Slot configuration
    slot_config_t slots[16];
    int slot_count;

    // Runtime data
    sensor_data_t sensors[8];
    actuator_state_t actuators[8];

    // Health
    int failed_cycles;
    float packet_loss_percent;
} rtu_device_t;

typedef struct {
    int slot;
    int subslot;
    slot_type_t type;  // SENSOR or ACTUATOR
    char name[64];
    char unit[16];
    measurement_type_t measurement_type;
    float scale_min;
    float scale_max;
    float alarm_low;
    float alarm_high;
    float warning_low;
    float warning_high;
} slot_config_t;
```

### 3. Cyclic Data Exchange Engine

```
Module: cyclic_engine

Functions:
- cyclic_engine_init(cycle_time_ms)
- cyclic_engine_start()
- cyclic_engine_stop()
- cyclic_engine_register_rtu(rtu_device_t*)
- cyclic_engine_unregister_rtu(station_name)
- cyclic_engine_read_input(station_name, slot) -> float
- cyclic_engine_read_input_status(station_name, slot) -> iops_t
- cyclic_engine_write_output(station_name, slot, command, pwm_duty)
- cyclic_engine_get_cycle_stats() -> cycle_stats_t

Requirements:
- Deterministic cycle execution (jitter < 10% of cycle time)
- Handle RTU disconnect gracefully (mark data stale)
- Buffer last-known-good values for transient disconnects
- Support cycle times: 1ms, 2ms, 4ms, 8ms, 16ms, 32ms
- Priority queue for time-critical actuator commands
```

### 4. Control Logic Engine

```
Module: control_engine

Functions:
- control_engine_init()
- control_engine_load_program(program_file)
- control_engine_start()
- control_engine_stop()
- control_engine_add_pid_loop(config) -> loop_id
- control_engine_add_sequence(config) -> sequence_id
- control_engine_add_interlock(config) -> interlock_id
- control_engine_set_setpoint(loop_id, value)
- control_engine_get_output(loop_id) -> float
- control_engine_force_output(station_name, slot, value)
- control_engine_release_output(station_name, slot)

Control Types:
1. PID Loops
   - pH control (acid/base dosing)
   - Level control (pump speed)
   - Pressure control (valve position)
   - Dissolved oxygen control (aerator speed)

2. Sequences
   - Backwash cycles
   - CIP (Clean-in-Place)
   - Filter rinse
   - Tank fill/drain

3. Interlocks
   - Low level pump protect
   - High pressure relief
   - Over-temperature shutdown
   - Cross-contamination prevention

Data Structures:
typedef struct {
    int loop_id;
    char name[64];

    // Input
    char input_rtu[64];
    int input_slot;

    // Output
    char output_rtu[64];
    int output_slot;

    // PID parameters
    float kp, ki, kd;
    float setpoint;
    float output_min, output_max;
    float deadband;

    // Runtime
    float pv;           // Process variable
    float cv;           // Control variable (output)
    float error;
    float integral;
    pid_mode_t mode;    // AUTO, MANUAL, CASCADE
} pid_loop_t;

typedef struct {
    int interlock_id;
    char name[64];

    // Condition
    char condition_rtu[64];
    int condition_slot;
    interlock_condition_t condition;  // ABOVE, BELOW, EQUAL
    float threshold;

    // Action
    char action_rtu[64];
    int action_slot;
    interlock_action_t action;  // FORCE_OFF, FORCE_ON, ALARM_ONLY

    bool tripped;
    uint64_t trip_time_ms;
} interlock_t;
```

### 5. Alarm Management

```
Module: alarm_manager

Functions:
- alarm_manager_init(database)
- alarm_manager_start()
- alarm_manager_stop()
- alarm_manager_create_rule(rtu, slot, condition, threshold, severity, delay_ms)
- alarm_manager_delete_rule(rule_id)
- alarm_manager_acknowledge(alarm_id, user)
- alarm_manager_acknowledge_all(user)
- alarm_manager_get_active() -> alarm_t[]
- alarm_manager_get_history(start_time, end_time) -> alarm_t[]
- alarm_manager_set_callback(on_alarm, on_clear, ctx)
- alarm_manager_suppress(rtu, slot, duration_ms)
- alarm_manager_get_statistics() -> alarm_stats_t

Alarm Priorities (ISA-18.2):
1. EMERGENCY - Immediate danger, automatic action required
2. HIGH - Abnormal, operator action required within minutes
3. MEDIUM - Abnormal, operator action required within shift
4. LOW - Status change, informational

Alarm States:
- ACTIVE_UNACK - Active, not acknowledged
- ACTIVE_ACK - Active, acknowledged
- CLEARED_UNACK - Returned to normal, not acknowledged
- CLEARED - Returned to normal, acknowledged

Data Structures:
typedef struct {
    int alarm_id;
    char rtu_station[64];
    int slot;
    alarm_severity_t severity;
    alarm_state_t state;

    char message[256];
    float value;
    float threshold;

    uint64_t raise_time_ms;
    uint64_t ack_time_ms;
    uint64_t clear_time_ms;
    char ack_user[64];
} alarm_t;
```

### 6. Data Historian

```
Module: historian

Functions:
- historian_init(database, config)
- historian_start()
- historian_stop()
- historian_add_tag(rtu, slot, sample_rate_ms, compression)
- historian_remove_tag(tag_id)
- historian_query(tag_id, start_time, end_time, interval) -> sample_t[]
- historian_query_multi(tag_ids[], start_time, end_time) -> dataset_t
- historian_get_statistics(tag_id) -> tag_stats_t
- historian_export_csv(tag_ids[], start_time, end_time, filename)
- historian_purge_old_data(retention_days)

Storage Requirements:
- Minimum 1 year retention at 1-second resolution
- Compression: Swinging door algorithm (configurable deadband)
- Database: TimescaleDB, InfluxDB, or SQLite with time-series extension
- Support for raw and aggregated data (min, max, avg per interval)

Data Structures:
typedef struct {
    int tag_id;
    char rtu_station[64];
    int slot;
    char tag_name[128];
    char unit[16];

    int sample_rate_ms;
    float deadband;
    compression_t compression;

    // Statistics
    uint64_t total_samples;
    uint64_t compressed_samples;
    float compression_ratio;
} historian_tag_t;
```

### 7. Web HMI / SCADA Interface

```
Module: web_hmi

Endpoints:
GET  /api/v1/rtus                    - List all RTUs
GET  /api/v1/rtus/{station}          - Get RTU details
GET  /api/v1/rtus/{station}/sensors  - Get sensor values
GET  /api/v1/rtus/{station}/actuators - Get actuator states
POST /api/v1/rtus/{station}/actuators/{slot} - Command actuator

GET  /api/v1/alarms                  - Get active alarms
GET  /api/v1/alarms/history          - Get alarm history
POST /api/v1/alarms/{id}/acknowledge - Acknowledge alarm

GET  /api/v1/trends/{tag_id}         - Get trend data
GET  /api/v1/trends/realtime         - WebSocket for live data

GET  /api/v1/control/pid             - List PID loops
GET  /api/v1/control/pid/{id}        - Get PID loop details
PUT  /api/v1/control/pid/{id}        - Update setpoint/mode

GET  /api/v1/system/health           - Controller health
GET  /api/v1/system/config           - Export configuration
POST /api/v1/system/config           - Import configuration

WebSocket Endpoints:
/ws/realtime    - Live sensor/actuator data stream
/ws/alarms      - Live alarm notifications
/ws/events      - System events

UI Pages:
1. Dashboard - Overview of all RTUs, active alarms, system health
2. Process View - P&ID style diagram with live values
3. RTU Detail - Single RTU sensors/actuators with manual control
4. Trends - Historical data visualization (multiple tags)
5. Alarms - Active alarm list, acknowledgment, history
6. Control - PID loops, sequences, interlocks
7. Configuration - RTU setup, alarm thresholds, user management
8. Reports - Daily/weekly summaries, compliance reports
```

### 8. Configuration Management

```
Module: config_manager

Functions:
- config_manager_init(config_path)
- config_manager_load()
- config_manager_save()
- config_manager_export_json() -> string
- config_manager_import_json(json_string)
- config_manager_get_rtu_config(station_name) -> rtu_config_t
- config_manager_set_rtu_config(station_name, config)
- config_manager_validate() -> validation_result_t
- config_manager_backup(backup_path)
- config_manager_restore(backup_path)
- config_manager_push_to_rtu(station_name)  // Push config to RTU
- config_manager_pull_from_rtu(station_name) // Pull config from RTU

RTU Config Push/Pull:
- RTU exposes /config endpoint for export
- Controller can provision RTU configuration
- Supports bulk configuration of multiple RTUs
```

### 9. User Authentication & Authorization

```
Module: auth_manager

Functions:
- auth_manager_init(database)
- auth_manager_authenticate(username, password) -> session_token
- auth_manager_validate_token(token) -> user_t*
- auth_manager_check_permission(user, resource, action) -> bool
- auth_manager_create_user(username, password, role)
- auth_manager_delete_user(username)
- auth_manager_update_password(username, new_password)
- auth_manager_list_users() -> user_t[]
- auth_manager_audit_log(user, action, resource, details)

Roles:
- VIEWER - Read-only access to all data
- OPERATOR - Acknowledge alarms, manual control
- ENGINEER - Modify setpoints, alarm thresholds
- ADMIN - Full system configuration, user management

Audit Requirements:
- Log all control actions with user, timestamp, old/new values
- Log all configuration changes
- Log all login attempts (success/failure)
- Retain audit log minimum 1 year
```

### 10. Inter-RTU Coordination Logic

```
Module: coordination

Functions:
- coordination_init()
- coordination_add_cascade(upstream_rtu, downstream_rtu, config)
- coordination_add_load_balance(rtus[], config)
- coordination_add_failover(primary_rtu, backup_rtu, config)
- coordination_evaluate()

Use Cases:

1. Cascade Control
   - Tank A level controls Pump B speed
   - RTU#1 reads level, Controller calculates, RTU#2 runs pump

2. Load Balancing
   - Distribute flow across multiple parallel filters
   - Balance runtime hours across redundant pumps

3. Failover
   - If primary pump fails, start backup pump
   - If sensor fails, use alternate measurement

4. Cross-Plant Coordination
   - Upstream plant signals downstream plant
   - Prevent overflow by coordinating levels

Data Flow:
RTU#1 Sensor → Controller Logic → RTU#2 Actuator
     ↑                                    ↓
   Input                              Output
   (read)                            (write)
```

### 11. Redundancy & High Availability

```
Requirements:
- Hot standby controller support
- Automatic failover < 100ms
- State synchronization between primary/backup
- Bumpless transfer (outputs don't glitch)
- Split-brain prevention

Implementation:
- Heartbeat between controllers (10ms interval)
- Shared state database (PostgreSQL with synchronous replication)
- Virtual IP for HMI access
- PROFINET controller redundancy (S2 system redundancy)
```

---

## Database Schema

```sql
-- RTU Registry
CREATE TABLE rtus (
    id SERIAL PRIMARY KEY,
    station_name VARCHAR(64) UNIQUE NOT NULL,
    ip_address VARCHAR(15),
    vendor_id INTEGER,
    device_id INTEGER,
    connection_state VARCHAR(20),
    last_seen TIMESTAMP,
    config JSONB,
    created_at TIMESTAMP DEFAULT NOW(),
    updated_at TIMESTAMP DEFAULT NOW()
);

-- Slot Configuration
CREATE TABLE slots (
    id SERIAL PRIMARY KEY,
    rtu_id INTEGER REFERENCES rtus(id),
    slot INTEGER NOT NULL,
    subslot INTEGER DEFAULT 0,
    slot_type VARCHAR(10), -- 'SENSOR' or 'ACTUATOR'
    name VARCHAR(64),
    unit VARCHAR(16),
    measurement_type VARCHAR(32),
    scale_min REAL,
    scale_max REAL,
    alarm_low REAL,
    alarm_high REAL,
    warning_low REAL,
    warning_high REAL,
    UNIQUE(rtu_id, slot, subslot)
);

-- Alarm Rules
CREATE TABLE alarm_rules (
    id SERIAL PRIMARY KEY,
    rtu_id INTEGER REFERENCES rtus(id),
    slot INTEGER,
    name VARCHAR(128),
    condition VARCHAR(20),
    threshold REAL,
    severity VARCHAR(20),
    delay_ms INTEGER DEFAULT 0,
    enabled BOOLEAN DEFAULT TRUE
);

-- Alarm History
CREATE TABLE alarm_history (
    id SERIAL PRIMARY KEY,
    rule_id INTEGER REFERENCES alarm_rules(id),
    state VARCHAR(20),
    value REAL,
    raise_time TIMESTAMP,
    ack_time TIMESTAMP,
    clear_time TIMESTAMP,
    ack_user VARCHAR(64),
    message TEXT
);

-- Historian Tags
CREATE TABLE historian_tags (
    id SERIAL PRIMARY KEY,
    rtu_id INTEGER REFERENCES rtus(id),
    slot INTEGER,
    tag_name VARCHAR(128),
    sample_rate_ms INTEGER,
    deadband REAL,
    compression VARCHAR(20)
);

-- Historian Data (TimescaleDB hypertable)
CREATE TABLE historian_data (
    time TIMESTAMPTZ NOT NULL,
    tag_id INTEGER REFERENCES historian_tags(id),
    value REAL,
    quality INTEGER
);
SELECT create_hypertable('historian_data', 'time');

-- PID Loops
CREATE TABLE pid_loops (
    id SERIAL PRIMARY KEY,
    name VARCHAR(64),
    input_rtu_id INTEGER REFERENCES rtus(id),
    input_slot INTEGER,
    output_rtu_id INTEGER REFERENCES rtus(id),
    output_slot INTEGER,
    kp REAL, ki REAL, kd REAL,
    setpoint REAL,
    output_min REAL, output_max REAL,
    deadband REAL,
    mode VARCHAR(10) DEFAULT 'AUTO',
    enabled BOOLEAN DEFAULT TRUE
);

-- Interlocks
CREATE TABLE interlocks (
    id SERIAL PRIMARY KEY,
    name VARCHAR(64),
    condition_rtu_id INTEGER REFERENCES rtus(id),
    condition_slot INTEGER,
    condition_type VARCHAR(10),
    threshold REAL,
    action_rtu_id INTEGER REFERENCES rtus(id),
    action_slot INTEGER,
    action_type VARCHAR(20),
    enabled BOOLEAN DEFAULT TRUE,
    tripped BOOLEAN DEFAULT FALSE,
    trip_time TIMESTAMP
);

-- Users
CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    username VARCHAR(64) UNIQUE NOT NULL,
    password_hash VARCHAR(256) NOT NULL,
    role VARCHAR(20) NOT NULL,
    created_at TIMESTAMP DEFAULT NOW(),
    last_login TIMESTAMP
);

-- Audit Log
CREATE TABLE audit_log (
    id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT NOW(),
    user_id INTEGER REFERENCES users(id),
    action VARCHAR(64),
    resource VARCHAR(128),
    old_value TEXT,
    new_value TEXT,
    ip_address VARCHAR(45)
);
```

---

## Technology Stack Recommendations

| Component | Recommended | Alternatives |
|-----------|-------------|--------------|
| Language | C/C++ (real-time), Python (HMI) | Rust, Go |
| PROFINET Stack | Siemens, codesys | Open-source p-net |
| Database | PostgreSQL + TimescaleDB | InfluxDB, SQLite |
| Web Framework | FastAPI (Python) | Node.js, Go Fiber |
| Web UI | React + Material-UI | Vue.js, Angular |
| Real-time Charts | Apache ECharts | Plotly, Grafana |
| Message Queue | Redis Pub/Sub | MQTT, ZeroMQ |
| Containerization | Docker + Docker Compose | Podman |

---

## File Structure

```
water-treat-controller/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   ├── profinet-integration.md
│   ├── api-reference.md
│   └── user-manual.md
├── src/
│   ├── main.c
│   ├── profinet/
│   │   ├── profinet_controller.c
│   │   ├── profinet_controller.h
│   │   ├── dcp_discovery.c
│   │   └── cyclic_exchange.c
│   ├── registry/
│   │   ├── rtu_registry.c
│   │   └── rtu_registry.h
│   ├── control/
│   │   ├── control_engine.c
│   │   ├── pid_loop.c
│   │   ├── sequence_engine.c
│   │   └── interlock_manager.c
│   ├── alarms/
│   │   ├── alarm_manager.c
│   │   └── alarm_manager.h
│   ├── historian/
│   │   ├── historian.c
│   │   └── compression.c
│   ├── coordination/
│   │   ├── cascade_control.c
│   │   ├── load_balance.c
│   │   └── failover.c
│   ├── db/
│   │   ├── database.c
│   │   └── migrations/
│   ├── config/
│   │   └── config_manager.c
│   └── utils/
│       ├── logger.c
│       └── time_utils.c
├── web/
│   ├── api/
│   │   ├── main.py
│   │   ├── routes/
│   │   │   ├── rtus.py
│   │   │   ├── alarms.py
│   │   │   ├── trends.py
│   │   │   └── control.py
│   │   └── websocket/
│   │       └── realtime.py
│   └── ui/
│       ├── package.json
│       ├── src/
│       │   ├── App.tsx
│       │   ├── pages/
│       │   │   ├── Dashboard.tsx
│       │   │   ├── ProcessView.tsx
│       │   │   ├── RTUDetail.tsx
│       │   │   ├── Trends.tsx
│       │   │   ├── Alarms.tsx
│       │   │   └── Configuration.tsx
│       │   └── components/
│       │       ├── RTUCard.tsx
│       │       ├── SensorGauge.tsx
│       │       ├── ActuatorControl.tsx
│       │       ├── AlarmBanner.tsx
│       │       └── TrendChart.tsx
│       └── public/
├── tests/
│   ├── test_profinet.c
│   ├── test_control.c
│   └── test_alarms.c
├── docker/
│   ├── Dockerfile.controller
│   ├── Dockerfile.web
│   └── docker-compose.yml
└── scripts/
    ├── setup.sh
    ├── deploy.sh
    └── backup.sh
```

---

## Integration Test Checklist

```
[ ] RTU Discovery
    [ ] Discover RTU via DCP
    [ ] Retrieve station name, IP, device ID
    [ ] Auto-configure slot mapping

[ ] Connection
    [ ] Establish PROFINET AR
    [ ] Handle RTU reboot gracefully
    [ ] Reconnect after network disruption

[ ] Cyclic Data Exchange
    [ ] Read all sensor slots at cycle rate
    [ ] Write actuator commands with < 10ms latency
    [ ] Detect stale data (IOPS BAD)

[ ] Control Logic
    [ ] PID loop tracks setpoint
    [ ] Interlock trips and recovers correctly
    [ ] Sequence executes all steps

[ ] Alarms
    [ ] Alarm raised when threshold exceeded
    [ ] Alarm cleared when value returns to normal
    [ ] Acknowledge persists across restart

[ ] Historian
    [ ] Data logged at configured rate
    [ ] Query returns correct time range
    [ ] Compression reduces storage

[ ] Multi-RTU
    [ ] Coordinate control across 2+ RTUs
    [ ] Handle mixed RTU connect/disconnect
    [ ] Load balance across redundant units

[ ] HMI
    [ ] Real-time values update in browser
    [ ] Manual control changes actuator state
    [ ] Alarm notification appears immediately

[ ] Failover
    [ ] Backup controller takes over < 100ms
    [ ] No output glitch during failover
    [ ] State synchronized after recovery
```

---

## Compliance & Standards

- **IEC 61158** - PROFINET protocol
- **IEC 61131-3** - PLC programming languages (Structured Text for control logic)
- **ISA-18.2** - Alarm management
- **ISA-101** - HMI design (color standards already in RTU LEDs)
- **IEC 62443** - Industrial cybersecurity
- **21 CFR Part 11** - Electronic records (if pharmaceutical use)

---

This specification provides complete integration requirements for a PROFINET controller to manage the Water Treatment RTU network. The RTU firmware exposes a standard PROFINET I/O device interface, and this controller implements the coordination, control logic, alarming, and HMI layer.
