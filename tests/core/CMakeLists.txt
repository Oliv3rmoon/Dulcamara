# Nightshade Player - EasyRPG Player's libretro core built for this computer, and a headless frontend for it
# (headless.c). tools/build-easyrpg.sh host configures it; tests/test_core.py runs the result on TestGame.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(nightshade_core_check LANGUAGES C CXX)

set(EASYRPG_PLAYER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/easyrpg-player" CACHE PATH "EasyRPG Player's source")
add_subdirectory("${EASYRPG_PLAYER_DIR}" easyrpg-player EXCLUDE_FROM_ALL)

add_executable(easyrpg-headless headless.c)
target_include_directories(easyrpg-headless PRIVATE "${EASYRPG_PLAYER_DIR}/builds/libretro/libretro-common/include")
target_link_libraries(easyrpg-headless PRIVATE easyrpg_libretro)
set_target_properties(easyrpg-headless PROPERTIES LINKER_LANGUAGE CXX C_STANDARD 11)

# liblcf's encodings without ICU (tools/patches/liblcf-iconv.patch)
find_package(liblcf CONFIG REQUIRED)
add_executable(lcf-encodings encodings.cpp)
target_link_libraries(lcf-encodings PRIVATE liblcf::liblcf)
set_target_properties(lcf-encodings PROPERTIES CXX_STANDARD 17)
