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
