#!/usr/bin/env bash
#
# bootstrap.sh - Quick start script for Water-Treat RTU
#
# What: Single-command bootstrap that handles deps, build, and optional install
# Why: Simplifies first-time setup, especially for development
#
# Usage:
#   ./scripts/bootstrap.sh              # Build only (user mode)
#   ./scripts/bootstrap.sh --install    # Build + install (requires sudo)
#   ./scripts/bootstrap.sh --check      # Pre-flight check only
#   ./scripts/bootstrap.sh --help       # Show help
#
# This script will:
#   1. Run pre-flight checks (system requirements)
#   2. Install dependencies if needed
#   3. Build the project
#   4. Optionally install (with --install)
#

set -uo pipefail

# ------------------------------------------------------------------------------
# Source shared environment
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
source "${SCRIPT_DIR}/env.sh"

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------

DO_INSTALL=false
CHECK_ONLY=false

for arg in "$@"; do
    case "${arg}" in
        --install|-i)
            DO_INSTALL=true
            ;;
        --check|-c)
            CHECK_ONLY=true
            ;;
        --help|-h)
            cat <<EOF
Usage: $0 [OPTIONS]

Bootstrap Water-Treat RTU development environment.

Options:
  --install, -i   Build and install (requires sudo for install step)
  --check, -c     Run pre-flight checks only, don't build
  --help, -h      Show this help message

Examples:
  $0                    # Check deps and build
  $0 --check            # Just verify system requirements
  $0 --install          # Full setup including installation
  sudo $0 --install     # Same, but run entire script as root

Without --install, the binary is built but not installed system-wide.
You can run it directly from ./build/water-treat for development.
EOF
            exit 0
            ;;
        *)
            warn "Unknown option: ${arg}"
            ;;
    esac
done

# ------------------------------------------------------------------------------
# Pre-flight Checks
# ------------------------------------------------------------------------------
# What: Verify system meets requirements before attempting build
# Why: Fail fast with clear messages instead of cryptic build errors
# Edge cases: Different distros have different package names
#

preflight_check() {
    header "Pre-flight Checks"
    local failed=false

    # Check for required build tools
    info "Checking build tools..."

    # GCC or Clang
    if command -v gcc &>/dev/null; then
        detail "gcc: $(gcc --version | head -1)"
    elif command -v clang &>/dev/null; then
        detail "clang: $(clang --version | head -1)"
    else
        breaking "No C compiler found (need gcc or clang)"
        failed=true
    fi

    # CMake
    if command -v cmake &>/dev/null; then
        detail "cmake: $(cmake --version | head -1)"
    else
        breaking "CMake not found"
        failed=true
    fi

    # Make or Ninja
    if command -v make &>/dev/null; then
        detail "make: $(make --version | head -1)"
    elif command -v ninja &>/dev/null; then
        detail "ninja: $(ninja --version)"
    else
        breaking "No build system found (need make or ninja)"
        failed=true
    fi

    # pkg-config
    if command -v pkg-config &>/dev/null; then
        detail "pkg-config: $(pkg-config --version)"
    else
        non_breaking "pkg-config not found (may cause issues finding libraries)"
    fi

    # Check for required libraries
    info "Checking library dependencies..."

    # SQLite3
    if pkg-config --exists sqlite3 2>/dev/null; then
        detail "sqlite3: $(pkg-config --modversion sqlite3)"
    elif [[ -f /usr/include/sqlite3.h ]]; then
        detail "sqlite3: found (no pkg-config)"
    else
        breaking "SQLite3 development files not found"
        failed=true
    fi

    # ncurses
    if pkg-config --exists ncurses 2>/dev/null || pkg-config --exists ncursesw 2>/dev/null; then
        local ncver
        ncver=$(pkg-config --modversion ncursesw 2>/dev/null || pkg-config --modversion ncurses 2>/dev/null)
        detail "ncurses: ${ncver}"
    elif [[ -f /usr/include/ncurses.h ]] || [[ -f /usr/include/ncursesw/ncurses.h ]]; then
        detail "ncurses: found (no pkg-config)"
    else
        breaking "ncurses development files not found"
        failed=true
    fi

    # pthreads (usually part of libc on Linux)
    if [[ -f /usr/include/pthread.h ]]; then
        detail "pthreads: available"
    else
        breaking "pthreads not found"
        failed=true
    fi

    # Optional: libgpiod (for GPIO support on embedded)
    if pkg-config --exists libgpiod 2>/dev/null; then
        local gpiover
        gpiover=$(pkg-config --modversion libgpiod)
        detail "libgpiod: ${gpiover}"
        # Check for v1 vs v2 API
        if [[ "${gpiover}" == 1.* ]]; then
            detail "  (v1 API - compatible)"
        elif [[ "${gpiover}" == 2.* ]]; then
            non_breaking "libgpiod v2 detected - may need v1 API compatibility"
        fi
    else
        non_breaking "libgpiod not found (GPIO features will be disabled)"
    fi

    # Check architecture
    info "Checking system..."
    detail "Architecture: ${ARCH} (${ARCH_TRIPLET:-unknown triplet})"
    detail "OS: $(uname -s) $(uname -r)"

    # Check available disk space (need at least 100MB for build)
    local avail_kb
    avail_kb=$(df -k "${PROJECT_ROOT}" | tail -1 | awk '{print $4}')
    if [[ ${avail_kb} -lt 102400 ]]; then
        non_breaking "Low disk space: $((avail_kb / 1024))MB available"
    else
        detail "Disk space: $((avail_kb / 1024))MB available"
    fi

    # Check for /run directory (tmpfs for health metrics)
    if [[ -d /run ]] && [[ -w /run ]]; then
        detail "/run (tmpfs): available"
    else
        non_breaking "/run not writable - health metrics may need alternate path"
    fi

    echo ""
    if [[ "${failed}" == "true" ]]; then
        error "Pre-flight checks failed"
        echo ""
        echo "Install missing dependencies:"
        echo "  Debian/Ubuntu: sudo apt install build-essential cmake libsqlite3-dev libncurses-dev"
        echo "  Fedora:        sudo dnf install gcc cmake sqlite-devel ncurses-devel"
        echo "  Arch:          sudo pacman -S base-devel cmake sqlite ncurses"
        return 1
    fi

    success "Pre-flight checks passed"
    return 0
}

