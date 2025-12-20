#!/usr/bin/env bash
#
# install-deps.sh - Install build dependencies for Water-Treat RTU
#
# What: Installs all packages needed to build the Water-Treat firmware
# Why: Automates the manual apt-get commands from INSTALL.md
#      Handles libgpiod v1/v2 API incompatibility automatically
#
# Usage:
#   sudo ./scripts/install-deps.sh
#
# Requirements:
#   - Root privileges (for package installation)
#   - Debian-based system (tested), or experimental support for Fedora/Arch
#

set -uo pipefail

# ------------------------------------------------------------------------------
# Source shared environment
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
source "${SCRIPT_DIR}/env.sh"

# ------------------------------------------------------------------------------
# Package Manager Detection
# ------------------------------------------------------------------------------
# What: Detect which package manager is available
# Why: Different distros use different package managers
# Edge cases: Unknown package manager causes graceful failure with guidance
# User impact: Script adapts to their system or tells them what's not supported
#

declare -A SYSTEM=()

detect_package_manager() {
    if command -v apt-get &>/dev/null; then
        SYSTEM[pkg_manager]="apt"
        SYSTEM[pkg_manager_name]="APT (Debian/Ubuntu)"
    elif command -v dnf &>/dev/null; then
        SYSTEM[pkg_manager]="dnf"
        SYSTEM[pkg_manager_name]="DNF (Fedora/RHEL 8+)"
    elif command -v yum &>/dev/null; then
        SYSTEM[pkg_manager]="yum"
        SYSTEM[pkg_manager_name]="YUM (RHEL/CentOS 7)"
    elif command -v pacman &>/dev/null; then
        SYSTEM[pkg_manager]="pacman"
        SYSTEM[pkg_manager_name]="Pacman (Arch)"
    else
        SYSTEM[pkg_manager]="unknown"
        SYSTEM[pkg_manager_name]="Unknown"
    fi
}

# ------------------------------------------------------------------------------
# OS Detection
# ------------------------------------------------------------------------------
# What: Read /etc/os-release to identify the distribution
# Why: Informational output and potential distro-specific handling
# Edge cases: Missing os-release is non-breaking (just shows "unknown")
# User impact: They see what system was detected
#

detect_os() {
    if [[ -f /etc/os-release ]]; then
        # shellcheck source=/dev/null
        . /etc/os-release
        SYSTEM[os_id]="${ID:-unknown}"
        SYSTEM[os_version]="${VERSION_ID:-unknown}"
        SYSTEM[os_name]="${PRETTY_NAME:-${ID:-unknown}}"
    else
        SYSTEM[os_id]="unknown"
        SYSTEM[os_version]="unknown"
        SYSTEM[os_name]="Unknown Linux"
        non_breaking "Cannot detect OS version (/etc/os-release missing)"
    fi
}

# ------------------------------------------------------------------------------
# Package Name Mappings
# ------------------------------------------------------------------------------
# What: Maps generic package names to distro-specific names
# Why: Same library has different package names across distros
#      e.g., "sqlite-dev" -> "libsqlite3-dev" (apt) vs "sqlite-devel" (dnf)
# Edge cases: Missing mappings fall back to generic name (may fail)
# User impact: Correct packages get installed regardless of distro
#

# Debian/Ubuntu (APT) - TESTED, VALIDATED
declare -A PKG_MAP_APT=(
    [build-essential]="build-essential"
    [cmake]="cmake"
    [pkg-config]="pkg-config"
    [ninja]="ninja-build"
    [make]="make"
    [git]="git"
    [sqlite-dev]="libsqlite3-dev"
    [sqlite]="sqlite3"
    [ncurses-dev]="libncurses-dev"
    [curl-dev]="libcurl4-openssl-dev"
    [systemd-dev]="libsystemd-dev"
    [cjson-dev]="libcjson-dev"
    [gpiod-dev]="libgpiod-dev"
    [i2c-tools]="i2c-tools"
    # For building libgpiod from source
    [autoconf]="autoconf"
    [automake]="automake"
    [libtool]="libtool"
    [m4]="m4"
    [autoconf-archive]="autoconf-archive"
)

