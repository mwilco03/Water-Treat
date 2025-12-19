# CMake Toolchain File for ARMv6 (Raspberry Pi 1, Pi Zero original)
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain/armv6-linux-gnueabihf.cmake ..
#
# Prerequisites:
#   sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
#
# Target boards:
#   - Raspberry Pi 1 Model A/B/A+/B+
#   - Raspberry Pi Zero (original, not Zero 2 W)
#   - Raspberry Pi Compute Module 1

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Cross compiler (same as armhf, different flags)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config for cross-compilation
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")

# Compiler flags for ARMv6 (Pi Zero/Pi 1)
# Uses VFP (not NEON - ARMv6 doesn't have NEON)
set(CMAKE_C_FLAGS_INIT "-march=armv6 -mfpu=vfp -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv6 -mfpu=vfp -mfloat-abi=hard")

# Strip binaries for smaller size (important for Pi Zero's limited resources)
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Os -DNDEBUG")