# ------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------

main() {
    echo "=========================================="
    echo "  Water-Treat RTU Bootstrap"
    echo "=========================================="
    echo ""

    # Run pre-flight checks
    if ! preflight_check; then
        exit 1
    fi

    if [[ "${CHECK_ONLY}" == "true" ]]; then
        echo ""
        success "System ready for build"
        exit 0
    fi

    # Check if deps need to be installed
    header "Dependencies"

    if [[ -x "${SCRIPT_DIR}/install-deps.sh" ]]; then
        info "Running dependency installer..."
        if ! "${SCRIPT_DIR}/install-deps.sh"; then
            error "Dependency installation failed"
            exit 1
        fi
    else
        warn "install-deps.sh not found, skipping dependency check"
    fi

    # Build
    header "Build"

    if [[ -x "${SCRIPT_DIR}/build.sh" ]]; then
        info "Building project..."
        if ! "${SCRIPT_DIR}/build.sh"; then
            error "Build failed"
            exit 1
        fi
    else
        # Fallback: manual CMake build
        info "Building with CMake..."
        mkdir -p "${PROJECT_ROOT}/build"
        cd "${PROJECT_ROOT}/build"
        cmake .. -DCMAKE_BUILD_TYPE=Release
        make -j"$(nproc)"
    fi

    # Install if requested
    if [[ "${DO_INSTALL}" == "true" ]]; then
        header "Installation"

        if [[ $EUID -ne 0 ]]; then
            warn "Installation requires root privileges"
            info "Running: sudo ${SCRIPT_DIR}/install.sh"
            sudo "${SCRIPT_DIR}/install.sh"
        else
            "${SCRIPT_DIR}/install.sh"
        fi
    fi

    # Summary
    echo ""
    echo "=========================================="
    echo "  Bootstrap Complete"
    echo "=========================================="
    echo ""

    if [[ "${DO_INSTALL}" == "true" ]]; then
        success "Water-Treat RTU installed"
        echo ""
        echo "Start the service:"
        echo "  sudo systemctl start water-treat"
        echo ""
        echo "Or run interactively:"
        echo "  sudo water-treat"
    else
        success "Water-Treat RTU built successfully"
        echo ""
        echo "Run directly (development):"
        echo "  ./build/water-treat"
        echo ""
        echo "Or install system-wide:"
        echo "  sudo ./scripts/install.sh"
        echo "  # or: $0 --install"
    fi
}

main "$@"
