# Dulcamara - the app for this computer (macOS or Linux, SDL2 in a window), with EasyRPG Player's core as
# tools/build-easyrpg.sh host builds it: for working on the interface without a console. tools/build-desktop.sh
# configures it. With NP_PREVIEW it draws off screen, plays a button script and saves screenshots
# (NP_SCRIPT, NP_SHOTS), as Mandrake's interface preview does.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(dulcamara_desktop LANGUAGES C CXX)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${ROOT}/third_party/easyrpg-player" easyrpg-player EXCLUDE_FROM_ALL)

find_package(SDL2 CONFIG REQUIRED)
find_package(Iconv REQUIRED)
option(NP_PREVIEW "Draw off screen and follow NP_SCRIPT" OFF)

file(GLOB SOURCES "${ROOT}/src/*.c" "${ROOT}/src/core/*.c" "${ROOT}/src/ui/*.c")
list(APPEND SOURCES "${ROOT}/src/platform/plat_sdl.c" "${ROOT}/src/platform/xbr.c" "${ROOT}/src/platform/blit.c"
    "${ROOT}/src/platform/vk/vk_probe.c")
add_executable(dulcamara ${SOURCES})
target_include_directories(dulcamara PRIVATE "${ROOT}/src" "${ROOT}/third_party/stb"
    "${ROOT}/examples/update-check" "${ROOT}/third_party/easyrpg-player/builds/libretro/libretro-common/include")
target_compile_options(dulcamara PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra>)
if(NP_PREVIEW)
    target_compile_definitions(dulcamara PRIVATE NP_PREVIEW)
endif()
target_link_libraries(dulcamara PRIVATE easyrpg_libretro SDL2::SDL2 Iconv::Iconv m)
set_target_properties(dulcamara PROPERTIES LINKER_LANGUAGE CXX)
