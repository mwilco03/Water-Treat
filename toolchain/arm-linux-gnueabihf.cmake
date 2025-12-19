# CMake Toolchain File for 32-bit ARM (armhf)
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain/arm-linux-gnueabihf.cmake ..
#
# Prerequisites:
#   sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
#
# For cross-compiled libraries, set:
#   -DCMAKE_SYSROOT=/path/to/sysroot
#   -DCMAKE_FIND_ROOT_PATH=/path/to/sysroot

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Cross compiler
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Target environment (adjust if using custom sysroot)
# set(CMAKE_SYSROOT /path/to/arm-sysroot)
# set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config for cross-compilation
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")

# Compiler flags for Raspberry Pi 3/4 (Cortex-A53/A72)
# Adjust for your target:
#   Pi Zero/1: -march=armv6 -mfpu=vfp
#   Pi 2:      -march=armv7-a -mfpu=neon-vfpv4
#   Pi 3/4:    -march=armv8-a -mfpu=neon-fp-armv8
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")

# Strip binaries for smaller size
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
