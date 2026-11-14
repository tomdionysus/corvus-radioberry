# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs the prebuilt gateware images (images/, built with tools/build.sh
# and committed). Each goes to the shared gateware directory, which rbprobe
# reads, and into the firmware directory, from which the driver loads the
# one for its board.

set(RADIOBERRY_GATEWARE_DIR ${CMAKE_INSTALL_FULL_DATADIR}/radioberry/gateware)

# The Debian package turns this off: its install script links the images
# into /usr/lib/firmware instead.
option(RADIOBERRY_INSTALL_FIRMWARE "Install the images into RADIOBERRY_FIRMWARE_DIR" ON)

foreach(image IN ITEMS rpi4-i2s rpi5-pio)
    set(_file ${CMAKE_CURRENT_SOURCE_DIR}/images/${image}/CL025/radioberry-${image}.rbf)
    install(FILES ${_file} DESTINATION ${RADIOBERRY_GATEWARE_DIR}/${image}/CL025)
    if(RADIOBERRY_INSTALL_FIRMWARE)
        install(FILES ${_file} DESTINATION ${RADIOBERRY_FIRMWARE_DIR})
    endif()
endforeach()
