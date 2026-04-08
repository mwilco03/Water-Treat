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
    GSDML_INSTALL_DIR="/usr/share/${PROJECT_NAME}/gsd"
    FIRMWARE_SHARE_DIR="/usr/local/share/${PROJECT_NAME}/firmware"

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
            # Check if user is already in this group
            local user_groups
            user_groups="$(groups "${user}" 2>/dev/null)" || user_groups=""
            if [[ -z "${user_groups}" ]] || ! echo "${user_groups}" | grep -qw "${group}"; then
                if ! usermod -aG "${group}" "${user}" 2>/dev/null; then
                    non_breaking "Could not add ${user} to group ${group}"
                fi
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
            if ! usermod -aG "${group}" "${user}" 2>/dev/null; then
                non_breaking "Could not add ${user} to group ${group}"
            fi
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

    # Data directory (service-owned, group-writable for admin access)
    install -d -m 770 "${VAR_DIR}" || {
        breaking "Failed to create ${VAR_DIR}"
        return 1
    }
    # Try to set ownership, but don't fail if user doesn't exist
    chown "${PROJECT_NAME}:${PROJECT_NAME}" "${VAR_DIR}" 2>/dev/null || \
        chown root:root "${VAR_DIR}"
    detail "${VAR_DIR}"

    # Log directory (service-owned, group-writable for admin access)
    install -d -m 770 "${LOG_DIR}" || {
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

    # Enable the service so it survives reboot. New RTUs deployed to the field
    # need to come back online after a power cycle without a tech doing
    # `systemctl enable` by hand. Set INSTALL_NO_ENABLE=1 to opt out.
    if [[ "${INSTALL_NO_ENABLE:-0}" != "1" ]]; then
        if systemctl enable "${PROJECT_NAME}.service" >/dev/null 2>&1; then
            detail "Enabled ${PROJECT_NAME}.service for boot"
        else
            non_breaking "systemctl enable ${PROJECT_NAME}.service failed (run manually)"
        fi
    else
        detail "Skipping systemctl enable (INSTALL_NO_ENABLE=1)"
    fi

    success "Service installed"
    return 0
}

# ------------------------------------------------------------------------------
# Install Data Files (GSDML, env, config example)
# ------------------------------------------------------------------------------
# What: Copy non-binary runtime artifacts the daemon needs to find at startup
# Why: The binary searches /usr/share/${PROJECT_NAME}/gsd/ for the GSDML XML and
#      /etc/${PROJECT_NAME}/water-treat.env for the systemd environment overrides.
#      Without these, GSDML HTTP fetch returns 404 and the operator has no
#      discoverable sample config on the deployed system.
# Edge cases:
#   - GSDML may be missing from source tree (failure)
#   - water-treat.env may already exist (preserve operator edits)
#   - water-treat.conf.example always overwrites (it is a reference, not config)
#
install_data_files() {
    action "Installing data files (GSDML, config samples)"

    # 1. GSDML XML — required for /gsdml HTTP endpoint and controller discovery
    local gsdml_src
    gsdml_src="$(find "${PROJECT_ROOT}/gsd" -maxdepth 1 -name 'GSDML-*.xml' -print -quit 2>/dev/null)"
    if [[ -z "${gsdml_src}" ]]; then
        breaking "GSDML file not found in ${PROJECT_ROOT}/gsd/"
        return 1
    fi

    install -d -m 755 "${GSDML_INSTALL_DIR}" || {
        breaking "Failed to create ${GSDML_INSTALL_DIR}"
        return 1
    }
    install -m 644 "${gsdml_src}" "${GSDML_INSTALL_DIR}/" || {
        breaking "Failed to install GSDML to ${GSDML_INSTALL_DIR}"
        return 1
    }
    detail "${GSDML_INSTALL_DIR}/$(basename "${gsdml_src}")"

    # 2. Environment file — referenced by the systemd unit (EnvironmentFile=-...).
    #    Do NOT overwrite an existing file; operators may have set WT_HTTP_PORT etc.
    local env_src="${PROJECT_ROOT}/etc/water-treat.env"
    local env_dst="${ETC_DIR}/water-treat.env"
    if [[ -f "${env_src}" ]]; then
        if [[ -f "${env_dst}" ]]; then
            detail "${env_dst} (preserved existing)"
        else
            install -m 644 "${env_src}" "${env_dst}" || {
                non_breaking "Failed to install ${env_dst}"
            }
            detail "${env_dst}"
        fi
    fi

    # 3. Sample config — reference for the operator. Always refresh on install
    #    so the example matches the installed binary's expected schema.
    local conf_src="${PROJECT_ROOT}/etc/water-treat.conf.example"
    local conf_dst="${ETC_DIR}/water-treat.conf.example"
    if [[ -f "${conf_src}" ]]; then
        install -m 644 "${conf_src}" "${conf_dst}" || {
            non_breaking "Failed to install ${conf_dst}"
        }
        detail "${conf_dst}"
    fi

    success "Data files installed"
    return 0
}

