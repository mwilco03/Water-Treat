# Installation Guide

Complete installation instructions for the Water Treatment RTU firmware across all supported platforms.

## Table of Contents

1. [Quick Start (Raspberry Pi)](#quick-start-raspberry-pi)
2. [Platform-Specific Installation](#platform-specific-installation)
   - [Raspberry Pi (All Models)](#raspberry-pi-all-models)
   - [Orange Pi Zero 3 / 2W (H618)](#orange-pi-zero-3--2w-h618)
   - [ODROID (XU4, C4, N2)](#odroid-xu4-c4-n2)
   - [Luckfox Lyra (RK3506)](#luckfox-lyra-rk3506)
3. [Cross-Compilation](#cross-compilation)
4. [Building p-net (PROFINET Library)](#building-p-net-profinet-library)
5. [Kernel Configuration](#kernel-configuration)
6. [Post-Installation](#post-installation)

---

## Quick Start (Raspberry Pi)

For Raspberry Pi 4B with Raspberry Pi OS (Bookworm):

```bash
# 1. Install dependencies
sudo apt update
sudo apt install -y \
    build-essential cmake git \
    libncurses5-dev libsqlite3-dev \
    libcurl4-openssl-dev libcjson-dev \
    libgpiod-dev libsystemd-dev

# 2. Clone and build
git clone https://github.com/your-org/Water-Treat.git
cd Water-Treat
mkdir build && cd build
cmake ..
make -j$(nproc)

# 3. Install
sudo make install

# 4. Enable required kernel modules (see Kernel Configuration)
sudo raspi-config  # Enable I2C, SPI, 1-Wire

# 5. Run
sudo profinet-monitor
```

---

## Platform-Specific Installation

### Raspberry Pi (All Models)

**Supported:** Pi 3B, 3B+, 4B, 5, Zero 2W, CM4

#### Prerequisites

| Component | Requirement |
|-----------|-------------|
| OS | Raspberry Pi OS Lite (Bookworm) recommended |
| Kernel | 6.1+ (for libgpiod v2 compatibility) |
| Storage | 8GB+ SD card |
| Network | Ethernet (required for PROFINET) |

#### Dependencies

```bash
sudo apt update && sudo apt install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libncurses5-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libcjson-dev \
    libgpiod-dev \
    libsystemd-dev \
    i2c-tools
```

#### GPIO Chip Mapping

| Model | GPIO Chip | Notes |
|-------|-----------|-------|
| Pi 3B/3B+/4B | `gpiochip0` | Standard 40-pin header |
| Pi 5 | `gpiochip4` | New RP1 southbridge chip |
| Pi Zero 2W | `gpiochip0` | 40-pin header |

#### Build

```bash
cd Water-Treat
mkdir build && cd build
cmake -DENABLE_LED_SUPPORT=ON ..
make -j4
sudo make install
```

#### Raspberry Pi 5 Specific Notes

The Pi 5 uses the RP1 southbridge with a different GPIO chip:

```bash
# Verify GPIO chip
gpiodetect
# Should show: gpiochip4 [pinctrl-rp1] (54 lines)

# The firmware auto-detects Pi 5 and uses gpiochip4
```

---

### Orange Pi Zero 3 / 2W (H618)

**Supported:** Orange Pi Zero 3, Zero 2W (Allwinner H618 SoC)

#### Prerequisites

| Component | Requirement |
|-----------|-------------|
| OS | Armbian (Ubuntu/Debian based) |
| Kernel | 5.15+ (mainline preferred) |
| Image | [Armbian Downloads](https://www.armbian.com/orange-pi-zero-3/) |

#### Dependencies

```bash
sudo apt update && sudo apt install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libncurses5-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libcjson-dev \
    libgpiod-dev \
    libsystemd-dev \
    i2c-tools
```

#### GPIO Chip Mapping

| Function | GPIO Chip | Line | Physical Pin |
|----------|-----------|------|--------------|
| Main GPIO | `gpiochip0` | 0-287 | 26-pin header |
| I2C-2 | - | SDA=PI6, SCL=PI5 | Pin 3, 5 |
| UART | - | TX=PH0, RX=PH1 | Pin 8, 10 |

#### Device Tree Overlays

Enable I2C and SPI via `armbian-config`:

```bash
sudo armbian-config
# System → Hardware → Enable: i2c2, spi-spidev
sudo reboot
```

Or manually edit `/boot/armbianEnv.txt`:

```
overlays=i2c2 spi-spidev
param_spidev_spi_bus=1
```

#### Build

```bash
cd Water-Treat
mkdir build && cd build
cmake ..
make -j4
sudo make install
```

---

### ODROID (XU4, C4, N2)

**Supported:** ODROID-XU4, ODROID-C4, ODROID-N2/N2+

#### ODROID-XU4 (Samsung Exynos 5422)

| Component | Requirement |
|-----------|-------------|
| OS | Ubuntu 20.04+ / Armbian |
| Kernel | 4.14+ (Hardkernel) or 5.x (mainline) |

```bash
# Dependencies
sudo apt update && sudo apt install -y \
    build-essential cmake git pkg-config \
    libncurses5-dev libsqlite3-dev \
    libcurl4-openssl-dev libcjson-dev \
    libgpiod-dev libsystemd-dev

# GPIO uses /dev/gpiochip0 and /dev/gpiochip1
# Export pins via: gpioset gpiochip1 25=1
```

**GPIO Chip Mapping (XU4):**

| Chip | Lines | Function |
|------|-------|----------|
| `gpiochip0` | 0-7 | GPA0 bank |
| `gpiochip1` | 0-255 | Main GPX/GPB banks |

#### ODROID-C4 / N2 (Amlogic S905X3 / S922X)

| Component | Requirement |
|-----------|-------------|
| OS | Ubuntu 20.04+ / Armbian |
| Kernel | 5.10+ recommended |

```bash
# Dependencies (same as XU4)
sudo apt update && sudo apt install -y \
    build-essential cmake git pkg-config \
    libncurses5-dev libsqlite3-dev \
    libcurl4-openssl-dev libcjson-dev \
    libgpiod-dev libsystemd-dev
```

**GPIO Chip Mapping (C4/N2):**

| Chip | Lines | Header Pins |
|------|-------|-------------|
| `gpiochip0` | 0-15 | GPIOX bank |
| `gpiochip1` | 0-100+ | Main GPIO banks |

#### Build for ODROID

```bash
cd Water-Treat
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

---

### Luckfox Lyra (RK3506)

**Supported:** Luckfox Lyra, Pico series (Rockchip RK3506)

#### Prerequisites

| Component | Requirement |
|-----------|-------------|
| OS | Buildroot (official SDK) or Ubuntu |
| Kernel | 5.10+ (Rockchip BSP) |
| SDK | [Luckfox SDK](https://github.com/LuckfoxTECH/luckfox-pico) |

#### Native Build (on device)

If running Ubuntu on the Lyra:

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake git pkg-config \
    libncurses5-dev libsqlite3-dev \
    libcurl4-openssl-dev libgpiod-dev
```

#### Cross-Compilation (recommended)

See [Cross-Compilation](#cross-compilation) section below.

#### GPIO Chip Mapping

| Chip | Bank | Lines |
|------|------|-------|
| `gpiochip0` | GPIO0 | 0-31 |
| `gpiochip1` | GPIO1 | 0-31 |
| `gpiochip2` | GPIO2 | 0-31 |
| `gpiochip3` | GPIO3 | 0-31 |

---

## Cross-Compilation

For building on x86_64 host for ARM targets.

### Setup Cross-Compiler

#### 32-bit ARM (armhf)

```bash
# Ubuntu/Debian
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf

# Verify
arm-linux-gnueabihf-gcc --version
```

#### 64-bit ARM (aarch64)

```bash
# Ubuntu/Debian
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Verify
aarch64-linux-gnu-gcc --version
```

### Cross-Compile Dependencies

You need ARM versions of the libraries. Options:

#### Option 1: Use Multiarch (Debian/Ubuntu)

```bash
# Add ARM architecture
sudo dpkg --add-architecture arm64  # or armhf for 32-bit

sudo apt update
sudo apt install \
    libncurses5-dev:arm64 \
    libsqlite3-dev:arm64 \
    libcurl4-openssl-dev:arm64 \
    libcjson-dev:arm64 \
    libgpiod-dev:arm64
```

#### Option 2: Build Sysroot from Target

```bash
# Copy libraries from target device
rsync -avz pi@raspberrypi:/usr/lib/aarch64-linux-gnu/ sysroot/usr/lib/
rsync -avz pi@raspberrypi:/usr/include/ sysroot/usr/include/
```

### Build with Toolchain File

```bash
cd Water-Treat
mkdir build-arm64 && cd build-arm64

# 64-bit ARM
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain/aarch64-linux-gnu.cmake ..

# 32-bit ARM
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain/arm-linux-gnueabihf.cmake ..

make -j$(nproc)
```

### Deploy to Target

```bash
# Copy binary
scp profinet-monitor pi@raspberrypi:/usr/local/bin/

# Copy config
scp -r ../systemd pi@raspberrypi:/tmp/
ssh pi@raspberrypi 'sudo cp /tmp/systemd/*.service /etc/systemd/system/'
```

---

## Building p-net (PROFINET Library)

The p-net library provides PROFINET I/O Device functionality.

### Native Build

```bash
# Dependencies
sudo apt install -y git cmake ninja-build

# Clone p-net
git clone https://github.com/rtlabs-com/p-net.git
cd p-net

# Build
mkdir build && cd build
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DBUILD_SHARED_LIBS=ON \
    ..
ninja
sudo ninja install
sudo ldconfig
```

### Cross-Compile p-net

```bash
cd p-net
mkdir build-arm64 && cd build-arm64

cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/aarch64-linux-gnu.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX=/path/to/sysroot/usr/local \
    ..
ninja
ninja install
```

### Verify Installation

```bash
# Check library
ls /usr/local/lib/libpnet*
# Should show: libpnet.so, libpnet.so.0.1.0

# Check headers
ls /usr/local/include/pnet_api.h
```

---

## Kernel Configuration

### Raspberry Pi (raspi-config)

```bash
sudo raspi-config
# Interface Options → I2C → Enable
# Interface Options → SPI → Enable
# Interface Options → 1-Wire → Enable
sudo reboot
```

Or edit `/boot/config.txt`:

```ini
# I2C
dtparam=i2c_arm=on
dtparam=i2c_baudrate=400000

# SPI
dtparam=spi=on

# 1-Wire (default GPIO 4)
dtoverlay=w1-gpio,gpiopin=4

# PWM (2 channels)
dtoverlay=pwm-2chan,pin=18,func=2,pin2=13,func2=4
```

### Orange Pi / Armbian

Edit `/boot/armbianEnv.txt`:

```ini
overlays=i2c2 spi-spidev w1-gpio
param_w1_pin=PA10
```

Or use `armbian-config`:

```bash
sudo armbian-config
# System → Hardware → Select overlays
```

### ODROID

Edit `/boot/config.ini` or use `odroid-config`:

```bash
sudo odroid-config
# Enable I2C, SPI, etc.
```

### Verify Kernel Modules

```bash
# I2C
lsmod | grep i2c
ls /dev/i2c-*
sudo i2cdetect -y 1

# SPI
lsmod | grep spi
ls /dev/spidev*

# 1-Wire
lsmod | grep w1
ls /sys/bus/w1/devices/
```

---

## Post-Installation

### Create System User

```bash
sudo useradd -r -s /bin/false water-treat
sudo usermod -aG gpio,i2c,spi water-treat
```

### Create Directories

```bash
sudo mkdir -p /etc/water-treat
sudo mkdir -p /var/lib/water-treat
sudo mkdir -p /var/log/water-treat
sudo mkdir -p /var/backup/water-treat
sudo chown -R water-treat:water-treat /var/lib/water-treat
```

### Install Systemd Service

```bash
sudo cp systemd/profinet-monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable profinet-monitor
sudo systemctl start profinet-monitor
```

### Verify Installation

```bash
# Check service status
sudo systemctl status profinet-monitor

# View logs
journalctl -u profinet-monitor -f

# Test TUI (requires root for GPIO)
sudo profinet-monitor
```

### GPIO Permissions (non-root access)

To run without root, add user to `gpio` group:

```bash
sudo usermod -aG gpio $USER
# Logout and login again

# Verify
groups
# Should include: gpio
```

For systems without gpio group, create udev rule:

```bash
sudo tee /etc/udev/rules.d/99-gpio.rules << 'EOF'
SUBSYSTEM=="gpio", KERNEL=="gpiochip*", MODE="0660", GROUP="gpio"
EOF
sudo udevadm control --reload-rules
```

---

## Troubleshooting Installation

### CMake Can't Find Libraries

```bash
# Verify pkg-config paths
pkg-config --libs ncurses sqlite3 libcjson

# Set PKG_CONFIG_PATH if needed
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

### p-net Not Found

```bash
# Verify p-net installation
ls /usr/local/lib/libpnet*
ls /usr/local/include/pnet_api.h

# Update library cache
sudo ldconfig

# Set library path
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

### GPIO Permission Denied

```bash
# Check device permissions
ls -la /dev/gpiochip*

# Add user to gpio group
sudo usermod -aG gpio $USER

# Or run as root
sudo profinet-monitor
```

### I2C Device Not Found

```bash
# Scan I2C bus
sudo i2cdetect -y 1

# Check if module loaded
lsmod | grep i2c

# Load module manually
sudo modprobe i2c-dev
```

---

## Version Compatibility Matrix

| Platform | Kernel | libgpiod | GCC | Status |
|----------|--------|----------|-----|--------|
| RPi 4B (Bookworm) | 6.1+ | 1.6+ | 12+ | Tested |
| RPi 5 (Bookworm) | 6.1+ | 2.0+ | 12+ | Tested |
| RPi 3B+ (Bullseye) | 5.15 | 1.6 | 10 | Tested |
| RPi (Trixie/Debian 13) | 6.6+ | 2.1+ | 13+ | Functional* |
| Orange Pi Zero 3 | 5.15+ | 1.6+ | 11+ | Tested |
| ODROID-C4 | 5.10+ | 1.6+ | 10+ | Tested |
| Luckfox Lyra | 5.10 | 1.6 | 11 | Beta |

\* **Trixie Note**: Builds and runs but has known issues with health status display causing screen glitching. See [CHANGELOG.md](CHANGELOG.md) for details.

---

*Document Version: 1.1.0*
*Last Updated: 2025-12-21*
