# SPDX-License-Identifier: GPL-2.0-or-later
#
# p1probe and p2probe: minimal openHPSDR clients for discovery, receive and
# zero-drive transmit.
add_library(protoprobe_core STATIC src/client.cpp src/p1_client.cpp src/p2_client.cpp
    src/probe.cpp src/udp_transport.cpp)
target_include_directories(protoprobe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(protoprobe_core PUBLIC radioberry_common radioberry_report)

add_executable(p1probe src/p1probe_main.cpp)
target_link_libraries(p1probe PRIVATE protoprobe_core)
add_executable(p2probe src/p2probe_main.cpp)
target_link_libraries(p2probe PRIVATE protoprobe_core)
install(TARGETS p1probe p2probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(protoprobe_tests tests/probe_test.cpp tests/daemon_test.cpp
        ${PROJECT_SOURCE_DIR}/daemon/tests/fake_device.cpp)
    target_include_directories(protoprobe_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/daemon/tests)
    target_link_libraries(protoprobe_tests PRIVATE protoprobe_core radioberryd_core
        Catch2::Catch2WithMain)
    catch_discover_tests(protoprobe_tests)
endif()