# Fedora/RHEL (DNF) - EXPERIMENTAL, UNTESTED
# Package names based on Fedora naming conventions
declare -A PKG_MAP_DNF=(
    [build-essential]="gcc gcc-c++ make"
    [cmake]="cmake"
    [pkg-config]="pkgconf-pkg-config"
    [ninja]="ninja-build"
    [make]="make"
    [git]="git"
    [sqlite-dev]="sqlite-devel"
    [sqlite]="sqlite"
    [ncurses-dev]="ncurses-devel"
    [curl-dev]="libcurl-devel"
    [systemd-dev]="systemd-devel"
    [cjson-dev]="cjson-devel"
    [gpiod-dev]="libgpiod-devel"
    [i2c-tools]="i2c-tools"
    [autoconf]="autoconf"
    [automake]="automake"
    [libtool]="libtool"
    [m4]="m4"
    [autoconf-archive]="autoconf-archive"
)

# Arch Linux (Pacman) - EXPERIMENTAL, UNTESTED
# Package names based on Arch package database
declare -A PKG_MAP_PACMAN=(
    [build-essential]="base-devel"
    [cmake]="cmake"
    [pkg-config]="pkgconf"
    [ninja]="ninja"
    [make]="make"
    [git]="git"
    [sqlite-dev]="sqlite"
    [sqlite]="sqlite"
    [ncurses-dev]="ncurses"
    [curl-dev]="curl"
    [systemd-dev]="systemd-libs"
    [cjson-dev]="cjson"
    [gpiod-dev]="libgpiod"
    [i2c-tools]="i2c-tools"
    [autoconf]="autoconf"
    [automake]="automake"
    [libtool]="libtool"
    [m4]="m4"
    [autoconf-archive]="autoconf-archive"
)

# ------------------------------------------------------------------------------
# Package Resolution
# ------------------------------------------------------------------------------
# What: Convert generic package names to distro-specific names
# Why: Caller uses generic names, we translate to what apt/dnf/pacman expects
# Edge cases: Unknown package falls back to generic name
# User impact: Transparent - they just see packages installing
#

resolve_package() {
    local generic="$1"
    local resolved=""

    case "${SYSTEM[pkg_manager]}" in
        apt)
            resolved="${PKG_MAP_APT[$generic]:-$generic}"
            ;;
        dnf|yum)
            resolved="${PKG_MAP_DNF[$generic]:-$generic}"
            ;;
        pacman)
            resolved="${PKG_MAP_PACMAN[$generic]:-$generic}"
            ;;
        *)
            resolved="$generic"
            ;;
    esac

    echo "$resolved"
}

# ------------------------------------------------------------------------------
# Package Installation
# ------------------------------------------------------------------------------
# What: Install the required packages using the detected package manager
# Why: Different package managers have different invocations
# Edge cases: apt-get update failures are non-breaking (warn only)
#             Package install failures are breaking (halt)
# User impact: Sees packages being installed, or clear error if failed
#