# ------------------------------------------------------------------------------
# Install Kernel Module Auto-Load Configuration
# ------------------------------------------------------------------------------
# What: Write /etc/modules-load.d/water-treat.conf so the kernel modules
#       water-treat sensor/actuator drivers depend on are loaded at boot.
#
# Why: HIL recon proved that on a fresh deploy of Armbian / Debian / Raspberry
#      Pi OS, the spidev, w1-gpio, w1-therm, and i2c-dev modules are NOT
#      loaded by default. Drivers that need them (MCP3008 SPI ADC, DS18B20
#      1-wire temperature, ADS1115/BME280 I2C) silently fail at sensor-add
#      time with cryptic open() errors. Operators have no in-product hint
#      that a kmod is missing.
#
# Cross-platform: this is needed on Odroid-XU4 (Exynos 5422), Le Potato
#       (Amlogic S905X), Raspberry Pi (BCM2835/2711), and any libgpiod-based
#       Linux. Different boards expose different default loaded modules and
#       it's safer to declare what we need than to assume.
#
# Edge cases:
#   - Modules already loaded → modules-load.d is a no-op
#   - Module unavailable on this kernel → systemd-modules-load.service logs
#     a warning at boot but does not block water-treat startup
#   - Existing /etc/modules-load.d/water-treat.conf → overwritten (we own it)
#
install_kernel_modules_conf() {
    action "Configuring kernel module auto-load"

    local modload_dir="/etc/modules-load.d"
    local modload_file="${modload_dir}/${PROJECT_NAME}.conf"

    install -d -m 755 "${modload_dir}" || {
        non_breaking "Failed to create ${modload_dir}"
        return 0
    }

    cat > "${modload_file}" << 'KMODEOF'
# Kernel modules required by water-treat sensor and actuator drivers.
# Installed by water-treat install.sh. Loaded at boot by systemd-modules-load.
#
# spidev    : MCP3008 (SPI ADC) — opens /dev/spidev*.0
# i2c-dev   : ADS1115, BME280, TCS34725, etc — opens /dev/i2c-N
# w1-gpio   : DS18B20 1-wire bit-banging master driver
# w1-therm  : DS18B20 temperature device class (consumes w1-gpio)
#
# i2c-dev is built-in on most kernels but listing it is harmless.
# If a module isn't available on this kernel, systemd-modules-load
# will log a warning and continue — it does NOT block water-treat.
spidev
i2c-dev
w1-gpio
w1-therm
KMODEOF

    chmod 644 "${modload_file}" || true
    detail "${modload_file}"

    # Also try to load them now so the operator doesn't have to reboot
    # to test sensors that depend on these modules. Failures are non-fatal:
    # the modules-load.d entry will pick up at the next boot regardless.
    if command -v modprobe &>/dev/null; then
        local loaded=0 failed=0
        for mod in spidev w1-gpio w1-therm; do
            if modprobe "${mod}" 2>/dev/null; then
                loaded=$((loaded + 1))
            else
                failed=$((failed + 1))
            fi
        done
        if [[ ${failed} -gt 0 ]]; then
            non_breaking "Loaded ${loaded}/3 kernel modules now (${failed} failed); will retry at boot"
        else
            detail "Loaded ${loaded}/3 kernel modules now"
        fi
    fi

    success "Kernel module auto-load configured"
    return 0
}

