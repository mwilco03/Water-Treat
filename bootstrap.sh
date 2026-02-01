#!/bin/bash
# =============================================================================
# Water-Treat RTU Bootstrap Script
# =============================================================================
# One-liner entry point for installation, upgrade, and removal.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/mwilco03/Water-Treat/main/bootstrap.sh | sudo bash
#   curl -fsSL .../bootstrap.sh | sudo bash -s -- install   # First-time setup
#   curl -fsSL .../bootstrap.sh | sudo bash -s -- upgrade   # Update/fix existing (preserves config)
#   curl -fsSL .../bootstrap.sh | sudo bash -s -- wipe      # Complete removal
#   curl -fsSL .../bootstrap.sh | sudo bash -s -- fresh     # Wipe + install from scratch
#
# Copyright (C) 2024-2025
# SPDX-License-Identifier: GPL-3.0-or-later
# =============================================================================

set -euo pipefail

# =============================================================================
# Constants
# =============================================================================

readonly BOOTSTRAP_VERSION="1.0.0"
readonly REPO_URL="https://github.com/mwilco03/Water-Treat.git"
readonly REPO_RAW_URL="https://raw.githubusercontent.com/mwilco03/Water-Treat"
readonly INSTALL_DIR="/opt/water-treat"
readonly VERSION_FILE="$INSTALL_DIR/.version"
readonly CONFIG_DIR="/etc/water-treat"
readonly DATA_DIR="/var/lib/water-treat"
readonly PNET_DATA_DIR="/var/lib/water-treat/pnet"
readonly LOG_DIR="/var/log/water-treat"
readonly BACKUP_DIR="/var/backups/water-treat"
readonly BOOTSTRAP_LOG="/var/log/water-treat-bootstrap.log"
readonly MIN_DISK_SPACE_MB=512
readonly REQUIRED_TOOLS=("git" "curl" "cmake" "make" "gcc")
readonly BUILD_DEPS=("build-essential" "cmake" "libncurses5-dev" "libsqlite3-dev" "libcurl4-openssl-dev" "libcjson-dev" "libgpiod-dev" "libsystemd-dev" "ca-certificates" "ntpsec-ntpdate")

# Shared protocol headers from Water-Controller
readonly CONTROLLER_REPO="mwilco03/Water-Controller"
readonly CONTROLLER_BRANCH="main"
readonly CONTROLLER_RAW_URL="https://raw.githubusercontent.com/${CONTROLLER_REPO}/${CONTROLLER_BRANCH}/shared/include"
readonly SHARED_PROTOCOL_FILES=("user_sync_protocol.h" "config_sync_protocol.h")

# Service name
readonly SERVICE_NAME="water-treat"

# Global state
QUIET_MODE="false"
VERBOSE_MODE="false"
CLEANUP_DIRS=()

# Colors for output
readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly NC='\033[0m'

# =============================================================================
# Logging Functions
# =============================================================================

init_logging() {
    local log_dir
    log_dir=$(dirname "$BOOTSTRAP_LOG")

    if [[ -w "$log_dir" ]] || [[ $EUID -eq 0 ]]; then
        if [[ $EUID -ne 0 ]]; then
            sudo mkdir -p "$log_dir" 2>/dev/null || true
            sudo touch "$BOOTSTRAP_LOG" 2>/dev/null || true
            sudo chmod 644 "$BOOTSTRAP_LOG" 2>/dev/null || true
        else
            mkdir -p "$log_dir" 2>/dev/null || true
            touch "$BOOTSTRAP_LOG" 2>/dev/null || true
        fi
    fi
}

write_log() {
    local level="$1"
    local message="$2"
    local timestamp
    timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    if [[ -w "$BOOTSTRAP_LOG" ]] || [[ $EUID -eq 0 ]]; then
        if [[ $EUID -ne 0 ]]; then
            echo "[$timestamp] [$level] $message" | sudo tee -a "$BOOTSTRAP_LOG" >/dev/null 2>&1 || true
        else
            echo "[$timestamp] [$level] $message" >> "$BOOTSTRAP_LOG" 2>/dev/null || true
        fi
    fi
}

log_info() {
    write_log "INFO" "$1"
    if [[ "$QUIET_MODE" != "true" ]]; then
        echo -e "${GREEN}[INFO]${NC} $1" >&2
    fi
}

log_warn() {
    write_log "WARN" "$1"
    if [[ "$QUIET_MODE" != "true" ]]; then
        echo -e "${YELLOW}[WARN]${NC} $1" >&2
    fi
}