install_packages() {
    # Generic package list
    local -a required_packages=(
        build-essential
        cmake
        pkg-config
        ninja
        make
        git
        sqlite-dev
        sqlite
        ncurses-dev
        curl-dev
        systemd-dev
        cjson-dev
        gpiod-dev
        i2c-tools
        # For libgpiod source build if needed
        autoconf
        automake
        libtool
        m4
        autoconf-archive
    )

    # Resolve all package names for this distro
    local -a resolved_packages=()
    for pkg in "${required_packages[@]}"; do
        # resolve_package may return multiple packages (e.g., build-essential on dnf)
        for resolved in $(resolve_package "$pkg"); do
            resolved_packages+=("$resolved")
        done
    done

    action "Installing ${#resolved_packages[@]} packages via ${SYSTEM[pkg_manager]}"

    case "${SYSTEM[pkg_manager]}" in
        apt)
            # Update package lists
            if ! apt-get update; then
                non_breaking "apt-get update had issues (continuing anyway)"
            fi

            # Install packages
            # SC2068: We want word splitting here for multiple packages
            if ! DEBIAN_FRONTEND=noninteractive apt-get install -y "${resolved_packages[@]}"; then
                breaking "Package installation failed"
                return 1
            fi
            ;;

        dnf)
            warn "DNF support is EXPERIMENTAL and UNTESTED"
            warn "Package names may need adjustment for your system"
            warn "Please report issues: https://github.com/mwilco03/Water-Treat/issues"
            echo ""

            if ! dnf install -y "${resolved_packages[@]}"; then
                breaking "Package installation failed"
                echo ""
                error "If package names are wrong, please report which packages failed"
                return 1
            fi
            ;;

        yum)
            warn "YUM support is EXPERIMENTAL and UNTESTED"
            warn "Package names may need adjustment for your system"
            warn "Please report issues: https://github.com/mwilco03/Water-Treat/issues"
            echo ""

            if ! yum install -y "${resolved_packages[@]}"; then
                breaking "Package installation failed"
                return 1
            fi
            ;;

        pacman)
            warn "Pacman support is EXPERIMENTAL and UNTESTED"
            warn "Package names may need adjustment for your system"
            warn "Please report issues: https://github.com/mwilco03/Water-Treat/issues"
            echo ""

            if ! pacman -Sy --noconfirm "${resolved_packages[@]}"; then
                breaking "Package installation failed"
                return 1
            fi
            ;;

        *)
            breaking "Unsupported package manager: ${SYSTEM[pkg_manager]}"
            echo ""
            error "Supported package managers:"
            error "  - apt (Debian, Ubuntu, Raspberry Pi OS)"
            error "  - dnf (Fedora, RHEL 8+) - experimental"
            error "  - yum (RHEL 7, CentOS 7) - experimental"
            error "  - pacman (Arch Linux) - experimental"
            echo ""
            error "Please install dependencies manually. See INSTALL.md for package list."
            return 1
            ;;
    esac

    success "Packages installed"
    return 0
}

# ------------------------------------------------------------------------------
# libgpiod Version Detection
# ------------------------------------------------------------------------------
# What: Check if libgpiod is installed and which API version
# Why: This codebase uses libgpiod v1 API. Debian Bookworm+ ships v2.
#      v1 and v2 APIs are incompatible - different function names.
# Edge cases: Not installed, v1 installed, v2 installed
# User impact: Automatic - we handle the mismatch for them
#

declare -A LIBGPIOD=()

detect_libgpiod() {
    info "Checking libgpiod installation..."

    LIBGPIOD[installed]="no"
    LIBGPIOD[version]=""
    LIBGPIOD[api]=""

    # Check via pkg-config
    if ! pkg-config --exists libgpiod 2>/dev/null; then
        detail "libgpiod not found via pkg-config"
        return 0
    fi

    LIBGPIOD[installed]="yes"
    LIBGPIOD[version]="$(pkg-config --modversion libgpiod 2>/dev/null)"

    # Determine API version from version number
    # v1.x uses the v1 API (gpiod_chip_open, gpiod_line_request_*)
    # v2.x uses the v2 API (gpiod_chip_open_by_path, gpiod_line_config_*)
    case "${LIBGPIOD[version]}" in
        1.*)
            LIBGPIOD[api]="v1"
            detail "Found libgpiod ${LIBGPIOD[version]} (v1 API - compatible)"
            ;;
        2.*)
            LIBGPIOD[api]="v2"
            detail "Found libgpiod ${LIBGPIOD[version]} (v2 API - incompatible)"
            ;;
        *)
            LIBGPIOD[api]="unknown"
            detail "Found libgpiod ${LIBGPIOD[version]} (unknown API version)"
            ;;
    esac
}