# ------------------------------------------------------------------------------
# Install set_network_parameters Helper for p-net DCP Set
# ------------------------------------------------------------------------------
# What: Ship a no-op /usr/local/bin/set_network_parameters script that p-net
#       PNAL invokes when the controller sends a DCP Set frame to change
#       the device's IP address or station name.
#
# Why: p-net PNAL Linux OSAL shells out to a script of this name on every
#      DCP Set frame. Without the script, p-net logs a fork-failed warning
#      at startup and silently rejects subsequent DCP Set requests from the
#      controller. The PNAL fork is hardwired into p-net itself; we can
#      either patch p-net (upstream change) or ship the script (deploy fix).
#      Shipping the script is the smaller change.
#
# Behavior: This script logs the requested change to the journal and exits 0.
#       It does NOT actually reconfigure the network — that path is owned by
#       the bootstrap-time station name detection from MAC. We accept the
#       p-net DCP Set request to keep the controller happy, but the actual
#       station identity comes from the MAC-derived name.
#
#       Operators who WANT the controller to be able to change the station
#       name dynamically can replace this script with one that calls
#       hostnamectl + writes water-treat.conf + restarts the daemon.
#
install_network_helper() {
    action "Installing set_network_parameters helper"

    local helper="${BIN_DIR}/set_network_parameters"
    cat > "${helper}" << 'HELPEREOF'
#!/bin/sh
# set_network_parameters — invoked by p-net PNAL on DCP Set frames.
#
# Arguments (per p-net pnal_linux.c):
#   $1 = network interface (e.g. "enx001e0639ec3b")
#   $2 = IP address       (dotted decimal)
#   $3 = netmask          (dotted decimal)
#   $4 = gateway          (dotted decimal)
#   $5 = host/station name
#   $6 = permanent flag   ("1" for nv-store, "0" for runtime-only)
#
# Default behavior: log to journal and exit 0. The water-treat daemon
# derives its station name from the interface MAC at startup; this script
# satisfies p-net's PNAL contract without actually reconfiguring the
# network. To opt in to dynamic reconfiguration, replace this script with
# one that calls `ip addr`, `hostnamectl`, etc.
logger -t set_network_parameters \
  "DCP Set received: iface=$1 ip=$2 mask=$3 gw=$4 station=$5 permanent=$6"
exit 0
HELPEREOF

    chmod 755 "${helper}" || {
        non_breaking "Failed to chmod ${helper}"
        return 0
    }
    detail "${helper}"
    success "set_network_parameters helper installed"
    return 0
}

# ------------------------------------------------------------------------------
# Build and Install RP2040 LED Controller Firmware
# ------------------------------------------------------------------------------
# What: Build the RP2040 LED controller firmware if Pico SDK is available
# Why: Provides platform-agnostic LED control via USB
# Edge cases: SDK not installed (skip with warning), ARM toolchain missing (skip)
# User impact: Firmware .uf2 file installed to share directory for easy flashing
#

FIRMWARE_DIR="${PROJECT_ROOT}/firmware/rp2040_led_controller"
# NOTE: FIRMWARE_SHARE_DIR is set inside read_project_name() because it depends on
# PROJECT_NAME, which is read from the CMake cache at runtime. Defining it at module
# scope here would expand PROJECT_NAME="" and produce "/usr/local/share//firmware".

