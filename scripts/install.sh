#!/usr/bin/env bash
#
# install.sh - Install Water-Treat RTU as a system service
#
# What: Installs the built binary, creates service user, installs systemd service
# Why: Production deployment needs proper system integration
#
# Usage:
#   sudo ./scripts/install.sh
#
# Requirements:
#   - Root privileges
#   - Binary must be built first (./scripts/build.sh)
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
# What: Installation paths following FHS (Filesystem Hierarchy Standard)
# Why: Standard locations that systemd, logrotate, etc. expect
# Edge cases: All paths derived from project name for consistency
#

BUILD_DIR="${PROJECT_ROOT}/build"
BIN_DIR="/usr/local/bin"

# These get set after we read project name from CMake
PROJECT_NAME=""
ETC_DIR=""
VAR_DIR=""
LOG_DIR=""
SERVICE_FILE=""
MANIFEST_FILE=""

# ------------------------------------------------------------------------------
# Root Check
# ------------------------------------------------------------------------------
# What: Verify script is running as root
# Why: System installation requires root privileges
# Edge cases: Running without sudo is a breaking error
# User impact: Clear message to re-run with sudo
#

check_root() {
    if [[ $EUID -ne 0 ]]; then
        breaking "Root privileges required"
        echo ""
        error "This script installs system files and requires root."
        error "Please run: sudo $0"
        echo ""
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------------------
# Read Project Name from CMake Cache
# ------------------------------------------------------------------------------
# What: Get the project name from CMake's cache file
# Why: CMakeLists.txt is the source of truth, not hardcoded values
# Edge cases: Missing cache means build hasn't run yet
# User impact: Error points them to run build.sh first
#

read_project_name() {
    local cache_file="${BUILD_DIR}/CMakeCache.txt"

    if [[ ! -f "${cache_file}" ]]; then
        breaking "CMake cache not found: ${cache_file}"
        echo ""
        error "You need to build the project first."
        error "Run: ./scripts/build.sh"
        return 1
    fi

    PROJECT_NAME="$(cmake_cache_get "CMAKE_PROJECT_NAME")"

    if [[ -z "${PROJECT_NAME}" ]]; then
        breaking "Could not read CMAKE_PROJECT_NAME from cache"
        return 1
    fi

    detail "Project name: ${PROJECT_NAME}"

    # Now set all derived paths
    ETC_DIR="/etc/${PROJECT_NAME}"
    VAR_DIR="/var/lib/${PROJECT_NAME}"
    LOG_DIR="/var/log/${PROJECT_NAME}"
    SERVICE_FILE="/etc/systemd/system/${PROJECT_NAME}.service"
    MANIFEST_FILE="${ETC_DIR}/.install-manifest"

    return 0
}

# ------------------------------------------------------------------------------
# Locate Binary
# ------------------------------------------------------------------------------
# What: Find the built binary in the build directory
# Why: Need to know what to install
# Edge cases: Binary not found means build failed or wasn't run
# User impact: Error points them to run build.sh
#

locate_binary() {
    local binary="${BUILD_DIR}/${PROJECT_NAME}"

    if [[ ! -x "${binary}" ]]; then
        # Maybe it's not directly in build/
        binary="$(find "${BUILD_DIR}" -maxdepth 2 -type f -name "${PROJECT_NAME}" -executable 2>/dev/null | head -1)"
    fi

    if [[ -z "${binary}" || ! -x "${binary}" ]]; then
        breaking "Binary '${PROJECT_NAME}' not found in ${BUILD_DIR}"
        echo ""
        error "Build the project first: ./scripts/build.sh"
        return 1
    fi

    echo "${binary}"
}

# ------------------------------------------------------------------------------
# Create Service User
# ------------------------------------------------------------------------------
# What: Create a dedicated system user for running the service
# Why: Security - don't run as root if possible (though GPIO may require it)
#      Service file currently uses root, but user exists for future use
# Edge cases: User already exists (that's fine, skip creation)
# User impact: User sees "creating user" or "user exists"
#

create_service_user() {
    local user="${PROJECT_NAME}"
    local groups_to_add=()

    # Check which hardware groups exist
    for group in gpio i2c spi dialout; do
        if getent group "${group}" &>/dev/null; then
            groups_to_add+=("${group}")
        fi
    done

    if id "${user}" &>/dev/null; then
        detail "User '${user}' already exists"

        # Add to any missing groups
        for group in "${groups_to_add[@]}"; do
            if ! groups "${user}" 2>/dev/null | grep -qw "${group}"; then
                usermod -aG "${group}" "${user}" 2>/dev/null || true
            fi
        done
    else
        action "Creating service user: ${user}"

        useradd --system --no-create-home --shell /usr/sbin/nologin "${user}" || {
            non_breaking "Could not create user '${user}'"
            # Continue - the service will run as root anyway per service file
            return 0
        }

        # Add to hardware groups
        for group in "${groups_to_add[@]}"; do
            usermod -aG "${group}" "${user}" 2>/dev/null || true
        done

        if [[ ${#groups_to_add[@]} -gt 0 ]]; then
            detail "Added to groups: ${groups_to_add[*]}"
        fi

        success "User created"
    fi

    return 0
}

# ------------------------------------------------------------------------------
# Create Directories
# ------------------------------------------------------------------------------
# What: Create config, data, and log directories
# Why: Service needs places to store configuration and runtime data
# Edge cases: Directories might already exist (that's fine)
# User impact: They see directories being created
#

create_directories() {
    action "Creating directories"

    # Config directory (root-owned, world-readable)
    install -d -m 755 "${ETC_DIR}" || {
        breaking "Failed to create ${ETC_DIR}"
        return 1
    }
    detail "${ETC_DIR}"

    # Data directory (service-owned)
    install -d -m 750 "${VAR_DIR}" || {
        breaking "Failed to create ${VAR_DIR}"
        return 1
    }
    # Try to set ownership, but don't fail if user doesn't exist
    chown "${PROJECT_NAME}:${PROJECT_NAME}" "${VAR_DIR}" 2>/dev/null || \
        chown root:root "${VAR_DIR}"
    detail "${VAR_DIR}"

    # Log directory (service-owned)
    install -d -m 750 "${LOG_DIR}" || {
        breaking "Failed to create ${LOG_DIR}"
        return 1
    }
    chown "${PROJECT_NAME}:${PROJECT_NAME}" "${LOG_DIR}" 2>/dev/null || \
        chown root:root "${LOG_DIR}"
    detail "${LOG_DIR}"

    success "Directories created"
    return 0
}

# ------------------------------------------------------------------------------
# Install Binary
# ------------------------------------------------------------------------------
# What: Copy the built binary to /usr/local/bin
# Why: Standard location for locally-installed binaries
# Edge cases: Overwriting existing binary (that's expected for upgrades)
# User impact: They see where the binary is installed
#

install_binary() {
    local source_binary="$1"
    local dest_binary="${BIN_DIR}/${PROJECT_NAME}"

    action "Installing binary"

    install -m 755 "${source_binary}" "${dest_binary}" || {
        breaking "Failed to install binary to ${dest_binary}"
        return 1
    }

    detail "${dest_binary}"
    success "Binary installed"
    return 0
}

# ------------------------------------------------------------------------------
# Install Service File
# ------------------------------------------------------------------------------
# What: Copy the systemd service file from repo to /etc/systemd/system
# Why: Enables systemctl start/stop/enable functionality
#      We copy rather than generate - repo is source of truth
# Edge cases: Service file might not exist in repo
# User impact: They get a working systemd service
#

install_service_file() {
    local source_service="${PROJECT_ROOT}/systemd/${PROJECT_NAME}.service"

    if [[ ! -f "${source_service}" ]]; then
        breaking "Service file not found: ${source_service}"
        echo ""
        error "The systemd service file should exist in the repository."
        error "Expected: systemd/${PROJECT_NAME}.service"
        return 1
    fi

    action "Installing systemd service"

    install -m 644 "${source_service}" "${SERVICE_FILE}" || {
        breaking "Failed to install service file"
        return 1
    }

    detail "${SERVICE_FILE}"

    # Reload systemd to pick up the new service
    systemctl daemon-reload || {
        non_breaking "systemctl daemon-reload failed"
    }

    success "Service installed"
    return 0
}

# ------------------------------------------------------------------------------
# Write Install Manifest
# ------------------------------------------------------------------------------
# What: Record what was installed and where
# Why: uninstall.sh reads this to know what to remove
#      Changes in install.sh automatically flow to uninstall
# Edge cases: Manifest directory might not exist (but we created it)
# User impact: Enables clean uninstall later
#

write_manifest() {
    action "Writing install manifest"

    cat > "${MANIFEST_FILE}" <<EOF
# Water-Treat Install Manifest
# Generated: $(date -Iseconds)
# This file is read by uninstall.sh to cleanly remove the installation
#
PROJECT_NAME=${PROJECT_NAME}
BINARY=${BIN_DIR}/${PROJECT_NAME}
SERVICE_FILE=${SERVICE_FILE}
ETC_DIR=${ETC_DIR}
VAR_DIR=${VAR_DIR}
LOG_DIR=${LOG_DIR}
USER=${PROJECT_NAME}
EOF

    detail "${MANIFEST_FILE}"
    success "Manifest written"
    return 0
}

# ------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------

main() {
    echo "========================================"
    echo "  Water-Treat System Installer"
    echo "========================================"
    echo ""

    # Must be root
    check_root || exit 1

    # Get project info from CMake
    header "Discovery"
    read_project_name || exit 1

    # Find the binary
    local binary
    binary="$(locate_binary)" || exit 1
    detail "Binary: ${binary}"

    echo ""
    info "Installation targets:"
    detail "Binary:  ${BIN_DIR}/${PROJECT_NAME}"
    detail "Config:  ${ETC_DIR}/"
    detail "Data:    ${VAR_DIR}/"
    detail "Logs:    ${LOG_DIR}/"
    detail "Service: ${SERVICE_FILE}"

    # Install
    header "Installation"
    create_service_user
    create_directories || exit 1
    install_binary "${binary}" || exit 1
    install_service_file || exit 1
    write_manifest || exit 1

    # Summary
    if summary; then
        echo ""
        echo "========================================"
        success "Installation complete!"
        echo ""
        echo "Commands:"
        echo "  sudo systemctl start ${PROJECT_NAME}   - Start the service"
        echo "  sudo systemctl enable ${PROJECT_NAME}  - Start on boot"
        echo "  sudo systemctl status ${PROJECT_NAME}  - Check status"
        echo "  sudo journalctl -u ${PROJECT_NAME} -f  - View logs"
        echo ""
        echo "Uninstall:"
        echo "  sudo ./scripts/uninstall.sh"
        echo "========================================"
    else
        exit 1
    fi
}

main "$@"
