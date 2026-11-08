# SPDX-License-Identifier: GPL-2.0-or-later
#
# Sets RADIOBERRY_VERSION from `git describe` at configure time and writes
# a header with it into the build tree.

find_package(Git QUIET)
set(RADIOBERRY_VERSION "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _rb_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rb_describe_result)
    if(_rb_describe_result EQUAL 0 AND _rb_describe)
        set(RADIOBERRY_VERSION "${_rb_describe}")
    endif()
endif()

configure_file(${CMAKE_CURRENT_LIST_DIR}/version.hpp.in
               ${CMAKE_BINARY_DIR}/generated/radioberry/version.hpp @ONLY)
