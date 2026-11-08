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

# Protocol 1, the sockets and the daemon around them.
add_library(radioberryd_core STATIC
    src/p1/protocol1.cpp
    src/p1/server.cpp
    src/net.cpp
    src/daemon.cpp
    src/options.cpp)
target_link_libraries(radioberryd_core PUBLIC radioberryd_device spdlog::spdlog)

add_executable(radioberryd src/main.cpp)
target_link_libraries(radioberryd PRIVATE radioberryd_core radioberry_device)
install(TARGETS radioberryd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(daemon_tests
        tests/fake_device.cpp
        tests/ring_test.cpp
        tests/control_test.cpp
        tests/monitor_test.cpp
        tests/rx_pump_test.cpp
        tests/tx_pump_test.cpp
        tests/service_test.cpp
        tests/protocol1_test.cpp
        tests/server_test.cpp
        tests/options_test.cpp
        tests/daemon_test.cpp)
    target_link_libraries(daemon_tests PRIVATE radioberryd_core Catch2::Catch2WithMain)
    catch_discover_tests(daemon_tests)
endif()
