# SPDX-License-Identifier: GPL-2.0-or-later
add_library(radioberry_common STATIC
    src/sha256.cpp
    src/gateware_catalog.cpp
    src/fpga_config.cpp
    src/command.cpp
    src/board.cpp)
target_include_directories(radioberry_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(radioberry_common PUBLIC radioberry_abi)

# Report tree rendered as text, JSON and YAML, shared by the tools.
add_library(radioberry_report STATIC src/node.cpp src/render.cpp)
target_include_directories(radioberry_report PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(radioberry_report PUBLIC yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json)

# Hardware glue kept out of the unit-tested libraries.
add_library(radioberry_gpiod STATIC src/gpiod_config_pins.cpp)
target_link_libraries(radioberry_gpiod PUBLIC radioberry_common PkgConfig::GPIOD)
add_library(radioberry_device STATIC src/linux_device.cpp)
target_link_libraries(radioberry_device PUBLIC radioberry_common)

if(RADIOBERRY_BUILD_TESTS)
    add_executable(common_tests
        tests/sha256_test.cpp
        tests/gateware_catalog_test.cpp
        tests/fpga_config_test.cpp
        tests/command_test.cpp
        tests/board_test.cpp
        tests/node_test.cpp
        tests/render_test.cpp)
    target_link_libraries(common_tests PRIVATE
        radioberry_common radioberry_report Catch2::Catch2WithMain)
    catch_discover_tests(common_tests)
endif()
