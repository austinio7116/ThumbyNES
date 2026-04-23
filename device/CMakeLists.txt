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
target_compile_definitions(peanut_gb PUBLIC PEANUT_FULL_GBC_SUPPORT=1)
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

# ----------------------------------------------------------------------
# Vendored PicoDrive MD/Genesis core (LGPLv2 — see vendor/VENDORING.md)
#   Same patch profile as the host build (FAME_BIG_ENDIAN + MCD stubs
#   + heap-allocated PicoMem). Additionally for device:
#     - YM2612_TABLES_IN_FLASH: precomputed YM2612 log tables (213 KB)
#       linked as const flash data instead of runtime-built on heap.
#     - IRAM_ATTR section redirect so hot render/CPU paths run from
#       SRAM (copy-in-at-boot) instead of XIP flash.
# ----------------------------------------------------------------------
set(PICODRIVE_DIR ${NES_ROOT}/vendor/picodrive)
file(GLOB PICODRIVE_PICO_SRC   CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/*.c)
file(GLOB PICODRIVE_SOUND_SRC  CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/sound/*.c)
file(GLOB PICODRIVE_CARTHW_SRC CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/carthw/*.c)
file(GLOB PICODRIVE_SVP_SRC    CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/carthw/svp/*.c)
file(GLOB PICODRIVE_PICOHW_SRC CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/pico/*.c)
file(GLOB PICODRIVE_UNZIP_SRC  CONFIGURE_DEPENDS ${PICODRIVE_DIR}/unzip/*.c)
list(FILTER PICODRIVE_PICO_SRC EXCLUDE REGEX "pico/debugCPU\\.c$")
list(FILTER PICODRIVE_PICO_SRC EXCLUDE REGEX "pico/pico_cmn\\.c$")
list(FILTER PICODRIVE_SVP_SRC  EXCLUDE REGEX "carthw/svp/compiler\\.c$")
# draw2.c is the alternate fast renderer (POPT_ALT_RENDERER). We use
# the normal per-line renderer in draw.c exclusively. Excluding draw2
# saves ~103 KB BSS (PicoDraw2FB_ 82K + HighCache2A/B 19K). Stubs for
# the three externally-visible functions live in thumby_platform.c.
list(FILTER PICODRIVE_PICO_SRC EXCLUDE REGEX "pico/draw2\\.c$")
# ym2413.c + emu2413/*.c are the Japanese Master System FM add-on.
# Never used by MD (cart uses YM2612 instead) and an SMS-specific
# corner case. Excluding saves ~267 KB (ym2413 tll_table 128K +
# emu2413 internal state + code). Stubs for OPLL_* and YM2413_* and
# PsndDoYM2413 in thumby_platform.c.
list(FILTER PICODRIVE_SOUND_SRC EXCLUDE REGEX "sound/ym2413\\.c$")

# Generate YM2612 log tables (213 KB) at build time — linked as
# const flash so the MD core doesn't need 220 KB of heap at init.
set(MD_YM_TAB_C   ${CMAKE_CURRENT_BINARY_DIR}/md_ym2612_tab_data.c)
set(MD_YM_TAB_GEN ${CMAKE_CURRENT_BINARY_DIR}/md_ym2612_tab_gen)
add_custom_command(
    OUTPUT  ${MD_YM_TAB_GEN}
    COMMAND cc -O2 -o ${MD_YM_TAB_GEN} ${NES_ROOT}/tools/gen_md_ym2612_tab.c -lm
    DEPENDS ${NES_ROOT}/tools/gen_md_ym2612_tab.c
    VERBATIM
)
add_custom_command(
    OUTPUT  ${MD_YM_TAB_C}
    COMMAND ${MD_YM_TAB_GEN} ${MD_YM_TAB_C}
    DEPENDS ${MD_YM_TAB_GEN}
    VERBATIM
)

# Same pattern for cz80's 256 KB of SBC/ADC/SUB/ADD flag tables.
set(MD_CZ80_SZHVC_C   ${CMAKE_CURRENT_BINARY_DIR}/md_cz80_szhvc_data.c)
set(MD_CZ80_SZHVC_GEN ${CMAKE_CURRENT_BINARY_DIR}/md_cz80_szhvc_gen)
add_custom_command(
    OUTPUT  ${MD_CZ80_SZHVC_GEN}
    COMMAND cc -O2 -o ${MD_CZ80_SZHVC_GEN} ${NES_ROOT}/tools/gen_md_cz80_szhvc.c
    DEPENDS ${NES_ROOT}/tools/gen_md_cz80_szhvc.c
    VERBATIM
)
add_custom_command(
    OUTPUT  ${MD_CZ80_SZHVC_C}
    COMMAND ${MD_CZ80_SZHVC_GEN} ${MD_CZ80_SZHVC_C}
    DEPENDS ${MD_CZ80_SZHVC_GEN}
    VERBATIM
)

# FAME 68K JumpTable — 256 KB of function pointers, precomputed at
# build time and emitted as #include'd designated initialiser data
# that famec.c picks up under FAME_JUMPTABLE_IN_FLASH.
set(MD_FAME_JT_INC ${CMAKE_CURRENT_BINARY_DIR}/famec_jumptable_data.inc)
set(MD_FAME_JT_GEN ${CMAKE_CURRENT_BINARY_DIR}/md_fame_jt_gen)
add_custom_command(
    OUTPUT  ${MD_FAME_JT_GEN}
    COMMAND cc -O2 -I${NES_ROOT}
                -o ${MD_FAME_JT_GEN} ${NES_ROOT}/tools/gen_md_fame_jumptable.c
    DEPENDS ${NES_ROOT}/tools/gen_md_fame_jumptable.c
            ${PICODRIVE_DIR}/cpu/fame/famec_jumptable_init.inc
    VERBATIM
)
add_custom_command(
    OUTPUT  ${MD_FAME_JT_INC}
    COMMAND ${MD_FAME_JT_GEN} ${MD_FAME_JT_INC}
    DEPENDS ${MD_FAME_JT_GEN}
    VERBATIM
)
# famec.c #includes "famec_jumptable_data.inc" via relative path; the
# generated file lives in the build tree, so add the build dir to
# famec.c's include path. Apply the -O2 override from below and -I
# for the generated header together.
set_source_files_properties(
    ${PICODRIVE_DIR}/cpu/fame/famec.c
    PROPERTIES
        OBJECT_DEPENDS ${MD_FAME_JT_INC}
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}"
)

add_library(picodrive STATIC
    ${PICODRIVE_PICO_SRC}
    ${PICODRIVE_SOUND_SRC}
    ${PICODRIVE_CARTHW_SRC}
    ${PICODRIVE_SVP_SRC}
    ${PICODRIVE_PICOHW_SRC}
    ${PICODRIVE_UNZIP_SRC}
    ${PICODRIVE_DIR}/cpu/fame/famec.c
    ${PICODRIVE_DIR}/cpu/cz80/cz80.c
    ${PICODRIVE_DIR}/thumby_platform.c
    ${MD_YM_TAB_C}
    ${MD_CZ80_SZHVC_C}
)
target_include_directories(picodrive PRIVATE
    ${PICODRIVE_DIR}/pico
    ${PICODRIVE_DIR}/cpu
)
# Only the root — picodrive's own source uses `#include <cpu/...>`
# and `#include <pico/...>` against this root. PUBLIC-propagating
# the ${PICODRIVE_DIR}/pico path would shadow pico-sdk's `<pico.h>`
# (picodrive has its own pico/pico.h for the 68K core's public API).
target_include_directories(picodrive PUBLIC
    ${PICODRIVE_DIR}
)
target_compile_definitions(picodrive PUBLIC
    EMU_F68K
    _USE_CZ80
    NO_32X
    FAME_BIG_ENDIAN
    YM2612_TABLES_IN_FLASH
    CZ80_SZHVC_IN_FLASH
    FAME_JUMPTABLE_IN_FLASH  # 256 KB FAME opcode dispatch table → flash
    THUMBY_YM2413_EXCLUDED   # sound/ym2413.c not in build; stubs active
    THUMBY_DRAW2_EXCLUDED    # pico/draw2.c not in build; stubs active
    # Rename picodrive's z80 API so its symbols don't collide with
    # smsplus's z80 implementation at final link. Both libraries are
    # linked into nesrun_device, but only one runs per cart. The
    # rename is compile-only; call sites in pico.c / memory.c etc.
    # get the new names via these defines, smsplus keeps the originals.
    z80_init=pd_z80_init
    z80_reset=pd_z80_reset
    z80_exit=pd_z80_exit
    z80_read=pd_z80_read
    z80_pack=pd_z80_pack
    z80_unpack=pd_z80_unpack
    z80_debug=pd_z80_debug
)
target_compile_options(picodrive PRIVATE
    -O3
    # pico-sdk gcc targets M33 with 16-byte function alignment, but
    # cpu68k_map requires bit 0 clear for function pointers. Keep the
    # 2-byte align to match the host build.
    -falign-functions=2
    -fno-strict-aliasing
    -Wno-array-bounds
    -Wno-format
    -Wno-maybe-uninitialized
    -Wno-pointer-sign
    -Wno-sign-compare
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)
# FAME's 18k-line famec.c with its inlined opcode dispatch chokes
# gcc at -O3 (7+ min / 600 MB RSS). Upstream's Makefile drops this
# single file to -O2 — mirror that.
set_source_files_properties(
    ${PICODRIVE_DIR}/cpu/fame/famec.c
    PROPERTIES COMPILE_OPTIONS "-O2;-fno-expensive-optimizations"
)
# Route the 5 hot functions identified by the host profile (draw.c
# tile composition, VDP line finalize, FAME dispatch, cz80, YM2612
# channel render) into .time_critical.md so the pico-sdk linker
# script copies them into SRAM at boot. Same trick used for nofrendo
# and smsplus.
target_compile_definitions(picodrive PRIVATE
    "IRAM_ATTR=__attribute__((section(\".time_critical.md\")))"
)
# cart.c / state.c pull in zlib's crc32 + inflate. crc32 IS called at
# ROM-load time (rom_crc32 fires for carthw.cfg sections that only
# specify check_crc32, like the unlicensed multicarts) — it has a
# real implementation in thumby_platform.c. The inflate/gz* paths
# only fire for ZIP/CSO/save-state compression which we don't wire
# up, so stubbing them to address-0 is safe — the linker just needs
# the symbols to resolve.
target_link_options(picodrive INTERFACE
    "-Wl,--defsym=inflateInit2_=0"
    "-Wl,--defsym=inflate=0"
    "-Wl,--defsym=inflateEnd=0"
    "-Wl,--defsym=inflateReset=0"
    "-Wl,--defsym=gzwrite=0"
    "-Wl,--defsym=gzread=0"
    "-Wl,--defsym=gzopen=0"
    "-Wl,--defsym=gzeof=0"
    "-Wl,--defsym=gzseek=0"
    "-Wl,--defsym=gzclose=0"
    "-Wl,--defsym=gzsetparams=0"
)

# Cross-platform glue lives outside the device directory; build it
# here as a small static lib.
add_library(thumbynes_core STATIC
    ${NES_ROOT}/src/nes_core.c
    ${NES_ROOT}/src/sms_core.c
    ${NES_ROOT}/src/gb_core.c
    ${NES_ROOT}/src/md_core.c
)
target_include_directories(thumbynes_core PUBLIC
    ${NES_ROOT}/src
    ${CMAKE_CURRENT_LIST_DIR}     # for thumby_state_bridge.h
)
target_compile_definitions(thumbynes_core PUBLIC MD_LINE_SCRATCH)
# Tried placing md_core_scan_end in .time_critical.md (RAM-copied)
# via IRAM_ATTR — no measurable FPS gain on Sonic 2, so leaving it
# in XIP flash and saving the SRAM.
target_compile_definitions(thumbynes_core PRIVATE THUMBY_STATE_BRIDGE)
target_link_libraries(thumbynes_core PUBLIC nofrendo smsplus peanut_gb picodrive)

# MD dynamic IRAM — Cz80_Exec is tagged into .md_iram_pool in flash
# (see md_iram.ld) and memcpy'd to heap at mdc_init so it runs from
# SRAM. Zero BSS cost across cores (heap-allocated only while MD is
# active). Host builds stay in XIP flash.
option(MD_IRAM_DYNAMIC "Dynamic IRAM for MD hot functions" ON)
if (MD_IRAM_DYNAMIC)
    target_compile_definitions(thumbynes_core PUBLIC MD_IRAM_DYNAMIC=1)
    target_compile_definitions(picodrive      PUBLIC MD_IRAM_DYNAMIC=1)
endif()

#
# Audio mode (FULL / HALF / OFF) is now a runtime menu item —
# see the `Audio` choice in md_run.c's in-game menu. No compile
# flag needed.
#

# ----------------------------------------------------------------------
# nesrun_device — Phase 3 firmware: lobby + picker, no emulator yet.
# ----------------------------------------------------------------------
# FatFs library — ThumbyOne slot builds consume the shared copy at
# ThumbyOne/common/lib/fatfs/ (symlinked from mp-thumby/lib/fatfs so
# the MicroPython slot also sees bit-identical code). Standalone
# builds keep the vendored copy in device/fatfs/ so ThumbyNES can
# still build outside ThumbyOne. Our own diskio glue (fatfs/diskio.c)
# stays either way — it routes FatFs sectors to nes_flash_disk and
# is NES-specific.
if (THUMBYONE_SLOT_MODE)
    set(NES_FATFS_LIB_SRC ${THUMBYONE_FATFS_SRC})
    set(NES_FATFS_LIB_INC ${THUMBYONE_FATFS_INCLUDE})
else()
    set(NES_FATFS_LIB_SRC
        fatfs/ff.c
        fatfs/ffsystem.c
        fatfs/ffunicode.c)
    set(NES_FATFS_LIB_INC ${CMAKE_CURRENT_LIST_DIR}/fatfs)
endif()

# MSC + tinyUSB are only linked into the standalone build. The
# ThumbyOne slot build gets its USB handled by the lobby, so the
# slot itself never enumerates — skip nes_msc.c, usb_descriptors.c,
# and the tinyusb_* link libs entirely. Also frees ~8 KB of SRAM
# and a couple KB of flash the slot doesn't need to carry.
set(NES_SRC_BASE
    nes_device_main.c
    nes_lcd_gc9107.c
    nes_buttons.c
    nes_audio_pwm.c
    nes_flash_disk.c
    nes_picker.c
    nes_font.c
    nes_thumb.c
    nes_menu.c
    nes_battery.c
    nes_led.c
    thumby_state_bridge.c
    nes_run.c
    sms_run.c
    gb_run.c
    md_run.c
    fatfs/diskio.c
    ${NES_FATFS_LIB_SRC}
)
if (MD_IRAM_DYNAMIC)
    list(APPEND NES_SRC_BASE md_iram.c)
endif()
if (NOT THUMBYONE_SLOT_MODE)
    list(APPEND NES_SRC_BASE
        nes_msc.c
        usb_descriptors.c
    )
endif()

add_executable(nesrun_device ${NES_SRC_BASE})
target_include_directories(nesrun_device PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${NES_FATFS_LIB_INC}
)
target_compile_options(nesrun_device PRIVATE -O3 -Wno-unused-parameter)

set(NES_LINK_BASE
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
    hardware_pio   # shared thumbyone_backlight (PIO PWM on GP7 —
                    # HW PWM slice 3 aliases with audio on GP23)
)
if (NOT THUMBYONE_SLOT_MODE)
    list(APPEND NES_LINK_BASE tinyusb_device tinyusb_board)
endif()
target_link_libraries(nesrun_device PRIVATE ${NES_LINK_BASE})
pico_enable_stdio_usb(nesrun_device 0)
pico_enable_stdio_uart(nesrun_device 0)

if (MD_IRAM_DYNAMIC)
    # --wrap routes direct bl / function-pointer assignments to each
    # hot function through our thunks in md_iram.c, which dispatch
    # via function pointers (flash initially, heap after mdc_init).
    target_link_options(nesrun_device PRIVATE
        -Wl,--wrap=Cz80_Exec
        # YM2612UpdateOne_ / FinalizeLine555 wraps disabled for
        # bisection — see md_iram.c for the thunks (still present
        # but unused until wraps are re-enabled).
    )
    # Custom linker script adds the .md_iram_pool section in flash
    # with __md_iram_pool_start / __md_iram_pool_end symbols that
    # md_iram.c reads to memcpy the block into heap.
    pico_set_linker_script(nesrun_device ${CMAKE_CURRENT_LIST_DIR}/md_memmap.ld)
endif()

if (THUMBYONE_SLOT_MODE)
    # Embedded in ThumbyOne's NES partition. See common/slot_layout.h
    # in the ThumbyOne repo for the canonical flash-offset values.
    target_compile_definitions(nesrun_device PRIVATE
        THUMBYONE_SLOT_MODE=1
        FLASH_DISK_OFFSET=0x660000u
        FLASH_DISK_SIZE=0x9A0000u   # 9.6 MB
    )
    # Compile ThumbyOne's handoff source into this target directly
    # (common is not a CMake library — see common/CMakeLists.txt
    # for why). Parent project exports THUMBYONE_COMMON_SRC and
    # THUMBYONE_COMMON_INCLUDE from its add_subdirectory(common).
    target_sources(nesrun_device PRIVATE
        ${THUMBYONE_COMMON_SRC}
        ${THUMBYONE_COMMON_BATTERY_SRC}
        ${THUMBYONE_COMMON_FS_STATS_SRC}
        ${THUMBYONE_COMMON_BACKLIGHT_SRC}
        ${THUMBYONE_COMMON_SETTINGS_SRC})
    target_include_directories(nesrun_device PRIVATE
        ${THUMBYONE_COMMON_INCLUDE}
        ${THUMBYONE_COMMON_BATTERY_INCLUDE}
        ${THUMBYONE_COMMON_FS_STATS_INCLUDE}
        ${THUMBYONE_COMMON_BACKLIGHT_INCLUDE}
        ${THUMBYONE_COMMON_SETTINGS_INCLUDE})
    pico_set_uf2_family(nesrun_device "absolute")
    pico_add_extra_outputs(nesrun_device)
else()
    pico_add_extra_outputs(nesrun_device)
endif()