find_pico_sdk() {
    # Check explicit environment variable first
    if [[ -n "${PICO_SDK_PATH:-}" && -d "${PICO_SDK_PATH}" ]]; then
        echo "${PICO_SDK_PATH}"
        return 0
    fi

    # Check common locations
    local locations=(
        "${HOME}/pico/pico-sdk"
        "/opt/pico-sdk"
        "/usr/local/pico-sdk"
        "/usr/share/pico-sdk"
    )

    for loc in "${locations[@]}"; do
        if [[ -d "${loc}" && -f "${loc}/CMakeLists.txt" ]]; then
            echo "${loc}"
            return 0
        fi
    done

    # Check if running as root - try the user's home
    if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" ]]; then
        local user_home
        user_home="$(getent passwd "${SUDO_USER}" | cut -d: -f6)"
        if [[ -d "${user_home}/pico/pico-sdk" ]]; then
            echo "${user_home}/pico/pico-sdk"
            return 0
        fi
    fi

    return 1
}

build_rp2040_firmware() {
    # Check if firmware source exists
    if [[ ! -d "${FIRMWARE_DIR}" ]]; then
        detail "RP2040 firmware source not found, skipping"
        return 0
    fi

    action "Checking RP2040 firmware build requirements"

    # Check for ARM toolchain
    if ! command -v arm-none-eabi-gcc &>/dev/null; then
        non_breaking "ARM toolchain not found - skipping RP2040 firmware build"
        detail "Install with: sudo apt install gcc-arm-none-eabi"
        return 0
    fi
    detail "ARM toolchain: $(arm-none-eabi-gcc --version | head -1)"

    # Check for Pico SDK
    local sdk_path
    if ! sdk_path="$(find_pico_sdk)"; then
        non_breaking "Pico SDK not found - skipping RP2040 firmware build"
        detail "Install SDK to ~/pico/pico-sdk or set PICO_SDK_PATH"
        return 0
    fi
    detail "Pico SDK: ${sdk_path}"

    action "Building RP2040 LED controller firmware"

    local fw_build_dir="${FIRMWARE_DIR}/build"

    # Clean and create build directory
    rm -rf "${fw_build_dir}"
    mkdir -p "${fw_build_dir}"

    # Configure
    if ! (cd "${fw_build_dir}" && PICO_SDK_PATH="${sdk_path}" cmake .. 2>&1); then
        non_breaking "RP2040 firmware cmake failed"
        return 0
    fi

    # Build
    if ! (cd "${fw_build_dir}" && make -j"$(nproc)" 2>&1); then
        non_breaking "RP2040 firmware build failed"
        return 0
    fi

    # Check for output
    local uf2_file="${fw_build_dir}/led_controller.uf2"
    if [[ ! -f "${uf2_file}" ]]; then
        non_breaking "RP2040 firmware .uf2 not found after build"
        return 0
    fi

    # Install the firmware file
    action "Installing RP2040 firmware"
    install -d -m 755 "${FIRMWARE_SHARE_DIR}"
    install -m 644 "${uf2_file}" "${FIRMWARE_SHARE_DIR}/led_controller.uf2"
    detail "${FIRMWARE_SHARE_DIR}/led_controller.uf2"

    success "RP2040 firmware built and installed"
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
FIRMWARE_DIR=${FIRMWARE_SHARE_DIR}
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
    install_data_files || exit 1
    install_kernel_modules_conf || exit 1
    install_network_helper || exit 1

    # Optional: Build and install RP2040 firmware
    header "RP2040 LED Controller (Optional)"
    build_rp2040_firmware

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

        # Show firmware flashing instructions if firmware was built
        if [[ -f "${FIRMWARE_SHARE_DIR}/led_controller.uf2" ]]; then
            echo "RP2040 LED Controller:"
            echo "  1. Hold BOOTSEL button on RP2040"
            echo "  2. Connect USB while holding button"
            echo "  3. Release button, then run:"
            echo "     sudo mount /dev/sda1 /mnt"
            echo "     sudo cp ${FIRMWARE_SHARE_DIR}/led_controller.uf2 /mnt/"
            echo "     sync"
            echo ""
        fi

        echo "Uninstall:"
        echo "  sudo ./scripts/uninstall.sh"
        echo "========================================"
    else
        exit 1
    fi
}

main "$@"
