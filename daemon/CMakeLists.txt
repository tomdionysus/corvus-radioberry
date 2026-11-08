# SPDX-License-Identifier: GPL-2.0-or-later
#
# radioberryd (section 3 of TODO/ACTIVE.md).

# Device layer over /dev/radioberry: command registers, RX and TX pumps,
# statistics and faults.
add_library(radioberryd_device STATIC
    src/device/control.cpp
    src/device/monitor.cpp
    src/device/rx_pump.cpp
    src/device/tx_pump.cpp
    src/device/service.cpp)
target_include_directories(radioberryd_device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(radioberryd_device PUBLIC radioberry_common)

if(RADIOBERRY_BUILD_TESTS)
    add_executable(daemon_tests
        tests/fake_device.cpp
        tests/ring_test.cpp
        tests/control_test.cpp
        tests/monitor_test.cpp
        tests/rx_pump_test.cpp
        tests/tx_pump_test.cpp
        tests/service_test.cpp)
    target_link_libraries(daemon_tests PRIVATE radioberryd_device Catch2::Catch2WithMain)
    catch_discover_tests(daemon_tests)
endif()
