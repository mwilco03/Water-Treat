# RTU Team: Shared Protocol Files Installation Guide

**Document Version**: 1.0
**Date**: 2026-01-22
**Author**: Water-Controller Team

## Overview

The Water-Controller and Water-Treat (RTU) systems share critical protocol definitions that MUST remain synchronized. This document describes how the RTU installation script should fetch these files and validate they came from the correct source.

## Static Coupling Declaration

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         STATIC COUPLING NOTICE                              │
│                                                                             │
│  Repository: mwilco03/Water-Controller (main branch)                        │
│  ────────────────────────────────────────────────────────────────────────   │
│  Source of Truth:                                                           │
│    /shared/include/user_sync_protocol.h                                     │
│    /shared/include/config_sync_protocol.h                                   │
│                                                                             │
│  Consumer: mwilco03/Water-Treat (RTU firmware)                              │
│  ────────────────────────────────────────────────────────────────────────   │
│  Install Location:                                                          │
│    /path/to/rtu/include/shared/user_sync_protocol.h                         │
│    /path/to/rtu/include/shared/config_sync_protocol.h                       │
│                                                                             │
│  CRITICAL: Do NOT modify these files locally in Water-Treat.                │
│            All changes MUST go through Water-Controller repo.               │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Files to Fetch

| File | GitHub Raw URL | Description |
|------|----------------|-------------|
| `user_sync_protocol.h` | `https://raw.githubusercontent.com/mwilco03/Water-Controller/main/shared/include/user_sync_protocol.h` | User credential sync via PROFINET 0xF840 |
| `config_sync_protocol.h` | `https://raw.githubusercontent.com/mwilco03/Water-Controller/main/shared/include/config_sync_protocol.h` | Device/sensor/actuator config sync 0xF841-0xF845 |

## Installation Script Requirements

### 1. Fetch Files (Do NOT Clone Entire Repo)

Use `curl` or `wget` to fetch individual files:

```bash
#!/bin/bash
# fetch_shared_protocols.sh

CONTROLLER_REPO="mwilco03/Water-Controller"
BRANCH="main"
BASE_URL="https://raw.githubusercontent.com/${CONTROLLER_REPO}/${BRANCH}/shared/include"

DEST_DIR="${1:-./include/shared}"
mkdir -p "$DEST_DIR"

FILES=(
    "user_sync_protocol.h"
    "config_sync_protocol.h"
)

for file in "${FILES[@]}"; do
    echo "Fetching $file..."
    curl -sSL "${BASE_URL}/${file}" -o "${DEST_DIR}/${file}"
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to fetch $file" >&2
        exit 1
    fi
done

echo "Shared protocol files downloaded to $DEST_DIR"
```

### 2. Validation (REQUIRED)

After fetching, validate that files contain expected markers to confirm they came from the correct source and are the expected version.

```bash
#!/bin/bash
# validate_protocols.sh

DEST_DIR="${1:-./include/shared}"

# Expected validation markers
USER_SYNC_MAGIC="0x55534552"
USER_SYNC_VERSION="2"
USER_SYNC_INDEX="0xF840"
CONFIG_SYNC_VERSION="1"
CONFIG_SYNC_DEVICE_INDEX="0xF841"

validate_file() {
    local file="$1"
    local marker="$2"
    local value="$3"

    if ! grep -q "#define ${marker}.*${value}" "$file"; then
        echo "ERROR: $file missing expected ${marker} = ${value}" >&2
        return 1
    fi
    return 0
}

echo "Validating protocol files..."

# Validate user_sync_protocol.h
FILE="${DEST_DIR}/user_sync_protocol.h"
if [ ! -f "$FILE" ]; then
    echo "ERROR: $FILE not found" >&2
    exit 1
fi

validate_file "$FILE" "USER_SYNC_MAGIC" "$USER_SYNC_MAGIC" || exit 1
validate_file "$FILE" "USER_SYNC_PROTOCOL_VERSION" "$USER_SYNC_VERSION" || exit 1
validate_file "$FILE" "USER_SYNC_RECORD_INDEX" "$USER_SYNC_INDEX" || exit 1

# Validate config_sync_protocol.h
FILE="${DEST_DIR}/config_sync_protocol.h"
if [ ! -f "$FILE" ]; then
    echo "ERROR: $FILE not found" >&2
    exit 1
fi

validate_file "$FILE" "CONFIG_SYNC_PROTOCOL_VERSION" "$CONFIG_SYNC_VERSION" || exit 1
validate_file "$FILE" "CONFIG_SYNC_DEVICE_INDEX" "$CONFIG_SYNC_DEVICE_INDEX" || exit 1

echo "All protocol files validated successfully."
echo ""
echo "Protocol Versions:"
grep "USER_SYNC_PROTOCOL_VERSION" "${DEST_DIR}/user_sync_protocol.h"
grep "CONFIG_SYNC_PROTOCOL_VERSION" "${DEST_DIR}/config_sync_protocol.h"
```

