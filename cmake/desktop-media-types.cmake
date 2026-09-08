#
# desktop-media-types.cmake: fill in a desktop file's MimeType= from dn
#
# Copyright The Dawn Authors
# SPDX-License-Identifier: MPL-2.0
#
# Usage: cmake [-DEMULATOR=wine] -DDN=dn -DINPUT=in -DOUTPUT=out
#            -P desktop-media-types.cmake
#

execute_process(COMMAND ${EMULATOR} "${DN}" --list-supported-media-types
	OUTPUT_VARIABLE types OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE result)
if (NOT result EQUAL 0 OR NOT types)
	message(FATAL_ERROR "${DN} reported no media types (${result})")
endif()

# Desktop entries separate and terminate list values with semicolons,
# which is also how CMake joins the elements of a list.
string(REPLACE "\n" ";" types "${types}")
file(READ "${INPUT}" contents)
string(REGEX REPLACE "MimeType=[^\n]*" "MimeType=${types};" contents
	"${contents}")
file(WRITE "${OUTPUT}" "${contents}")
