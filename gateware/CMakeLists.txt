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

# The gateware-v2 Raspberry Pi 4 receive image, which the driver's I2S
# transport loads as radioberry-rpi4-i2s.rbf (10CL025 only so far).
set(_rpi4 ${CMAKE_CURRENT_SOURCE_DIR}/../gateware-v2/images/rpi4-i2s/CL025/radioberry-rpi4-i2s.rbf)
if(EXISTS ${_rpi4})
    install(FILES ${_rpi4} DESTINATION ${RADIOBERRY_GATEWARE_DIR}/rpi4-i2s/CL025)
endif()
# The gateware-v2 Raspberry Pi 5 image, which the driver's PIO transport
# loads as radioberry-rpi5-pio.rbf (10CL025 only so far).
set(_rpi5 ${CMAKE_CURRENT_SOURCE_DIR}/../gateware-v2/images/rpi5-pio/CL025/radioberry-rpi5-pio.rbf)
if(EXISTS ${_rpi5})
    install(FILES ${_rpi5} DESTINATION ${RADIOBERRY_GATEWARE_DIR}/rpi5-pio/CL025)
endif()

# The Debian package turns this off: its install script links the image
# for the FPGA the user chooses instead.
option(RADIOBERRY_INSTALL_FIRMWARE "Install the FPGATYPE image as the driver's firmware" ON)
set(_selected ${CMAKE_CURRENT_SOURCE_DIR}/rpi-5/${FPGATYPE}/radioberry.rbf)
if(NOT RADIOBERRY_INSTALL_FIRMWARE)
    message(STATUS "Driver firmware not installed (RADIOBERRY_INSTALL_FIRMWARE=OFF)")
elseif(EXISTS ${_selected})
    install(FILES ${_selected} DESTINATION ${RADIOBERRY_FIRMWARE_DIR})
    if(FPGATYPE STREQUAL "CL025" AND EXISTS ${_rpi4})
        install(FILES ${_rpi4} DESTINATION ${RADIOBERRY_FIRMWARE_DIR})
    endif()
    if(FPGATYPE STREQUAL "CL025" AND EXISTS ${_rpi5})
        install(FILES ${_rpi5} DESTINATION ${RADIOBERRY_FIRMWARE_DIR})
    endif()
else()
    message(STATUS "No gateware image for ${FPGATYPE} in gateware/rpi-5 yet; not installed")
endif()