### 3. Integration into RTU Install Script

Add to your existing RTU installation (e.g., `bootstrap.sh` or `install.sh`):

```bash
# In RTU installation script

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHARED_INCLUDE="${SCRIPT_DIR}/include/shared"

echo "=== Fetching Shared Protocol Definitions ==="
echo "Source: github.com/mwilco03/Water-Controller/shared/include"

# Fetch
"${SCRIPT_DIR}/scripts/fetch_shared_protocols.sh" "$SHARED_INCLUDE"

# Validate (MUST pass or installation fails)
"${SCRIPT_DIR}/scripts/validate_protocols.sh" "$SHARED_INCLUDE"
if [ $? -ne 0 ]; then
    echo "FATAL: Protocol validation failed. Cannot continue." >&2
    echo "Check that Water-Controller main branch is accessible." >&2
    exit 1
fi

echo "=== Shared Protocols Ready ==="
```

## Protocol Version Matrix

| Protocol | Version | Record Index | Purpose |
|----------|---------|--------------|---------|
| User Sync | 2 | 0xF840 | User credentials (username, password hash, role) |
| Device Config | 1 | 0xF841 | RTU device settings (watchdog, authority mode) |
| Sensor Config | 1 | 0xF842 | Sensor slot configuration |
| Actuator Config | 1 | 0xF843 | Actuator slot configuration |
| RTU Status | 1 | 0xF844 | RTU → Controller health status |
| Enrollment | 1 | 0xF845 | Device binding/enrollment |

## Key Constants (for Validation)

From `user_sync_protocol.h`:
```c
#define USER_SYNC_PROTOCOL_VERSION  2
#define USER_SYNC_MAGIC             0x55534552  // "USER"
#define USER_SYNC_RECORD_INDEX      0xF840
#define USER_SYNC_MAX_USERS         16
#define USER_SYNC_USERNAME_LEN      32
#define USER_SYNC_HASH_LEN          24
#define USER_SYNC_SALT              "NaCl4Life"
```

From `config_sync_protocol.h`:
```c
#define CONFIG_SYNC_PROTOCOL_VERSION    1
#define CONFIG_SYNC_DEVICE_INDEX        0xF841
#define CONFIG_SYNC_SENSOR_INDEX        0xF842
#define CONFIG_SYNC_ACTUATOR_INDEX      0xF843
#define CONFIG_SYNC_STATUS_INDEX        0xF844
#define CONFIG_SYNC_ENROLLMENT_INDEX    0xF845
#define ENROLLMENT_MAGIC                0x454E524C  // "ENRL"
```

## Wire Format Summary

### User Sync Payload (0xF840)

```
Header (20 bytes):
  magic:u32         = 0x55534552 ("USER")
  version:u8        = 2
  operation:u8      = 0x00 (full), 0x01 (add/update), 0x02 (delete)
  user_count:u8     = 0-16
  reserved:u8       = 0
  timestamp:u32     = Unix timestamp
  nonce:u32         = Random (replay detection)
  checksum:u16      = CRC16-CCITT of user records
  reserved2:u16     = 0

User Record (64 bytes each, max 16):
  user_id:u32       = Unique ID from controller DB
  username:char[32] = Null-terminated username
  password_hash:char[24] = "DJB2:%08X:%08X" format
  role:u8           = 0=Viewer, 1=Operator, 2=Engineer, 3=Admin
  flags:u8          = 0x01=Active, 0x02=SyncToRTUs
  reserved:u8[2]    = Padding
```

### Hash Algorithm

Both sides MUST use identical hashing:
```c
// DJB2 with salt "NaCl4Life"
uint32_t hash = 5381;
for each char c in (salt + password):
    hash = ((hash << 5) + hash) + c;

// Wire format: "DJB2:<salt_hash>:<combined_hash>"
```

## Troubleshooting

### File Not Found (404)
- Check that main branch exists and files are in `/shared/include/`
- Check network connectivity to GitHub

### Validation Failed
- Protocol version may have been bumped - check Water-Controller releases
- File may have been corrupted during download - re-fetch

### Version Mismatch at Runtime
- RTU and Controller must run same protocol version
- Update RTU firmware after Controller protocol changes

## Change Management

1. **Controller makes protocol change** → Updates `shared/include/*.h`
2. **Controller bumps version** → e.g., `USER_SYNC_PROTOCOL_VERSION 2 → 3`
3. **Controller commits/pushes to main**
4. **RTU runs install/update** → Fetches new headers
5. **RTU validates** → Confirms new version markers
6. **RTU rebuilds** → Compiles with new protocol definitions

## Contact

For protocol questions or coordination:
- Water-Controller repo: `github.com/mwilco03/Water-Controller`
- Issues: `github.com/mwilco03/Water-Controller/issues`
