# AppKit-only identities beside Dawn.app. Compile with swiftc directly so this
# also works with Makefile generators, without enabling CMake's Swift language.
find_program(DN_SWIFTC_EXECUTABLE swiftc REQUIRED)
find_program(DN_LIPO_EXECUTABLE lipo REQUIRED)

set(dn_launcher_arches ${CMAKE_OSX_ARCHITECTURES})
if (NOT dn_launcher_arches)
	set(dn_launcher_arches "${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(dn_launcher_sdk "${CMAKE_OSX_SYSROOT}")
if (NOT IS_DIRECTORY "${dn_launcher_sdk}")
	execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
		OUTPUT_VARIABLE dn_launcher_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
		COMMAND_ERROR_IS_FATAL ANY)
endif()

set(dn_launcher_slices)
foreach (arch IN LISTS dn_launcher_arches)
	set(slice "${CMAKE_CURRENT_BINARY_DIR}/dn-launcher-${arch}")
	add_custom_command(OUTPUT "${slice}"
		COMMAND "${DN_SWIFTC_EXECUTABLE}" -swift-version 5 -O
			-target "${arch}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}"
			-sdk "${dn_launcher_sdk}" -framework AppKit
			"${CMAKE_CURRENT_SOURCE_DIR}/mode-launcher.swift" -o "${slice}"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mode-launcher.swift"
		VERBATIM)
	list(APPEND dn_launcher_slices "${slice}")
endforeach()

set(dn_launcher "${CMAKE_CURRENT_BINARY_DIR}/dn-launcher")
add_custom_command(OUTPUT "${dn_launcher}"
	COMMAND "${DN_LIPO_EXECUTABLE}" -create ${dn_launcher_slices} -output "${dn_launcher}"
	DEPENDS ${dn_launcher_slices} VERBATIM)

foreach (DN_LAUNCH_ID IN ITEMS CropJpeg Commander)
	if (DN_LAUNCH_ID STREQUAL CropJpeg)
		set(DN_LAUNCH_NAME "Dawn JPEG Cropper")
		set(DN_LAUNCH_MODE cropjpeg)
		set(DN_LAUNCH_TYPE_NAME JPEG)
		set(DN_LAUNCH_UTI public.jpeg)
		set(DN_LAUNCH_ROLE Editor)
	else()
		set(DN_LAUNCH_NAME "Dawn Commander")
		set(DN_LAUNCH_MODE commander)
		set(DN_LAUNCH_TYPE_NAME Folder)
		set(DN_LAUNCH_UTI public.folder)
		set(DN_LAUNCH_ROLE Viewer)
	endif()
	set(bundle "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${DN_LAUNCH_NAME}.app")
	file(MAKE_DIRECTORY "${bundle}/Contents/MacOS" "${bundle}/Contents/Resources")
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Launcher-Info.plist.in"
		"${bundle}/Contents/Info.plist" @ONLY)
	add_custom_command(OUTPUT "${bundle}/Contents/MacOS/dn-launcher"
			"${bundle}/Contents/Resources/dn.icns"
		COMMAND "${CMAKE_COMMAND}" -E copy "${dn_launcher}" "${bundle}/Contents/MacOS/dn-launcher"
		COMMAND "${CMAKE_COMMAND}" -E copy "${DN_ICON_ICNS}" "${bundle}/Contents/Resources/dn.icns"
		DEPENDS "${dn_launcher}" "${DN_ICON_ICNS}" VERBATIM)
	add_custom_target(dn-launcher-${DN_LAUNCH_ID} ALL
		DEPENDS "${bundle}/Contents/MacOS/dn-launcher" "${bundle}/Contents/Resources/dn.icns")
	add_dependencies(dn-launcher-${DN_LAUNCH_ID} dn)
	install(DIRECTORY "${bundle}" DESTINATION . USE_SOURCE_PERMISSIONS)
endforeach()
