# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.25)

project(radioberry LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(RADIOBERRY_BUILD_TESTS "Build the unit tests" ON)

set(FPGATYPE "CL025" CACHE STRING "FPGA on the Radioberry: CL016 or CL025")
set_property(CACHE FPGATYPE PROPERTY STRINGS CL016 CL025)
if(NOT FPGATYPE MATCHES "^(CL016|CL025)$")
    message(FATAL_ERROR "FPGATYPE must be CL016 or CL025, not '${FPGATYPE}'")
endif()
set(RADIOBERRY_FIRMWARE_DIR "/lib/firmware" CACHE PATH "Where the driver's gateware is installed")

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)

include(GNUInstallDirs)
include(cmake/Version.cmake)
include(cmake/Instrumentation.cmake)
include(cmake/KernelModule.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiodcxx>=2.0)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(spdlog REQUIRED)
find_library(I2C_LIBRARY NAMES i2c REQUIRED)

if(RADIOBERRY_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
    include(CTest)
    include(Catch)
endif()

add_subdirectory(driver)
add_subdirectory(daemon)
add_subdirectory(tools)
add_subdirectory(gateware)

message(STATUS "radioberry ${RADIOBERRY_VERSION}: FPGATYPE=${FPGATYPE}, "
               "kernel module ${RADIOBERRY_BUILD_MODULE} (${KERNEL_RELEASE})")
