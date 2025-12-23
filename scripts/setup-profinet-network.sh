#!/bin/bash
#
# PROFINET Network Configuration Script for Water-Treat RTU
#
# This script configures the network interface for PROFINET communication
# between the RTU (Water-Treat) and Controller (Water-Controller).
#
# Requirements:
# - Dedicated Ethernet interface for PROFINET (not shared with management)
# - Static IP addressing on the PROFINET network segment
# - Real-time priority for PROFINET traffic
#
# Usage: sudo ./setup-profinet-network.sh [interface] [ip_address]
#        Example: sudo ./setup-profinet-network.sh eth0 192.168.100.10
#

set -e

# Default configuration
PROFINET_IFACE="${1:-eth0}"
PROFINET_IP="${2:-192.168.100.10}"
PROFINET_NETMASK="255.255.255.0"
PROFINET_GATEWAY=""  # No gateway needed for local PROFINET segment

# Station name (must match GSD/controller configuration)
STATION_NAME="water-treat-rtu"

echo "=============================================="
echo "  PROFINET Network Configuration"
echo "=============================================="
echo "Interface:    $PROFINET_IFACE"
echo "IP Address:   $PROFINET_IP"
echo "Netmask:      $PROFINET_NETMASK"
echo "Station Name: $STATION_NAME"
echo "=============================================="

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root"
    exit 1
fi

# Check if interface exists
if ! ip link show "$PROFINET_IFACE" &>/dev/null; then
    echo "ERROR: Interface $PROFINET_IFACE not found"
    echo "Available interfaces:"
    ip link show | grep -E "^[0-9]+:" | cut -d: -f2 | tr -d ' '
    exit 1
fi

# Step 1: Configure static IP
echo ""
echo "[1/5] Configuring static IP address..."
ip addr flush dev "$PROFINET_IFACE" 2>/dev/null || true
ip addr add "$PROFINET_IP/$PROFINET_NETMASK" dev "$PROFINET_IFACE"
ip link set "$PROFINET_IFACE" up

# Step 2: Disable IPv6 on PROFINET interface (PROFINET uses IPv4 only)
echo "[2/5] Disabling IPv6 on PROFINET interface..."
sysctl -w "net.ipv6.conf.$PROFINET_IFACE.disable_ipv6=1" &>/dev/null || true

# Step 3: Enable promiscuous mode for raw Ethernet frame handling
echo "[3/5] Enabling promiscuous mode..."
ip link set "$PROFINET_IFACE" promisc on

# Step 4: Configure real-time priority for PROFINET (optional, for performance)
echo "[4/5] Configuring network priority..."

# Set high priority for PROFINET multicast (DCP discovery)
# PROFINET DCP uses multicast 01:0E:CF:00:00:00
iptables -t mangle -A OUTPUT -o "$PROFINET_IFACE" -j DSCP --set-dscp 46 2>/dev/null || true

# Disable flow control for deterministic timing
ethtool -A "$PROFINET_IFACE" rx off tx off 2>/dev/null || true

# Step 5: Set hostname for DCP discovery
echo "[5/5] Configuring hostname for DCP..."
# Set the station name as hostname (optional - p-net handles this internally)
# hostnamectl set-hostname "$STATION_NAME" 2>/dev/null || true

echo ""
echo "=============================================="
echo "  Configuration Complete"
echo "=============================================="
echo ""
echo "PROFINET Network Summary:"
echo "  Interface:     $PROFINET_IFACE"
echo "  IP Address:    $(ip addr show "$PROFINET_IFACE" | grep 'inet ' | awk '{print $2}')"
echo "  MAC Address:   $(ip link show "$PROFINET_IFACE" | grep ether | awk '{print $2}')"
echo "  Promiscuous:   $(ip link show "$PROFINET_IFACE" | grep -q PROMISC && echo 'Yes' || echo 'No')"
echo ""
echo "Next steps:"
echo "  1. Ensure Water-Controller is configured with compatible settings"
echo "  2. Controller should use IP in same subnet (e.g., 192.168.100.1)"
echo "  3. Import GSD file (GSDML-V2.4-WaterTreat-RTU-20241222.xml) into controller"
echo "  4. Configure module slots in controller to match RTU configuration"
echo "  5. Start water-treat application with: ./build/water-treat -c /etc/water-treat/water-treat.conf"
echo ""
echo "Typical PROFINET Network Layout:"
echo ""
echo "  +-----------------+          +-----------------+"
echo "  | Water-Controller|          |   Water-Treat   |"
echo "  | (IO Controller) |          |     (RTU)       |"
echo "  |                 |          |                 |"
echo "  | 192.168.100.1   |<-------->| 192.168.100.10  |"
echo "  +-----------------+  PROFINET +-----------------+"
echo "       eth0                           eth0"
echo ""
