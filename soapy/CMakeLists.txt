# SPDX-License-Identifier: GPL-2.0-or-later
#
# The SoapySDR module (RADIOBERRY_BUILD_SOAPY). It reuses radioberryd's
# device layer, session and transmit policy by compiling their sources a
# second time, position-independent and with hidden symbols, into its own
# library; the daemon's targets are not touched.

find_package(SoapySDR CONFIG QUIET)
if(NOT SoapySDR_FOUND)
    message(FATAL_ERROR
        "RADIOBERRY_BUILD_SOAPY needs SoapySDR's development files "
        "(apt install libsoapysdr-dev), or configure with -DRADIOBERRY_BUILD_SOAPY=OFF")
endif()
find_package(Threads REQUIRED)

set(_daemon ${PROJECT_SOURCE_DIR}/daemon/src)
set(_common ${PROJECT_SOURCE_DIR}/tools/common)

# Everything but the registration, for the module and the tests.
add_library(radioberry_soapy STATIC
    src/convert.cpp
    src/find.cpp
    src/log.cpp
    src/radio.cpp
    src/settings.cpp
    ${_daemon}/device/control.cpp
    ${_daemon}/device/monitor.cpp
    ${_daemon}/device/rx_pump.cpp
    ${_daemon}/device/service.cpp
    ${_daemon}/device/tx_pump.cpp
    ${_daemon}/options.cpp
    ${_daemon}/realtime.cpp
    ${_daemon}/session.cpp
    ${_daemon}/tx_policy.cpp
    ${_common}/src/command.cpp
    ${_common}/src/linux_device.cpp)
set_target_properties(radioberry_soapy PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(radioberry_soapy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${_daemon}
    ${_common}/include
    ${CMAKE_BINARY_DIR}/generated)
# spdlog header-only: the module's own copy, so its logger is not the host
# program's.
target_link_libraries(radioberry_soapy PUBLIC
    SoapySDR radioberry_abi spdlog::spdlog_header_only yaml-cpp::yaml-cpp Threads::Threads)

# The module SoapySDR loads, in its module directory for this ABI.
add_library(radioberrySupport MODULE src/registration.cpp)
target_link_libraries(radioberrySupport PRIVATE radioberry_soapy -Wl,--no-undefined)
set_target_properties(radioberrySupport PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
install(TARGETS radioberrySupport
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/SoapySDR/modules${SOAPY_SDR_ABI_VERSION})

if(RADIOBERRY_BUILD_TESTS)
    add_executable(soapy_tests
        tests/convert_test.cpp
        tests/find_test.cpp
        tests/log_test.cpp
        tests/radio_test.cpp
        tests/settings_test.cpp
        ${PROJECT_SOURCE_DIR}/daemon/tests/fake_device.cpp)
    target_include_directories(soapy_tests PRIVATE ${PROJECT_SOURCE_DIR}/daemon/tests)
    target_link_libraries(soapy_tests PRIVATE radioberry_soapy Catch2::Catch2WithMain)
    target_compile_definitions(soapy_tests PRIVATE
        RADIOBERRY_DEFAULT_CONFIG="${PROJECT_SOURCE_DIR}/daemon/systemd/config.yaml")
    catch_discover_tests(soapy_tests)

    # SoapySDR itself loads the built module. Not in the sanitizer build: an
    # instrumented module cannot load into the uninstrumented SoapySDRUtil.
    find_program(SOAPY_SDR_UTIL SoapySDRUtil)
    if(SOAPY_SDR_UTIL AND NOT RADIOBERRY_SANITIZE)
        add_test(NAME soapy_module_loads
                 COMMAND ${CMAKE_COMMAND} -E env
                         SOAPY_SDR_PLUGIN_PATH=$<TARGET_FILE_DIR:radioberrySupport>
                         ${SOAPY_SDR_UTIL} --check=radioberry)
    endif()

    # Hardware tests through SoapySDR and the module it finds. Not part of
    # ctest: they need the RadioBerry and an idle device.
    add_executable(rbsoapytest tests/hw/radio_hw_test.cpp)
    target_link_libraries(rbsoapytest PRIVATE SoapySDR Catch2::Catch2WithMain)
endif()