log_error() {
    write_log "ERROR" "$1"
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

log_step() {
    write_log "STEP" "$1"
    if [[ "$QUIET_MODE" != "true" ]]; then
        echo -e "${BLUE}[STEP]${NC} $1" >&2
    fi
}

log_debug() {
    write_log "DEBUG" "$1"
}

log_verbose() {
    write_log "VERBOSE" "$1"
    if [[ "$VERBOSE_MODE" == "true" ]] && [[ "$QUIET_MODE" != "true" ]]; then
        echo -e "  $1" >&2
    fi
}

# =============================================================================
# Helper Functions
# =============================================================================

run_privileged() {
    if [[ $EUID -ne 0 ]]; then
        sudo "$@"
    else
        "$@"
    fi
}

cleanup_all() {
    local dir
    for dir in "${CLEANUP_DIRS[@]}"; do
        if [[ -n "$dir" ]] && [[ -d "$dir" ]]; then
            log_debug "Cleaning up: $dir"
            rm -rf "$dir" 2>/dev/null || true
        fi
    done
    CLEANUP_DIRS=()
}

register_cleanup() {
    local dir="$1"
    CLEANUP_DIRS+=("$dir")
    trap cleanup_all EXIT
}

prompt_user() {
    local prompt="$1"
    local response=""

    if [[ -t 0 ]]; then
        read -r -p "$prompt" response
    elif [[ -e /dev/tty ]]; then
        read -r -p "$prompt" response < /dev/tty
    else
        log_warn "No interactive terminal available, assuming 'no'"
        response="n"
    fi

    echo "$response"
}

# =============================================================================
# Discovery Functions
# =============================================================================

_LAST_DISCOVERY_ERROR=""
_LAST_DISCOVERY_METHOD=""

discover_network() {
    local target="$1"
    local timeout="${2:-10}"
    _LAST_DISCOVERY_ERROR=""
    _LAST_DISCOVERY_METHOD=""

    if [[ -z "$target" ]]; then
        _LAST_DISCOVERY_ERROR="No target specified"
        return 1
    fi

    # If piped from github, network already proven
    if [[ ! -t 0 ]] && [[ "$target" == *"github.com"* ]]; then
        _LAST_DISCOVERY_METHOD="script was piped from remote source"
        return 0
    fi

    if command -v curl &>/dev/null; then
        local curl_error
        if curl_error=$(curl -fsSL --connect-timeout "$timeout" --max-time "$timeout" "$target" -o /dev/null 2>&1); then
            _LAST_DISCOVERY_METHOD="curl to $target"
            return 0
        else
            if [[ "$curl_error" == *"Could not resolve"* ]]; then
                _LAST_DISCOVERY_ERROR="DNS resolution failed for $target"
            elif [[ "$curl_error" == *"Connection refused"* ]]; then
                _LAST_DISCOVERY_ERROR="Connection refused by $target"
            elif [[ "$curl_error" == *"timed out"* ]]; then
                _LAST_DISCOVERY_ERROR="Connection to $target timed out"
            else
                _LAST_DISCOVERY_ERROR="Failed to reach $target: $curl_error"
            fi
            _LAST_DISCOVERY_METHOD="curl to $target"
            return 1
        fi
    fi

    if command -v wget &>/dev/null; then
        if wget -q --timeout="$timeout" --spider "$target" 2>/dev/null; then
            _LAST_DISCOVERY_METHOD="wget to $target"
            return 0
        fi
        _LAST_DISCOVERY_ERROR="wget failed to reach $target"
        _LAST_DISCOVERY_METHOD="wget to $target"
        return 1
    fi

    _LAST_DISCOVERY_ERROR="No curl or wget available"
    return 1
}

# =============================================================================
# Network/Station Detection
# =============================================================================

detect_network_interface() {
    # Priority order: eth* > enp* > ens* > wlan*
    local iface=""
    for pattern in "eth*" "enp*" "ens*" "wlan*"; do
        for candidate in /sys/class/net/$pattern; do
            [[ -e "$candidate" ]] || continue
            local name
            name=$(basename "$candidate")
            [[ "$name" == "lo" ]] && continue
            # Check if it has a valid MAC (not all zeros)
            local mac
            mac=$(cat "$candidate/address" 2>/dev/null || echo "")
            if [[ -n "$mac" && "$mac" != "00:00:00:00:00:00" ]]; then
                iface="$name"
                break 2
            fi
        done
    done
    echo "$iface"
}

# =============================================================================
# PROFINET Station Name Generation
# =============================================================================
# Format: rtu-XXXX where XXXX = last 4 hex chars of MAC address (lowercase)
#
# PROFINET IEC 61158-6 Requirements:
#   - Regex: ^[a-z0-9][a-z0-9-]{0,62}$
#   - First char: lowercase letter or digit
#   - Remaining: lowercase letters, digits, hyphens ONLY
#   - Max 63 characters
#   - NO uppercase, underscores, dots, or spaces
#
# Example: MAC aa:bb:cc:dd:ee:ff -> station name "rtu-eeff"
#
# This name is used by the PROFINET controller for DCP discovery.
# Invalid names will cause connection failures.
# =============================================================================
detect_station_name() {
    local iface="$1"
    if [[ -z "$iface" ]]; then
        echo "rtu-0000"
        return
    fi
    local mac_file="/sys/class/net/${iface}/address"
    if [[ ! -f "$mac_file" ]]; then
        echo "rtu-0000"
        return
    fi
    # Extract last 4 hex chars of MAC (bytes 5 and 6), convert to lowercase
    # Example: aa:bb:cc:dd:ee:ff -> eeff
    local mac suffix
    mac=$(cat "$mac_file")
    suffix=$(echo "$mac" | awk -F: '{print tolower($5 $6)}')
    echo "rtu-${suffix}"
}

# =============================================================================
# p-net NV Storage Purge (Clear ALL p-net state)
# =============================================================================
# CRITICAL: This function clears ALL p-net NV (non-volatile) storage files
# to ensure a clean PROFINET state on install/upgrade.
#
# Why this matters:
# 1. Station name contamination: The p-net library has "rt-labs-dev" as its
#    compiled-in default. If NV files exist with this value, p-net IGNORES
#    our configured station name.
#
# 2. Stale AR (Application Relationship) state: When the RTU crashes or is
#    improperly shutdown, p-net may persist AR state. On restart, the controller
#    tries to connect but the RTU rejects with PNIO error codes:
#      - status1=0x00000001 (AR block error)
#      - status2=0x00000003 (AR already exists / session mismatch)
#
#    Clearing ALL pf_* files forces the RTU to start fresh without stale AR.
#
# 3. DCP Set-Name contamination: The controller can re-contaminate via DCP
#    Set-Name AFTER purge runs, so we clear unconditionally every time.
#
# p-net NV files use "pf_" prefix (pf_ip, pf_im, pf_pdport, pf_ar, etc.)
# =============================================================================
purge_pnet_nv_storage() {
    local pnet_dir="$1"

    if [[ -z "$pnet_dir" || ! -d "$pnet_dir" ]]; then
        return 0
    fi

    local purged=0
    local file

    for file in "$pnet_dir"/*; do
        [[ -f "$file" ]] || continue

        local basename
        basename=$(basename "$file")

        # Only delete p-net NV files (pf_* prefix)
        # This preserves any application data files we might store there
        if [[ "$basename" == pf_* ]]; then
            log_verbose "Removing p-net NV file: $file"
            run_privileged rm -f "$file" && ((purged++))
        fi
    done

    if [[ $purged -gt 0 ]]; then
        log_info "Cleared $purged p-net NV file(s) - ensures clean PROFINET state"
        log_info "This clears stale AR state and station name cache"
    else
        log_debug "No p-net NV files found in $pnet_dir"
    fi

    return 0
}

# Legacy alias for backwards compatibility
purge_pnet_contamination() {
    purge_pnet_nv_storage "$@"
}

# =============================================================================
# System Detection
# =============================================================================

detect_system_state() {
    if [[ ! -d "$INSTALL_DIR" ]]; then
        echo "fresh"
        return 0
    fi

    if [[ -f "$VERSION_FILE" ]]; then
        if grep -q '"commit_sha"' "$VERSION_FILE" 2>/dev/null; then
            echo "installed"
            return 0
        fi
    fi

    # Check for partial installation
    if [[ -f "$INSTALL_DIR/build/water-treat" ]] || \
       [[ -f "/etc/systemd/system/${SERVICE_NAME}.service" ]]; then
        echo "corrupted"
        return 0
    fi

    local file_count
    file_count=$(find "$INSTALL_DIR" -maxdepth 1 -mindepth 1 2>/dev/null | wc -l)
    if [[ "$file_count" -eq 0 ]]; then
        echo "fresh"
    else
        echo "corrupted"
    fi
}

get_installed_version() {
    if [[ ! -f "$VERSION_FILE" ]]; then
        echo ""
        return 1
    fi
    grep -oP '"version"\s*:\s*"\K[^"]+' "$VERSION_FILE" 2>/dev/null || echo "unknown"
}

get_installed_sha() {
    if [[ ! -f "$VERSION_FILE" ]]; then
        echo ""
        return 1
    fi
    grep -oP '"commit_sha"\s*:\s*"\K[^"]+' "$VERSION_FILE" 2>/dev/null
}

# =============================================================================
# Validation Functions
# =============================================================================

check_root() {
    if [[ $EUID -ne 0 ]]; then
        if ! command -v sudo &>/dev/null; then
            log_error "sudo is not installed and not running as root"
            return 1
        fi
        if sudo -v 2>/dev/null; then
            log_info "Will use sudo for privileged operations"
            return 0
        else
            log_error "This script requires root or sudo capability"
            return 1
        fi
    fi
    return 0
}

check_required_tools() {
    local missing=()
    local tool

    for tool in git curl; do
        if ! command -v "$tool" &>/dev/null; then
            missing+=("$tool")
        fi
    done

    if [[ ${#missing[@]} -eq 0 ]]; then
        log_debug "Required tools present"
        return 0
    fi

    log_warn "Missing tools: ${missing[*]}"
    log_info "Attempting to install..."

    if command -v apt-get &>/dev/null; then
        run_privileged apt-get update -qq
        run_privileged apt-get install -y "${missing[@]}"
    elif command -v dnf &>/dev/null; then
        run_privileged dnf install -y "${missing[@]}"
    elif command -v yum &>/dev/null; then
        run_privileged yum install -y "${missing[@]}"
    elif command -v pacman &>/dev/null; then
        run_privileged pacman -Sy --noconfirm "${missing[@]}"
    else
        log_error "No supported package manager found"
        return 1
    fi

    return 0
}

check_build_deps() {
    log_step "Checking build dependencies..."

    if command -v apt-get &>/dev/null; then
        log_info "Installing build dependencies..."
        run_privileged apt-get update -qq
        run_privileged apt-get install -y "${BUILD_DEPS[@]}"
    elif command -v dnf &>/dev/null; then
        run_privileged dnf install -y cmake make gcc ncurses-devel sqlite-devel libcurl-devel cjson-devel libgpiod-devel
    elif command -v yum &>/dev/null; then
        run_privileged yum install -y cmake make gcc ncurses-devel sqlite-devel libcurl-devel cjson-devel
    else
        log_warn "Could not auto-install build dependencies"
        log_info "Please ensure cmake, make, gcc and development libraries are installed"
    fi

    # Install libicu (needed by .NET Core / GitHub Actions runner).
    # Package name includes a version that varies by OS release (libicu74, libicu76, etc.)
    if command -v apt-cache &>/dev/null; then
        local icu_pkg
        icu_pkg=$(apt-cache search '^libicu[0-9]' 2>/dev/null | grep -v java | head -1 | awk '{print $1}')
        if [[ -n "$icu_pkg" ]]; then
            log_info "Installing $icu_pkg (ICU runtime for .NET / CI runners)..."
            run_privileged apt-get install -y "$icu_pkg" || log_warn "Failed to install $icu_pkg (non-critical)"
        fi
    fi

    # Verify critical tools
    local critical_missing=()
    for tool in cmake make gcc; do
        if ! command -v "$tool" &>/dev/null; then
            critical_missing+=("$tool")
        fi
    done

    if [[ ${#critical_missing[@]} -gt 0 ]]; then
        log_error "Missing critical build tools: ${critical_missing[*]}"
        return 1
    fi

    log_info "Build dependencies ready"
    return 0
}

check_network() {
    log_info "Checking network connectivity..."

    if discover_network "https://github.com" 10; then
        log_debug "Network connectivity confirmed"
        return 0
    fi

    log_warn "Network check issue: $_LAST_DISCOVERY_ERROR"

    local max_retries=3
    local retry_delay=2

    for ((attempt=2; attempt<=max_retries; attempt++)); do
        log_info "Retrying (attempt $attempt/$max_retries)..."
        sleep "$retry_delay"

        if discover_network "https://github.com" 10; then
            log_info "Network connectivity confirmed on attempt $attempt"
            return 0
        fi

        retry_delay=$((retry_delay * 2))
    done

    log_error "Cannot reach GitHub after $max_retries attempts"
    return 1
}

check_disk_space() {
    local target_dir="${1:-/opt}"
    local required_mb="${2:-$MIN_DISK_SPACE_MB}"

    while [[ ! -d "$target_dir" ]] && [[ "$target_dir" != "/" ]]; do
        target_dir="$(dirname "$target_dir")"
    done

    local available_mb
    available_mb=$(df -m "$target_dir" 2>/dev/null | awk 'NR==2 {print $4}')

    if [[ -z "$available_mb" ]] || [[ "$available_mb" -lt "$required_mb" ]]; then
        log_error "Insufficient disk space: ${available_mb:-0}MB available, ${required_mb}MB required"
        return 1
    fi

    log_info "Disk space: ${available_mb}MB available"
    return 0
}

validate_environment() {
    log_step "Validating environment..."

    check_root || return 1
    check_required_tools || return 1
    check_network || return 1
    check_disk_space "/opt" "$MIN_DISK_SPACE_MB" || return 1

    log_info "Environment validation passed"
    return 0
}

# =============================================================================
# Version Management
# =============================================================================

get_remote_sha() {
    local branch="${1:-main}"
    local ref="refs/heads/$branch"

    if [[ "$branch" == v* ]]; then
        ref="refs/tags/$branch"
    fi

    git ls-remote "$REPO_URL" "$ref" 2>/dev/null | awk '{print $1}'
}

preflight_version_check() {
    local target_branch="${1:-main}"

    log_step "Running pre-flight version check..."

    local local_sha
    local remote_sha

    local_sha=$(get_installed_sha)
    if [[ -z "$local_sha" ]]; then
        log_info "No version file found, treating as fresh install"
        return 0
    fi

    log_info "Installed commit: ${local_sha:0:12}"

    remote_sha=$(get_remote_sha "$target_branch")
    if [[ -z "$remote_sha" ]]; then
        log_error "Could not fetch remote version"
        return 2
    fi

    log_info "Remote commit:    ${remote_sha:0:12}"

    if [[ "$local_sha" == "$remote_sha" ]]; then
        log_info "Already at latest version"
        return 1
    fi

    log_info "Update available: ${local_sha:0:12} -> ${remote_sha:0:12}"
    return 0
}

write_version_file() {
    local staging_dir="$1"

    local commit_sha=""
    local branch=""

    if [[ -f "$staging_dir/.commit_sha" ]]; then
        commit_sha=$(cat "$staging_dir/.commit_sha")
    fi

    if [[ -f "$staging_dir/.branch" ]]; then
        branch=$(cat "$staging_dir/.branch")
    fi

    local previous_version=""
    local previous_sha=""
    if [[ -f "$VERSION_FILE" ]]; then
        previous_version=$(get_installed_version)
        previous_sha=$(get_installed_sha)
    fi

    local version_content
    version_content=$(cat <<EOF
{
  "schema_version": 1,
  "package": "water-treat",
  "version": "1.0.0",
  "commit_sha": "$commit_sha",
  "commit_short": "${commit_sha:0:7}",
  "branch": "$branch",
  "installed_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "installed_by": "bootstrap.sh",
  "bootstrap_version": "$BOOTSTRAP_VERSION",
  "previous_version": "$previous_version",
  "previous_sha": "$previous_sha"
}
EOF
)

    echo "$version_content" | run_privileged tee "$VERSION_FILE" > /dev/null
    log_info "Version file written: $VERSION_FILE"
}

# =============================================================================
# Staging Functions
# =============================================================================

create_staging_dir() {
    local action="${1:-install}"
    local timestamp
    timestamp=$(date +%Y%m%d_%H%M%S)

    local tmp_space var_tmp_space tmp_base
    tmp_space=$(df -m /tmp 2>/dev/null | awk 'NR==2 {print $4}') || tmp_space=0
    var_tmp_space=$(df -m /var/tmp 2>/dev/null | awk 'NR==2 {print $4}') || var_tmp_space=0

    if [[ "${var_tmp_space:-0}" -gt "${tmp_space:-0}" ]] || [[ "${tmp_space:-0}" -lt 256 ]]; then
        tmp_base="/var/tmp"
    else
        tmp_base="/tmp"
    fi

    local staging_dir="${tmp_base}/water-treat-${action}-${timestamp}-$$"
    mkdir -p "$staging_dir"
    echo "$staging_dir"
}

clone_to_staging() {
    local staging_dir="$1"
    local branch="${2:-main}"

    log_step "Cloning repository..."

    local clone_output
    if ! clone_output=$(git clone --depth 1 --branch "$branch" "$REPO_URL" "$staging_dir/repo" 2>&1); then
        log_error "Failed to clone repository"
        log_error "Git output: $clone_output"
        return 1
    fi

    if [[ ! -d "$staging_dir/repo/.git" ]]; then
        log_error "Clone verification failed"
        return 1
    fi

    local commit_sha
    commit_sha=$(cd "$staging_dir/repo" && git rev-parse HEAD)

    log_info "Cloned: ${commit_sha:0:7}"

    echo "$commit_sha" > "$staging_dir/.commit_sha"
    echo "$branch" > "$staging_dir/.branch"

    return 0
}

# =============================================================================
# Shared Protocol Fetch
# =============================================================================

fetch_shared_protocols() {
    local source_dir="$1"
    local shared_dir="${source_dir}/include/shared"

    log_step "Fetching shared protocol headers from Water-Controller..."

    mkdir -p "$shared_dir"

    local failed=0
    for file in "${SHARED_PROTOCOL_FILES[@]}"; do
        local url="${CONTROLLER_RAW_URL}/${file}"
        local dest="${shared_dir}/${file}"

        log_verbose "Fetching ${file}..."

        # Retry with exponential backoff
        local max_retries=4
        local delay=2
        local success=false

        for ((attempt=1; attempt<=max_retries; attempt++)); do
            if curl -fsSL --connect-timeout 10 "$url" -o "$dest" 2>/dev/null; then
                # Verify file is not empty and looks like a C header
                if [[ -s "$dest" ]] && grep -q "#ifndef" "$dest"; then
                    success=true
                    break
                fi
            fi

            if [[ $attempt -lt $max_retries ]]; then
                log_verbose "Retry $attempt for ${file} in ${delay}s..."
                sleep $delay
                delay=$((delay * 2))
            fi
        done

        if [[ "$success" == "true" ]]; then
            log_verbose "Downloaded: ${file}"
        else
            log_error "Failed to fetch: ${file}"
            ((failed++))
        fi
    done

    if [[ $failed -gt 0 ]]; then
        log_error "Failed to fetch $failed protocol file(s) from Water-Controller"
        log_error "Check network connectivity to GitHub"
        return 1
    fi

    # Validate protocol files
    validate_shared_protocols "$shared_dir" || return 1

    log_info "Shared protocol headers fetched and validated"
    return 0
}

validate_shared_protocols() {
    local shared_dir="$1"

    log_verbose "Validating protocol headers..."

    # Validate user_sync_protocol.h
    local user_sync="${shared_dir}/user_sync_protocol.h"
    if [[ ! -f "$user_sync" ]]; then
        log_error "Missing: user_sync_protocol.h"
        return 1
    fi

    # Check required markers
    local markers=(
        "USER_SYNC_MAGIC.*0x55534552"
        "USER_SYNC_PROTOCOL_VERSION.*2"
        "USER_SYNC_RECORD_INDEX.*0xF840"
        "USER_SYNC_MAX_USERS.*16"
        "USER_SYNC_SALT.*NaCl4Life"
    )

    for marker in "${markers[@]}"; do
        if ! grep -qE "$marker" "$user_sync"; then
            log_error "user_sync_protocol.h: missing marker pattern: $marker"
            return 1
        fi
    done

    # Validate config_sync_protocol.h
    local config_sync="${shared_dir}/config_sync_protocol.h"
    if [[ ! -f "$config_sync" ]]; then
        log_error "Missing: config_sync_protocol.h"
        return 1
    fi

    local config_markers=(
        "CONFIG_SYNC_PROTOCOL_VERSION.*1"
        "CONFIG_SYNC_DEVICE_INDEX.*0xF841"
        "ENROLLMENT_MAGIC.*0x454E524C"
    )

    for marker in "${config_markers[@]}"; do
        if ! grep -qE "$marker" "$config_sync"; then
            log_error "config_sync_protocol.h: missing marker pattern: $marker"
            return 1
        fi
    done

    log_verbose "Protocol headers validated successfully"
    return 0
}

# =============================================================================
# Build Functions
# =============================================================================

build_from_source() {
    local source_dir="$1"

    log_step "Building from source..."

    if [[ ! -f "$source_dir/CMakeLists.txt" ]]; then
        log_error "CMakeLists.txt not found in $source_dir"
        return 1
    fi

    # Fetch shared protocol headers from Water-Controller
    fetch_shared_protocols "$source_dir" || {
        log_error "Cannot build without protocol headers from Water-Controller"
        return 1
    }

    local build_dir="$source_dir/build"
    mkdir -p "$build_dir"

    log_info "Running cmake..."
    (cd "$build_dir" && cmake ..) || {
        log_error "CMake configuration failed"
        return 1
    }

    log_info "Compiling (this may take a few minutes)..."
    local nproc_count
    nproc_count=$(nproc 2>/dev/null || echo 2)
    (cd "$build_dir" && make -j"$nproc_count") || {
        log_error "Compilation failed"
        return 1
    }

    if [[ ! -f "$build_dir/water-treat" ]]; then
        log_error "Binary not found after build"
        return 1
    fi

    log_info "Build successful"
    return 0
}

install_files() {
    local source_dir="$1"

    log_step "Installing files..."

    # Create directories
    run_privileged mkdir -p "$INSTALL_DIR"
    run_privileged mkdir -p "$CONFIG_DIR"
    run_privileged mkdir -p "$DATA_DIR"
    run_privileged mkdir -p "$PNET_DATA_DIR"
    run_privileged mkdir -p "$LOG_DIR"

    # CRITICAL: Clear ALL p-net NV storage for clean PROFINET state
    # This addresses two issues:
    # 1. Station name contamination (e.g., "rt-labs-dev" from p-net defaults)
    # 2. Stale AR state causing PNIO errors (status1=0x00000001, status2=0x00000003)
    #    which means "AR already exists" - the RTU rejects reconnection attempts
    # Clearing pf_* files forces fresh AR negotiation with the controller.
    purge_pnet_nv_storage "$PNET_DATA_DIR"

    # Copy source to install location
    run_privileged cp -a "$source_dir/." "$INSTALL_DIR/"

    # Install binary to /usr/local/bin
    if [[ -f "$INSTALL_DIR/build/water-treat" ]]; then
        run_privileged cp "$INSTALL_DIR/build/water-treat" /usr/local/bin/water-treat
        run_privileged chmod +x /usr/local/bin/water-treat
        # Set capabilities for raw socket access (PROFINET needs this)
        if command -v setcap &>/dev/null; then
            run_privileged setcap cap_net_raw+ep /usr/local/bin/water-treat || \
                log_warn "Could not set capabilities, will need to run as root"
        fi
        log_info "Binary installed: /usr/local/bin/water-treat"
    fi

    # Auto-detect network settings for config
    local iface station_name
    iface=$(detect_network_interface)
    station_name=$(detect_station_name "$iface")
    log_info "Detected interface: ${iface:-none}"
    log_info "Detected station_name: $station_name"

    # Create default config if not exists (proper INI format)
    if [[ ! -f "$CONFIG_DIR/water-treat.conf" ]]; then
        run_privileged tee "$CONFIG_DIR/water-treat.conf" > /dev/null <<EOF
# Water-Treat RTU Configuration
# Generated by bootstrap.sh on $(date)
#
# PROFINET Station Name: ${station_name}
#   Format: rtu-XXXX (last 4 hex chars of MAC address, lowercase)
#   Requirements: lowercase a-z, 0-9, hyphens only (IEC 61158-6)
#   NO uppercase, underscores, dots allowed

[system]
device_name = $station_name
log_level = info
log_file = $LOG_DIR/monitor.log
daemon_mode = false

[network]
interface = $iface
dhcp_enabled = true

[profinet]
enabled = true
station_name = $station_name
vendor_id = 0x0493
device_id = 0x0001
product_name = Water Treatment RTU
min_device_interval = 32
data_dir = $PNET_DATA_DIR

[database]
path = $DATA_DIR/water-treat.db
create_if_missing = true
busy_timeout_ms = 5000

[logging]
enabled = true
interval_seconds = 60
retention_days = 30
destination = 1

[health]
enabled = true
http_enabled = true
http_port = 9081
file_path = $DATA_DIR/health.prom
update_interval_seconds = 10
EOF
        log_info "Config created: $CONFIG_DIR/water-treat.conf"
        log_info "Station name set to: $station_name"
    else
        log_info "Config exists, preserving: $CONFIG_DIR/water-treat.conf"
        # Check if station_name is configured
        if ! grep -q "^station_name" "$CONFIG_DIR/water-treat.conf" 2>/dev/null; then
            log_warn "No station_name in config - RTU may not respond to DCP"
        fi
    fi

    # Create environment file for systemd
    if [[ ! -f "$CONFIG_DIR/water-treat.env" ]]; then
        run_privileged tee "$CONFIG_DIR/water-treat.env" > /dev/null <<EOF
# Water-Treat RTU Environment Variables
# Loaded by systemd service - override config file values here
# WT_HTTP_PORT=9081
# WT_LOG_LEVEL=info
EOF
    fi

    return 0
}

create_systemd_service() {
    log_step "Creating systemd service..."

    local service_file="/etc/systemd/system/${SERVICE_NAME}.service"

    # Use repo service file if available, otherwise create one
    if [[ -f "$INSTALL_DIR/systemd/water-treat.service" ]]; then
        run_privileged cp "$INSTALL_DIR/systemd/water-treat.service" "$service_file"
        log_info "Installed service from repository"
    else
        run_privileged tee "$service_file" > /dev/null <<EOF
[Unit]
Description=Water Treatment RTU - PROFINET I/O Device
Documentation=https://github.com/mwilco03/Water-Treat
After=network.target local-fs.target
Wants=network.target

[Service]
Type=notify
NotifyAccess=main
ExecStart=/usr/local/bin/water-treat --daemon
ExecReload=/bin/kill -HUP \$MAINPID
Restart=always
RestartSec=5

User=root
Group=root
WorkingDirectory=$DATA_DIR

WatchdogSec=30
StandardOutput=journal
StandardError=journal
SyslogIdentifier=water-treat

EnvironmentFile=-$CONFIG_DIR/water-treat.env
Environment=TERM=dumb

# Security hardening
ProtectSystem=strict
ReadWritePaths=$DATA_DIR $LOG_DIR
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
        log_info "Created systemd service"
    fi

    run_privileged systemctl daemon-reload
    run_privileged systemctl enable "${SERVICE_NAME}.service"

    log_info "Systemd service enabled"
}

# =============================================================================
# Backup Functions
# =============================================================================

create_backup() {
    local backup_reason="${1:-backup}"
    local timestamp
    timestamp=$(date +%Y%m%d_%H%M%S)

    local backup_path="${BACKUP_DIR}/${backup_reason}-${timestamp}"

    log_step "Creating backup..."

    run_privileged mkdir -p "$BACKUP_DIR" || {
        log_error "Failed to create backup directory"
        return 1
    }

    if [[ -d "$INSTALL_DIR" ]]; then
        run_privileged cp -a "$INSTALL_DIR" "$backup_path" || {
            log_error "Failed to backup installation"
            return 1
        }
        log_info "Backup created: $backup_path"
        echo "$backup_path"
        return 0
    else
        log_warn "No installation to backup"
        return 1
    fi
}

cleanup_old_backups() {
    local keep_count="${1:-3}"

    if [[ ! -d "$BACKUP_DIR" ]]; then
        return 0
    fi

    find "$BACKUP_DIR" -maxdepth 1 -mindepth 1 -type d -printf '%T@ %p\n' 2>/dev/null | \
        sort -n | \
        head -n -"$keep_count" | \
        cut -d' ' -f2- | \
        while read -r old_backup; do
            log_debug "Removing old backup: $old_backup"
            run_privileged rm -rf "$old_backup"
        done
}

# =============================================================================
# Action Handlers
# =============================================================================

do_install() {
    local branch="${1:-main}"
    local force="${2:-false}"

    local state
    state=$(detect_system_state)

    case "$state" in
        fresh)
            log_info "Fresh system detected"
            ;;
        installed)
            if [[ "$force" == "true" ]]; then
                log_warn "Existing installation found, --force specified"
            else
                log_error "Water-Treat is already installed"
                log_info "Use 'upgrade' to update, or 'install --force' to reinstall"
                log_info "Current version: $(get_installed_version)"
                return 1
            fi
            ;;
        corrupted)
            log_warn "Corrupted installation detected, will attempt to fix"
            ;;
    esac

    # Kill any rogue water-treat processes before install
    # Prevents duplicate p-net instances causing duplicate DCP responses
    if pgrep -x "water-treat" >/dev/null 2>&1; then
        log_warn "Found running water-treat process(es), stopping..."
        run_privileged systemctl stop "${SERVICE_NAME}.service" 2>/dev/null || true
        run_privileged pkill -9 -x "water-treat" 2>/dev/null || true
        sleep 1
    fi

    # Check build dependencies
    check_build_deps || return 1

    # Create staging
    local staging_dir
    staging_dir=$(create_staging_dir "install")
    register_cleanup "$staging_dir"

    # Clone
    clone_to_staging "$staging_dir" "$branch" || return 1

    # Build
    build_from_source "$staging_dir/repo" || return 1

    # Install
    install_files "$staging_dir/repo" || return 1

    # Create service
    create_systemd_service || return 1

    # Write version file
    write_version_file "$staging_dir"

    # Start service
    log_step "Starting service..."
    run_privileged systemctl start "${SERVICE_NAME}.service" || {
        log_warn "Service failed to start (may require GPIO access)"
    }

    # Summary
    log_info ""
    log_info "========================================"
    log_info "  WATER-TREAT RTU INSTALLATION COMPLETE"
    log_info "========================================"
    log_info ""
    log_info "Binary:      /usr/local/bin/water-treat"
    log_info "Config:      $CONFIG_DIR/water-treat.conf"
    log_info "Data:        $DATA_DIR"
    log_info "Logs:        $LOG_DIR"
    log_info ""
    log_info "Service commands:"
    log_info "  sudo systemctl status $SERVICE_NAME"
    log_info "  sudo systemctl start $SERVICE_NAME"
    log_info "  sudo systemctl stop $SERVICE_NAME"
    log_info "  sudo systemctl restart $SERVICE_NAME"
    log_info ""
    log_info "Default login: admin / H2OhYeah!"
    log_info ""

    return 0
}

do_upgrade() {
    local branch="${1:-main}"
    local force="${2:-false}"

    local state
    state=$(detect_system_state)

    case "$state" in
        fresh)
            log_error "No installation found. Use 'install' instead."
            return 1
            ;;
        installed)
            log_info "Existing installation: $(get_installed_version)"
            ;;
        corrupted)
            log_warn "Corrupted installation. Consider 'fresh' instead."
            if [[ "$force" != "true" ]]; then
                return 1
            fi
            ;;
    esac

    # Pre-flight check
    if [[ "$force" != "true" ]]; then
        preflight_version_check "$branch"
        local result=$?
        if [[ $result -eq 1 ]]; then
            return 0
        elif [[ $result -eq 2 ]]; then
            log_error "Pre-flight check failed. Use --force to skip."
            return 1
        fi
    fi

    # Create backup
    local backup_dir=""
    backup_dir=$(create_backup "pre-upgrade") || true

    # Stop service
    log_info "Stopping service..."
    run_privileged systemctl stop "${SERVICE_NAME}.service" 2>/dev/null || true

    # Kill any rogue water-treat processes not managed by systemd
    # This catches manually-started instances, containers, or stale processes
    # that would cause duplicate DCP responses (two p-net instances = two station names)
    if pgrep -x "water-treat" >/dev/null 2>&1; then
        log_warn "Found rogue water-treat process(es) not managed by systemd"
        run_privileged pkill -9 -x "water-treat" 2>/dev/null || true
        sleep 1
        if pgrep -x "water-treat" >/dev/null 2>&1; then
            log_error "Could not kill all water-treat processes"
            log_error "Run: sudo pkill -9 water-treat"
            return 1
        fi
        log_info "Killed rogue water-treat process(es)"
    fi

    # Check build dependencies
    check_build_deps || return 1

    # Create staging
    local staging_dir
    staging_dir=$(create_staging_dir "upgrade")
    register_cleanup "$staging_dir"

    # Clone
    clone_to_staging "$staging_dir" "$branch" || return 1

    # Build
    build_from_source "$staging_dir/repo" || {
        log_error "Build failed"
        if [[ -n "$backup_dir" ]]; then
            log_info "Backup available: $backup_dir"
        fi
        return 1
    }

    # Install (preserving config)
    install_files "$staging_dir/repo" || return 1

    # Write version file
    write_version_file "$staging_dir"

    # Start service
    log_step "Starting service..."
    run_privileged systemctl start "${SERVICE_NAME}.service" || {
        log_warn "Service failed to start"
    }

    cleanup_old_backups 2

    log_info "Upgrade completed successfully!"
    return 0
}

do_wipe() {
    log_step "Starting complete system wipe..."

    # Stop service
    run_privileged systemctl stop "${SERVICE_NAME}.service" 2>/dev/null || true
    run_privileged systemctl disable "${SERVICE_NAME}.service" 2>/dev/null || true
    run_privileged rm -f "/etc/systemd/system/${SERVICE_NAME}.service"
    run_privileged systemctl daemon-reload

    # Kill ALL water-treat processes (including rogue/manual instances)
    run_privileged pkill -9 -x "water-treat" 2>/dev/null || true
    sleep 1

    # Remove binary
    run_privileged rm -f /usr/local/bin/water-treat

    # Remove all directories
    log_info "Removing all Water-Treat files..."
    run_privileged rm -rf "$INSTALL_DIR"
    run_privileged rm -rf "$CONFIG_DIR"
    run_privileged rm -rf "$DATA_DIR"
    run_privileged rm -rf "$LOG_DIR"
    run_privileged rm -rf "$BACKUP_DIR"

    # Remove bootstrap log file
    run_privileged rm -f "$BOOTSTRAP_LOG"

    # Clean temp files from both /tmp and /var/tmp
    run_privileged rm -rf /tmp/water-treat-* 2>/dev/null || true
    run_privileged rm -rf /var/tmp/water-treat-* 2>/dev/null || true

    # Remove any stale PID files, lock files, and runtime logs
    run_privileged rm -f /var/run/water-treat.pid 2>/dev/null || true
    run_privileged rm -f /run/water-treat.pid 2>/dev/null || true
    run_privileged rm -rf /run/water-treat 2>/dev/null || true

    # Clean local development build if running from source tree
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [[ -f "$script_dir/CMakeLists.txt" && -d "$script_dir/build" ]]; then
        log_info "Cleaning local development build..."
        rm -rf "$script_dir/build"
    fi

    # Remove runtime directory and logs (created by systemd RuntimeDirectory=water-treat)
    run_privileged rm -rf /run/water-treat 2>/dev/null || true

    # Verify removal
    local remaining=()
    [[ -d "$INSTALL_DIR" ]] && remaining+=("$INSTALL_DIR")
    [[ -d "$CONFIG_DIR" ]] && remaining+=("$CONFIG_DIR")
    [[ -d "$DATA_DIR" ]] && remaining+=("$DATA_DIR")
    [[ -d "$LOG_DIR" ]] && remaining+=("$LOG_DIR")
    [[ -f "$BOOTSTRAP_LOG" ]] && remaining+=("$BOOTSTRAP_LOG")

    if [[ ${#remaining[@]} -gt 0 ]]; then
        log_warn "Some files could not be removed: ${remaining[*]}"
    else
        log_info "All Water-Treat traces removed"
    fi

    log_info "System wipe completed"
    return 0
}

do_fresh() {
    local branch="${1:-main}"

    log_step "Starting fresh install (wipe + install)..."

    # Wipe first
    do_wipe || {
        log_error "Wipe failed"
        return 1
    }

    # Validate environment
    validate_environment || return 1

    # Install
    do_install "$branch" "true"
    return $?
}

# =============================================================================
# Help and Usage
# =============================================================================

show_help() {
    cat <<EOF
Water-Treat RTU Bootstrap Script v$BOOTSTRAP_VERSION

USAGE:
    bootstrap.sh [ACTION] [OPTIONS]

ACTIONS:
    install     First-time setup (default for fresh systems)
    upgrade     Update or fix existing installation (preserves config)
    wipe        Complete removal: binary, configs, data, logs
    fresh       Wipe + install from scratch

OPTIONS:
    --branch <name>     Use specific git branch (default: main)
    --force             Force action even if checks fail
    --quiet, -q         Suppress non-essential output
    --verbose, -v       Show detailed output
    --help, -h          Show this help message
    --version           Show version information

EXAMPLES:
    # First-time install
    curl -fsSL $REPO_RAW_URL/main/bootstrap.sh | sudo bash

    # Update existing installation
    curl -fsSL $REPO_RAW_URL/main/bootstrap.sh | sudo bash -s -- upgrade

    # Complete removal
    curl -fsSL $REPO_RAW_URL/main/bootstrap.sh | sudo bash -s -- wipe

    # Start over from scratch
    curl -fsSL $REPO_RAW_URL/main/bootstrap.sh | sudo bash -s -- fresh

    # Install from specific branch
    curl -fsSL .../bootstrap.sh | sudo bash -s -- install --branch develop

DIRECTORIES:
    Installation:   $INSTALL_DIR
    Configuration:  $CONFIG_DIR
    Data:           $DATA_DIR
    Logs:           $LOG_DIR

For more information: https://github.com/mwilco03/Water-Treat
EOF
}

show_version() {
    echo "Water-Treat RTU Bootstrap v$BOOTSTRAP_VERSION"

    local state
    state=$(detect_system_state)

    if [[ "$state" == "installed" ]]; then
        echo "Installed version: $(get_installed_version)"
        echo "Installed commit:  $(get_installed_sha | cut -c1-12)"
    else
        echo "Installation status: $state"
    fi
}

# =============================================================================
# Main Entry Point
# =============================================================================

main() {
    local action=""
    local branch="main"
    local force="false"

    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case "$1" in
            install|upgrade|wipe|fresh)
                action="$1"
                shift
                ;;
            --branch)
                branch="$2"
                shift 2
                ;;
            --force)
                force="true"
                shift
                ;;
            --quiet|-q)
                QUIET_MODE="true"
                shift
                ;;
            --verbose|-v)
                VERBOSE_MODE="true"
                shift
                ;;
            --help|-h)
                show_help
                exit 0
                ;;
            --version)
                show_version
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done

    # Initialize logging
    init_logging
    log_debug "Bootstrap started: action=$action branch=$branch force=$force"

    # Auto-detect action if not specified
    if [[ -z "$action" ]]; then
        local state
        state=$(detect_system_state)

        case "$state" in
            fresh)
                action="install"
                log_info "Fresh system detected, will install"
                ;;
            installed)
                action="upgrade"
                log_info "Existing installation detected, will upgrade"
                ;;
            corrupted)
                log_warn "Corrupted installation detected"
                action="fresh"
                ;;
        esac
    fi

    # Validate environment (except for wipe)
    if [[ "$action" != "wipe" ]]; then
        validate_environment || exit 1
    else
        check_root || exit 1
    fi

    # Execute action
    case "$action" in
        install)
            do_install "$branch" "$force"
            ;;
        upgrade)
            do_upgrade "$branch" "$force"
            ;;
        wipe)
            do_wipe
            ;;
        fresh)
            do_fresh "$branch"
            ;;
        *)
            log_error "Unknown action: $action"
            show_help
            exit 1
            ;;
    esac

    exit $?
}

# Run main
main "$@"
