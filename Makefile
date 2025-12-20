# Water-Treat Build System
#
# What: Convenience wrapper around the scripts in scripts/
# Why: Conventional interface - `make deps`, `make build`, etc.
#      Self-documenting via `make help`
#
# Usage:
#   make help       - Show available targets
#   make deps       - Install dependencies (requires sudo)
#   make build      - Build in debug mode
#   make release    - Build in release mode
#   make install    - Install to system (requires sudo)
#   make uninstall  - Remove from system (requires sudo)
#   make clean      - Remove build directory
#
# Developers can use CMake directly if they prefer.
#

.PHONY: help deps build release debug test clean install uninstall purge

# Default target - show help
help:
	@echo "Water-Treat Build System"
	@echo ""
	@echo "Usage: make <target>"
	@echo ""
	@echo "Setup:"
	@echo "  deps         Install build dependencies (requires sudo)"
	@echo ""
	@echo "Build:"
	@echo "  build        Build in debug mode (default)"
	@echo "  debug        Build in debug mode"
	@echo "  release      Build in release mode"
	@echo "  test         Build with tests enabled"
	@echo "  clean        Remove build directory"
	@echo ""
	@echo "Install:"
	@echo "  install      Install to system (requires sudo)"
	@echo "  uninstall    Remove from system, keep data (requires sudo)"
	@echo "  purge        Remove from system including data (requires sudo)"
	@echo ""
	@echo "Examples:"
	@echo "  sudo make deps      # Install dependencies"
	@echo "  make release        # Build optimized binary"
	@echo "  sudo make install   # Install to system"
	@echo ""
	@echo "For developers:"
	@echo "  CMake can be used directly:"
	@echo "    mkdir build && cd build"
	@echo "    cmake -G Ninja .."
	@echo "    ninja"

# Install dependencies (requires root)
deps:
	@./scripts/install-deps.sh

# Build targets
build: debug

debug:
	@./scripts/build.sh debug

release:
	@./scripts/build.sh release

test:
	@./scripts/build.sh debug test

# Clean build directory
clean:
	@echo "Removing build directory..."
	@rm -rf build/
	@echo "Done."

# Installation targets (require root)
install:
	@./scripts/install.sh

uninstall:
	@./scripts/uninstall.sh

purge:
	@./scripts/uninstall.sh --purge

# Rebuild from scratch
rebuild: clean build

# Full release rebuild
rebuild-release: clean release
