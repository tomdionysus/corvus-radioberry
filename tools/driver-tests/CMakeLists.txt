# SPDX-License-Identifier: GPL-2.0-or-later
#
# Hardware tests of radioberry.ko through /dev/radioberry. Not part of
# ctest: they need the Radioberry and an idle device. tools/ci-pi.sh runs
# them when the device is present and free.
if(RADIOBERRY_BUILD_TESTS)
    add_executable(rbdrivertest driver_test.cpp)
    target_link_libraries(rbdrivertest PRIVATE
        radioberry_abi radioberryd_device radioberry_device Catch2::Catch2WithMain)
endif()
