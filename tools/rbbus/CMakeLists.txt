# SPDX-License-Identifier: GPL-2.0-or-later
#
# rbbus: the RX and TX buses clocked by the CPU on a Raspberry Pi 4, with
# the driver's own bus code (driver/kernel/rb_bitbang.h).
add_library(rbbus_core STATIC src/app.cpp)
target_include_directories(rbbus_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/driver/kernel)
target_link_libraries(rbbus_core PUBLIC radioberry_common radioberry_report)

add_executable(rbbus src/main.cpp)
target_link_libraries(rbbus PRIVATE rbbus_core)
install(TARGETS rbbus RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(rbbus_tests tests/app_test.cpp)
    target_include_directories(rbbus_tests PRIVATE ${PROJECT_SOURCE_DIR}/driver/tests)
    target_link_libraries(rbbus_tests PRIVATE rbbus_core Catch2::Catch2WithMain)
    catch_discover_tests(rbbus_tests)
endif()
