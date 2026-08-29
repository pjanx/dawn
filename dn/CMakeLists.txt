qt_add_executable(dn
	action.cpp
	app.cpp
	display-profile.cpp
	gpu.cpp
	kit-browser.cpp
	kit-chrome.cpp
	kit-cie-diagram.cpp
	kit.cpp
	kit-viewer.cpp
	main.cpp
	overlay.cpp
	renderer.cpp
	sheet.cpp
	thumbnail-cache.cpp
	thumbnailer.cpp
	url.cpp
	window.cpp
	xdg.cpp
	icons.qrc
)
set_target_properties(dn PROPERTIES AUTOMOC ON AUTORCC ON)
target_include_directories(dn PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/libdn"
)

# Widget geometry is integral device pixels, and the only way in is an explicit
# conversion: a silent float truncation here is a layout bug.  Scoped to dn, as
# vendored dependencies do not build clean under it.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(dn PRIVATE -Werror=float-conversion)
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	target_compile_options(dn PRIVATE -Werror=implicit-int-conversion)
endif()

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

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/cmf-cie1931-2deg-1nm.h
	COMMAND ${AWK} -f ${CMAKE_CURRENT_SOURCE_DIR}/cmf-to-header.awk
		${CMAKE_CURRENT_SOURCE_DIR}/ciexyz31_1.csv
		${CMAKE_CURRENT_BINARY_DIR}/generated/cmf-cie1931-2deg-1nm.h
	DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/cmf-to-header.awk
		${CMAKE_CURRENT_SOURCE_DIR}/ciexyz31_1.csv
	COMMENT "Generate CIE 1931 2° 1 nm CMF header"
	VERBATIM
)
add_custom_target(dn-cmf-header
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/cmf-cie1931-2deg-1nm.h)
add_dependencies(dn dn-cmf-header)

dawn_embed_shader(dn dn-overlay.vert dn_overlay_vert)
dawn_embed_shader(dn dn-overlay.frag dn_overlay_frag)
dawn_embed_shader(dn dn-dither.frag dn_dither_frag)
dawn_embed_shader(dn fullscreen.vert fullscreen_vert)

