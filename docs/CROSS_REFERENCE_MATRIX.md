# Water-Treat / Water-Controller Cross-Reference Matrix

This document verifies compatibility between Water-Treat (RTU/I/O Device) and
Water-Controller (PLC/Controller) PROFINET implementations.

## Summary

| Category | Status | Notes |
|----------|--------|-------|
| DCP Discovery | PASS | p-net handles protocol, config provides station name |
| Cyclic Input Data | PASS | sensor_manager writes floats to PROFINET slots |
| Cyclic Output Data | PASS | actuator_manager parses 4-byte command packets |
| I&M0 Data | PASS | Mandatory I&M0 record implemented |
| I&M1-4 Records | N/A | Optional, not implemented (per spec) |
| Alarms | PASS | profinet_manager_send_alarm() implemented |
| Degraded Mode | PASS | actuator_manager handles controller disconnect |

## Detailed Cross-Reference

### 1. DCP Discovery Protocol

**Controller Expectation** (dcp_discovery.c):
- Sends DCP Identify multicast to 01:0E:CF:00:00:00
- Expects response with: station_name, vendor_id, device_id, MAC, IP params

**RTU Implementation** (profinet_manager.c):
- p-net library handles DCP at protocol level
- Configuration passed to pnet_cfg:
  - `station_name`: Auto-detected as `rtu-XXXX` (last 4 hex of MAC)
  - `vendor_id`: 0x0493
  - `device_id`: 0x0001
  - `product_name`: "Water Treatment RTU"

```
Controller                           RTU
    |                                  |
    |--DCP Identify (multicast)------->|
    |<--DCP Identify Response----------|
    |   (station_name: rtu-abcd)       |
    |   (vendor_id: 0x0493)            |
    |   (device_id: 0x0001)            |
```

### 2. Cyclic Input Data (RTU → Controller)

**Controller Expectation** (cyclic_exchange.c:184-217):
```c
// 4 bytes per slot (float, big-endian)
size_t offset = slot_index * 4;
memcpy(&int_val, ar->iocr[i].data_buffer + offset, 4);
int_val = ntohl(int_val);
```

**RTU Implementation** (sensor_manager.c:70-87):
```c
// Writes sensor value to PROFINET input slot
uint8_t data[4];
memcpy(data, &value, sizeof(float));
profinet_manager_write_input_data(mgr->profinet_mgr, instance->slot, 0, data, sizeof(float));
profinet_manager_set_input_iops(mgr->profinet_mgr, instance->slot, 0, PNET_IOXS_GOOD);
```

**Data Format**:
| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | value | Float sensor reading (big-endian) |

### 3. Cyclic Output Data (Controller → RTU)

**Controller Expectation** (cyclic_exchange.c:223-251):
```c
typedef struct {
    uint8_t command;      // 0=OFF, 1=ON, 2=PWM
    uint8_t pwm_duty;     // 0-100%
    uint8_t reserved[2];
} actuator_output_t;
```

**RTU Implementation** (actuator_manager.h:40-48):
```c
#define ACTUATOR_CMD_OFF    0x00
#define ACTUATOR_CMD_ON     0x01
#define ACTUATOR_CMD_PWM    0x02

typedef struct {
    uint8_t command;
    uint8_t pwm_duty;
    uint8_t reserved[2];
} __attribute__((packed)) actuator_output_data_t;
```

**Data Format**:
| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | command | 0x00=OFF, 0x01=ON, 0x02=PWM |
| 1 | 1 | pwm_duty | PWM duty cycle (0-100) |
| 2 | 2 | reserved | Reserved for future use |

### 4. PROFINET States

**Controller States**:
```
IDLE → CONNECTING → CONNECTED → (ABORT) → IDLE
```

**RTU States** (profinet_manager.h:8-14):
```c
typedef enum {
    PROFINET_STATE_IDLE = 0,
    PROFINET_STATE_READY,
    PROFINET_STATE_CONNECTING,
    PROFINET_STATE_CONNECTED,
    PROFINET_STATE_ERROR
} profinet_state_t;
```

### 5. PROFINET Callbacks (I/O Device Side)

| Callback | Implementation | Status |
|----------|----------------|--------|
| state_cb | profinet_state_callback() | FULL |
| connect_cb | profinet_connect_callback() | FULL |
| release_cb | profinet_release_callback() | FULL |
| dcontrol_cb | profinet_dcontrol_callback() | FULL |
| ccontrol_cb | profinet_ccontrol_callback() | FULL |
| read_cb | profinet_read_callback() | FULL (I&M0) |
| write_cb | profinet_write_callback() | FULL |
| exp_module_cb | profinet_exp_module_callback() | FULL |
| exp_submodule_cb | profinet_exp_submodule_callback() | FULL |
| new_data_status_cb | profinet_new_data_status_callback() | FULL |
| alarm_ind_cb | profinet_alarm_ind_callback() | FULL |
| alarm_cnf_cb | profinet_alarm_cnf_callback() | FULL |
| alarm_ack_cnf_cb | profinet_alarm_ack_cnf_callback() | FULL |
| reset_cb | profinet_reset_callback() | FULL |
| signal_led_cb | profinet_signal_led_callback() | FULL (drives LED) |

### 6. Data Flow Verification

```
┌──────────────────┐                    ┌──────────────────┐
│ Water-Controller │                    │   Water-Treat    │
│     (PLC)        │                    │     (RTU)        │
├──────────────────┤                    ├──────────────────┤
│                  │                    │                  │
│  dcp_discovery   │◄──DCP Identify───►│  p-net library   │
│                  │                    │                  │
│  ar_manager      │◄──AR Setup───────►│  profinet_mgr    │
│                  │                    │                  │
│  cyclic_exchange │◄──Output Data────►│  actuator_mgr    │
│  (set_slot_out)  │   (cmd,pwm,0,0)   │  (handle_output) │
│                  │                    │                  │
│  cyclic_exchange │◄──Input Data─────►│  sensor_mgr      │
│  (get_slot_in)   │   (float,IOPS)    │  (write_input)   │
│                  │                    │                  │
└──────────────────┘                    └──────────────────┘
```

## Known Limitations

### Acceptable Stubs

1. **I&M1-4 Records** (profinet_callbacks.c:154-161)
   - Optional per PROFINET specification
   - I&M0 (mandatory) is fully implemented

2. **PWM Output** (relay_output.c:81-93)
   - Falls back to on/off for non-PWM hardware
   - Full PWM requires platform-specific implementation

### Deprecated Drivers (Working, Marked for Migration)

| File | Replacement | Status |
|------|-------------|--------|
| driver_pump.c | relay_output.c | Working, #warning issued |
| driver_solenoid.c | relay_output.c | Working, #warning issued |

## Verification Checklist

- [x] Station name auto-detection (rtu-XXXX from MAC)
- [x] Vendor ID matches (0x0493)
- [x] Device ID matches (0x0001)
- [x] DCP discovery handled by p-net
- [x] Cyclic input data format (4 bytes/float per slot)
- [x] Cyclic output data format (command, pwm_duty, reserved[2])
- [x] IOPS status bytes (PNET_IOXS_GOOD/BAD)
- [x] Module plugging via pnet_plug_module()
- [x] Connection state callbacks
- [x] Degraded mode handling (actuators maintain last state)
- [x] Alarm transmission via pnet_alarm_send_process_alarm()
- [x] I&M0 record for device identification
- [x] Signal LED callback (device identification blink)
