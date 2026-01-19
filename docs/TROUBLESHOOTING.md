# Water-Treat RTU Troubleshooting Guide

This guide covers common issues encountered when deploying and operating the Water-Treat RTU.

## Table of Contents

- [TUI Issues](#tui-issues)
- [Service Startup Issues](#service-startup-issues)
- [PROFINET Issues](#profinet-issues)
- [Network Issues](#network-issues)
- [Database Issues](#database-issues)

---

## TUI Issues

### TUI Shows Garbled Characters and Crashes with Segmentation Fault

**Symptoms:**
- Water droplet logo displays as `�~V~H` instead of `█████`
- Application crashes with "Segmentation fault" when typing
- Display corruption when entering username/password

**Cause:**
The system locale is not set to UTF-8. The TUI uses Unicode block characters (`█` U+2588) for the logo, which require UTF-8 encoding. Without proper locale settings, ncurses cannot render these characters and crashes.

**Solution:**

```bash
# Set locale for current session
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8

# Run water-treat
/usr/local/bin/water-treat
```

**Permanent Fix:**

```bash
# Option 1: Set system-wide locale
sudo localectl set-locale LANG=en_US.UTF-8

# Option 2: Add to shell profile
echo 'export LANG=en_US.UTF-8' >> ~/.bashrc
echo 'export LC_ALL=en_US.UTF-8' >> ~/.bashrc

# Ensure locale is generated
sudo locale-gen en_US.UTF-8
```

**Verification:**
```bash
locale
# Should show: LANG=en_US.UTF-8
```

---

### TUI Crashes with "Error opening terminal: unknown"

**Symptoms:**
- Service restart loops (check with `systemctl show water-treat --property=NRestarts`)
- Log shows: "Error opening terminal: unknown"
- Only happens when running under systemd

**Cause:**
The application is trying to initialize the ncurses TUI when running under systemd, but there's no terminal (TTY) available.

**Solution:**

The application should auto-detect headless mode. If it doesn't:

```bash
# Option 1: Set WT_HEADLESS environment variable
echo "WT_HEADLESS=1" >> /etc/water-treat/water-treat.env
sudo systemctl daemon-reload
sudo systemctl restart water-treat

# Option 2: Ensure TERM=dumb is set in service
# (Already configured in default service file)
```

**Verification:**
```bash
systemctl status water-treat
# Should show: Active: active (running)
# NRestarts should be low (< 5)
```

---

### Multiple water-treat Processes Running

**Symptoms:**
- TUI display corruption
- Input goes to wrong process
- `pgrep -a water-treat` shows multiple PIDs

**Cause:**
Multiple instances fighting for the terminal. Usually caused by:
- Systemd service running AND manual TUI execution
- Crash loop spawning new processes

**Solution:**

```bash
# Kill all instances
sudo pkill -9 water-treat

# Stop the service
sudo systemctl stop water-treat

# Verify all gone
pgrep -a water-treat
# Should return nothing

# Choose ONE mode:
# For daemon mode (background):
sudo systemctl start water-treat

# OR for TUI mode (interactive):
/usr/local/bin/water-treat
```

---

## Service Startup Issues

### Service Enters Restart Loop

**Symptoms:**
- `systemctl status water-treat` shows frequent restarts
- `systemctl show water-treat --property=NRestarts` shows high count
- SD card wearing out from log writes

**Cause:**
Application crashing repeatedly. Common causes:
1. Missing locale (see TUI issues above)
2. Database permission issues
3. PROFINET initialization failure

**Solution:**

```bash
# Check restart count
systemctl show water-treat --property=NRestarts

# Check logs for error
journalctl -u water-treat -n 50 --no-pager

# Reset failure counter after fixing
sudo systemctl reset-failed water-treat
```

**Rate Limiting:**
The service is configured to allow 5 restarts within 5 minutes, then stop. After 5 minutes, it will allow restarts again.

---

### Service Fails to Start - Permission Denied

**Symptoms:**
- Log shows permission errors for `/var/lib/water-treat/`
- Database cannot be opened

**Solution:**

```bash
# Fix ownership
sudo chown -R root:root /var/lib/water-treat
sudo chmod 755 /var/lib/water-treat
sudo chmod 644 /var/lib/water-treat/*.db

# Restart
sudo systemctl restart water-treat
```

---

## PROFINET Issues

### Health Shows "PROFINET not compiled (HAVE_PNET undefined)"

**Symptoms:**
- `/health` endpoint shows PROFINET status as "unknown"
- Message: "Not compiled (HAVE_PNET undefined)"

**Cause:**
The p-net library was not available during compilation. This is expected for development builds.

**Solution:**

For production RTU deployment, rebuild with p-net:

```bash
# Install p-net library first
# Then rebuild water-treat
curl -fsSL https://raw.githubusercontent.com/mwilco03/Water-Treat/main/bootstrap.sh | sudo bash -s -- fresh
```

---

### Health Shows "Interface 'eth0' has no IPv4 address"

**Symptoms:**
- PROFINET status: CRITICAL
- Message mentions interface has no IPv4

**Cause:**
The configured network interface doesn't have an IP address assigned.

**Solution:**

```bash
# Check interface status
ip addr show eth0

# If no IP, configure it:
# Option 1: DHCP
sudo dhclient eth0

# Option 2: Static IP
sudo ip addr add 192.168.6.100/24 dev eth0
sudo ip link set eth0 up

# Update config if interface name is different
# Edit /etc/water-treat/water-treat.conf
# [network]
# interface = end0   # or your actual interface name
```

---

### Health Shows "DISABLED in config!"

**Symptoms:**
- PROFINET status: DEGRADED
- Message: "DISABLED in config! Set [profinet] enabled=true"

**Cause:**
PROFINET is disabled in the configuration file. This is a misconfiguration for a production RTU.

**Solution:**

```bash
# Edit config
sudo nano /etc/water-treat/water-treat.conf

# Ensure this is set:
# [profinet]
# enabled = true

# Restart
sudo systemctl restart water-treat
```

---

## Network Issues

### Cannot Reach Controller

**Symptoms:**
- PROFINET shows "Waiting for controller connection"
- RTU not appearing in Controller's discovery

**Diagnosis:**

```bash
# Check network connectivity
ping 192.168.6.13  # Controller IP

# Check interface is up
ip link show

# Check IP configuration
ip addr show

# Check ARP cache
cat /proc/net/arp

# Check routing
ip route show
```

**Common Fixes:**

```bash
# Bring interface up
sudo ip link set eth0 up

# Add route to controller subnet
sudo ip route add 192.168.6.0/24 dev eth0

# Check firewall
sudo iptables -L -n
```

---

## Database Issues

### "Loaded 0 modules from database"

**Symptoms:**
- Log shows no modules loaded
- Sensors page is empty

**Cause:**
This is expected on first run. Modules/sensors must be configured via the TUI or by importing a configuration.

**Solution:**

1. Log into the TUI (admin / H2OhYeah!)
2. Go to Sensors page
3. Add sensor configurations
4. Or import a pre-configured database

---

### Database Locked

**Symptoms:**
- Errors about database being locked
- Operations timing out

**Cause:**
Multiple processes accessing the database, or a crashed process left a lock.

**Solution:**

```bash
# Stop all water-treat processes
sudo pkill -9 water-treat
sudo systemctl stop water-treat

# Check for lock files
ls -la /var/lib/water-treat/*.db*

# Remove stale locks (if present)
rm -f /var/lib/water-treat/data.db-journal
rm -f /var/lib/water-treat/data.db-wal
rm -f /var/lib/water-treat/data.db-shm

# Restart
sudo systemctl start water-treat
```

---

## Diagnostic Commands

### Quick Health Check

```bash
# Service status
systemctl status water-treat

# Health endpoint
curl -s http://localhost:9081/health | jq .

# Prometheus metrics
curl -s http://localhost:9081/metrics

# Recent logs
journalctl -u water-treat -n 20 --no-pager
```

### System Information

```bash
# Process list
pgrep -a water-treat

# Resource usage
top -p $(pgrep water-treat | head -1)

# Network interfaces
ip addr show

# Locale
locale

# Terminal
echo $TERM
```

### Configuration Check

```bash
# View current config
cat /etc/water-treat/water-treat.conf

# View environment overrides
cat /etc/water-treat/water-treat.env

# Export running config via API
curl -s http://localhost:9081/config | jq .
```

---

## Getting Help

If issues persist:

1. Collect diagnostic information:
   ```bash
   journalctl -u water-treat --since "1 hour ago" > water-treat-logs.txt
   curl -s http://localhost:9081/health > health.json
   curl -s http://localhost:9081/config > config.json
   ```

2. Report issues at: https://github.com/mwilco03/Water-Treat/issues

Include:
- RTU hardware model
- Output of `uname -a`
- Log excerpts
- Health and config JSON files
