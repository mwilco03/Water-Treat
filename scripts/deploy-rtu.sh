#!/bin/bash
# =============================================================================
# deploy-rtu.sh - Deploy Water-Treat RTU to target device
# =============================================================================
#
# Quick deployment script for RTU devices. Handles:
#   - Building from source (or using pre-built binary)
#   - Auto-detecting station_name from MAC address
#   - Creating proper config file with detected values
#   - Setting up systemd service
#   - Creating p-net NV storage directory
#
# Usage (on RTU):
#   curl -fsSL https://raw.githubusercontent.com/mwilco03/Water-Treat/main/scripts/deploy-rtu.sh | sudo bash
#
# Or from local checkout:
#   sudo ./scripts/deploy-rtu.sh
#
# =============================================================================

set -euo pipefail

# =============================================================================
# Configuration
# =============================================================================

readonly INSTALL_DIR="/opt/water-treat"
readonly CONFIG_DIR="/etc/water-treat"
readonly DATA_DIR="/var/lib/water-treat"
readonly PNET_DATA_DIR="/var/lib/water-treat/pnet"
readonly LOG_DIR="/var/log/water-treat"
readonly SERVICE_NAME="water-treat"
readonly REPO_URL="https://github.com/mwilco03/Water-Treat.git"

# Colors
readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly NC='\033[0m'

# =============================================================================
# Output Helpers
# =============================================================================

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
step()    { echo -e "\n${GREEN}==>${NC} $*"; }

# =============================================================================
# System Detection
# =============================================================================

detect_network_interface() {
    # Priority order: eth* > enp* > ens* > wlan*
    local iface=""

    for pattern in "eth*" "enp*" "ens*" "wlan*"; do
        for candidate in /sys/class/net/$pattern; do
            [[ -e "$candidate" ]] || continue
            local name=$(basename "$candidate")
            [[ "$name" == "lo" ]] && continue

            # Check if it has a valid MAC (not all zeros)
            local mac=$(cat "$candidate/address" 2>/dev/null || echo "")
            if [[ -n "$mac" && "$mac" != "00:00:00:00:00:00" ]]; then
                iface="$name"
                break 2
            fi
        done
    done

    echo "$iface"
}

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

    # Extract last 4 hex chars of MAC, convert to lowercase
    local mac=$(cat "$mac_file")
    local suffix=$(echo "$mac" | awk -F: '{print tolower($5 $6)}')

    echo "rtu-${suffix}"
}

detect_ip_address() {
    local iface="$1"

    if [[ -z "$iface" ]]; then
        echo ""
        return
    fi

    ip -4 addr show "$iface" 2>/dev/null | grep -oP 'inet \K[0-9.]+' | head -1
}

# =============================================================================
# Installation Functions
# =============================================================================

check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root"
        echo "Usage: sudo $0"
        exit 1
    fi
}

install_dependencies() {
    step "Installing dependencies..."

    if command -v apt-get &>/dev/null; then
        apt-get update -qq
        apt-get install -y \
            build-essential cmake git \
            libncurses5-dev libsqlite3-dev \
            libcurl4-openssl-dev || true

        # Optional dependencies (don't fail if unavailable)
        apt-get install -y libcjson-dev libgpiod-dev libsystemd-dev 2>/dev/null || true

        success "Dependencies installed"
    else
        warn "apt-get not found, assuming dependencies are installed"
    fi
}

clone_repository() {
    step "Cloning Water-Treat repository..."

    if [[ -d "$INSTALL_DIR/.git" ]]; then
        info "Repository exists, pulling latest..."
        (cd "$INSTALL_DIR" && git pull --ff-only) || {
            warn "Pull failed, doing fresh clone"
            rm -rf "$INSTALL_DIR"
        }
    fi

    if [[ ! -d "$INSTALL_DIR" ]]; then
        git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
    fi

    success "Repository ready: $INSTALL_DIR"
}

fetch_protocol_headers() {
    step "Fetching shared protocol headers..."

    local shared_dir="$INSTALL_DIR/include/shared"
    mkdir -p "$shared_dir"

    local base_url="https://raw.githubusercontent.com/mwilco03/Water-Controller/main/shared/include"
    local files=("user_sync_protocol.h" "config_sync_protocol.h")

    for file in "${files[@]}"; do
        info "Fetching $file..."
        if curl -fsSL "$base_url/$file" -o "$shared_dir/$file"; then
            success "$file downloaded"
        else
            error "Failed to download $file"
            return 1
        fi
    done
}

build_from_source() {
    step "Building from source..."

    local build_dir="$INSTALL_DIR/build"
    mkdir -p "$build_dir"

    info "Running cmake..."
    (cd "$build_dir" && cmake ..) || {
        error "CMake failed"
        return 1
    }

    info "Compiling (this may take a few minutes on Pi)..."
    local jobs=$(nproc 2>/dev/null || echo 2)
    (cd "$build_dir" && make -j"$jobs") || {
        error "Compilation failed"
        return 1
    }

    if [[ ! -f "$build_dir/water-treat" ]]; then
        error "Binary not found after build"
        return 1
    fi

    success "Build complete"
}

install_binary() {
    step "Installing binary..."

    cp "$INSTALL_DIR/build/water-treat" /usr/local/bin/water-treat
    chmod +x /usr/local/bin/water-treat

    # Set capabilities for raw socket access (PROFINET needs this)
    if command -v setcap &>/dev/null; then
        setcap cap_net_raw+ep /usr/local/bin/water-treat || {
            warn "Could not set capabilities, will need to run as root"
        }
    fi

    success "Binary installed: /usr/local/bin/water-treat"
}

