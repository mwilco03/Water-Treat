cmake_minimum_required(VERSION 3.10)
project(water-treat VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ------------------------------------------------------------------------------
# Dynamic Version from Git
# ------------------------------------------------------------------------------
# What: Extract version from git tags at build time
# Why: Single source of truth - no hardcoded version strings
# Edge cases: No tags = use PROJECT_VERSION, no git = use PROJECT_VERSION
#
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)

if(GIT_RESULT EQUAL 0 AND GIT_VERSION)
    # Strip leading 'v' if present (v0.1.0 -> 0.1.0)
    string(REGEX REPLACE "^v" "" VERSION_STRING "${GIT_VERSION}")
else()
    set(VERSION_STRING "${CMAKE_PROJECT_VERSION}-dev")
endif()

message(STATUS "Building ${CMAKE_PROJECT_NAME} version: ${VERSION_STRING}")

# Compiler flags per DEVELOPMENT_GUIDELINES.md Part 6.1:
# -Wall -Wextra: Enable most warnings
# -Wpedantic: Enable strict ISO C compliance warnings
# -Wformat=2 -Wformat-security: Enhanced format string checking
# -Wnull-dereference: Warn about null pointer dereferences
# -Wno-unused-parameter: Allow unused parameters (common in callbacks)
# -Wno-stringop-truncation: Suppress strncpy truncation warnings (handled by SAFE_STRNCPY)
# -Wno-format-truncation: Suppress snprintf truncation warnings (acceptable for display strings)
# -Wno-sign-compare: Suppress sign comparison warnings in MIN/MAX macros with mixed types
# -fstack-protector-strong: Enable stack buffer overflow protection
#
# Note: -Werror is enabled for Release builds to catch issues before deployment
#
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Wformat=2 -Wformat-security")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wnull-dereference -fstack-protector-strong")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter -Wno-stringop-truncation -Wno-format-truncation -Wno-sign-compare")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -Werror")

# Core sources
set(SOURCES_CORE
    src/main.c
    src/config/config.c
    src/config/config_validate.c
    src/config/config_resolver.c
    src/db/database.c
    src/db/db_modules.c
    src/db/db_events.c
    src/db/db_alarms.c
    src/db/db_actuators.c
    src/utils/logger.c
    src/platform/board_detect.c
    src/platform/hw_discover.c
    src/auth/auth.c
)

# New unified driver architecture (following Linux IIO / Zephyr patterns)
set(SOURCES_DRIVERS_NEW
    src/sensors/analog/analog_sensor.c      # Replaces driver_ph, driver_tds, driver_turbidity
    src/drivers/digital/relay_output.c      # Replaces driver_pump, driver_solenoid
    src/drivers/bus/gpio_hal.c              # Modern GPIO HAL (libgpiod or sysfs fallback)
)

# Legacy drivers (to be migrated to new architecture)
set(SOURCES_DRIVERS_LEGACY
    src/sensors/hardware/hw_interface.c
    src/sensors/drivers/driver_ads1115.c
    src/sensors/drivers/driver_mcp3008.c
    src/sensors/drivers/driver_ds18b20.c
    src/sensors/drivers/driver_dht22.c
    src/sensors/drivers/driver_web_poll.c
    src/sensors/drivers/driver_tcs34725.c
    src/sensors/drivers/driver_hx711.c
    src/sensors/drivers/driver_float_switch.c
    src/sensors/drivers/driver_bme280.c
    src/sensors/drivers/driver_jsn_sr04t.c
)

# Sensor manager
set(SOURCES_SENSORS
    src/sensors/sensor_instance.c
    src/sensors/sensor_manager.c
    src/sensors/formula_evaluator.c
)

# Subsystems
set(SOURCES_SUBSYSTEMS
    src/logging/data_logger.c
    src/alarms/alarm_manager.c
    src/actuators/actuator_manager.c
    src/profinet/profinet_manager.c
    src/profinet/profinet_callbacks.c
    src/health/health_check.c
)

