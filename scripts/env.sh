#!/usr/bin/env bash
#
# env.sh - Shared environment setup for Water-Treat build system
#
# This file is SOURCED, not executed. All other scripts source this file
# to get consistent environment configuration.
#
# Usage:
#   source "${SCRIPT_DIR}/env.sh"
#
# What: Sets up PKG_CONFIG_PATH, CPPFLAGS, LDFLAGS, and architecture detection
# Why: Single source of truth for environment. Avoids duplication across scripts.
# Edge cases: If paths don't exist, they're silently skipped. Idempotent.
#

# ------------------------------------------------------------------------------
# Guard against double-sourcing
# ------------------------------------------------------------------------------
# What: Prevents env.sh from running twice if sourced multiple times
# Why: Prepending to PATH/PKG_CONFIG_PATH repeatedly causes issues
# Edge cases: First source sets the flag, subsequent sources exit early
#
[[ -n "${_WATER_TREAT_ENV_LOADED:-}" ]] && return 0
readonly _WATER_TREAT_ENV_LOADED=1

# ------------------------------------------------------------------------------
# Path Discovery
# ------------------------------------------------------------------------------
# What: Determine where this script lives and where the project root is
# Why: Scripts live in scripts/, project root is one level up
# Edge cases: Works whether sourced or executed, handles symlinks
#

# Get the directory containing this script
# BASH_SOURCE[0] is this file, even when sourced
if [[ -n "${BASH_SOURCE[0]:-}" ]]; then
    ENV_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
    # Fallback for non-bash shells (shouldn't happen, but defensive)
    ENV_SCRIPT_DIR="$(pwd)"
fi

# Project root is parent of scripts/
PROJECT_ROOT="$(dirname "${ENV_SCRIPT_DIR}")"

# Export for use by other scripts
export PROJECT_ROOT
export SCRIPTS_DIR="${ENV_SCRIPT_DIR}"

# ------------------------------------------------------------------------------
# Architecture Detection
# ------------------------------------------------------------------------------
# What: Detect CPU architecture and derive the GNU triplet for library paths
# Why: Different architectures install libraries to different paths
#      e.g., /usr/lib/aarch64-linux-gnu vs /usr/lib/x86_64-linux-gnu
# Edge cases: Unknown architectures get empty triplet, scripts should handle this
# User impact: None - this is transparent detection
#

ARCH="$(uname -m)"
case "${ARCH}" in
    aarch64)
        ARCH_TRIPLET="aarch64-linux-gnu"
        ARCH_BITS=64
        ;;
    armv7l|armhf)
        ARCH_TRIPLET="arm-linux-gnueabihf"
        ARCH_BITS=32
        ;;
    armv6l)
        ARCH_TRIPLET="arm-linux-gnueabihf"
        ARCH_BITS=32
        ;;
    x86_64)
        ARCH_TRIPLET="x86_64-linux-gnu"
        ARCH_BITS=64
        ;;
    i686|i386)
        ARCH_TRIPLET="i386-linux-gnu"
        ARCH_BITS=32
        ;;
    *)
        # Unknown architecture - leave empty, code must handle this
        ARCH_TRIPLET=""
        ARCH_BITS=""
        ;;
esac

export ARCH
export ARCH_TRIPLET
export ARCH_BITS

# ------------------------------------------------------------------------------
# PKG_CONFIG_PATH Setup
# ------------------------------------------------------------------------------
# What: Build list of pkg-config search paths for finding libraries
# Why: Libraries installed to /usr/local need their .pc files discoverable
#      Different architectures have different lib paths
# Edge cases: Only adds paths that actually exist on this system
# User impact: pkg-config finds locally-installed libraries like libgpiod
#

_pkg_paths=()

# Local installation paths (highest priority - our libgpiod v1 installs here)
[[ -d /usr/local/lib/pkgconfig ]] && _pkg_paths+=("/usr/local/lib/pkgconfig")
[[ -d /usr/local/lib64/pkgconfig ]] && _pkg_paths+=("/usr/local/lib64/pkgconfig")

