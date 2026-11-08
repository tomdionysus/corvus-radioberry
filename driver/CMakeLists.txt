# SPDX-License-Identifier: GPL-2.0-or-later
#
# Kernel module, device tree overlay and ioctl ABI header.

set(RADIOBERRY_OVERLAY_DIR "/boot/firmware/overlays" CACHE PATH "Where the overlay is installed")

radioberry_add_kernel_module(radioberry
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/kernel
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/Kbuild
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/radioberry.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/rb_logic.h
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/rb_pio.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/radioberry_ioctl.h)

find_program(DTC_EXECUTABLE dtc)
if(DTC_EXECUTABLE)
    set(_dtbo ${CMAKE_CURRENT_BINARY_DIR}/radioberry.dtbo)
    add_custom_command(
        OUTPUT ${_dtbo}
        COMMAND ${DTC_EXECUTABLE} -@ -q -I dts -O dtb -o ${_dtbo}
                ${CMAKE_CURRENT_SOURCE_DIR}/radioberry.dts
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/radioberry.dts
        COMMENT "Building device tree overlay radioberry.dtbo"
        VERBATIM)
    add_custom_target(radioberry_overlay ALL DEPENDS ${_dtbo})
    install(FILES ${_dtbo} DESTINATION ${RADIOBERRY_OVERLAY_DIR})
else()
    message(WARNING "dtc not found; the device tree overlay is not built")
endif()

# The ABI header is the only interface between the module and userspace.
add_library(radioberry_abi INTERFACE)
target_include_directories(radioberry_abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
install(FILES include/radioberry_ioctl.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(driver_tests tests/logic_test.cpp tests/pio_test.cpp)
    target_include_directories(driver_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel)
    target_link_libraries(driver_tests PRIVATE radioberry_abi Catch2::Catch2WithMain)
    catch_discover_tests(driver_tests)
endif()
