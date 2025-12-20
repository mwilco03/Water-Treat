#!/usr/bin/env bash
#
# build.sh - Build the Water-Treat RTU firmware
#
# What: Thin wrapper around CMake that handles common options
# Why: Operators don't need to know CMake syntax
#      Developers can use CMake directly if they prefer
#
# Usage:
#   ./scripts/build.sh [OPTIONS]
#
# Options:
#   release    Build in Release mode (optimized)
#   debug      Build in Debug mode (default, with symbols)
#   clean      Remove build directory first
#   test       Enable building tests
#   help       Show help
#

set -uo pipefail

# ------------------------------------------------------------------------------
# Source shared environment
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
source "${SCRIPT_DIR}/env.sh"

# ------------------------------------------------------------------------------
# Configuration
# ------------------------------------------------------------------------------
# What: Default values that can be overridden by arguments
# Why: Sensible defaults, easy customization
#

BUILD_TYPE="Debug"
DO_CLEAN=false
BUILD_TESTS="OFF"
BUILD_DIR="${PROJECT_ROOT}/build"

# ------------------------------------------------------------------------------
# Argument Parsing
# ------------------------------------------------------------------------------
# What: Parse command-line arguments
# Why: Simple interface for operators
# Edge cases: Unknown arguments are warnings, not errors
# User impact: Flexible invocation
#

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Options:
  release    Build in Release mode (optimized, no debug symbols)
  debug      Build in Debug mode (default, with debug symbols)
  clean      Remove build directory before building
  test       Enable building and running tests
  help       Show this help message

Examples:
  $0                    # Debug build
  $0 release            # Release build
  $0 clean release      # Clean rebuild in Release mode
  $0 debug test         # Debug build with tests

Environment:
  PROJECT_ROOT: ${PROJECT_ROOT}
  BUILD_DIR:    ${BUILD_DIR}
EOF
}

parse_args() {
    for arg in "$@"; do
        case "${arg,,}" in  # lowercase for case-insensitive
            release)
                BUILD_TYPE="Release"
                ;;
            debug)
                BUILD_TYPE="Debug"
                ;;
            clean)
                DO_CLEAN=true
                ;;
            test|tests)
                BUILD_TESTS="ON"
                ;;
            help|-h|--help)
                usage
                exit 0
                ;;
            *)
                warn "Unknown option: ${arg} (ignoring)"
                ;;
        esac
    done
}

# ------------------------------------------------------------------------------
# Generator Detection
# ------------------------------------------------------------------------------
# What: Prefer Ninja over Make if available
# Why: Ninja is faster for incremental builds
# Edge cases: Fall back to Make if Ninja not installed
# User impact: Faster builds if they have Ninja
#

detect_generator() {
    if command -v ninja &>/dev/null; then
        echo "Ninja"
    else
        echo "Unix Makefiles"
    fi
}

# ------------------------------------------------------------------------------
# Dependency Check
# ------------------------------------------------------------------------------
# What: Quick check that required tools exist
# Why: Fail fast with clear message rather than cryptic CMake errors
# Edge cases: Missing tools are breaking
# User impact: Clear guidance to run install-deps.sh
#

check_build_tools() {
    local missing=()

    for cmd in cmake pkg-config; do
        if ! command -v "${cmd}" &>/dev/null; then
            missing+=("${cmd}")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        breaking "Missing required tools: ${missing[*]}"
        echo ""
        error "Run: sudo ./scripts/install-deps.sh"
        return 1
    fi

    return 0
}

# ------------------------------------------------------------------------------
# Quick Library Check
# ------------------------------------------------------------------------------
# What: Verify required libraries are available via pkg-config
# Why: Better error message than CMake's output
# Edge cases: Missing libraries point to install-deps.sh
# User impact: Clear guidance on what's missing
#

check_libraries() {
    local -a required=(sqlite3 ncurses libcurl libsystemd libcjson libgpiod)
    local missing=()

    for lib in "${required[@]}"; do
        if ! pkg-config --exists "${lib}" 2>/dev/null; then
            missing+=("${lib}")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        breaking "Missing libraries: ${missing[*]}"
        echo ""
        error "Run: sudo ./scripts/install-deps.sh"
        return 1
    fi

    # Special check for libgpiod version
    local gpiod_ver
    gpiod_ver="$(pkg-config --modversion libgpiod 2>/dev/null || echo "0")"
    case "${gpiod_ver}" in
        1.*)
            # Good
            ;;
        *)
            breaking "libgpiod ${gpiod_ver} found but v1.x required"
            echo ""
            error "Run: sudo ./scripts/install-deps.sh"
            error "This will build libgpiod v1 from source"
            return 1
            ;;
    esac

    return 0
}

# ------------------------------------------------------------------------------
# CMake Configuration
# ------------------------------------------------------------------------------
# What: Run cmake to configure the build
# Why: Sets up build system with correct options
# Edge cases: Configuration failures are breaking
# User impact: They see CMake output for diagnostics
#

