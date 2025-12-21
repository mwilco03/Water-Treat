# Changelog

All notable changes to the Water Treatment RTU project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.2] - 2025-12-21

### Platform Testing

- **Raspberry Pi OS Lite 13 (Trixie)**: Tested - functional with known issues
  - Build and installation: Working
  - Core functionality: Operational
  - Status: Usable but not fully stable

### Added

- **bootstrap.sh**: New single-command bootstrap script for quick project setup
  - Pre-flight checks verify system requirements before build
  - Handles dependency installation, build, and optional install
  - Usage: `./scripts/bootstrap.sh` (build) or `./scripts/bootstrap.sh --install`

### Fixed

- **INSTALL.md directory mismatch**: Documentation referenced `/var/lib/profinet-monitor` but application uses `/var/lib/water-treat`. Fixed directory paths in post-installation instructions.

- **TUI screen corruption from logging**: Console logging (stderr) was corrupting ncurses display. Changed logger to file-only mode for both TUI and daemon modes. Errors now go to log file only, preserving terminal integrity. (`main.c:637-647`)

- **Health file error spam**: Error message spammed every 10 seconds when directory didn't exist. Now:
  - Rate-limited: First error logged, then only every 5 minutes
  - Auto-creates directory if possible (graceful degradation)
  - Changed log level from ERROR to WARNING
  - (`health_check.c:368-416`)

- **Hardware interface error spam**: I2C, SPI, GPIO, and 1-Wire errors could spam logs during sensor polling when hardware was disconnected or flaky. Now rate-limited:
  - First error logged immediately, subsequent errors only every 5 minutes
  - Error count tracked per subsystem (I2C read/write, SPI, GPIO read/write, 1-Wire)
  - Reset on successful operation
  - (`hw_interface.c:14-45, 81-152, 208-223, 367-424, 488-524`)

- **Web polling error spam**: HTTP/curl errors and JSON parsing failures could spam logs when network was unreliable. Now rate-limited with same pattern.
  - (`driver_web_poll.c:17-23, 84-141, 189-207`)

- **Logger silent failure on missing directory**: Log file would silently fail to open if parent directory didn't exist. Now:
  - Auto-creates parent directories before opening file
  - Warns to stderr if directory creation fails
  - (`logger.c:23-63, 108-121`)

### Changed

- **Health metrics default path**: Changed from `/var/lib/water-treat/health.prom` to `/run/water-treat/health.prom` (tmpfs). Benefits:
  - Reduces SD card wear on embedded systems (RAM-backed)
  - 8,640 writes/day now go to tmpfs instead of flash
  - Cleared on reboot (appropriate for metrics)
  - (`config.c:167-174`)

### Known Issues

- **TUI Screen Glitching**: Main interface may flicker on Trixie (ncurses 6.5+). Yeddo's fixes (`770b08e`, `be229e3`) for login/wizard pulsing are present. The 100ms main loop refresh is by design. Further investigation needed for full compatibility.

### Notes

This release marks initial testing on Debian Trixie (testing). While the application builds and runs, users may experience visual artifacts in the terminal UI. The glitching is cosmetic and does not affect core functionality.

### Embedded System Considerations

This release includes optimizations for memory-constrained embedded systems:
- Health metrics now use tmpfs by default (no SD card wear)
- Rate-limited error logging reduces log file growth
- Pre-flight checks in bootstrap.sh verify system requirements early

## [0.2.1] - 2025-12-19

### Added

- Progressive disclosure I/O configuration wizard
- Optimized `install-deps.sh` script

### Fixed

- p-net CMake build failure by pinning to v0.2.0

## [0.2.0] - 2025-12-18

### Added

- Core architecture principles documentation
- Unified driver architecture following Linux IIO / Zephyr patterns
- Modern GPIO HAL with libgpiod support and sysfs fallback
- Relay output driver replacing legacy pump/solenoid drivers
- Analog sensor driver replacing legacy pH/TDS/turbidity drivers

### Changed

- Migrated GPIO interface to libgpiod (deprecated sysfs GPIO)
- Unified analog sensor abstraction layer

## [0.1.0] - 2024-12-16

### Added

- Initial release
- PROFINET I/O Device implementation using p-net library
- Sensor abstraction layer with multi-protocol support (I2C, SPI, 1-Wire, GPIO)
- Actuator control with GPIO relay support
- ncurses-based TUI for configuration and diagnostics
- SQLite database for configuration persistence
- Offline autonomy with last-state-saved functionality
- Store & forward logging for remote unavailability
- Authentication system with local and controller-synced users
- Alarm management subsystem
- Health check monitoring
- systemd service integration
