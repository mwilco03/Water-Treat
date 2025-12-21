#!/bin/bash
#
# firstrun.sh - Water-Treat RTU First Boot Provisioning
#
# What: Zero-touch provisioning script for Raspberry Pi
# Why: Works without cloud-init, runs once on first boot
#
# Usage: Place on boot partition (/boot/firmware/firstrun.sh)
#
# Features:
#   - Non-blocking: explicit guards, no set -e
#   - Network check with bounded wait
#   - State marker prevents re-run
#   - Failure backoff (pauses after 20 failures)
#   - Hostname from MAC: rtu-XXXX
#   - Interface priority: eth* > enp* > ens* > wlan*
#   - Staged install with atomic activation
#   - Retry on reboot if network unavailable
#

# ------------------------------------------------------------------------------
# Create Required Paths First
# ------------------------------------------------------------------------------
mkdir -p /var/log/water-treat
mkdir -p /var/lib/water-treat
mkdir -p /opt
mkdir -p /home/admin

# ------------------------------------------------------------------------------
# Logging
# ------------------------------------------------------------------------------
exec > >(tee -a /var/log/water-treat/first-boot.log) 2>&1
echo "===== Provisioning attempt: $(date) ====="

# ------------------------------------------------------------------------------
# Constants
# ------------------------------------------------------------------------------
STATE_DIR=/var/lib/water-treat
INSTALL_ROOT=/opt/Water-Treat
STAGING=/opt/.water-treat-staging
MOTD_FILE=/etc/motd
FAIL_COUNT_FILE=$STATE_DIR/fail.count
MAX_FAILURES=20

# ------------------------------------------------------------------------------
# Failure Tracking
# ------------------------------------------------------------------------------
get_fail_count() {
    if [ -f "$FAIL_COUNT_FILE" ]; then
        cat "$FAIL_COUNT_FILE" 2>/dev/null || echo 0
    else
        echo 0
    fi
}

increment_fail_count() {
    count=$(get_fail_count)
    echo $((count + 1)) > "$FAIL_COUNT_FILE"
}

clear_fail_count() {
    rm -f "$FAIL_COUNT_FILE"
}

# ------------------------------------------------------------------------------
# Idempotency Check
# ------------------------------------------------------------------------------
if [ -f "$STATE_DIR/first-boot.ok" ]; then
    echo "Provisioning already completed - exiting cleanly."
    exit 0
fi

# ------------------------------------------------------------------------------
# Failure Threshold Check
# ------------------------------------------------------------------------------
fail_count=$(get_fail_count)
if [ "$fail_count" -ge "$MAX_FAILURES" ]; then
    echo "Provisioning paused after $fail_count failures"
    cat > "$MOTD_FILE" <<EOF
Water-Treat RTU

Provisioning paused after repeated failures.

To investigate:
  cat /var/log/water-treat/first-boot.log

To retry:
  rm $FAIL_COUNT_FILE
  rm $STATE_DIR/first-boot.failed
  reboot

Manual help may be needed.
EOF
    exit 0
fi

# ------------------------------------------------------------------------------
# Create User (if not exists)
# ------------------------------------------------------------------------------
if ! id admin >/dev/null 2>&1; then
    echo "Creating admin user..."
    useradd -m -s /bin/bash -G sudo,gpio,i2c,spi,dialout admin || true
    echo 'admin:H2OhYeah!' | chpasswd || true
fi

# ------------------------------------------------------------------------------
# SSH: Ensure Enabled
# ------------------------------------------------------------------------------
systemctl enable ssh 2>/dev/null || systemctl enable sshd 2>/dev/null || true
systemctl start ssh 2>/dev/null || systemctl start sshd 2>/dev/null || true

# ------------------------------------------------------------------------------
# Hostname from MAC
# ------------------------------------------------------------------------------
# Priority: eth* > enp* > ens* > wlan*
BEST_IFACE=""
BEST_PRIORITY=0

