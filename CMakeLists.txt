cmake_minimum_required(VERSION 3.10)
project(profinet-monitor VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Core sources
set(SOURCES_CORE
    src/main.c
    src/config/config.c
    src/db/database.c
    src/db/db_modules.c
    src/db/db_events.c
    src/db/db_alarms.c
    src/db/db_actuators.c
    src/utils/logger.c
    src/platform/board_detect.c
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
    src/sensors/drivers/driver_turbidity.c   # Deprecated: use analog_sensor.c
    src/sensors/drivers/driver_tds.c         # Deprecated: use analog_sensor.c
    src/sensors/drivers/driver_ph.c          # Deprecated: use analog_sensor.c
    src/sensors/drivers/driver_hx711.c
    src/sensors/drivers/driver_float_switch.c
    src/sensors/drivers/driver_solenoid.c    # Deprecated: use relay_output.c
    src/sensors/drivers/driver_pump.c        # Deprecated: use relay_output.c
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
    src/tui/pages/page_system.c
    src/tui/pages/page_sensors.c
    src/tui/pages/page_network.c
    src/tui/pages/page_profinet.c
    src/tui/pages/page_status.c
    src/tui/pages/page_alarms.c
    src/tui/pages/page_logging.c
)

set(SOURCES
    ${SOURCES_CORE}
    ${SOURCES_DRIVERS_NEW}
    ${SOURCES_DRIVERS_LEGACY}
    ${SOURCES_SENSORS}
    ${SOURCES_SUBSYSTEMS}
    ${SOURCES_TUI}
)

find_package(Threads REQUIRED)
find_package(CURL QUIET)
find_package(PkgConfig REQUIRED)

if(NOT CURL_FOUND)
    message(WARNING "CURL not found - remote logging will be disabled")
endif()

pkg_check_modules(NCURSES REQUIRED ncurses)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(CJSON libcjson)

# libgpiod - modern GPIO interface (preferred over deprecated sysfs)
pkg_check_modules(GPIOD libgpiod)
if(GPIOD_FOUND)
    message(STATUS "libgpiod found - using modern GPIO interface")
else()
    message(WARNING "libgpiod not found - falling back to deprecated sysfs GPIO")
endif()

# systemd integration for sd_notify and watchdog
pkg_check_modules(SYSTEMD libsystemd)

find_library(PNET_LIBRARY pnet HINTS /usr/local/lib)
find_path(PNET_INCLUDE_DIR pnet_api.h HINTS /usr/local/include)

if(PNET_LIBRARY AND PNET_INCLUDE_DIR)
    set(HAVE_PNET TRUE)
else()
    set(HAVE_PNET FALSE)
    message(WARNING "p-net not found")
endif()

find_library(TINYEXPR_LIBRARY tinyexpr HINTS /usr/local/lib)

add_executable(profinet-monitor ${SOURCES})

if(HAVE_PNET)
    target_compile_definitions(profinet-monitor PRIVATE HAVE_PNET=1)
    target_include_directories(profinet-monitor PRIVATE ${PNET_INCLUDE_DIR})
    target_link_libraries(profinet-monitor ${PNET_LIBRARY})
endif()

if(TINYEXPR_LIBRARY)
    target_compile_definitions(profinet-monitor PRIVATE HAVE_TINYEXPR=1)
    target_link_libraries(profinet-monitor ${TINYEXPR_LIBRARY})
endif()

if(CJSON_FOUND)
    target_compile_definitions(profinet-monitor PRIVATE HAVE_CJSON=1)
    target_include_directories(profinet-monitor PRIVATE ${CJSON_INCLUDE_DIRS})
    target_link_libraries(profinet-monitor ${CJSON_LIBRARIES})
endif()

# libgpiod for modern GPIO support
if(GPIOD_FOUND)
    target_compile_definitions(profinet-monitor PRIVATE HAVE_GPIOD=1)
    target_include_directories(profinet-monitor PRIVATE ${GPIOD_INCLUDE_DIRS})
    target_link_libraries(profinet-monitor ${GPIOD_LIBRARIES})
endif()

# systemd for sd_notify and watchdog support
if(SYSTEMD_FOUND)
    target_compile_definitions(profinet-monitor PRIVATE HAVE_SYSTEMD=1)
    target_include_directories(profinet-monitor PRIVATE ${SYSTEMD_INCLUDE_DIRS})
    target_link_libraries(profinet-monitor ${SYSTEMD_LIBRARIES})
endif()

target_include_directories(profinet-monitor PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/drivers
    ${CMAKE_SOURCE_DIR}/src/sensors
    ${NCURSES_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)

if(CURL_FOUND)
    target_compile_definitions(profinet-monitor PRIVATE HAVE_CURL=1)
    target_include_directories(profinet-monitor PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(profinet-monitor ${CURL_LIBRARIES})
endif()

target_link_libraries(profinet-monitor
    ${NCURSES_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    Threads::Threads
    m
)

install(TARGETS profinet-monitor DESTINATION bin)

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