# ------------------------------------------------------------------------------
# libgpiod v1 Build from Source
# ------------------------------------------------------------------------------
# What: Clone and build libgpiod 1.6.3 from kernel.org
# Why: If system has v2 or nothing, we need v1 for API compatibility
# Edge cases: Network failure, missing build tools, install failure
# User impact: Takes a minute to build, but solves the compatibility issue
#

build_libgpiod_v1() {
    local version="${1:-1.6.3}"
    local prefix="/usr/local"
    local repo="https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git"

    action "Building libgpiod v${version} from source"
    detail "This provides v1 API compatibility"
    detail "Installing to ${prefix}"

    # Create temp directory for build
    local build_dir
    build_dir="$(mktemp -d)" || {
        breaking "Cannot create temporary directory"
        return 1
    }

    # Cleanup on exit from this function
    # shellcheck disable=SC2064
    trap "rm -rf '${build_dir}'" RETURN

    cd "${build_dir}" || {
        breaking "Cannot enter temporary directory"
        return 1
    }

    # Clone
    detail "Cloning repository..."
    if ! git clone --quiet --depth 1 --branch "v${version}" "${repo}" libgpiod 2>/dev/null; then
        # Try without --branch (older git) or full clone
        if ! git clone --quiet "${repo}" libgpiod; then
            breaking "Failed to clone libgpiod repository"
            error "Check network connectivity to git.kernel.org"
            return 1
        fi
        cd libgpiod || return 1
        git checkout --quiet "v${version}" || {
            breaking "Failed to checkout v${version}"
            return 1
        }
    else
        cd libgpiod || return 1
    fi

    # Build
    detail "Running autogen.sh..."
    if ! ./autogen.sh --prefix="${prefix}" >/dev/null 2>&1; then
        breaking "autogen.sh failed"
        error "Ensure autoconf, automake, libtool are installed"
        return 1
    fi

    detail "Compiling..."
    if ! make -j"$(nproc)" >/dev/null 2>&1; then
        breaking "Compilation failed"
        return 1
    fi

    detail "Installing to ${prefix}..."
    if ! make install >/dev/null 2>&1; then
        breaking "Installation failed"
        error "Ensure you have write access to ${prefix}"
        return 1
    fi

    # Update library cache
    ldconfig

    success "libgpiod v${version} installed to ${prefix}"

    # Update PKG_CONFIG_PATH to include /usr/local/lib/pkgconfig
    # This is critical - env.sh was sourced before this directory existed
    if [[ -d "${prefix}/lib/pkgconfig" ]]; then
        if [[ ":${PKG_CONFIG_PATH:-}:" != *":${prefix}/lib/pkgconfig:"* ]]; then
            export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
            detail "Updated PKG_CONFIG_PATH to include ${prefix}/lib/pkgconfig"
        fi
    fi

    # Verify it's now findable
    if pkg-config --exists libgpiod 2>/dev/null; then
        local new_ver
        new_ver="$(pkg-config --modversion libgpiod)"
        detail "Verified: pkg-config finds libgpiod ${new_ver}"
    else
        # This should not happen now, but provide guidance if it does
        non_breaking "pkg-config doesn't find libgpiod after install"
        non_breaking "Manually set: export PKG_CONFIG_PATH=${prefix}/lib/pkgconfig:\$PKG_CONFIG_PATH"
    fi

    return 0
}

# ------------------------------------------------------------------------------
# libgpiod Handling
# ------------------------------------------------------------------------------
# What: Determine what to do based on libgpiod state
# Why: Need v1 API, system might have v2 or nothing
# Edge cases: Already have v1 (do nothing), have v2 (build v1), have nothing (build v1)
# User impact: Automatic resolution of API compatibility
#

handle_libgpiod() {
    detect_libgpiod

    case "${LIBGPIOD[api]}" in
        "v1")
            success "libgpiod v1 API already available"
            return 0
            ;;

        "v2")
            warn "System has libgpiod v2 which uses incompatible API"
            warn "This project requires libgpiod v1 API"
            echo ""
            action "Building libgpiod v1 from source (installs to /usr/local)"

            build_libgpiod_v1 || return 1
            return 0
            ;;

        *)
            action "libgpiod not found - building v1 from source"
            build_libgpiod_v1 || return 1
            return 0
            ;;
    esac
}

