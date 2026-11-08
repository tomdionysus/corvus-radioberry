# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs the prebuilt gateware. Every image goes to the shared gateware
# directory (rbprobe --identify and rbload use them); the one matching
# FPGATYPE also becomes /lib/firmware/radioberry.rbf, which the driver loads.

set(RADIOBERRY_GATEWARE_DIR ${CMAKE_INSTALL_FULL_DATADIR}/radioberry/gateware)

foreach(type IN ITEMS CL016 CL025)
    set(image ${CMAKE_CURRENT_SOURCE_DIR}/rpi-5/${type}/radioberry.rbf)
    if(EXISTS ${image})
        install(FILES ${image} DESTINATION ${RADIOBERRY_GATEWARE_DIR}/rpi-5/${type})
    endif()
endforeach()

set(_selected ${CMAKE_CURRENT_SOURCE_DIR}/rpi-5/${FPGATYPE}/radioberry.rbf)
if(EXISTS ${_selected})
    install(FILES ${_selected} DESTINATION ${RADIOBERRY_FIRMWARE_DIR})
else()
    message(STATUS "No gateware image for ${FPGATYPE} in gateware/rpi-5 yet; not installed")
endif()