create_directories() {
    step "Creating directories..."

    mkdir -p "$CONFIG_DIR"
    mkdir -p "$DATA_DIR"
    mkdir -p "$PNET_DATA_DIR"
    mkdir -p "$LOG_DIR"

    # Set permissions
    chmod 755 "$DATA_DIR"
    chmod 755 "$PNET_DATA_DIR"
    chmod 755 "$LOG_DIR"

    success "Directories created"
    info "  Config:    $CONFIG_DIR"
    info "  Data:      $DATA_DIR"
    info "  p-net NV:  $PNET_DATA_DIR"
    info "  Logs:      $LOG_DIR"
}

create_config() {
    step "Creating configuration..."

    local config_file="$CONFIG_DIR/water-treat.conf"

    # Detect network settings
    local iface=$(detect_network_interface)
    local station_name=$(detect_station_name "$iface")
    local ip_addr=$(detect_ip_address "$iface")

    info "Detected interface:    $iface"
    info "Detected station_name: $station_name"
    info "Detected IP:           ${ip_addr:-DHCP}"

    # Don't overwrite existing config
    if [[ -f "$config_file" ]]; then
        warn "Config exists: $config_file"
        info "Preserving existing configuration"

        # Check if station_name is set
        if grep -q "^station_name" "$config_file"; then
            info "Station name already configured"
        else
            info "Adding auto-detected station_name to config..."
            sed -i "s/^# station_name = .*/station_name = $station_name/" "$config_file" || true
        fi
        return 0
    fi

    # Create new config
    cat > "$config_file" <<EOF
# Water-Treat RTU Configuration
# Generated by deploy-rtu.sh on $(date)
# Station name auto-detected from MAC address

[system]
device_name = $station_name
log_level = info
log_file = $LOG_DIR/monitor.log
daemon_mode = false

[network]
# Interface auto-detected: $iface
interface = $iface
dhcp_enabled = true

[profinet]
enabled = true
# Station name MUST be set for DCP to respond
# Auto-detected from MAC: ${iface:-unknown} -> $station_name
station_name = $station_name
vendor_id = 0x0493
device_id = 0x0001
product_name = Water Treatment RTU
min_device_interval = 32

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

    success "Config created: $config_file"
    info "Station name set to: $station_name"
}

create_systemd_service() {
    step "Creating systemd service..."

    local service_file="/etc/systemd/system/${SERVICE_NAME}.service"

    # Use the full service file from repo if available
    if [[ -f "$INSTALL_DIR/systemd/water-treat.service" ]]; then
        cp "$INSTALL_DIR/systemd/water-treat.service" "$service_file"
        success "Installed service from repository"
    else
        # Fallback: create minimal service
        cat > "$service_file" <<EOF
[Unit]
Description=Water-Treat RTU - PROFINET I/O Device
Documentation=https://github.com/mwilco03/Water-Treat
After=network.target

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

ProtectSystem=strict
ReadWritePaths=$DATA_DIR $LOG_DIR $PNET_DATA_DIR
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
        success "Created minimal service file"
    fi

    # Create environment file
    cat > "$CONFIG_DIR/water-treat.env" <<EOF
# Water-Treat environment overrides
# These take precedence over config file values
# WT_HTTP_PORT=9081
# WT_LOG_LEVEL=info
EOF

    systemctl daemon-reload
    systemctl enable "${SERVICE_NAME}.service"

    success "Service enabled: $SERVICE_NAME"
}

start_service() {
    step "Starting service..."

    systemctl start "${SERVICE_NAME}.service" || {
        error "Failed to start service"
        echo ""
        echo "Check logs with: journalctl -u $SERVICE_NAME -f"
        return 1
    }

    sleep 2

    if systemctl is-active --quiet "${SERVICE_NAME}.service"; then
        success "Service running!"
    else
        warn "Service may not have started correctly"
        echo "Check: systemctl status $SERVICE_NAME"
    fi
}

show_status() {
    step "Deployment Summary"

    local iface=$(detect_network_interface)
    local station_name=$(detect_station_name "$iface")
    local ip_addr=$(detect_ip_address "$iface")

    echo ""
    echo "  Station Name:  $station_name"
    echo "  Interface:     $iface"
    echo "  IP Address:    ${ip_addr:-DHCP}"
    echo "  HTTP Health:   http://${ip_addr:-localhost}:9081/health"
    echo ""
    echo "  Config:        $CONFIG_DIR/water-treat.conf"
    echo "  Logs:          journalctl -u $SERVICE_NAME -f"
    echo "  Status:        systemctl status $SERVICE_NAME"
    echo ""

    # Check if responding to PROFINET
    echo "To verify PROFINET DCP response, run from controller:"
    echo "  # Using PRONETA or similar tool, send DCP Identify"
    echo "  # RTU should respond as: $station_name"
    echo ""
}

# =============================================================================
# Main
# =============================================================================

main() {
    echo ""
    echo "=========================================="
    echo "  Water-Treat RTU Deployment"
    echo "=========================================="
    echo ""

    check_root

    install_dependencies
    clone_repository
    fetch_protocol_headers
    build_from_source
    install_binary
    create_directories
    create_config
    create_systemd_service
    start_service
    show_status

    success "Deployment complete!"
}

main "$@"
