# SPDX-License-Identifier: GPL-2.0-or-later
add_library(rbprobe_core STATIC
    src/app.cpp
    src/collect.cpp
    src/text.cpp)
target_include_directories(rbprobe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rbprobe_core PUBLIC
    radioberry_common radioberry_report)

add_executable(rbprobe src/main.cpp src/live_source.cpp)
target_link_libraries(rbprobe PRIVATE
    rbprobe_core radioberry_gpiod spdlog::spdlog ${I2C_LIBRARY})
install(TARGETS rbprobe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(rbprobe_tests
        tests/fake_source.cpp
        tests/text_test.cpp
        tests/collect_test.cpp
        tests/app_test.cpp)
    target_link_libraries(rbprobe_tests PRIVATE rbprobe_core Catch2::Catch2WithMain)
    catch_discover_tests(rbprobe_tests)
endif()
