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

# Standalone vs. embedded-in-ThumbyOne build.
#
#   THUMBYONE_SLOT_MODE is set by the ThumbyOne top-level build
#   when it pulls this firmware in via add_subdirectory. In that
#   mode the parent project has already configured the Pico SDK,
#   called project(), and will consume our ELF directly — we
#   skip the top-level scaffolding and the standalone UF2 output.
#
#   When THUMBYONE_SLOT_MODE is unset, this file configures the
#   SDK + project itself, just like before. The standalone build
#   at `cmake -B build_device -S device` still works unchanged.
if (NOT THUMBYONE_SLOT_MODE)
    set(PICO_BOARD tinycircuits_thumby_color_rp2350)
    set(PICO_PLATFORM rp2350)
    include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

    project(thumbynes_device C CXX ASM)
    set(CMAKE_C_STANDARD 11)

    pico_sdk_init()
endif()

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
    # Redirect nofrendo's state.c stdio calls (fopen/fwrite/fread/
    # fseek/fclose) through device/thumby_state_bridge.[ch] which
    # wraps FatFs's f_open / f_write / etc. The host build uses
    # libc stdio directly.
    THUMBY_STATE_BRIDGE
)
target_include_directories(nofrendo PRIVATE ${CMAKE_CURRENT_LIST_DIR})
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

# ----------------------------------------------------------------------
# Vendored smsplus SMS / Game Gear core (GPLv2 — see vendor/VENDORING.md)
# ----------------------------------------------------------------------
set(SMSPLUS_DIR ${NES_ROOT}/vendor/smsplus)
file(GLOB SMSPLUS_TOP_SRC   CONFIGURE_DEPENDS ${SMSPLUS_DIR}/*.c)
file(GLOB SMSPLUS_CPU_SRC   CONFIGURE_DEPENDS ${SMSPLUS_DIR}/cpu/*.c)
file(GLOB SMSPLUS_SOUND_SRC CONFIGURE_DEPENDS ${SMSPLUS_DIR}/sound/*.c)

# Build the SZHVC generator with the host compiler (not the cross
# toolchain) and run it to produce the flash-resident flag tables.
set(SMS_SZHVC_C   ${CMAKE_CURRENT_BINARY_DIR}/sms_z80_szhvc_data.c)
set(SMS_SZHVC_GEN ${CMAKE_CURRENT_BINARY_DIR}/sms_szhvc_gen)
add_custom_command(
    OUTPUT  ${SMS_SZHVC_GEN}
    COMMAND cc -O2 -o ${SMS_SZHVC_GEN} ${NES_ROOT}/tools/gen_sms_z80_szhvc.c
    DEPENDS ${NES_ROOT}/tools/gen_sms_z80_szhvc.c
    VERBATIM
)
add_custom_command(
    OUTPUT  ${SMS_SZHVC_C}
    COMMAND ${SMS_SZHVC_GEN} ${SMS_SZHVC_C}
    DEPENDS ${SMS_SZHVC_GEN}
    VERBATIM
)

add_library(smsplus STATIC
    ${SMSPLUS_TOP_SRC}
    ${SMSPLUS_CPU_SRC}
    ${SMSPLUS_SOUND_SRC}
    ${SMS_SZHVC_C}
)

# ----------------------------------------------------------------------
# Vendored peanut_gb (single-header DMG core) + minigb_apu
# ----------------------------------------------------------------------
set(PEANUT_DIR ${NES_ROOT}/vendor/peanut_gb)
add_library(peanut_gb STATIC ${PEANUT_DIR}/minigb_apu.c)
target_include_directories(peanut_gb PUBLIC ${PEANUT_DIR})
target_compile_options(peanut_gb PRIVATE
    -O3
    -Wno-implicit-fallthrough
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -fno-strict-aliasing
)
target_include_directories(smsplus PUBLIC
    ${SMSPLUS_DIR}
    ${SMSPLUS_DIR}/cpu
    ${SMSPLUS_DIR}/sound
)
target_compile_options(smsplus PRIVATE
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
# Same state-save bridge nofrendo uses — see the comment above.
# Also redirect IRAM_ATTR to a linker section the pico-sdk copies into
# SRAM at boot, so smsplus's `z80_execute()` (tagged in vendor/smsplus/
# cpu/z80.c) runs from RAM instead of XIP flash — same trick nofrendo
# uses for nes6502_execute() / ppu_renderline().
target_compile_definitions(smsplus PRIVATE
    THUMBY_STATE_BRIDGE
    "IRAM_ATTR=__attribute__((section(\".time_critical.sms\")))"
)
target_include_directories(smsplus PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Cross-platform glue lives outside the device directory; build it
# here as a small static lib.
add_library(thumbynes_core STATIC
    ${NES_ROOT}/src/nes_core.c
    ${NES_ROOT}/src/sms_core.c
    ${NES_ROOT}/src/gb_core.c
)
target_include_directories(thumbynes_core PUBLIC
    ${NES_ROOT}/src
    ${CMAKE_CURRENT_LIST_DIR}     # for thumby_state_bridge.h
)
target_compile_definitions(thumbynes_core PRIVATE THUMBY_STATE_BRIDGE)
target_link_libraries(thumbynes_core PUBLIC nofrendo smsplus peanut_gb)

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
    nes_thumb.c
    nes_menu.c
    nes_battery.c
    thumby_state_bridge.c
    nes_run.c
    sms_run.c
    gb_run.c
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
    hardware_adc
    tinyusb_device
    tinyusb_board
)
pico_enable_stdio_usb(nesrun_device 0)
pico_enable_stdio_uart(nesrun_device 0)

if (THUMBYONE_SLOT_MODE)
    # Embedded in ThumbyOne's NES partition.
    #
    # - Flash-disk offset is overridden to point at the shared FAT
    #   region in ThumbyOne's flash layout (0x660000, 9.6 MB).
    #   Without this override, FatFs would try to read/write
    #   inside the NES partition itself (because the default
    #   FLASH_DISK_OFFSET of 0x100000 is inside the 1 MB NES
    #   slot when rom_chain_image's ATRANS mapping is active),
    #   and the auto-reformat-on-mount-failure would erase our
    #   own code. See common/slot_layout.h in the ThumbyOne repo
    #   for the canonical values.
    target_compile_definitions(nesrun_device PRIVATE
        FLASH_DISK_OFFSET=0x660000u
        FLASH_DISK_SIZE=0x9A0000u   # 9.6 MB
    )
    # UF2 family: "absolute" so ThumbyOne's combiner treats these
    # blocks as direct-address flash writes. The image itself is
    # still linked at 0x10000000; the parent combiner rebases the
    # UF2 block target addresses to the NES partition offset.
    pico_set_uf2_family(nesrun_device "absolute")
    pico_add_extra_outputs(nesrun_device)
else()
    # Standalone: emit the normal UF2 alongside the ELF.
    pico_add_extra_outputs(nesrun_device)
endif()