# Architecture-specific local paths
if [[ -n "${ARCH_TRIPLET}" ]]; then
    [[ -d "/usr/local/lib/${ARCH_TRIPLET}/pkgconfig" ]] && \
        _pkg_paths+=("/usr/local/lib/${ARCH_TRIPLET}/pkgconfig")
fi

# System paths (lower priority)
[[ -d /usr/lib/pkgconfig ]] && _pkg_paths+=("/usr/lib/pkgconfig")
[[ -d /usr/lib64/pkgconfig ]] && _pkg_paths+=("/usr/lib64/pkgconfig")

if [[ -n "${ARCH_TRIPLET}" ]]; then
    [[ -d "/usr/lib/${ARCH_TRIPLET}/pkgconfig" ]] && \
        _pkg_paths+=("/usr/lib/${ARCH_TRIPLET}/pkgconfig")
fi

# Share paths
[[ -d /usr/share/pkgconfig ]] && _pkg_paths+=("/usr/share/pkgconfig")

# Build the PKG_CONFIG_PATH string
# Prepend our paths to any existing PKG_CONFIG_PATH
if [[ ${#_pkg_paths[@]} -gt 0 ]]; then
    _new_pkg_path=""
    for _p in "${_pkg_paths[@]}"; do
        if [[ -z "${_new_pkg_path}" ]]; then
            _new_pkg_path="${_p}"
        else
            _new_pkg_path="${_new_pkg_path}:${_p}"
        fi
    done

    if [[ -n "${PKG_CONFIG_PATH:-}" ]]; then
        export PKG_CONFIG_PATH="${_new_pkg_path}:${PKG_CONFIG_PATH}"
    else
        export PKG_CONFIG_PATH="${_new_pkg_path}"
    fi
fi

unset _pkg_paths _new_pkg_path _p

# ------------------------------------------------------------------------------
# Compiler/Linker Flags
# ------------------------------------------------------------------------------
# What: Set CPPFLAGS and LDFLAGS for finding locally-installed libraries
# Why: Libraries in /usr/local aren't always in the default search path
#      -Wl,-rpath ensures the runtime linker finds libs without LD_LIBRARY_PATH
# Edge cases: Only adds flags if directories exist
# User impact: CMake and direct compilation find libgpiod, p-net, etc.
#

# Include paths
if [[ -d /usr/local/include ]]; then
    export CPPFLAGS="-I/usr/local/include ${CPPFLAGS:-}"
    export CFLAGS="-I/usr/local/include ${CFLAGS:-}"
fi

# Library paths with runtime path
if [[ -d /usr/local/lib ]]; then
    export LDFLAGS="-L/usr/local/lib -Wl,-rpath,/usr/local/lib ${LDFLAGS:-}"
fi

# Architecture-specific paths
if [[ -n "${ARCH_TRIPLET}" && -d "/usr/local/lib/${ARCH_TRIPLET}" ]]; then
    export LDFLAGS="-L/usr/local/lib/${ARCH_TRIPLET} -Wl,-rpath,/usr/local/lib/${ARCH_TRIPLET} ${LDFLAGS}"
fi

# ------------------------------------------------------------------------------
# LD_LIBRARY_PATH (Runtime)
# ------------------------------------------------------------------------------
# What: Ensure runtime linker finds libraries in /usr/local
# Why: Some systems don't have /usr/local/lib in ldconfig by default
# Edge cases: Only adds if directory exists
# User impact: Running binaries finds shared libraries without extra config
#

if [[ -d /usr/local/lib ]]; then
    export LD_LIBRARY_PATH="/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

# ------------------------------------------------------------------------------
# Output Helpers
# ------------------------------------------------------------------------------
# What: Consistent colored output functions for all scripts
# Why: DRY - define once, use everywhere. Consistent UX.
# Edge cases: Works in non-TTY (colors are still emitted, most tools strip them)
# User impact: Clear, color-coded feedback
#

readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly CYAN='\033[0;36m'
readonly BOLD='\033[1m'
readonly NC='\033[0m'  # No Color

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
detail()  { echo -e "${CYAN}  ->${NC} $*"; }
header()  { echo -e "\n${BOLD}=== $* ===${NC}\n"; }

# Export functions for subshells
export -f info success warn error detail header

# ------------------------------------------------------------------------------
# Condition Tracking
# ------------------------------------------------------------------------------
# What: Arrays and functions for tracking breaking vs non-breaking issues
# Why: Scripts can accumulate warnings but fail on real problems
#      Summary at end shows everything that happened
# Edge cases: Arrays start empty, safe to append
# User impact: Clear distinction between "might be a problem" and "definitely broken"
#

declare -a BREAKING_ISSUES=()
declare -a WARNINGS=()
declare -a ACTIONS_TAKEN=()

breaking() {
    BREAKING_ISSUES+=("$1")
    error "$1"
}

non_breaking() {
    WARNINGS+=("$1")
    warn "$1"
}

action() {
    ACTIONS_TAKEN+=("$1")
    info "$1"
}

# Summary function - call at end of scripts
# Returns 0 if no breaking issues, 1 otherwise
summary() {
    echo ""
    echo "========================================"
    echo "  Summary"
    echo "========================================"

    if [[ ${#ACTIONS_TAKEN[@]} -gt 0 ]]; then
        echo -e "${GREEN}Actions taken:${NC}"
        for a in "${ACTIONS_TAKEN[@]}"; do
            echo "  - ${a}"
        done
    fi

    if [[ ${#WARNINGS[@]} -gt 0 ]]; then
        echo -e "${YELLOW}Warnings:${NC}"
        for w in "${WARNINGS[@]}"; do
            echo "  - ${w}"
        done
    fi

    if [[ ${#BREAKING_ISSUES[@]} -gt 0 ]]; then
        echo -e "${RED}Blocking issues:${NC}"
        for b in "${BREAKING_ISSUES[@]}"; do
            echo "  - ${b}"
        done
        echo ""
        return 1
    fi

    echo ""
    return 0
}

export -f breaking non_breaking action summary

# ------------------------------------------------------------------------------
# CMake Cache Reading
# ------------------------------------------------------------------------------
# What: Function to read values from CMake's cache
# Why: CMakeLists.txt is source of truth for project name, binary name, etc.
#      We read from cache rather than parsing CMakeLists.txt with regex
# Edge cases: Returns empty if cache doesn't exist or key not found
# User impact: None - internal utility
#
# Usage: value=$(cmake_cache_get "CMAKE_PROJECT_NAME" "/path/to/build")
#

cmake_cache_get() {
    local key="$1"
    local build_dir="${2:-${PROJECT_ROOT}/build}"
    local cache_file="${build_dir}/CMakeCache.txt"

    if [[ ! -f "${cache_file}" ]]; then
        return 1
    fi

    # CMakeCache.txt format: KEY:TYPE=VALUE
    # Use a variable to capture grep result and preserve exit status
    local line
    line="$(grep "^${key}:" "${cache_file}" 2>/dev/null)" || return 1
    echo "${line}" | cut -d= -f2-
}

export -f cmake_cache_get

# ------------------------------------------------------------------------------
# Debug Output (if requested)
# ------------------------------------------------------------------------------
# What: Show what was configured if DEBUG_ENV is set
# Why: Helps troubleshoot environment issues
# Edge cases: Only runs if explicitly requested
# User impact: Set DEBUG_ENV=1 before sourcing to see configuration
#

if [[ "${DEBUG_ENV:-0}" == "1" ]]; then
    echo "=== Water-Treat Environment ==="
    echo "PROJECT_ROOT:    ${PROJECT_ROOT}"
    echo "SCRIPTS_DIR:     ${SCRIPTS_DIR}"
    echo "ARCH:            ${ARCH}"
    echo "ARCH_TRIPLET:    ${ARCH_TRIPLET}"
    echo "PKG_CONFIG_PATH: ${PKG_CONFIG_PATH:-<not set>}"
    echo "CPPFLAGS:        ${CPPFLAGS:-<not set>}"
    echo "LDFLAGS:         ${LDFLAGS:-<not set>}"
    echo "================================"
fi
