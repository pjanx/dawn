qt_add_executable(dn
	main.cpp
	app.cpp
	window.cpp
	renderer.cpp
	gpu.cpp
	kit.cpp
	chrome.cpp
	app-menu.cpp
	hint.cpp
	action.cpp
	viewer.cpp
	browser.cpp
	thumbnailer.cpp
	thumbnail-cache.cpp
	overlay.cpp
	cie-diagram.cpp
	sheet.cpp
	xdg.cpp
	display-profile.cpp
	icons.qrc
)
set_target_properties(dn PROPERTIES
	AUTOMOC ON
	AUTORCC ON
	MACOSX_BUNDLE ON
	MACOSX_BUNDLE_GUI_IDENTIFIER name.janouch.dn)
target_include_directories(dn PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
	"${CMAKE_SOURCE_DIR}/libdn"
)
find_package(Threads REQUIRED)
target_link_libraries(dn PRIVATE
	dawn::libdn
	Qt6::Gui
	PkgConfig::WEBP
	PkgConfig::WEBPDEMUX
	PkgConfig::WEBPMUX
	PkgConfig::RESVG
	Vulkan::Vulkan
	Threads::Threads
)

set(DN_UI_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(DN_UI_SHADER_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DN_UI_SHADER_GEN}")

function(dn_ui_embed_shader src symbol)
	get_filename_component(name "${src}" NAME)
	set(spv "${DN_UI_SHADER_GEN}/${name}.spv")
	string(REPLACE "." "-" header_stem "${name}")
	set(hdr "${DN_UI_SHADER_GEN}/${header_stem}-spv.h")
	add_custom_command(
		OUTPUT "${spv}" "${hdr}"
		COMMAND "${GLSLANG_VALIDATOR}" -V "${src}" -o "${spv}"
		COMMAND "${CMAKE_COMMAND}"
			"-DINPUT=${spv}" "-DOUTPUT=${hdr}" "-DSYMBOL=${symbol}"
			-P "${PROJECT_SOURCE_DIR}/cmake/embed-spirv.cmake"
		DEPENDS "${src}" "${PROJECT_SOURCE_DIR}/cmake/embed-spirv.cmake"
		COMMENT "SPIR-V ${name}"
		VERBATIM
	)
	target_sources(dn PRIVATE "${hdr}")
endfunction()

dn_ui_embed_shader("${DN_UI_SHADER_DIR}/overlay.vert" overlay_vert)
dn_ui_embed_shader("${DN_UI_SHADER_DIR}/overlay.frag" overlay_frag)
dn_ui_embed_shader("${CMAKE_SOURCE_DIR}/libdn/shaders/fullscreen.vert"
	fullscreen_vert)
dn_ui_embed_shader("${DN_UI_SHADER_DIR}/dither.frag" dither_frag)

if(WIN32)
	target_sources(dn PRIVATE
		display-profile-windows.cpp
		assoc-windows.cpp
	)
	target_link_libraries(dn PRIVATE gdi32 shell32 shlwapi ole32 advapi32)
	set_target_properties(dn PROPERTIES
		WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
elseif(APPLE)
	enable_language(OBJCXX)
	find_library(DN_APPKIT_FRAMEWORK AppKit REQUIRED)
	find_library(DN_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
	find_library(DN_CORESERVICES_FRAMEWORK CoreServices REQUIRED)
	target_sources(dn PRIVATE
		display-profile-macos.mm
		assoc-macos.mm
		app-menu-macos.mm
	)
	target_link_libraries(dn PRIVATE
		"${DN_APPKIT_FRAMEWORK}"
		"${DN_COREGRAPHICS_FRAMEWORK}"
		"${DN_CORESERVICES_FRAMEWORK}"
	)
else()
	pkg_check_modules(DN_COLORD REQUIRED IMPORTED_TARGET colord)
	target_sources(dn PRIVATE
		display-profile-linux.cpp
		assoc-unix.cpp
		instance.cpp
	)
	target_link_libraries(dn PRIVATE PkgConfig::DN_COLORD)
endif()

# Application icon: SVG → PNG/ICO (xT) on non-Apple; Swift squircle ICNS (xM)
# on Darwin. Qt looks up IDI_ICON1 and nothing else.
set(DN_ICON_SVG "${CMAKE_CURRENT_SOURCE_DIR}/dn.svg")
if(APPLE)
	find_program(DN_SWIFT_EXECUTABLE swift REQUIRED)
	set(MACOSX_BUNDLE_ICON_FILE dn.icns)
	set(DN_ICON_ICNS "${CMAKE_CURRENT_BINARY_DIR}/${MACOSX_BUNDLE_ICON_FILE}")
	add_custom_command(OUTPUT "${DN_ICON_ICNS}"
		COMMAND "${DN_SWIFT_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/gen-icon.swift" "${DN_ICON_ICNS}"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen-icon.swift"
		COMMENT "Generating dn application icon" VERBATIM)
	set_source_files_properties("${DN_ICON_ICNS}" PROPERTIES
		MACOSX_PACKAGE_LOCATION Resources)
	target_sources(dn PRIVATE "${DN_ICON_ICNS}")
	set_target_properties(dn PROPERTIES
		MACOSX_BUNDLE_ICON_FILE "${MACOSX_BUNDLE_ICON_FILE}")
else()
	include(IconUtils)
	set(DN_ICON_BASE "${PROJECT_SOURCE_DIR}/icons")
	set(DN_ICON_PNG_LIST)
	foreach(DN_ICON_SIZE 16 32 48 256)
		icon_to_png(dn "${DN_ICON_SVG}" ${DN_ICON_SIZE}
			"${DN_ICON_BASE}" DN_ICON_PNG)
		list(APPEND DN_ICON_PNG_LIST "${DN_ICON_PNG}")
	endforeach()

	if(WIN32)
		list(REMOVE_ITEM DN_ICON_PNG_LIST "${DN_ICON_PNG}")
		set(DN_ICON_ICO "${CMAKE_CURRENT_BINARY_DIR}/dn.ico")
		icon_for_win32("${DN_ICON_ICO}" "${DN_ICON_PNG_LIST}" "${DN_ICON_PNG}")

		set(DN_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/dn.rc")
		# Qt specifically looks up IDI_ICON1 and nothing else.
		add_custom_command(OUTPUT "${DN_ICON_RC}"
			COMMAND ${CMAKE_COMMAND} -E echo "IDI_ICON1 ICON \"dn.ico\""
				> ${DN_ICON_RC} VERBATIM)
		set_property(SOURCE "${DN_ICON_RC}"
			APPEND PROPERTY OBJECT_DEPENDS ${DN_ICON_ICO})
		target_sources(dn PRIVATE "${DN_ICON_RC}")
	else()
		add_custom_target(icons ALL DEPENDS ${DN_ICON_PNG_LIST})
		include(GNUInstallDirs)
		install(FILES "${DN_ICON_SVG}"
			DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
		install(DIRECTORY "${DN_ICON_BASE}"
			DESTINATION "${CMAKE_INSTALL_DATADIR}")
		install(FILES
			"${CMAKE_CURRENT_SOURCE_DIR}/dn.desktop"
			"${CMAKE_CURRENT_SOURCE_DIR}/dn-browse.desktop"
			DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
	endif()
endif()

if(DN_WITH_WAYLAND)
	pkg_get_variable(DN_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
	if(NOT DN_WAYLAND_PROTOCOLS_DIR)
		message(FATAL_ERROR "wayland-protocols pkgdatadir not found")
	endif()

	pkg_get_variable(DN_WAYLAND_SCANNER wayland-scanner wayland_scanner)
	if(NOT DN_WAYLAND_SCANNER)
		find_program(DN_WAYLAND_SCANNER wayland-scanner REQUIRED)
	endif()

	set(DN_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
	file(MAKE_DIRECTORY "${DN_PROTOCOL_DIR}")
	set(DN_COLOR_XML
		"${DN_WAYLAND_PROTOCOLS_DIR}/staging/color-management/color-management-v1.xml")
	set(DN_COLOR_HEADER
		"${DN_PROTOCOL_DIR}/color-management-v1-client-protocol.h")
	set(DN_COLOR_SOURCE
		"${DN_PROTOCOL_DIR}/color-management-v1-protocol.c")
	add_custom_command(
		OUTPUT "${DN_COLOR_HEADER}" "${DN_COLOR_SOURCE}"
		COMMAND "${DN_WAYLAND_SCANNER}" client-header
			"${DN_COLOR_XML}" "${DN_COLOR_HEADER}"
		COMMAND "${DN_WAYLAND_SCANNER}" private-code
			"${DN_COLOR_XML}" "${DN_COLOR_SOURCE}"
		DEPENDS "${DN_COLOR_XML}"
		COMMENT "wayland-scanner color-management-v1 for dn"
		VERBATIM
	)

	target_sources(dn PRIVATE
		wayland-color-bridge.cpp
		wayland-window.cpp
		"${DN_COLOR_HEADER}"
		"${DN_COLOR_SOURCE}"
	)
	target_include_directories(dn PRIVATE "${DN_PROTOCOL_DIR}")
	target_link_libraries(dn PRIVATE PkgConfig::DN_WAYLAND)
endif()

# User guide: build tree at share/doc/dn (../share from bin/), installed
# next to the application on each platform.
set(DN_HELP_DIR "${CMAKE_BINARY_DIR}/share/doc/dn")
file(MAKE_DIRECTORY "${DN_HELP_DIR}")
configure_file(
	"${CMAKE_SOURCE_DIR}/doc/dn.html.in"
	"${DN_HELP_DIR}/dn.html"
	@ONLY)
configure_file(
	"${CMAKE_SOURCE_DIR}/doc/stylesheet.css"
	"${DN_HELP_DIR}/stylesheet.css"
	COPYONLY)

if(APPLE)
	set_source_files_properties(
		"${DN_HELP_DIR}/dn.html"
		"${DN_HELP_DIR}/stylesheet.css"
		PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/share/doc/dn")
	target_sources(dn PRIVATE
		"${DN_HELP_DIR}/dn.html"
		"${DN_HELP_DIR}/stylesheet.css")
elseif(WIN32)
	install(FILES
		"${DN_HELP_DIR}/dn.html"
		"${DN_HELP_DIR}/stylesheet.css"
		DESTINATION share/doc/dn)
else()
	include(GNUInstallDirs)
	install(FILES
		"${DN_HELP_DIR}/dn.html"
		"${DN_HELP_DIR}/stylesheet.css"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dn")
endif()

if(BUILD_TESTING)
	add_test(NAME dn_help COMMAND dn --help)
	set_tests_properties(dn_help PROPERTIES
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
	)
endif()
