# SPDX-License-Identifier: GPL-2.0-or-later
#
# ft8rx: an example receive-only openHPSDR client (Protocol 1 or 2) that
# decodes FT8. The FT8 code comes from Corvus HF (ported from
# kgoba/ft8_lib); the message text code is ft8_lib's own (MIT,
# src/ft8_lib/LICENSE).

find_path(LIQUID_INCLUDE_DIR liquid/liquid.h REQUIRED)
find_library(LIQUID_LIBRARY liquid REQUIRED)

# ft8_lib's C, kept as upstream wrote it: not held to this project's warnings.
add_library(ft8_lib STATIC src/ft8_lib/message.c src/ft8_lib/text.c)
target_include_directories(ft8_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ft8_lib PRIVATE -w)
# It uses stpcpy, which strict C11 hides.
target_compile_definitions(ft8_lib PRIVATE _POSIX_C_SOURCE=200809L)

add_library(ft8_core STATIC
    src/ft8/codec.cpp
    src/ft8/constants.cpp
    src/ft8/decoder.cpp
    src/decimator.cpp
    src/message_text.cpp)
target_include_directories(ft8_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${LIQUID_INCLUDE_DIR})
target_link_libraries(ft8_core PUBLIC ft8_lib ${LIQUID_LIBRARY})

add_executable(ft8rx src/main.cpp src/p1_receiver.cpp src/p2_receiver.cpp)
target_link_libraries(ft8rx PRIVATE ft8_core Threads::Threads)

if(RADIOBERRY_BUILD_TESTS)
    add_executable(ft8_tests tests/ft8_test.cpp)
    target_link_libraries(ft8_tests PRIVATE ft8_core Catch2::Catch2WithMain)
    catch_discover_tests(ft8_tests)
endif()
