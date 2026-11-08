# SPDX-License-Identifier: GPL-2.0-or-later
add_library(rbctl_core STATIC src/app.cpp)
target_include_directories(rbctl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rbctl_core PUBLIC radioberry_common radioberry_report)

add_executable(rbctl src/main.cpp)
target_link_libraries(rbctl PRIVATE rbctl_core radioberry_device)
install(TARGETS rbctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(rbctl_tests tests/app_test.cpp)
    target_link_libraries(rbctl_tests PRIVATE rbctl_core Catch2::Catch2WithMain)
    catch_discover_tests(rbctl_tests)
endif()
