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
# Vendored peanut_gb (single-header DMG core) + minigb_apu
# ----------------------------------------------------------------------
set(PEANUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/peanut_gb)
add_library(peanut_gb STATIC ${PEANUT_DIR}/minigb_apu.c)
target_include_directories(peanut_gb PUBLIC ${PEANUT_DIR})
target_compile_options(peanut_gb PRIVATE -Wno-implicit-fallthrough -Wno-unused-function -Wno-unused-variable)

# ----------------------------------------------------------------------
# ThumbyNES runtime glue
# ----------------------------------------------------------------------
add_library(thumbynes STATIC src/nes_core.c src/sms_core.c src/gb_core.c)
target_include_directories(thumbynes PUBLIC src)
target_link_libraries(thumbynes PUBLIC nofrendo smsplus peanut_gb)

# ----------------------------------------------------------------------
# Phase 0 bench harness
# ----------------------------------------------------------------------
add_executable(nesbench src/bench_main.c)
target_link_libraries(nesbench PRIVATE thumbynes)

add_executable(smsbench src/sms_bench_main.c)
target_link_libraries(smsbench PRIVATE thumbynes)

add_executable(gbbench src/gb_bench_main.c)
target_link_libraries(gbbench PRIVATE thumbynes)

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
    message(STATUS "ThumbyNES: SDL2 found, building neshost + smshost + gbhost")
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
        message(STATUS "ThumbyNES: SDL2 (manual) found, building neshost + smshost + gbhost")
    else()
        message(STATUS "ThumbyNES: SDL2 not found, skipping neshost / smshost / gbhost")
    endif()
endif()