if (WIN32)
	target_sources(dn PRIVATE
		display-profile-windows.cpp
		assoc-windows.cpp
	)
	target_link_libraries(dn PRIVATE
		gdi32 shell32 shlwapi ole32 advapi32 user32)
	set_target_properties(dn PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
elseif (APPLE)
	enable_language(OBJCXX)
	find_library(DN_APPKIT_FRAMEWORK AppKit REQUIRED)
	find_library(DN_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
	find_library(DN_CORESERVICES_FRAMEWORK CoreServices REQUIRED)
	target_sources(dn PRIVATE
		display-profile-macos.mm
		assoc-macos.mm
		app-menu-macos.mm
		window-appearance-macos.mm
	)
	target_link_libraries(dn PRIVATE
		"${DN_APPKIT_FRAMEWORK}"
		"${DN_COREGRAPHICS_FRAMEWORK}"
		"${DN_CORESERVICES_FRAMEWORK}")
else()
	pkg_check_modules(DN_COLORD REQUIRED IMPORTED_TARGET colord)
	target_sources(dn PRIVATE
		display-profile-linux.cpp
		assoc-unix.cpp)
	target_link_libraries(dn PRIVATE PkgConfig::DN_COLORD)
endif()

if (NOT APPLE)
	target_sources(dn PRIVATE instance.cpp)
endif()

# Application icon: SVG → PNG/ICO (xT) on non-Apple; Swift squircle ICNS (xM)
# on Darwin. Qt looks up IDI_ICON1 and nothing else.
set(DN_ICON_SVG "${CMAKE_CURRENT_SOURCE_DIR}/dn.svg")
if (APPLE)
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

	# Mirrors supported_media_types(), restricted to macOS types.
	# See /System/Library/CoreServices/CoreTypes.bundle/Contents/Info.plist
	set(utis
		com.compuserve.gif
		com.microsoft.bmp
		com.truevision.tga-image
		org.webmproject.webp
		public.jpeg
		public.png
		public.svg-image
	)
	if (DAWN_WITH_LIBTIFF)
		list(APPEND utis public.tiff)
	endif()
	if (DAWN_WITH_LIBHEIF)
		list(APPEND utis public.avif public.heic public.heif)
	endif()
	if (DAWN_WITH_LIBJXL)
		list(APPEND utis public.jpeg-xl)
	endif()
	if (DAWN_WITH_OPENJPEG)
		list(APPEND utis public.jpeg-2000)
	endif()
	if (DAWN_WITH_LIBRAW)
		# The abstract type the vendor formats conform to.
		list(APPEND utis public.camera-raw-image)
	endif()
	if (DAWN_WITH_GDKPIXBUF)
		list(APPEND utis
			com.apple.icns
			com.apple.quicktime-image
			com.microsoft.ico
			public.xbitmap-image
		)
	endif()
	list(SORT utis)

	set(DN_MACOS_CONTENT_TYPES "")
	foreach (uti IN LISTS utis)
		string(APPEND DN_MACOS_CONTENT_TYPES
			"\n\t\t\t\t<string>${uti}</string>")
	endforeach()

	# Still a template afterwards: CMake fills ${MACOSX_BUNDLE_*} at generate
	# time, and Qt copies it once more in between.
	set(plist "${CMAKE_CURRENT_BINARY_DIR}/Info.plist.in")
	configure_file(Info.plist.in "${plist}" @ONLY)

	# XXX: I actually want to name the binary dn but the bundle Dawn.
	set_target_properties(dn PROPERTIES
		RUNTIME_OUTPUT_NAME "${PROJECT_NAME}"
		MACOSX_BUNDLE ON
		MACOSX_BUNDLE_GUI_IDENTIFIER name.janouch.dn
		MACOSX_BUNDLE_ICON_FILE "${MACOSX_BUNDLE_ICON_FILE}"
		MACOSX_BUNDLE_INFO_PLIST "${plist}"
		MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
		MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
		MACOSX_BUNDLE_COPYRIGHT "The Dawn Authors"
	)
else()
	include(IconUtils)
	set(DN_ICON_BASE "${PROJECT_BINARY_DIR}/icons")
	set(DN_ICON_PNG_LIST)
	foreach (DN_ICON_SIZE 16 32 48 256)
		icon_to_png(dn "${DN_ICON_SVG}" ${DN_ICON_SIZE}
			"${DN_ICON_BASE}" DN_ICON_PNG)
		list(APPEND DN_ICON_PNG_LIST "${DN_ICON_PNG}")
	endforeach()

	if (WIN32)
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

		# dn.desktop's MimeType= cannot be filled in at build time: it depends
		# on the gdk-pixbuf loaders installed on the target.  The parent list
		# runs this after dn itself is installed.
		set(DN_DESKTOP_UPDATER
			"${CMAKE_CURRENT_BINARY_DIR}/dn-update-desktop-files")
		configure_file(dn-update-desktop-files.in
			"${DN_DESKTOP_UPDATER}" @ONLY)
		install(PROGRAMS "${DN_DESKTOP_UPDATER}"
			DESTINATION "${CMAKE_INSTALL_BINDIR}")
		set(DN_DESKTOP_UPDATER "${DN_DESKTOP_UPDATER}" PARENT_SCOPE)
	endif()
endif()

if (DN_WITH_WAYLAND)
	pkg_get_variable(DN_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
	if (NOT DN_WAYLAND_PROTOCOLS_DIR)
		message(FATAL_ERROR "wayland-protocols pkgdatadir not found")
	endif()

	pkg_get_variable(DN_WAYLAND_SCANNER wayland-scanner wayland_scanner)
	if (NOT DN_WAYLAND_SCANNER)
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
	set(DN_XDG_XML
		"${DN_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
	set(DN_XDG_HEADER "${DN_PROTOCOL_DIR}/xdg-shell-client-protocol.h")
	add_custom_command(
		OUTPUT "${DN_COLOR_HEADER}" "${DN_COLOR_SOURCE}" "${DN_XDG_HEADER}"
		COMMAND "${DN_WAYLAND_SCANNER}" client-header
			"${DN_COLOR_XML}" "${DN_COLOR_HEADER}"
		COMMAND "${DN_WAYLAND_SCANNER}" private-code
			"${DN_COLOR_XML}" "${DN_COLOR_SOURCE}"
		COMMAND "${DN_WAYLAND_SCANNER}" client-header
			"${DN_XDG_XML}" "${DN_XDG_HEADER}"
		DEPENDS "${DN_COLOR_XML}" "${DN_XDG_XML}"
		COMMENT "wayland-scanner protocols for dn"
		VERBATIM)

	target_sources(dn PRIVATE
		wayland-color-bridge.cpp
		wayland-window.cpp
		"${DN_COLOR_HEADER}"
		"${DN_COLOR_SOURCE}"
		"${DN_XDG_HEADER}"
	)
	target_include_directories(dn PRIVATE "${DN_PROTOCOL_DIR}")
	target_link_libraries(dn PRIVATE PkgConfig::DN_WAYLAND Qt6::GuiPrivate)
endif()

# User guide: build tree at share/docs/dawnn (../share from bin/),
# installed next to the application on each platform.
set(DAWN_HELP_DIR "${PROJECT_BINARY_DIR}/share/doc/dawn")
file(MAKE_DIRECTORY "${DAWN_HELP_DIR}")
configure_file(
	"${PROJECT_SOURCE_DIR}/docs/dn.html.in"
	"${DAWN_HELP_DIR}/dn.html"
	@ONLY)
configure_file(
	"${PROJECT_SOURCE_DIR}/docs/stylesheet.css"
	"${DAWN_HELP_DIR}/stylesheet.css"
	COPYONLY)

if (APPLE)
	set_source_files_properties(
		"${DAWN_HELP_DIR}/dn.html"
		"${DAWN_HELP_DIR}/stylesheet.css"
		PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/share/doc/dawn")
	target_sources(dn PRIVATE
		"${DAWN_HELP_DIR}/dn.html"
		"${DAWN_HELP_DIR}/stylesheet.css"
	)
elseif (WIN32)
	install(FILES
		"${DAWN_HELP_DIR}/dn.html"
		"${DAWN_HELP_DIR}/stylesheet.css"
		DESTINATION share/doc/dawn)
else()
	include(GNUInstallDirs)
	install(FILES
		"${DAWN_HELP_DIR}/dn.html"
		"${DAWN_HELP_DIR}/stylesheet.css"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dawn")

	# This manual page generator should be good enough.
	set(ASCIIMAN "${PROJECT_SOURCE_DIR}/submodules/liberty/tools/asciiman.awk")
	foreach (page dn)
		set(page_output "${CMAKE_CURRENT_BINARY_DIR}/${page}.1")
		list(APPEND project_MAN_PAGES "${page_output}")
		add_custom_command(OUTPUT ${page_output}
			COMMAND env LC_ALL=C asciidoc-release-version=${PROJECT_VERSION}
				${AWK} -f ${ASCIIMAN} "${PROJECT_SOURCE_DIR}/docs/${page}.adoc"
				> ${page_output}
			DEPENDS "${PROJECT_SOURCE_DIR}/docs/${page}.adoc" ${ASCIIMAN}
			COMMENT "Generating man page for ${page}" VERBATIM)
	endforeach()

	add_custom_target(docs ALL DEPENDS ${project_MAN_PAGES})

	foreach (page ${project_MAN_PAGES})
		string(REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
		install(FILES "${page}"
			DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
	endforeach()
endif()

if (BUILD_TESTING)
	add_test(NAME dn-help COMMAND dn --help)
	set_tests_properties(dn-help PROPERTIES
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