# ------------------------------------------------------------------------------
# Verification
# ------------------------------------------------------------------------------
# What: Check that all required tools and libraries are available
# Why: Catch problems now rather than during build
# Edge cases: Missing items are reported but we continue checking all
# User impact: Clear list of what's missing if anything
#

verify_dependencies() {
    info "Verifying dependencies..."

    local all_good=true

    # Required commands
    local -a required_cmds=(cmake pkg-config gcc make)
    for cmd in "${required_cmds[@]}"; do
        if command -v "${cmd}" &>/dev/null; then
            detail "${cmd}: $(command -v "${cmd}")"
        else
            breaking "${cmd} not found in PATH"
            all_good=false
        fi
    done

    # Optional commands (nice to have)
    local -a optional_cmds=(ninja ccache)
    for cmd in "${optional_cmds[@]}"; do
        if command -v "${cmd}" &>/dev/null; then
            detail "${cmd}: $(command -v "${cmd}") (optional)"
        else
            detail "${cmd}: not found (optional - will use fallback)"
        fi
    done

    # Required libraries via pkg-config
    local -a required_libs=(sqlite3 ncurses libcurl libsystemd libcjson libgpiod)
    for lib in "${required_libs[@]}"; do
        if pkg-config --exists "${lib}" 2>/dev/null; then
            local ver
            ver="$(pkg-config --modversion "${lib}" 2>/dev/null || echo "?")"
            detail "${lib}: ${ver}"
        else
            breaking "${lib} not found via pkg-config"
            all_good=false
        fi
    done

    # Special check: libgpiod must be v1
    if pkg-config --exists libgpiod 2>/dev/null; then
        local gpiod_ver
        gpiod_ver="$(pkg-config --modversion libgpiod)"
        case "${gpiod_ver}" in
            1.*)
                # Good
                ;;
            *)
                breaking "libgpiod is ${gpiod_ver} but v1.x is required"
                all_good=false
                ;;
        esac
    fi

    if [[ "${all_good}" == "true" ]]; then
        success "All dependencies verified"
        return 0
    else
        return 1
    fi
}

# ------------------------------------------------------------------------------
# Root Check
# ------------------------------------------------------------------------------
# What: Verify script is running as root
# Why: Package installation requires root privileges
# Edge cases: Running without sudo is a breaking error
# User impact: Clear message to re-run with sudo
#

check_root() {
    if [[ $EUID -ne 0 ]]; then
        breaking "Root privileges required"
        echo ""
        error "This script installs system packages and requires root."
        error "Please run: sudo $0"
        echo ""
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------

main() {
    echo "========================================"
    echo "  Water-Treat Dependency Installer"
    echo "========================================"
    echo ""

    # Must be root
    check_root || exit 1

    # Detect system
    header "System Detection"
    detect_os
    detect_package_manager

    detail "OS: ${SYSTEM[os_name]}"
    detail "Package manager: ${SYSTEM[pkg_manager_name]}"
    detail "Architecture: ${ARCH} (${ARCH_TRIPLET:-unknown triplet})"

    # Check package manager is supported
    if [[ "${SYSTEM[pkg_manager]}" == "unknown" ]]; then
        breaking "No supported package manager found"
        summary
        exit 1
    fi

    # Install packages
    header "Package Installation"
    install_packages || {
        summary
        exit 1
    }

    # Handle libgpiod v1/v2 situation
    header "libgpiod Setup"
    handle_libgpiod || {
        summary
        exit 1
    }

    # Verify everything is in place
    header "Verification"
    verify_dependencies || {
        summary
        exit 1
    }

    # Print summary
    if summary; then
        echo ""
        success "Dependencies are ready!"
        echo ""
        echo "Next step: ./scripts/build.sh"
        echo ""
    else
        exit 1
    fi
}

main "$@"
