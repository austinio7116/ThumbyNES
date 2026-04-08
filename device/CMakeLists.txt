#
# ThumbyNES — Phase 3 device firmware build.
#
# Mirrors the ThumbyP8 device build. Pico SDK is pulled in BEFORE
# project() so it can install its toolchain.
#
# Build:
#   cd ThumbyNES
#   cmake -B build_device -S device \
#         -DPICO_SDK_PATH=/home/maustin/mp-thumby/lib/pico-sdk
#   cmake --build build_device -j8
#
# Output: build_device/nesrun_device.uf2
# Flash:  hold DOWN dpad while powering on to enter BOOTSEL,
#         then drag the .uf2 onto the RPI-RP2350 drive.
#
cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD tinycircuits_thumby_color_rp2350)
set(PICO_PLATFORM rp2350)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(thumbynes_device C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(NES_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# ----------------------------------------------------------------------
# Vendored Nofrendo NES core (GPLv2 — see vendor/VENDORING.md)
# ----------------------------------------------------------------------
set(NOFRENDO_DIR ${NES_ROOT}/vendor/nofrendo)
file(GLOB NOFRENDO_NES_SRC    CONFIGURE_DEPENDS ${NOFRENDO_DIR}/nes/*.c)
file(GLOB NOFRENDO_MAPPER_SRC CONFIGURE_DEPENDS ${NOFRENDO_DIR}/mappers/*.c)

add_library(nofrendo STATIC
    ${NOFRENDO_NES_SRC}
    ${NOFRENDO_MAPPER_SRC}
    ${NOFRENDO_DIR}/nofrendo.c
)
target_include_directories(nofrendo PUBLIC
    ${NOFRENDO_DIR}
    ${NOFRENDO_DIR}/nes
)
target_compile_definitions(nofrendo PRIVATE
    # Map IRAM_ATTR to a section attribute the pico-sdk linker
    # script copies into SRAM at boot. The hot 6502 + PPU loops
    # then run from RAM instead of XIP flash, dodging cache misses.
    "IRAM_ATTR=__attribute__((section(\".time_critical.nes\")))"
)
target_compile_options(nofrendo PRIVATE
    -O3
    -Wno-array-bounds
    -Wno-format
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-pointer-sign
    -Wno-implicit-function-declaration
    -fno-strict-aliasing
)

# Cross-platform glue (src/nes_core.c) lives outside the device
# directory; build it here as a small static lib.
add_library(thumbynes_core STATIC ${NES_ROOT}/src/nes_core.c)
target_include_directories(thumbynes_core PUBLIC ${NES_ROOT}/src)
target_link_libraries(thumbynes_core PUBLIC nofrendo)

# ----------------------------------------------------------------------
# nesrun_device — Phase 3 firmware: lobby + picker, no emulator yet.
# ----------------------------------------------------------------------
add_executable(nesrun_device
    nes_device_main.c
    nes_lcd_gc9107.c
    nes_buttons.c
    nes_audio_pwm.c
    nes_flash_disk.c
    nes_msc.c
    nes_picker.c
    nes_font.c
    nes_run.c
    usb_descriptors.c
    fatfs/ff.c
    fatfs/ffsystem.c
    fatfs/ffunicode.c
    fatfs/diskio.c
)
target_include_directories(nesrun_device PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fatfs
)
target_compile_options(nesrun_device PRIVATE -O3 -Wno-unused-parameter)
target_link_libraries(nesrun_device PRIVATE
    thumbynes_core
    pico_stdlib
    pico_unique_id
    hardware_clocks
    hardware_spi
    hardware_dma
    hardware_gpio
    hardware_pwm
    hardware_irq
    hardware_flash
    hardware_sync
    tinyusb_device
    tinyusb_board
)
pico_enable_stdio_usb(nesrun_device 0)
pico_enable_stdio_uart(nesrun_device 0)
pico_add_extra_outputs(nesrun_device)
