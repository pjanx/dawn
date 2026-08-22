#
# embed-shader.cmake: glslang SPIR-V compile + uint32_t header embed
#
# Copyright The dawn Authors
# SPDX-License-Identifier: MPL-2.0
#
# dawn_embed_shader(<target> <src> <symbol> [glslang args...])
#
# <src> is relative to ${PROJECT_SOURCE_DIR}/shaders.  Compiles it to
# SPIR-V and writes <target BINARY_DIR>/generated/<stem>-spv.h (stem =
# symbol with '_' → '-').  BINARY_DIR is the target's, so a call from
# another listfile still lands next to that target.  Targets defined
# in different directories (libdn vs dn) each get their own
# fullscreen-vert-spv.h.
#

function(dawn_embed_shader target src symbol)
	if(NOT GLSLANG_VALIDATOR)
		message(FATAL_ERROR "dawn_embed_shader requires GLSLANG_VALIDATOR")
	endif()
	if(NOT TARGET "${target}")
		message(FATAL_ERROR
			"dawn_embed_shader: target '${target}' does not exist")
	endif()

	string(REPLACE "_" "-" stem "${symbol}")
	get_target_property(bin_dir ${target} BINARY_DIR)
	set(gen_dir "${bin_dir}/generated")
	set(spv "${gen_dir}/${stem}.spv")
	set(hdr "${gen_dir}/${stem}-spv.h")
	set(shader_dir "${PROJECT_SOURCE_DIR}/shaders")
	set(src_abs "${shader_dir}/${src}")

	file(MAKE_DIRECTORY "${gen_dir}")
	add_custom_command(
		OUTPUT "${spv}" "${hdr}"
		COMMAND "${GLSLANG_VALIDATOR}"
			-V "-I${shader_dir}" ${ARGN} "${src_abs}" -o "${spv}"
		COMMAND "${CMAKE_COMMAND}"
			"-DINPUT=${spv}" "-DOUTPUT=${hdr}" "-DSYMBOL=${symbol}"
			-P "${PROJECT_SOURCE_DIR}/cmake/embed-spirv.cmake"
		DEPENDS "${src_abs}" "${shader_dir}/common.glsl"
			"${PROJECT_SOURCE_DIR}/cmake/embed-spirv.cmake"
		COMMENT "SPIR-V ${stem}"
		VERBATIM
	)

	target_sources(${target} PRIVATE "${hdr}")
	target_include_directories(${target} PRIVATE "${gen_dir}")
endfunction()