# TUI
set(SOURCES_TUI
    src/tui/tui_common.c
    src/tui/tui_main.c
    src/tui/widgets/widget_dialog.c
    src/tui/dialogs/dialog_helpers.c
    src/tui/dialogs/dialog_sensor.c
    src/tui/dialogs/dialog_alarm.c
    src/tui/dialogs/dialog_actuator.c
    src/tui/dialogs/dialog_io_wizard.c
    src/tui/pages/page_system.c
    src/tui/pages/page_sensors.c
    src/tui/pages/page_network.c
    src/tui/pages/page_profinet.c
    src/tui/pages/page_status.c
    src/tui/pages/page_alarms.c
    src/tui/pages/page_logging.c
    src/tui/pages/page_wizard.c
    src/tui/pages/page_actuators.c
    src/tui/pages/page_login.c
)

# HAL (conditionally included based on features)
set(SOURCES_HAL)

set(SOURCES
    ${SOURCES_CORE}
    ${SOURCES_DRIVERS_NEW}
    ${SOURCES_DRIVERS_LEGACY}
    ${SOURCES_SENSORS}
    ${SOURCES_SUBSYSTEMS}
    ${SOURCES_TUI}
    ${SOURCES_HAL}
)

find_package(Threads REQUIRED)
find_package(CURL QUIET)

# PkgConfig may not be available during cross-compilation
find_package(PkgConfig QUIET)

if(NOT CURL_FOUND)
    message(WARNING "CURL not found - remote logging will be disabled")
endif()

# For cross-compilation, pkg-config may not be available
# Libraries should be provided via toolchain file or sysroot
if(PKG_CONFIG_FOUND)
    pkg_check_modules(NCURSES REQUIRED ncurses)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    pkg_check_modules(CJSON libcjson)
else()
    message(WARNING "pkg-config not found - using default library names")
    # Fallback for cross-compilation: assume libraries are in sysroot
    set(NCURSES_LIBRARIES ncurses)
    set(SQLITE3_LIBRARIES sqlite3)
    set(NCURSES_FOUND TRUE)
    set(SQLITE3_FOUND TRUE)
endif()

# libgpiod - modern GPIO interface (preferred over deprecated sysfs)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(GPIOD libgpiod)
    pkg_check_modules(SYSTEMD libsystemd)
endif()

if(GPIOD_FOUND)
    message(STATUS "libgpiod found - using modern GPIO interface")
else()
    message(WARNING "libgpiod not found - falling back to deprecated sysfs GPIO")
endif()

# ============================================================================
# Optional LED Status Indicator Support (WS2812B/WS2811)
# ============================================================================
option(ENABLE_LED_SUPPORT "Enable WS2812B LED status indicator support" OFF)

if(ENABLE_LED_SUPPORT)
    # Check for rpi_ws281x library (Raspberry Pi specific)
    find_library(WS2811_LIBRARY ws2811 HINTS /usr/local/lib)
    find_path(WS2811_INCLUDE_DIR ws2811/ws2811.h HINTS /usr/local/include)

    if(WS2811_LIBRARY AND WS2811_INCLUDE_DIR)
        set(HAVE_RPI_WS281X TRUE)
        message(STATUS "rpi_ws281x found - enabling Pi-optimized LED backend")
    else()
        set(HAVE_RPI_WS281X FALSE)
        message(STATUS "rpi_ws281x not found - using SPI-only LED backend")
    endif()

    # Add LED HAL sources
    list(APPEND SOURCES_HAL
        src/hal/led_ws2812.c
        src/hal/led_backend_spi.c
        src/hal/led_status.c
    )

    # Add rpi_ws281x backend if available
    if(HAVE_RPI_WS281X)
        list(APPEND SOURCES_HAL src/hal/led_backend_rpi.c)
    endif()

    message(STATUS "LED status indicator support enabled")
else()
    message(STATUS "LED status indicator support disabled (use -DENABLE_LED_SUPPORT=ON to enable)")
endif()

# Re-assemble SOURCES with HAL (after optional LED files added)
set(SOURCES
    ${SOURCES_CORE}
    ${SOURCES_DRIVERS_NEW}
    ${SOURCES_DRIVERS_LEGACY}
    ${SOURCES_SENSORS}
    ${SOURCES_SUBSYSTEMS}
    ${SOURCES_TUI}
    ${SOURCES_HAL}
)

