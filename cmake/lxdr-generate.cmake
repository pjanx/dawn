#
# lxdr-generate.cmake: build-time LibertyXDR C++ header generation
#
# Copyright The Dawn Authors
# SPDX-License-Identifier: MPL-2.0
#
# lxdr_generate(<out_header> <namespace> <prefix_camel> <lxdr>...
#               [EXTERN <lxdr>...])
#
# Writes <out_header> (typically .../<name>.lxdr.hpp) from the listed
# .lxdr files.  EXTERN ones are read for their types only, and included
# from beside <out_header> rather than repeated.  Does not attach the
# header to any compile target.
#

function (lxdr_generate out_header namespace prefix_camel)
	cmake_parse_arguments(arg "" "" "EXTERN" ${ARGN})
	if (NOT AWK)
		message(FATAL_ERROR "lxdr_generate requires AWK")
	endif()
	if (NOT arg_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "lxdr_generate: no .lxdr inputs")
	endif()

	set(lxdrgen "${PROJECT_SOURCE_DIR}/submodules/liberty/tools/lxdrgen.awk")
	set(lxdrgen_cpp "${PROJECT_SOURCE_DIR}/ipc/lxdrgen-cpp.awk")

	get_filename_component(out_dir "${out_header}" DIRECTORY)
	get_filename_component(out_name "${out_header}" NAME)
	get_filename_component(out_subdir "${out_dir}" NAME)

	# The extern ones have to come first, and are named to AWK by the
	# include stem their own header was generated under.
	set(lxdr_abs)
	set(extern_stems)
	foreach (f IN LISTS arg_EXTERN arg_UNPARSED_ARGUMENTS)
		if (NOT IS_ABSOLUTE "${f}")
			get_filename_component(f "${f}" ABSOLUTE)
		endif()
		list(APPEND lxdr_abs "${f}")
	endforeach()
	foreach (f IN LISTS arg_EXTERN)
		get_filename_component(f "${f}" NAME)
		list(APPEND extern_stems "${out_subdir}/${f}")
	endforeach()
	list(JOIN extern_stems " " extern_arg)

	add_custom_command(
		OUTPUT "${out_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
		COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
			${AWK} -f "${lxdrgen}" -f "${lxdrgen_cpp}"
			-v "PrefixCamel=${prefix_camel}"
			-v "Namespace=${namespace}"
			-v "Extern=${extern_arg}"
			${lxdr_abs}
			> "${out_header}"
		DEPENDS ${lxdr_abs} "${lxdrgen}" "${lxdrgen_cpp}"
		COMMENT "Generating ${out_name}"
		VERBATIM
	)
endfunction()
