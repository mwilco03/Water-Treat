# Claude Code Project Instructions

## Critical Security Requirements

### Default Credentials Must ALWAYS Work

**CRITICAL**: The default credentials (`admin` / `H2OhYeah!`) must **ALWAYS** work for authentication on RTU devices. This is non-negotiable.

**Rationale**: Field service technicians need guaranteed access to RTU devices for:
- Initial commissioning before controller sync is established
- Emergency maintenance when controller is offline
- Network isolation scenarios
- Disaster recovery

**Implementation Rules**:
1. Local default admin account must always be created during `auth_init()`
2. Authentication priority: Synced users (from controller) > Local users (including default)
3. Never remove or disable the default admin creation code
4. Never make default credentials conditional on controller sync status

**Code Location**: `src/auth/auth.c` - `create_default_admin()` function

```c
#define DEFAULT_USERNAME    "admin"
#define DEFAULT_PASSWORD    "H2OhYeah!"  /* H2O + Oh Yeah! */
```

## Authentication Architecture

### User Sync from SCADA Controller
- Users synced via PROFINET acyclic record write at index `0xF840`
- DJB2 hash format: `"DJB2:%08X:%08X"` (salt_hash:password_hash)
- Salt: `"NaCl4Life"`
- Maximum 16 synced users (embedded constraint)
- Constant-time hash comparison to prevent timing attacks

### Authentication Priority
1. **Synced users** - Check `user_sync_authenticate()` first
2. **Local users** - Fall back to local SQLite database (includes default admin)

This ensures centrally-managed users take precedence while maintaining local fallback.

## PROFINET Station Name Requirements

**CRITICAL**: Station names must comply with IEC 61158-6 (PROFINET naming standard).

### Format: `rtu-XXXX`

Where `XXXX` is the **last 4 hex characters of the MAC address** (lowercase).

**Example**: MAC `aa:bb:cc:dd:ee:ff` → station name `rtu-eeff`

### Validation Rules (Regex: `^[a-z0-9][a-z0-9-]{0,62}$`)

| Rule | Requirement |
|------|-------------|
| First character | Lowercase letter (a-z) or digit (0-9) |
| Remaining characters | Lowercase letters, digits, or hyphens only |
| Maximum length | 63 characters |
| **FORBIDDEN** | Uppercase letters, underscores, dots, spaces |

### Code Locations

| File | Function | Purpose |
|------|----------|---------|
| `bootstrap.sh` | `detect_station_name()` | Auto-generates `rtu-XXXX` from MAC |
| `src/config/config_validate.c` | `validate_station_name()` | Validates format at runtime |

### Examples

```
VALID:
  rtu-eeff          (auto-generated from MAC)
  rtu-tank-1        (custom descriptive name)
  pump-station-01   (custom name)
  water-treat-rtu   (custom name)

INVALID:
  RTU-EEFF          (uppercase NOT allowed)
  rtu_tank_1        (underscores NOT allowed)
  water-rtu-XXXX    (uppercase NOT allowed)
  rtu.tank.1        (dots NOT allowed)
  -rtu-tank         (cannot start with hyphen)
```

### Why This Matters

The PROFINET controller uses the station name for **DCP (Discovery and Configuration Protocol)** to identify and connect to RTUs. An invalid station name will cause:
- DCP discovery failure
- Controller unable to establish AR (Application Relationship)
- Connection timeout

## Build Requirements

- C11 standard
- Compile with `-Wall -Wextra -Werror` (zero warnings)
- No dynamic allocation after init (static arrays only)
- Must support cross-compilation for embedded targets

## Board-Agnostic Development

**CRITICAL**: This codebase must run on ANY supported hardware platform. Never hardcode hardware-specific values.

### Network Interfaces

**NEVER hardcode interface names** like `eth0`, `eno1`, `enp0s3`, etc.

**Correct approach:**
1. Use configured value from config file (`[network] interface`)
2. If not configured, auto-detect using `detect_network_interface()`
3. Store in runtime variable (e.g., `g_netif_name`)
4. Reference that variable everywhere

```c
// WRONG - hardcoded
cfg.if_cfg.physical_ports[0].netif_name = "eth0";

// CORRECT - discovered/configured
cfg.if_cfg.physical_ports[0].netif_name = g_netif_name;
```

### Other Hardware Resources

- GPIO pins: Discover via `/sys/class/gpio` or device tree
- I2C buses: Detect available buses, don't assume `/dev/i2c-1`
- Serial ports: Use configured path or discover
- Storage paths: Use configured paths from config file

### API Documentation

When integrating external libraries:
1. **Always** read the actual API header on the target system
2. Check required vs optional fields in configuration structures
3. Don't guess based on generic documentation - verify against actual installed version
4. Test on actual target hardware, not just development environment

### Acceptable Hardcoded Values (Design Choices)

The following hardcoded values are **intentional design choices**, not violations of board-agnosticism:

| Location | Value | Rationale |
|----------|-------|-----------|
| `board_detect.c` | Per-board pin configs | **Source of truth** - defines GPIO/SPI/I2C mappings for each supported board |
| `dialog_actuator.c` | GPIO 17,27,22,23,24,25 | **Last-resort fallback** - only used if board_detect returns zero pins; user must manually select |
| `bootstrap.sh` | FHS paths (`/etc/`, `/var/lib/`) | **Linux standard** - Filesystem Hierarchy Standard paths, not hardware-specific |
| `scripts/*.sh` | CLI argument defaults | **User override** - scripts accept interface/path as CLI argument |
| Documentation | Example values | **Teaching material** - clearly labeled as examples, not runtime code |

### Values That Must Use Detection

| Category | Wrong | Correct |
|----------|-------|---------|
| Network interface | `"eth0"` | `detect_network_interface()` or config |
| SPI device | `"/dev/spidev0.0"` | `board_info.pins.spi_bus` → `/dev/spidev%d.0` |
| GPIO pins | `gpio_pin = 18` | `board_info.pins.pwm_channel_0` |
| I2C bus | `"/dev/i2c-1"` | `board_info.pins.i2c_bus_primary` |

### Pre-validation Pattern

When calling external libraries that return opaque errors (like p-net's `pnet_init()` returning NULL):

```c
// WRONG - guess at causes after failure
if (!pnet_init(&cfg)) {
    LOG_ERROR("Possible causes: 1. Interface missing, 2. Permissions, 3. ...");
}

// CORRECT - verify conditions BEFORE calling, get specific errors
if (access("/sys/class/net/eth0", F_OK) != 0) {
    LOG_ERROR("Interface '%s' not found", iface);
    return RESULT_ERROR;
}
if (socket(AF_PACKET, SOCK_RAW, ...) < 0) {
    LOG_ERROR("Cannot create raw socket: %s (need CAP_NET_RAW)", strerror(errno));
    return RESULT_ERROR;
}
// Now call - if it fails, we know it's an internal library issue
pnet_init(&cfg);
```

