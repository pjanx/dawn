# Public Domain

function (icon_to_png name svg size output_dir output)
	set (_dimensions "${size}x${size}")
	set (_png_path "${output_dir}/hicolor/${_dimensions}/apps")
	set (_png "${_png_path}/${name}.png")
	set (${output} "${_png}" PARENT_SCOPE)

	set (_find_program_REQUIRE)
	if (NOT ${CMAKE_VERSION} VERSION_LESS 3.18.0)
		set (_find_program_REQUIRE REQUIRED)
	endif ()

	find_program (rsvg_convert_EXECUTABLE rsvg-convert ${_find_program_REQUIRE})
	add_custom_command (OUTPUT "${_png}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_png_path}"
		COMMAND ${rsvg_convert_EXECUTABLE} "--output=${_png}"
			"--width=${size}" "--height=${size}" -- "${svg}"
		DEPENDS "${svg}"
		COMMENT "Generating ${name} ${_dimensions} application icon" VERBATIM)
endfunction ()

# You should include a 256x256 icon--which takes less space as raw PNG.
function (icon_for_win32 ico pngs pngs_raw)
	set (_raws)
	foreach (png ${pngs_raw})
		list (APPEND _raws "--raw=${png}")
	endforeach ()

	set (_find_program_REQUIRE)
	if (NOT ${CMAKE_VERSION} VERSION_LESS 3.18.0)
		set (_find_program_REQUIRE REQUIRED)
	endif ()

	find_program (icotool_EXECUTABLE icotool ${_find_program_REQUIRE})
	add_custom_command (OUTPUT "${ico}"
		COMMAND ${icotool_EXECUTABLE} -c -o "${ico}" ${_raws} -- ${pngs}
		DEPENDS ${pngs} ${pngs_raw}
		COMMENT "Generating Windows program icon" VERBATIM)
endfunction ()
