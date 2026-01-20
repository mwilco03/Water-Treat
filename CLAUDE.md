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