configure_build() {
    local generator
    generator="$(detect_generator)"

    action "Configuring build"
    detail "Generator: ${generator}"
    detail "Build type: ${BUILD_TYPE}"
    detail "Tests: ${BUILD_TESTS}"

    # Build CMake arguments
    local -a cmake_args=(
        -S "${PROJECT_ROOT}"
        -B "${BUILD_DIR}"
        -G "${generator}"
        "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    )

    # Add test option if enabled
    if [[ "${BUILD_TESTS}" == "ON" ]]; then
        cmake_args+=("-DBUILD_TESTS=ON")
    fi

    # Enable ccache if available
    if command -v ccache &>/dev/null; then
        cmake_args+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache")
        cmake_args+=("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
        detail "Using ccache for faster rebuilds"
    fi

    # Run CMake configuration
    echo ""
    if ! cmake "${cmake_args[@]}"; then
        breaking "CMake configuration failed"
        return 1
    fi

    success "Configuration complete"
    return 0
}

# ------------------------------------------------------------------------------
# Build Execution
# ------------------------------------------------------------------------------
# What: Run the actual build
# Why: Compiles the project
# Edge cases: Build failures are breaking
# User impact: They see compiler output
#

run_build() {
    local jobs
    jobs="$(nproc 2>/dev/null || echo 1)"

    action "Building with ${jobs} parallel jobs"

    echo ""
    if ! cmake --build "${BUILD_DIR}" -j"${jobs}"; then
        breaking "Build failed"
        return 1
    fi

    success "Build complete"
    return 0
}

# ------------------------------------------------------------------------------
# Read Project Info from CMake Cache
# ------------------------------------------------------------------------------
# What: Extract project metadata from CMake's cache
# Why: CMakeLists.txt is source of truth, we read from it (via cache)
# Edge cases: Cache might not exist yet (that's okay during configure)
# User impact: None - internal utility
#

read_project_info() {
    local project_name
    local binary_name

    project_name="$(cmake_cache_get "CMAKE_PROJECT_NAME")"

    if [[ -z "${project_name}" ]]; then
        # Cache doesn't exist yet - this is fine during initial configure
        project_name="(will be determined after configure)"
    fi

    echo "${project_name}"
}

# ------------------------------------------------------------------------------
# Report Build Output
# ------------------------------------------------------------------------------
# What: Find and display built binaries
# Why: User wants to know what was built and where
# Edge cases: No binaries found (unusual but possible)
# User impact: Clear indication of what to run
#

report_build_output() {
    info "Build outputs:"

    # Read project name from cache (now it should exist)
    local project_name
    project_name="$(cmake_cache_get "CMAKE_PROJECT_NAME")"

    if [[ -z "${project_name}" ]]; then
        warn "Could not read project name from CMake cache"
        project_name="profinet-monitor"  # Fallback
    fi

    local binary="${BUILD_DIR}/${project_name}"

    if [[ -x "${binary}" ]]; then
        local size
        size="$(du -h "${binary}" | cut -f1)"
        detail "${binary} (${size})"
        echo ""
        echo "Run: ${binary}"
    else
        # Search for any executable
        local found
        found="$(find "${BUILD_DIR}" -maxdepth 1 -type f -executable 2>/dev/null | head -1)"
        if [[ -n "${found}" ]]; then
            local size
            size="$(du -h "${found}" | cut -f1)"
            detail "${found} (${size})"
            echo ""
            echo "Run: ${found}"
        else
            warn "No binary found in build output"
        fi
    fi

    # Report test binary if built
    if [[ "${BUILD_TESTS}" == "ON" ]]; then
        local test_binary="${BUILD_DIR}/run_tests"
        if [[ -x "${test_binary}" ]]; then
            detail "${test_binary} (tests)"
            echo ""
            echo "Run tests: ${test_binary}"
            echo "Or: ctest --test-dir ${BUILD_DIR}"
        fi
    fi
}

# ------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------

main() {
    echo "========================================"
    echo "  Water-Treat Build"
    echo "========================================"
    echo ""

    parse_args "$@"

    # Show project info
    detail "Project root: ${PROJECT_ROOT}"
    detail "Build directory: ${BUILD_DIR}"
    detail "Architecture: ${ARCH}"
    echo ""

    # Check prerequisites
    header "Prerequisites"
    check_build_tools || exit 1
    check_libraries || exit 1
    success "Prerequisites satisfied"

    # Clean if requested
    if [[ "${DO_CLEAN}" == "true" ]]; then
        if [[ -d "${BUILD_DIR}" ]]; then
            action "Cleaning build directory"
            rm -rf "${BUILD_DIR}"
        fi
    fi

    # Configure
    header "Configure"
    configure_build || exit 1

    # Build
    header "Build"
    run_build || exit 1

    # Report
    header "Results"
    report_build_output

    echo ""
    echo "========================================"
    success "Build successful"
    echo ""
    echo "Next steps:"
    echo "  sudo ./scripts/install.sh  - Install to system"
    echo "  ${BUILD_DIR}/profinet-monitor - Run directly"
    echo "========================================"
}

main "$@"
