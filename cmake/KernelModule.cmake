# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds an out-of-tree kernel module with kbuild from CMake.
#
#   radioberry_add_kernel_module(<name>
#       SOURCE_DIR <dir with Kbuild>
#       [INCLUDE_DIR <extra header dir, passed to kbuild as RB_INCLUDE>]
#       SOURCES <files that trigger a rebuild>...)
#
# Adds target <name>_module, which runs `make -C <kernel build dir> M=...`
# in a copy of SOURCE_DIR inside the build tree (kbuild writes its objects
# next to the sources), and installs the .ko under
# /lib/modules/<release>/updates/ followed by depmod.

set(KERNEL_RELEASE "" CACHE STRING "Kernel release to build modules for (default: running kernel)")
if(NOT KERNEL_RELEASE)
    execute_process(COMMAND uname -r OUTPUT_VARIABLE KERNEL_RELEASE
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
set(KERNEL_BUILD_DIR "/lib/modules/${KERNEL_RELEASE}/build"
    CACHE PATH "Kernel build tree used by kbuild")
set(KERNEL_MODULE_INSTALL_DIR "/lib/modules/${KERNEL_RELEASE}/updates"
    CACHE PATH "Where cmake --install puts kernel modules")
option(RADIOBERRY_BUILD_MODULE "Build the kernel module (needs kernel headers)" ON)

if(RADIOBERRY_BUILD_MODULE AND NOT EXISTS "${KERNEL_BUILD_DIR}/Makefile")
    message(WARNING "No kernel build tree at ${KERNEL_BUILD_DIR}; kernel module disabled. "
                    "Install the linux-headers package for ${KERNEL_RELEASE}.")
    set(RADIOBERRY_BUILD_MODULE OFF)
endif()

function(radioberry_add_kernel_module name)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;INCLUDE_DIR" "SOURCES" ${ARGN})
    if(NOT RADIOBERRY_BUILD_MODULE)
        return()
    endif()
    set(work ${CMAKE_CURRENT_BINARY_DIR}/kbuild)
    set(ko ${work}/${name}.ko)
    add_custom_command(
        OUTPUT ${ko}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${ARG_SOURCE_DIR} ${work}
        COMMAND make -C ${KERNEL_BUILD_DIR} M=${work} RB_INCLUDE=${ARG_INCLUDE_DIR} modules
        DEPENDS ${ARG_SOURCES}
        COMMENT "Building kernel module ${name}.ko for ${KERNEL_RELEASE}"
        VERBATIM)
    add_custom_target(${name}_module ALL DEPENDS ${ko})
    install(FILES ${ko} DESTINATION ${KERNEL_MODULE_INSTALL_DIR})
    install(CODE "execute_process(COMMAND depmod -a ${KERNEL_RELEASE})")
endfunction()
