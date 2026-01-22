#!/bin/bash
# =============================================================================
# fetch_shared_protocols.sh - Fetch shared protocol headers from Water-Controller
# =============================================================================
#
# What: Downloads canonical protocol definition headers from the Water-Controller
#       repository to ensure RTU and Controller use identical wire formats.
#
# Why: Protocol definitions (magic numbers, versions, record indices, struct layouts)
#      MUST match exactly between Controller and RTU. Single source of truth in
#      Water-Controller prevents drift.
#
# Usage:
#   ./scripts/fetch_shared_protocols.sh [destination_dir]
#
# Default destination: ./include/shared
#
# See: docs/RTU_SHARED_PROTOCOL_SYNC.md for full specification
# =============================================================================

set -euo pipefail

# =============================================================================
# Configuration
# =============================================================================

readonly CONTROLLER_REPO="mwilco03/Water-Controller"
readonly BRANCH="main"
readonly BASE_URL="https://raw.githubusercontent.com/${CONTROLLER_REPO}/${BRANCH}/shared/include"

# Files to fetch (must exist in Water-Controller/shared/include/)
readonly FILES=(
    "user_sync_protocol.h"
    "config_sync_protocol.h"
)

# Default destination relative to script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "${SCRIPT_DIR}")"
DEFAULT_DEST="${PROJECT_ROOT}/include/shared"

# =============================================================================
# Output Helpers
# =============================================================================

readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }

# =============================================================================
# Network Retry Logic
# =============================================================================

# Retry with exponential backoff (2s, 4s, 8s, 16s)
fetch_with_retry() {
    local url="$1"
    local output="$2"
    local max_retries=4
    local delay=2

    for ((attempt=1; attempt<=max_retries; attempt++)); do
        if curl -sSL --fail --connect-timeout 10 "${url}" -o "${output}" 2>/dev/null; then
            return 0
        fi

        if [[ $attempt -lt $max_retries ]]; then
            warn "Fetch attempt $attempt failed, retrying in ${delay}s..."
            sleep $delay
            delay=$((delay * 2))
        fi
    done

    return 1
}

# =============================================================================
# Main
# =============================================================================

main() {
    local dest_dir="${1:-${DEFAULT_DEST}}"

    echo "========================================"
    echo "  Fetch Shared Protocol Definitions"
    echo "========================================"
    echo ""
    info "Source: github.com/${CONTROLLER_REPO}/${BRANCH}/shared/include"
    info "Destination: ${dest_dir}"
    echo ""

    # Create destination directory
    if ! mkdir -p "${dest_dir}"; then
        error "Failed to create destination directory: ${dest_dir}"
        exit 1
    fi

    # Fetch each file
    local failed=0
    for file in "${FILES[@]}"; do
        local url="${BASE_URL}/${file}"
        local dest="${dest_dir}/${file}"

        info "Fetching ${file}..."

        if fetch_with_retry "${url}" "${dest}"; then
            # Verify file is not empty and looks like a C header
            if [[ ! -s "${dest}" ]]; then
                error "${file}: Downloaded file is empty"
                rm -f "${dest}"
                ((failed++))
                continue
            fi

            # Basic sanity check - should contain #ifndef guard
            if ! grep -q "#ifndef" "${dest}"; then
                error "${file}: Does not appear to be a valid C header"
                rm -f "${dest}"
                ((failed++))
                continue
            fi

            success "${file} -> ${dest}"
        else
            error "Failed to fetch ${file} from ${url}"
            ((failed++))
        fi
    done

    echo ""

    if [[ $failed -gt 0 ]]; then
        error "${failed} file(s) failed to download"
        echo ""
        echo "Troubleshooting:"
        echo "  1. Check network connectivity to GitHub"
        echo "  2. Verify files exist at: github.com/${CONTROLLER_REPO}/tree/${BRANCH}/shared/include"
        echo "  3. Check if GitHub is accessible (not blocked by firewall)"
        exit 1
    fi

    success "All protocol files downloaded successfully"
    echo ""
    echo "Files in ${dest_dir}:"
    ls -la "${dest_dir}"/*.h 2>/dev/null || echo "  (none)"

    return 0
}

main "$@"
