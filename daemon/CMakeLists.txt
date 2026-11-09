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

find_path(LIQUID_INCLUDE_DIR liquid/liquid.h REQUIRED)
find_library(LIQUID_LIBRARY liquid REQUIRED)

# Protocols 1 and 2, the sockets and the daemon around them.
add_library(radioberryd_core STATIC
    src/dsp/decimator.cpp
    src/p1/protocol1.cpp
    src/p1/server.cpp
    src/p2/protocol2.cpp
    src/p2/server.cpp
    src/p2/network.cpp
    src/realtime.cpp
    src/session.cpp
    src/tx_policy.cpp
    src/net.cpp
    src/daemon.cpp
    src/options.cpp)
target_include_directories(radioberryd_core PRIVATE ${LIQUID_INCLUDE_DIR})
target_link_libraries(radioberryd_core PUBLIC radioberryd_device spdlog::spdlog yaml-cpp::yaml-cpp
    ${LIQUID_LIBRARY})

# libsystemd for logging to the journal under systemd.
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_executable(radioberryd src/main.cpp)
target_link_libraries(radioberryd PRIVATE radioberryd_core radioberry_device PkgConfig::SYSTEMD)
install(TARGETS radioberryd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

# The service: unit, udev rule (device group and systemd device unit),
# system user, and the config file, which is never overwritten.
set(RADIOBERRY_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system" CACHE PATH
    "Where radioberry.service is installed")
set(RADIOBERRY_SYSUSERS_DIR "${CMAKE_INSTALL_PREFIX}/lib/sysusers.d" CACHE PATH
    "Where the radioberry system user's sysusers.d entry is installed")
set(RADIOBERRY_UDEV_RULES_DIR "${CMAKE_INSTALL_PREFIX}/lib/udev/rules.d" CACHE PATH
    "Where the udev rule for /dev/radioberry is installed")
set(RADIOBERRY_CONFIG_DIR "/etc/radioberry" CACHE PATH "Where config.yaml is installed")
set(RADIOBERRYD ${CMAKE_INSTALL_FULL_SBINDIR}/radioberryd)
set(RADIOBERRY_CONFIG_FILE ${RADIOBERRY_CONFIG_DIR}/config.yaml)
configure_file(systemd/radioberry.service.in radioberry.service @ONLY)
configure_file(systemd/install.cmake.in install-service.cmake @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/radioberry.service
        DESTINATION ${RADIOBERRY_SYSTEMD_UNIT_DIR})
install(FILES systemd/60-radioberry.rules DESTINATION ${RADIOBERRY_UDEV_RULES_DIR})
install(FILES systemd/radioberry-sysusers.conf DESTINATION ${RADIOBERRY_SYSUSERS_DIR}
        RENAME radioberry.conf)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/install-service.cmake)

if(RADIOBERRY_BUILD_TESTS)
    add_executable(daemon_tests
        tests/fake_device.cpp
        tests/ring_test.cpp
        tests/control_test.cpp
        tests/monitor_test.cpp
        tests/rx_pump_test.cpp
        tests/tx_pump_test.cpp
        tests/service_test.cpp
        tests/decimator_test.cpp
        tests/protocol1_test.cpp
        tests/protocol2_test.cpp
        tests/server_test.cpp
        tests/p2_server_test.cpp
        tests/session_test.cpp
        tests/tx_policy_test.cpp
        tests/realtime_test.cpp
        tests/options_test.cpp
        tests/daemon_test.cpp)
    target_link_libraries(daemon_tests PRIVATE radioberryd_core Catch2::Catch2WithMain)
    target_compile_definitions(daemon_tests PRIVATE
        RADIOBERRY_DEFAULT_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/systemd/config.yaml")
    catch_discover_tests(daemon_tests)
endif()
