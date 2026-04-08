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
# ThumbyNES runtime glue
# ----------------------------------------------------------------------
add_library(thumbynes STATIC src/nes_core.c)
target_include_directories(thumbynes PUBLIC src)
target_link_libraries(thumbynes PUBLIC nofrendo)

# ----------------------------------------------------------------------
# Phase 0 bench harness
# ----------------------------------------------------------------------
add_executable(nesbench src/bench_main.c)
target_link_libraries(nesbench PRIVATE thumbynes)

# ----------------------------------------------------------------------
# Phase 1 SDL2 host runner (built only if SDL2 is available)
# ----------------------------------------------------------------------
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    add_executable(neshost src/host_main.c)
    target_link_libraries(neshost PRIVATE thumbynes SDL2::SDL2)
    message(STATUS "ThumbyNES: SDL2 found, building neshost")
else()
    # Older SDL2 cmake configs don't export the SDL2:: target.
    find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
    find_library(SDL2_LIBRARY NAMES SDL2)
    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
        add_executable(neshost src/host_main.c)
        target_include_directories(neshost PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(neshost PRIVATE thumbynes ${SDL2_LIBRARY})
        message(STATUS "ThumbyNES: SDL2 (manual) found, building neshost")
    else()
        message(STATUS "ThumbyNES: SDL2 not found, skipping neshost")
    endif()
endif()