find_library(PNET_LIBRARY pnet HINTS /usr/local/lib)
find_path(PNET_INCLUDE_DIR pnet_api.h HINTS /usr/local/include)

if(PNET_LIBRARY AND PNET_INCLUDE_DIR)
    set(HAVE_PNET TRUE)
else()
    set(HAVE_PNET FALSE)
    message(WARNING "p-net not found")
endif()

find_library(TINYEXPR_LIBRARY tinyexpr HINTS /usr/local/lib)

add_executable(water-treat ${SOURCES})

# Pass version string as compile definition
target_compile_definitions(water-treat PRIVATE VERSION_STRING="${VERSION_STRING}")

if(HAVE_PNET)
    target_compile_definitions(water-treat PRIVATE HAVE_PNET=1)
    target_include_directories(water-treat PRIVATE ${PNET_INCLUDE_DIR})
    target_link_libraries(water-treat ${PNET_LIBRARY})
endif()

if(TINYEXPR_LIBRARY)
    target_compile_definitions(water-treat PRIVATE HAVE_TINYEXPR=1)
    target_link_libraries(water-treat ${TINYEXPR_LIBRARY})
endif()

if(CJSON_FOUND)
    target_compile_definitions(water-treat PRIVATE HAVE_CJSON=1)
    target_include_directories(water-treat PRIVATE ${CJSON_INCLUDE_DIRS})
    target_link_libraries(water-treat ${CJSON_LIBRARIES})
endif()

# libgpiod for modern GPIO support
if(GPIOD_FOUND)
    target_compile_definitions(water-treat PRIVATE HAVE_GPIOD=1)
    target_include_directories(water-treat PRIVATE ${GPIOD_INCLUDE_DIRS})
    target_link_libraries(water-treat ${GPIOD_LIBRARIES})
endif()

# systemd for sd_notify and watchdog support
if(SYSTEMD_FOUND)
    target_compile_definitions(water-treat PRIVATE HAVE_SYSTEMD=1)
    target_include_directories(water-treat PRIVATE ${SYSTEMD_INCLUDE_DIRS})
    target_link_libraries(water-treat ${SYSTEMD_LIBRARIES})
endif()

# LED status indicator support
if(ENABLE_LED_SUPPORT)
    target_compile_definitions(water-treat PRIVATE LED_SUPPORT=1)
    if(HAVE_RPI_WS281X)
        target_compile_definitions(water-treat PRIVATE HAVE_RPI_WS281X=1)
        target_include_directories(water-treat PRIVATE ${WS2811_INCLUDE_DIR})
        target_link_libraries(water-treat ${WS2811_LIBRARY})
    endif()
endif()

target_include_directories(water-treat PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/drivers
    ${CMAKE_SOURCE_DIR}/src/sensors
    ${CMAKE_SOURCE_DIR}/src/hal
    ${CMAKE_SOURCE_DIR}/src/platform
    ${NCURSES_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)

if(CURL_FOUND)
    target_compile_definitions(water-treat PRIVATE HAVE_CURL=1)
    target_include_directories(water-treat PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(water-treat ${CURL_LIBRARIES})
endif()

target_link_libraries(water-treat
    ${NCURSES_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    Threads::Threads
    m
)

install(TARGETS water-treat DESTINATION bin)

# ============================================================================
# Unit Tests
# ============================================================================
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()

    set(TEST_SOURCES
        tests/test_main.c
        tests/test_formula.c
        tests/test_calibration.c
        tests/test_alarms.c
        tests/test_profinet_data.c
        tests/test_config.c
    )

    # Tests that need formula evaluator
    set(TEST_DEPS
        src/sensors/formula_evaluator.c
        src/utils/logger.c
    )

    add_executable(run_tests ${TEST_SOURCES} ${TEST_DEPS})

    target_include_directories(run_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
    )

    target_link_libraries(run_tests m)

    if(TINYEXPR_LIBRARY)
        target_compile_definitions(run_tests PRIVATE HAVE_TINYEXPR=1)
        target_link_libraries(run_tests ${TINYEXPR_LIBRARY})
    endif()

    add_test(NAME unit_tests COMMAND run_tests)
endif()
