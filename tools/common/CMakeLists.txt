# SPDX-License-Identifier: GPL-2.0-or-later
add_library(radioberry_common STATIC
    src/sha256.cpp
    src/gateware_catalog.cpp
    src/fpga_config.cpp)
target_include_directories(radioberry_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated)

# Hardware glue kept out of the unit-tested library.
add_library(radioberry_gpiod STATIC src/gpiod_config_pins.cpp)
target_link_libraries(radioberry_gpiod PUBLIC radioberry_common PkgConfig::GPIOD)

if(RADIOBERRY_BUILD_TESTS)
    add_executable(common_tests
        tests/sha256_test.cpp
        tests/gateware_catalog_test.cpp
        tests/fpga_config_test.cpp)
    target_link_libraries(common_tests PRIVATE radioberry_common Catch2::Catch2WithMain)
    catch_discover_tests(common_tests)
endif()
