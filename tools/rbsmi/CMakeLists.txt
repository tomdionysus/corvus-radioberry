# SPDX-License-Identifier: GPL-2.0-or-later
#
# rbsmi: the RX bus read by the BCM2711's SMI with DMA on a Raspberry Pi 4,
# parsed with the driver's own code (driver/kernel/rb_smi.h). A measurement
# tool for TODO/RPI4-SMI.md step 1; not installed.
add_library(rbsmi_core STATIC src/app.cpp)
target_include_directories(rbsmi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/driver/kernel)
target_link_libraries(rbsmi_core PUBLIC radioberry_common radioberry_report)

add_executable(rbsmi src/main.cpp)
target_link_libraries(rbsmi PRIVATE rbsmi_core)

if(RADIOBERRY_BUILD_TESTS)
    add_executable(rbsmi_tests tests/app_test.cpp)
    target_include_directories(rbsmi_tests PRIVATE ${PROJECT_SOURCE_DIR}/driver/tests)
    target_link_libraries(rbsmi_tests PRIVATE rbsmi_core Catch2::Catch2WithMain)
    catch_discover_tests(rbsmi_tests)
endif()
