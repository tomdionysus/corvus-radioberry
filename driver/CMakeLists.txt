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
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/rb_frame.h
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
# Userspace takes it from driver/include, or with RADIOBERRY_ABI_INCLUDE_DIR
# from an installed copy, whose ABI version must match this tree's.
set(RADIOBERRY_ABI_INCLUDE_DIR "" CACHE PATH
    "Build userspace against radioberry_ioctl.h in this directory instead of driver/include")
add_library(radioberry_abi INTERFACE)
if(RADIOBERRY_ABI_INCLUDE_DIR)
    set(_abi_header ${RADIOBERRY_ABI_INCLUDE_DIR}/radioberry_ioctl.h)
    if(NOT EXISTS ${_abi_header})
        message(FATAL_ERROR "RADIOBERRY_ABI_INCLUDE_DIR: no ${_abi_header}")
    endif()
    file(STRINGS ${_abi_header} _abi_installed REGEX "^#define RADIOBERRY_ABI_VERSION ")
    file(STRINGS include/radioberry_ioctl.h _abi_source REGEX "^#define RADIOBERRY_ABI_VERSION ")
    if(NOT _abi_installed STREQUAL _abi_source)
        message(FATAL_ERROR "${_abi_header} has '${_abi_installed}', this tree '${_abi_source}'")
    endif()
    file(READ ${_abi_header} _abi_installed_text)
    file(READ include/radioberry_ioctl.h _abi_source_text)
    if(NOT _abi_installed_text STREQUAL _abi_source_text)
        message(WARNING "${_abi_header} differs from driver/include/radioberry_ioctl.h "
                        "at the same ABI version")
    endif()
    target_include_directories(radioberry_abi INTERFACE ${RADIOBERRY_ABI_INCLUDE_DIR})
else()
    target_include_directories(radioberry_abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
install(FILES include/radioberry_ioctl.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(driver_tests tests/logic_test.cpp tests/pio_test.cpp tests/frame_test.cpp)
    # The module's own logic is tested against the source tree's header.
    target_include_directories(driver_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(driver_tests PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(driver_tests)
endif()
