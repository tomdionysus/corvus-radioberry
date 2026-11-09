# SPDX-License-Identifier: GPL-2.0-or-later
add_library(rbload_core STATIC src/app.cpp)
target_include_directories(rbload_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rbload_core PUBLIC radioberry_common radioberry_report)

add_executable(rbload src/main.cpp)
target_link_libraries(rbload PRIVATE rbload_core radioberry_gpiod)
install(TARGETS rbload RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(rbload_tests tests/app_test.cpp)
    target_link_libraries(rbload_tests PRIVATE rbload_core Catch2::Catch2WithMain)
    target_compile_definitions(rbload_tests PRIVATE RADIOBERRY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    catch_discover_tests(rbload_tests)
endif()
