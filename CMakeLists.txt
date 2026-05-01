cmake_minimum_required(VERSION 3.16)
project(ThumbyNES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-result
    -Wno-sign-compare
    -Wno-array-bounds
    -Wno-format
    -Wno-pointer-sign
    -Wno-implicit-function-declaration
    -fno-strict-aliasing
)

# ----------------------------------------------------------------------
# Vendored Nofrendo (GPLv2 — see vendor/VENDORING.md)
# ----------------------------------------------------------------------
set(NOFRENDO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/nofrendo)

file(GLOB NOFRENDO_NES_SRC     CONFIGURE_DEPENDS ${NOFRENDO_DIR}/nes/*.c)
file(GLOB NOFRENDO_MAPPER_SRC  CONFIGURE_DEPENDS ${NOFRENDO_DIR}/mappers/*.c)

add_library(nofrendo STATIC
    ${NOFRENDO_NES_SRC}
    ${NOFRENDO_MAPPER_SRC}
    ${NOFRENDO_DIR}/nofrendo.c
)
target_include_directories(nofrendo PUBLIC
    ${NOFRENDO_DIR}
    ${NOFRENDO_DIR}/nes
)
# Build standalone, NOT inside retro-go — utils.h falls back to printf.
target_compile_definitions(nofrendo PRIVATE)

# ----------------------------------------------------------------------
# Vendored smsplus (GPLv2 — see vendor/VENDORING.md)
# ----------------------------------------------------------------------
set(SMSPLUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/smsplus)

file(GLOB SMSPLUS_TOP_SRC   CONFIGURE_DEPENDS ${SMSPLUS_DIR}/*.c)
file(GLOB SMSPLUS_CPU_SRC   CONFIGURE_DEPENDS ${SMSPLUS_DIR}/cpu/*.c)
file(GLOB SMSPLUS_SOUND_SRC CONFIGURE_DEPENDS ${SMSPLUS_DIR}/sound/*.c)

# Precompute the Z80 SZHVC flag tables (256 KB) into flash so we don't
# need 256 KB of free heap to start the SMS core. See
# tools/gen_sms_z80_szhvc.c for the generator.
set(SMS_SZHVC_C   ${CMAKE_CURRENT_BINARY_DIR}/sms_z80_szhvc_data.c)
add_executable(sms_szhvc_gen tools/gen_sms_z80_szhvc.c)
set_target_properties(sms_szhvc_gen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_custom_command(
    OUTPUT  ${SMS_SZHVC_C}
    COMMAND $<TARGET_FILE:sms_szhvc_gen> ${SMS_SZHVC_C}
    DEPENDS sms_szhvc_gen
    VERBATIM
)

add_library(smsplus STATIC
    ${SMSPLUS_TOP_SRC}
    ${SMSPLUS_CPU_SRC}
    ${SMSPLUS_SOUND_SRC}
    ${SMS_SZHVC_C}
)
target_include_directories(smsplus PUBLIC
    ${SMSPLUS_DIR}
    ${SMSPLUS_DIR}/cpu
    ${SMSPLUS_DIR}/sound
)
# Standalone build — shared.h falls back to printf/no-IRAM_ATTR.
target_compile_definitions(smsplus PRIVATE)

# ----------------------------------------------------------------------
# Vendored PicoDrive (LGPLv2 — see vendor/VENDORING.md)
#   MD / Genesis only. FAME 68K interpreter, cz80 for sound coprocessor.
#   32X + Cyclone + ARM/MIPS asm trimmed from the tree. SMS is runtime-
#   gated (we always boot MD-mode) so sms.c / mode4.c compile as dead
#   code — simpler than stubbing the four link-level symbols. CD code
#   stays in because PicoDrive has no NO_CD flag; PAHW_MCD never fires.
# ----------------------------------------------------------------------
set(PICODRIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/picodrive)

file(GLOB PICODRIVE_PICO_SRC   CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/*.c)
file(GLOB PICODRIVE_SOUND_SRC  CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/sound/*.c)
file(GLOB PICODRIVE_CARTHW_SRC CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/carthw/*.c)
file(GLOB PICODRIVE_SVP_SRC    CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/carthw/svp/*.c)
file(GLOB PICODRIVE_PICOHW_SRC CONFIGURE_DEPENDS ${PICODRIVE_DIR}/pico/pico/*.c)
file(GLOB PICODRIVE_UNZIP_SRC  CONFIGURE_DEPENDS ${PICODRIVE_DIR}/unzip/*.c)

# Drop sources that aren't separate translation units, require 32X, or
# need assembly cores we don't build:
#   debugCPU.c — Musashi-vs-FAME validator (only compiled with pdb=1)
#   pico_cmn.c — #include'd by pico.c, not a standalone TU
#   carthw/svp/compiler.c — DRC, needs ARM emit_arm.c
#   pico/cd/*.c — Mega-CD support. ~20 KB of otherwise-always-resident
#     BSS (cdd, cdda_out_buffer, PicoCpuFS68k, Pico_msd, s68k_*_map)
#     that never executes because PAHW_MCD is never set. Stubs live in
#     thumby_platform.c for the ~11 entry points the runtime-gated
#     call sites reach.
list(FILTER PICODRIVE_PICO_SRC EXCLUDE REGEX "pico/debugCPU\\.c$")
list(FILTER PICODRIVE_PICO_SRC EXCLUDE REGEX "pico/pico_cmn\\.c$")
list(FILTER PICODRIVE_SVP_SRC  EXCLUDE REGEX "carthw/svp/compiler\\.c$")

# ----------------------------------------------------------------------
# YM2612 implementation selection. The default is PicoDrive's stock
# YM2612 (small, fast, MAME-derived). THUMBYNES_YM2612_GENPLUS=ON
# replaces it with the Genesis Plus GX YM2612 (larger, but matches the
# audio quality users get from RetroArch's Genesis Plus GX core).
# Only one of the two is compiled at any time; the unused source is
# filtered out below.
# ----------------------------------------------------------------------
option(THUMBYNES_YM2612_GENPLUS
    "Use Genesis Plus GX YM2612 (vendored separately) instead of PicoDrive's"
    OFF)

if(THUMBYNES_YM2612_GENPLUS)
    list(FILTER PICODRIVE_SOUND_SRC EXCLUDE REGEX "sound/ym2612\\.c$")
else()
    list(FILTER PICODRIVE_SOUND_SRC EXCLUDE REGEX "sound/ym2612_genplus\\.c$")
    list(FILTER PICODRIVE_SOUND_SRC EXCLUDE REGEX "sound/ym2612_shim\\.c$")
endif()

add_library(picodrive STATIC
    ${PICODRIVE_PICO_SRC}
    ${PICODRIVE_SOUND_SRC}
    ${PICODRIVE_CARTHW_SRC}
    ${PICODRIVE_SVP_SRC}
    ${PICODRIVE_PICOHW_SRC}
    ${PICODRIVE_UNZIP_SRC}
    ${PICODRIVE_DIR}/cpu/fame/famec.c
    ${PICODRIVE_DIR}/cpu/cz80/cz80.c
    # cpu/drc/cmn.c deliberately excluded — it declares a 4 MB static
    # tcache_default[] for dynamic recompilation. We don't build any
    # DRC (no SH2 DRC / no SVP DRC / no Cyclone), so the only reference
    # to drc_cmn_cleanup is under _SVP_DRC in svp.c, which we don't
    # define. Keeping cmn.c out of the build saves 4 MB BSS on device.
    ${PICODRIVE_DIR}/thumby_platform.c
)
target_include_directories(picodrive PUBLIC
    ${PICODRIVE_DIR}
    ${PICODRIVE_DIR}/pico
    ${PICODRIVE_DIR}/cpu
)
target_compile_definitions(picodrive PUBLIC
    EMU_F68K          # FAME 68K interpreter
    _USE_CZ80         # Z80 coprocessor
    NO_32X            # no 32X / SH2 DRC
    FAME_BIG_ENDIAN   # ROM stays raw big-endian in memory (XIP on
                      # device). FAME fetch + memory.c u16 reads +
                      # cart.c rom_read32 do the bswap on read. Byte
                      # reads work because MEM_BE2 drops to identity.
                      # Patches are all gated on FAME_BIG_ENDIAN so
                      # unflipping here restores upstream behavior.
)
if(THUMBYNES_YM2612_GENPLUS)
    target_compile_definitions(picodrive PUBLIC THUMBYNES_YM2612_GENPLUS=1)
endif()
# TEMPORARY — diagnosing device black-screen bug. Enables the same
# flash-resident tables the device uses (FAME JumpTable, YM2612 log
# tables, CZ80 SZHVC flag tables). Revert once we know whether one
# of these is the regression. Configure with
#   -DMD_TEST_DEVICE_FLAGS=1  (all three)
#   -DMD_TEST_JT_FLASH=1      (just the JumpTable)
if(DEFINED MD_TEST_JT_FLASH OR DEFINED MD_TEST_DEVICE_FLAGS)
    set(MD_FAME_JT_INC ${CMAKE_CURRENT_BINARY_DIR}/famec_jumptable_data.inc)
    set(MD_FAME_JT_GEN ${CMAKE_CURRENT_BINARY_DIR}/md_fame_jt_gen)
    add_custom_command(
        OUTPUT  ${MD_FAME_JT_GEN}
        COMMAND cc -O2 -I${CMAKE_CURRENT_SOURCE_DIR}
                    -o ${MD_FAME_JT_GEN} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_md_fame_jumptable.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_md_fame_jumptable.c
                ${PICODRIVE_DIR}/cpu/fame/famec_jumptable_init.inc
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${MD_FAME_JT_INC}
        COMMAND ${MD_FAME_JT_GEN} ${MD_FAME_JT_INC}
        DEPENDS ${MD_FAME_JT_GEN}
        VERBATIM
    )
    set_source_files_properties(
        ${PICODRIVE_DIR}/cpu/fame/famec.c
        PROPERTIES
            OBJECT_DEPENDS ${MD_FAME_JT_INC}
            INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}"
    )
    target_compile_definitions(picodrive PUBLIC FAME_JUMPTABLE_IN_FLASH)
endif()

if(DEFINED MD_TEST_DEVICE_FLAGS)
    set(MD_YM_TAB_C   ${CMAKE_CURRENT_BINARY_DIR}/md_ym2612_tab_data.c)
    set(MD_YM_TAB_GEN ${CMAKE_CURRENT_BINARY_DIR}/md_ym2612_tab_gen)
    add_custom_command(
        OUTPUT  ${MD_YM_TAB_GEN}
        COMMAND cc -O2 -o ${MD_YM_TAB_GEN} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_md_ym2612_tab.c -lm
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_md_ym2612_tab.c
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${MD_YM_TAB_C}
        COMMAND ${MD_YM_TAB_GEN} ${MD_YM_TAB_C}
        DEPENDS ${MD_YM_TAB_GEN}
        VERBATIM
    )
    set(MD_CZ80_SZHVC_C   ${CMAKE_CURRENT_BINARY_DIR}/md_cz80_szhvc_data.c)
    set(MD_CZ80_SZHVC_GEN ${CMAKE_CURRENT_BINARY_DIR}/md_cz80_szhvc_gen)
    add_custom_command(
        OUTPUT  ${MD_CZ80_SZHVC_GEN}
        COMMAND cc -O2 -o ${MD_CZ80_SZHVC_GEN} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_md_cz80_szhvc.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_md_cz80_szhvc.c
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${MD_CZ80_SZHVC_C}
        COMMAND ${MD_CZ80_SZHVC_GEN} ${MD_CZ80_SZHVC_C}
        DEPENDS ${MD_CZ80_SZHVC_GEN}
        VERBATIM
    )
    target_sources(picodrive PRIVATE ${MD_YM_TAB_C} ${MD_CZ80_SZHVC_C})
    target_compile_definitions(picodrive PUBLIC
        YM2612_TABLES_IN_FLASH
        CZ80_SZHVC_IN_FLASH
    )
endif()
# cart.c / state.c use crc32 + inflate + gz* — link system zlib
# transitively so dependent executables pick it up at the right spot.
target_link_libraries(picodrive PUBLIC z m)
# PicoDrive uses aggressive aliasing in the mode4/draw renderers.
target_compile_options(picodrive PRIVATE
    # Upstream Makefile sets this so function pointers have bit 0 = 0.
    # The cpu68k map encodes addresses by shifting right by 1 — function
    # pointers at odd addresses fail xmap_set's alignment check and
    # silently fall back to m68k_unmapped_read* handlers, giving the
    # 68K garbage and crashing the game.
    -falign-functions=2
    -fno-strict-aliasing
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-maybe-uninitialized
    -Wno-pointer-sign
    -Wno-sign-compare
    -Wno-implicit-function-declaration
    -Wno-array-bounds
    -Wno-format
)

# ----------------------------------------------------------------------
# Vendored peanut_gb (single-header DMG core) + minigb_apu
# ----------------------------------------------------------------------
set(PEANUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/peanut_gb)
add_library(peanut_gb STATIC ${PEANUT_DIR}/minigb_apu.c)
target_include_directories(peanut_gb PUBLIC ${PEANUT_DIR})
target_compile_definitions(peanut_gb PUBLIC PEANUT_FULL_GBC_SUPPORT=1)
target_compile_options(peanut_gb PRIVATE -Wno-implicit-fallthrough -Wno-unused-function -Wno-unused-variable)

# ----------------------------------------------------------------------
# Vendored HuExpress (GPLv2 — see vendor/VENDORING.md)
#   PC Engine / TurboGrafx-16 HuCard-only. CD, Arcade Card, SuperGrafx,
#   netplay, SDL OSD, debugger all trimmed from the tree.
#   XBUF render path (220 KB) keeps the host build simple; device build
#   will eventually require PCE_SCANLINE_RENDER — see PCE_PLAN.md §5.
# ----------------------------------------------------------------------
set(HUEXPRESS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/huexpress)

file(GLOB HUEXPRESS_ENGINE_SRC CONFIGURE_DEPENDS ${HUEXPRESS_DIR}/engine/*.c)

add_library(huexpress STATIC
    ${HUEXPRESS_ENGINE_SRC}
    ${HUEXPRESS_DIR}/thumby_platform.c
)
target_include_directories(huexpress PUBLIC
    ${HUEXPRESS_DIR}/engine
    ${HUEXPRESS_DIR}/includes
)
target_compile_definitions(huexpress PUBLIC
    THUMBY_BUILD         # swaps ESP-IDF headers for thumby_platform.h
    LSB_FIRST=1          # H6280 endianness
    FINAL_RELEASE=1      # drop debug printf/tracing blocks
    PCE_HUCARD_ONLY      # strips CD_track (74 KB BSS) and PCM buffer (64 KB heap)
    PCE_SCANLINE_RENDER  # enables the per-scanline direct-to-LCD renderer
                         # (see src/pce_render.c) and eliminates the
                         # XBUF / SPM / VRAM2 / VRAMS allocations.
)
target_compile_options(huexpress PRIVATE
    -Wno-unused-function -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-implicit-fallthrough
    -Wno-sign-compare -Wno-incompatible-pointer-types
)

# ----------------------------------------------------------------------
# ThumbyNES runtime glue
# ----------------------------------------------------------------------
add_library(thumbynes STATIC
    src/nes_core.c src/sms_core.c src/gb_core.c src/md_core.c
    src/pce_core.c src/pce_render.c)
target_include_directories(thumbynes PUBLIC src)
target_link_libraries(thumbynes PUBLIC nofrendo smsplus peanut_gb picodrive huexpress)

# ----------------------------------------------------------------------
# Phase 0 bench harness
# ----------------------------------------------------------------------
add_executable(nesbench src/bench_main.c)
target_link_libraries(nesbench PRIVATE thumbynes)

add_executable(smsbench src/sms_bench_main.c)
target_link_libraries(smsbench PRIVATE thumbynes)

add_executable(gbbench src/gb_bench_main.c)
target_link_libraries(gbbench PRIVATE thumbynes)

add_executable(mdbench src/md_bench_main.c)
target_link_libraries(mdbench PRIVATE thumbynes)

add_executable(pcebench src/pce_bench_main.c)
target_link_libraries(pcebench PRIVATE thumbynes)

# XIP-borrow variant of mdbench — mmap(PROT_READ) then mdc_load_rom_xip.
# Reproduces the device's read-only ROM scenario so device-only crashes
# that don't affect mdc_load_rom can be triaged on host.
add_executable(mdbench_xip src/md_bench_xip_main.c)
target_link_libraries(mdbench_xip PRIVATE thumbynes)

# MD_LINE_SCRATCH variant — reproduces the device's per-scanline
# downsample render path on host so we can triage the black-screen
# bug without flashing. Separate thumbynes_scratch lib so the main
# thumbynes (used by mdbench / mdhost) keeps the full-framebuffer
# path.
add_library(thumbynes_scratch STATIC src/md_core.c)
target_compile_definitions(thumbynes_scratch PUBLIC MD_LINE_SCRATCH)
target_include_directories(thumbynes_scratch PUBLIC src)
target_link_libraries(thumbynes_scratch PUBLIC picodrive)
add_executable(mdbench_scratch src/md_bench_scratch_main.c)
target_link_libraries(mdbench_scratch PRIVATE thumbynes_scratch)

# ----------------------------------------------------------------------
# Phase 1 SDL2 host runner (built only if SDL2 is available)
# ----------------------------------------------------------------------
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    add_executable(neshost src/host_main.c)
    target_link_libraries(neshost PRIVATE thumbynes SDL2::SDL2)
    add_executable(smshost src/sms_host_main.c)
    target_link_libraries(smshost PRIVATE thumbynes SDL2::SDL2)
    add_executable(gbhost src/gb_host_main.c)
    target_link_libraries(gbhost PRIVATE thumbynes SDL2::SDL2)
    add_executable(mdhost src/md_host_main.c)
    target_link_libraries(mdhost PRIVATE thumbynes SDL2::SDL2)
    add_executable(pcehost src/pce_host_main.c)
    target_link_libraries(pcehost PRIVATE thumbynes SDL2::SDL2)
    message(STATUS "ThumbyNES: SDL2 found, building neshost + smshost + gbhost + mdhost + pcehost")
else()
    # Older SDL2 cmake configs don't export the SDL2:: target.
    find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
    find_library(SDL2_LIBRARY NAMES SDL2)
    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
        add_executable(neshost src/host_main.c)
        target_include_directories(neshost PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(neshost PRIVATE thumbynes ${SDL2_LIBRARY})
        add_executable(smshost src/sms_host_main.c)
        target_include_directories(smshost PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(smshost PRIVATE thumbynes ${SDL2_LIBRARY})
        add_executable(gbhost src/gb_host_main.c)
        target_include_directories(gbhost PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(gbhost PRIVATE thumbynes ${SDL2_LIBRARY})
        add_executable(mdhost src/md_host_main.c)
        target_include_directories(mdhost PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(mdhost PRIVATE thumbynes ${SDL2_LIBRARY})
        add_executable(pcehost src/pce_host_main.c)
        target_include_directories(pcehost PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(pcehost PRIVATE thumbynes ${SDL2_LIBRARY})
        message(STATUS "ThumbyNES: SDL2 (manual) found, building neshost + smshost + gbhost + mdhost + pcehost")
    else()
        message(STATUS "ThumbyNES: SDL2 not found, skipping neshost / smshost / gbhost")
    endif()
endif()
