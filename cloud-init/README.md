# Water-Treat RTU Provisioning

## Cloud-Init (VMs, Cloud Instances)

Copy to your cloud-init datasource:
- `user-data`
- `meta-data`

## Raspberry Pi OS (No Cloud-Init)

### Installation

```bash
# Copy firstrun script
sudo cp firstrun.sh /opt/water-treat-firstrun.sh
sudo chmod +x /opt/water-treat-firstrun.sh

# Install systemd service
sudo cp water-treat-firstrun.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable water-treat-firstrun.service

# Reboot to run provisioning
sudo reboot
```

### What Happens

1. System boots, reaches network-online.target
2. Service runs `/opt/water-treat-firstrun.sh`
3. Script creates admin user, sets hostname from MAC
4. Waits for network, clones repo, builds, installs
5. Disables itself after success

### Files

| File | Purpose |
|------|---------|
| `firstrun.sh` | Provisioning script |
| `water-treat-firstrun.service` | Systemd service (runs after network) |

### Credentials

- User: `admin`
- Password: `H2OhYeah!`
- Groups: sudo, gpio, i2c, spi, dialout
