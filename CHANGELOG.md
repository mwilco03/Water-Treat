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

### Known Issues

- **Health Status Display Glitch**: The TUI causes screen rendering issues (flickering/glitching) on Trixie. Investigation findings:
  - **Root cause**: Aggressive 100ms full-redraw loop in `tui_main.c:286-289` combined with multiple `wrefresh()` calls per frame
  - **ncurses difference**: Trixie ships ncurses 6.5+ which has stricter terminal handling than Bookworm's ncurses 6.4
  - **Contributing factors**: Overlapping metric collection between health_check.c (background thread) and page_status.c
  - **Workaround**: Application remains functional despite visual artifacts
  - **Potential fix**: Migrate to `wnoutrefresh()` + single `doupdate()` pattern, conditional refresh on data change

### Notes

This release marks initial testing on Debian Trixie (testing). While the application builds and runs, users may experience visual artifacts in the terminal UI. The glitching is cosmetic and does not affect core functionality.

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
