# SPDX-License-Identifier: GPL-2.0-or-later
#
# wsprrx: an example receive-only openHPSDR client (Protocol 1 or 2) that
# decodes WSPR, the sister of ft8rx, whose Protocol 1 and 2 receivers it
# shares. The WSPR code is written from the protocol description.

find_path(LIQUID_INCLUDE_DIR liquid/liquid.h REQUIRED)
find_library(LIQUID_LIBRARY liquid REQUIRED)

add_library(wspr_core STATIC
    src/wspr/c2file.cpp
    src/wspr/codec.cpp
    src/wspr/decimator.cpp
    src/wspr/decoder.cpp
    src/wspr/fano.cpp)
target_include_directories(wspr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${LIQUID_INCLUDE_DIR})
target_link_libraries(wspr_core PUBLIC ${LIQUID_LIBRARY})

add_executable(wsprrx src/main.cpp)
target_link_libraries(wsprrx PRIVATE wspr_core openhpsdr_rx Threads::Threads)

if(RADIOBERRY_BUILD_TESTS)
    add_executable(wspr_tests tests/wspr_test.cpp)
    target_link_libraries(wspr_tests PRIVATE wspr_core Catch2::Catch2WithMain)
    catch_discover_tests(wspr_tests)
endif()
