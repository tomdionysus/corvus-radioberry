# SPDX-License-Identifier: GPL-2.0-or-later
add_library(rbcat_core STATIC src/app.cpp src/options.cpp src/stream.cpp)
target_include_directories(rbcat_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rbcat_core PUBLIC radioberry_common radioberry_report)

add_executable(rbcat src/main.cpp)
target_link_libraries(rbcat PRIVATE rbcat_core radioberry_device)
install(TARGETS rbcat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(rbcat_tests
        tests/fake_device.cpp
        tests/options_test.cpp
        tests/stream_test.cpp
        tests/app_test.cpp)
    target_link_libraries(rbcat_tests PRIVATE rbcat_core Catch2::Catch2WithMain)
    catch_discover_tests(rbcat_tests)
endif()