for iface in /sys/class/net/*; do
    [ -d "$iface" ] || continue
    name=$(basename "$iface")
    [ "$name" = "lo" ] && continue

    priority=1
    case "$name" in
        eth*)  priority=5 ;;
        enp*)  priority=4 ;;
        ens*)  priority=3 ;;
        wlan*) priority=2 ;;
    esac

    if [ "$priority" -gt "$BEST_PRIORITY" ]; then
        BEST_PRIORITY=$priority
        BEST_IFACE=$name
    fi
done

if [ -n "$BEST_IFACE" ]; then
    MAC_ADDR=$(cat /sys/class/net/$BEST_IFACE/address 2>/dev/null)
    if [ -n "$MAC_ADDR" ] && [ "$MAC_ADDR" != "00:00:00:00:00:00" ]; then
        # Extract last 4 hex chars
        MAC_SUFFIX=$(echo "$MAC_ADDR" | sed 's/://g' | tail -c 5)
        MAC_SUFFIX=$(echo "$MAC_SUFFIX" | tr '[:upper:]' '[:lower:]')
    fi
fi

if [ -n "$MAC_SUFFIX" ]; then
    hostnamectl set-hostname "rtu-${MAC_SUFFIX}" 2>/dev/null || \
        echo "rtu-${MAC_SUFFIX}" > /etc/hostname
    echo "Hostname set to rtu-${MAC_SUFFIX}"
fi

# ------------------------------------------------------------------------------
# Friendly MOTD (Waiting State)
# ------------------------------------------------------------------------------
cat > "$MOTD_FILE" <<'EOF'
Water-Treat RTU

Network not available yet.

You can still:
  - Log in and configure networking
  - Inspect hardware
  - Check logs: /var/log/water-treat/first-boot.log

Provisioning will retry on next boot once network is available.
EOF

# ------------------------------------------------------------------------------
# Network Check (Bounded, Non-Fatal)
# ------------------------------------------------------------------------------
echo "Checking for network (max 60s)..."
NET_OK=0
for i in $(seq 1 30); do
    if getent hosts github.com >/dev/null 2>&1; then
        NET_OK=1
        break
    fi
    sleep 2
done

if [ "$NET_OK" -ne 1 ]; then
    echo "Network not available - deferring install to next boot"
    increment_fail_count
    touch "$STATE_DIR/first-boot.failed"
    exit 0
fi

# ------------------------------------------------------------------------------
# Network Available: Update MOTD
# ------------------------------------------------------------------------------
cat > "$MOTD_FILE" <<'EOF'
Water-Treat RTU

Setting up Water-Treat RTU...

This takes a few minutes.
Progress: tail -f /var/log/water-treat/first-boot.log
EOF

# ------------------------------------------------------------------------------
# Staged Install
# ------------------------------------------------------------------------------
echo "Network detected - starting staged install"

rm -rf "$STAGING"
if ! git clone https://github.com/mwilco03/Water-Treat.git "$STAGING"; then
    echo "Git clone failed - will retry on next boot"
    increment_fail_count
    touch "$STATE_DIR/first-boot.failed"
    exit 0
fi

# ------------------------------------------------------------------------------
# Validate Clone
# ------------------------------------------------------------------------------
if [ ! -f "$STAGING/scripts/install.sh" ]; then
    echo "Validation failed - will retry on next boot"
    rm -rf "$STAGING"
    increment_fail_count
    touch "$STATE_DIR/first-boot.failed"
    exit 0
fi

# ------------------------------------------------------------------------------
# Activate Atomically
# ------------------------------------------------------------------------------
rm -rf "$INSTALL_ROOT"
mv "$STAGING" "$INSTALL_ROOT"
chown -R admin:admin "$INSTALL_ROOT"

# ------------------------------------------------------------------------------
# Install Dependencies
# ------------------------------------------------------------------------------
echo "Installing dependencies..."
if ! "$INSTALL_ROOT/scripts/install-deps.sh"; then
    echo "Dependency install failed - will retry on next boot"
    increment_fail_count
    touch "$STATE_DIR/first-boot.failed"
    exit 0
fi

# ------------------------------------------------------------------------------
# Build Application (as admin, not root - avoids ownership issues)
# ------------------------------------------------------------------------------
echo "Building application..."
if ! sudo -u admin "$INSTALL_ROOT/scripts/build.sh"; then
    echo "Build failed - will retry on next boot"
    increment_fail_count
    touch "$STATE_DIR/first-boot.failed"
    exit 0
fi

# ------------------------------------------------------------------------------
# Install Application
# ------------------------------------------------------------------------------
echo "Installing application..."
if ! "$INSTALL_ROOT/scripts/install.sh"; then
    echo "Install failed - will retry on next boot"
    increment_fail_count
    touch "$STATE_DIR/first-boot.failed"
    exit 0
fi

# ------------------------------------------------------------------------------
# Enable and Start Service
# ------------------------------------------------------------------------------
systemctl daemon-reload
systemctl enable water-treat || true
systemctl start water-treat || true

# ------------------------------------------------------------------------------
# Success Cleanup
# ------------------------------------------------------------------------------
rm -f "$STATE_DIR/first-boot.failed"
clear_fail_count
touch "$STATE_DIR/first-boot.ok"
touch /home/admin/.provisioning-complete
chown admin:admin /home/admin/.provisioning-complete

# Remove firstrun from boot partition (don't run again)
rm -f /boot/firmware/firstrun.sh 2>/dev/null || true
rm -f /boot/firstrun.sh 2>/dev/null || true

# ------------------------------------------------------------------------------
# Final MOTD
# ------------------------------------------------------------------------------
HOSTNAME=$(hostname)
cat > "$MOTD_FILE" <<EOF
Water-Treat RTU: $HOSTNAME
Provisioned: $(date '+%Y-%m-%d %H:%M')

Service commands:
  systemctl status water-treat
  systemctl restart water-treat
  journalctl -u water-treat -f

Source: $INSTALL_ROOT
EOF

echo "===== Provisioning completed successfully ====="
exit 0
