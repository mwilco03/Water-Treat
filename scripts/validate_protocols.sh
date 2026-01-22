#!/bin/bash
# =============================================================================
# validate_protocols.sh - Validate fetched shared protocol headers
# =============================================================================
#
# What: Validates that downloaded protocol headers contain expected markers
#       to confirm they came from the correct source and expected version.
#
# Why: Prevents build with corrupted/wrong files. Catches version mismatches
#      early (before runtime failures).
#
# Usage:
#   ./scripts/validate_protocols.sh [source_dir]
#
# Default source: ./include/shared
#
# Exit codes:
#   0 - All validations passed
#   1 - Validation failed (missing file, wrong version, etc.)
#
# See: docs/RTU_SHARED_PROTOCOL_SYNC.md for expected values
# =============================================================================

set -euo pipefail

# =============================================================================
# Expected Validation Markers (from RTU_SHARED_PROTOCOL_SYNC.md)
# =============================================================================

# user_sync_protocol.h
readonly USER_SYNC_MAGIC="0x55534552"
readonly USER_SYNC_VERSION="2"
readonly USER_SYNC_INDEX="0xF840"
readonly USER_SYNC_MAX_USERS="16"
readonly USER_SYNC_SALT="NaCl4Life"

# config_sync_protocol.h
readonly CONFIG_SYNC_VERSION="1"
readonly CONFIG_SYNC_DEVICE_INDEX="0xF841"
readonly CONFIG_SYNC_SENSOR_INDEX="0xF842"
readonly CONFIG_SYNC_ACTUATOR_INDEX="0xF843"
readonly CONFIG_SYNC_STATUS_INDEX="0xF844"
readonly CONFIG_SYNC_ENROLLMENT_INDEX="0xF845"
readonly ENROLLMENT_MAGIC="0x454E524C"

# =============================================================================
# Configuration
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "${SCRIPT_DIR}")"
DEFAULT_DIR="${PROJECT_ROOT}/include/shared"

# =============================================================================
# Output Helpers
# =============================================================================

readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[PASS]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
error()   { echo -e "${RED}[FAIL]${NC} $1" >&2; }

# =============================================================================
# Validation Functions
# =============================================================================

# Check that a #define exists with expected value
# Usage: validate_define FILE DEFINE_NAME EXPECTED_VALUE
validate_define() {
    local file="$1"
    local marker="$2"
    local expected="$3"

    # Handle string values (with quotes) vs numeric values
    # Pattern: #define MARKER value  or  #define MARKER "value"
    if grep -qE "^[[:space:]]*#define[[:space:]]+${marker}[[:space:]]+(${expected}|\"${expected}\")" "$file"; then
        success "${marker} = ${expected}"
        return 0
    else
        # Show what we found (if anything)
        local found
        found=$(grep -E "^[[:space:]]*#define[[:space:]]+${marker}[[:space:]]+" "$file" 2>/dev/null | head -1 || echo "(not found)")
        error "${marker}: expected '${expected}', found: ${found}"
        return 1
    fi
}

# Validate user_sync_protocol.h
validate_user_sync() {
    local file="$1"

    echo ""
    info "Validating: $(basename "$file")"
    echo "----------------------------------------"

    if [[ ! -f "$file" ]]; then
        error "File not found: $file"
        return 1
    fi

    local errors=0

    validate_define "$file" "USER_SYNC_MAGIC" "${USER_SYNC_MAGIC}" || ((errors++))
    validate_define "$file" "USER_SYNC_PROTOCOL_VERSION" "${USER_SYNC_VERSION}" || ((errors++))
    validate_define "$file" "USER_SYNC_RECORD_INDEX" "${USER_SYNC_INDEX}" || ((errors++))
    validate_define "$file" "USER_SYNC_MAX_USERS" "${USER_SYNC_MAX_USERS}" || ((errors++))
    validate_define "$file" "USER_SYNC_SALT" "${USER_SYNC_SALT}" || ((errors++))

    return $errors
}

# Validate config_sync_protocol.h
validate_config_sync() {
    local file="$1"

    echo ""
    info "Validating: $(basename "$file")"
    echo "----------------------------------------"

    if [[ ! -f "$file" ]]; then
        error "File not found: $file"
        return 1
    fi

    local errors=0

    validate_define "$file" "CONFIG_SYNC_PROTOCOL_VERSION" "${CONFIG_SYNC_VERSION}" || ((errors++))
    validate_define "$file" "CONFIG_SYNC_DEVICE_INDEX" "${CONFIG_SYNC_DEVICE_INDEX}" || ((errors++))
    validate_define "$file" "CONFIG_SYNC_SENSOR_INDEX" "${CONFIG_SYNC_SENSOR_INDEX}" || ((errors++))
    validate_define "$file" "CONFIG_SYNC_ACTUATOR_INDEX" "${CONFIG_SYNC_ACTUATOR_INDEX}" || ((errors++))
    validate_define "$file" "CONFIG_SYNC_STATUS_INDEX" "${CONFIG_SYNC_STATUS_INDEX}" || ((errors++))
    validate_define "$file" "CONFIG_SYNC_ENROLLMENT_INDEX" "${CONFIG_SYNC_ENROLLMENT_INDEX}" || ((errors++))
    validate_define "$file" "ENROLLMENT_MAGIC" "${ENROLLMENT_MAGIC}" || ((errors++))

    return $errors
}

# =============================================================================
# Main
# =============================================================================

main() {
    local source_dir="${1:-${DEFAULT_DIR}}"

    echo "========================================"
    echo "  Validate Shared Protocol Headers"
    echo "========================================"
    echo ""
    info "Source directory: ${source_dir}"

    if [[ ! -d "$source_dir" ]]; then
        error "Directory not found: $source_dir"
        echo ""
        echo "Run fetch_shared_protocols.sh first to download the headers."
        exit 1
    fi

    local total_errors=0

    # Validate user_sync_protocol.h
    if ! validate_user_sync "${source_dir}/user_sync_protocol.h"; then
        ((total_errors++))
    fi

    # Validate config_sync_protocol.h
    if ! validate_config_sync "${source_dir}/config_sync_protocol.h"; then
        ((total_errors++))
    fi

    echo ""
    echo "========================================"

    if [[ $total_errors -gt 0 ]]; then
        error "Validation FAILED: ${total_errors} file(s) with errors"
        echo ""
        echo "Possible causes:"
        echo "  1. Protocol version was bumped in Water-Controller"
        echo "  2. Files were corrupted during download"
        echo "  3. Wrong branch/repo was fetched"
        echo ""
        echo "Actions:"
        echo "  1. Check Water-Controller releases for protocol changes"
        echo "  2. Re-run fetch_shared_protocols.sh"
        echo "  3. Update this RTU codebase if protocol version changed"
        exit 1
    fi

    success "All protocol files validated successfully"
    echo ""

    # Print version summary
    echo "Protocol Versions:"
    grep -h "PROTOCOL_VERSION" "${source_dir}"/*.h 2>/dev/null | \
        sed 's/^[[:space:]]*/  /' || echo "  (none found)"

    return 0
}

main "$@"
