# RP2040 WS2812 LED Controller for Water-Treat RTU
# Provides USB CDC serial interface for LED control from host SBC

cmake_minimum_required(VERSION 3.13)

# Initialize the SDK (must be before project())
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(water_treat_led_controller C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Add executable
add_executable(led_controller
    main.c
)

# Generate PIO header from .pio file
pico_generate_pio_header(led_controller ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

# Link required libraries
target_link_libraries(led_controller
    pico_stdlib
    hardware_pio
    pico_unique_id
)

# Enable USB serial (CDC), disable UART
pico_enable_stdio_usb(led_controller 1)
pico_enable_stdio_uart(led_controller 0)

# Create UF2 output file
pico_add_extra_outputs(led_controller)

# Set USB device descriptors
target_compile_definitions(led_controller PRIVATE
    USBD_VID=0x2E8A
    USBD_PID=0x000A
    USBD_MANUFACTURER="WaterTreat"
    USBD_PRODUCT="LED Controller"
)
