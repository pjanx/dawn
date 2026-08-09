#
# LxdrGenerate.cmake: build-time LibertyXDR C++ header generation
#
# Copyright The dawn Authors
# SPDX-License-Identifier: MPL-2.0
#
# dn_lxdr_generate(<out_header> <namespace> <prefix_camel> <lxdr>...)
#
# Writes <out_header> (typically
# ${CMAKE_BINARY_DIR}/generated/ipc/<name>.lxdr.hpp) from the listed
# .lxdr files.  Does not attach the header to any compile target.
#

find_program(DN_CLANG_FORMAT NAMES clang-format)

function(dn_lxdr_generate out_header namespace prefix_camel)
	if(NOT AWK)
		message(FATAL_ERROR "dn_lxdr_generate requires AWK")
	endif()
	if(NOT ARGN)
		message(FATAL_ERROR "dn_lxdr_generate: no .lxdr inputs")
	endif()

	set(lxdrgen "${PROJECT_SOURCE_DIR}/ipc/lxdrgen.awk")
	set(lxdrgen_cpp "${PROJECT_SOURCE_DIR}/ipc/lxdrgen-cpp.awk")

	set(lxdr_abs)
	foreach(f IN LISTS ARGN)
		if(NOT IS_ABSOLUTE "${f}")
			get_filename_component(f "${f}" ABSOLUTE)
		endif()
		list(APPEND lxdr_abs "${f}")
	endforeach()

	get_filename_component(out_dir "${out_header}" DIRECTORY)
	get_filename_component(out_name "${out_header}" NAME)
	string(MAKE_C_IDENTIFIER "${out_name}" out_ident)

	set(format_cmd)
	if(DN_CLANG_FORMAT)
		set(format_cmd
			COMMAND "${DN_CLANG_FORMAT}"
				"--style=file:${PROJECT_SOURCE_DIR}/.clang-format"
				-i "${out_header}")
	endif()

	add_custom_command(
		OUTPUT "${out_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
		COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
			${AWK} -f "${lxdrgen}" -f "${lxdrgen_cpp}"
			-v "PrefixCamel=${prefix_camel}"
			-v "Namespace=${namespace}"
			${lxdr_abs}
			> "${out_header}"
		${format_cmd}
		DEPENDS ${lxdr_abs} "${lxdrgen}" "${lxdrgen_cpp}"
		COMMENT "Generating ${out_name}"
		VERBATIM
	)
	add_custom_target(dn_lxdr_${out_ident} DEPENDS "${out_header}")
endfunction()
