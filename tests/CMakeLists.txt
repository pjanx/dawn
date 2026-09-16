pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

add_executable(gen-fixtures gen-fixtures.cpp)
target_link_libraries(gen-fixtures PRIVATE
	PkgConfig::ZLIB
	dawn-lcms2
)

if (LIBPNG_FOUND)
	add_executable(gen-sample gen-sample.c)
	target_link_libraries(gen-sample PRIVATE
		PkgConfig::LIBPNG
		dawn-lcms2
	)
endif()

set(DAWN_TEST_FIXTURES_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(DAWN_TEST_FIXTURE_FILES
	"${DAWN_TEST_FIXTURES_DIR}/quads420.jpg"
	"${DAWN_TEST_FIXTURES_DIR}/quads420-progressive.jpg"
	"${DAWN_TEST_FIXTURES_DIR}/black.png"
	"${DAWN_TEST_FIXTURES_DIR}/cmyk-lab.icc"
	"${DAWN_TEST_FIXTURES_DIR}/blue.bmp"
	"${DAWN_TEST_FIXTURES_DIR}/blue.jpg"
	"${DAWN_TEST_FIXTURES_DIR}/blue.png"
	"${DAWN_TEST_FIXTURES_DIR}/blue.svg"
	"${DAWN_TEST_FIXTURES_DIR}/blue.tga"
	"${DAWN_TEST_FIXTURES_DIR}/blue.tif"
	"${DAWN_TEST_FIXTURES_DIR}/blue.webp"
	"${DAWN_TEST_FIXTURES_DIR}/blue16.png"
	"${DAWN_TEST_FIXTURES_DIR}/display-p3-red_vs_srgb-red.png"
	"${DAWN_TEST_FIXTURES_DIR}/green.bmp"
	"${DAWN_TEST_FIXTURES_DIR}/green.jpg"
	"${DAWN_TEST_FIXTURES_DIR}/green.png"
	"${DAWN_TEST_FIXTURES_DIR}/green.svg"
	"${DAWN_TEST_FIXTURES_DIR}/green.tga"
	"${DAWN_TEST_FIXTURES_DIR}/green.tif"
	"${DAWN_TEST_FIXTURES_DIR}/green.webp"
	"${DAWN_TEST_FIXTURES_DIR}/green16.png"
	"${DAWN_TEST_FIXTURES_DIR}/red.bmp"
	"${DAWN_TEST_FIXTURES_DIR}/red.jpg"
	"${DAWN_TEST_FIXTURES_DIR}/red.png"
	"${DAWN_TEST_FIXTURES_DIR}/red.svg"
	"${DAWN_TEST_FIXTURES_DIR}/red.tga"
	"${DAWN_TEST_FIXTURES_DIR}/red.tif"
	"${DAWN_TEST_FIXTURES_DIR}/red.webp"
	"${DAWN_TEST_FIXTURES_DIR}/red16.png"
	"${DAWN_TEST_FIXTURES_DIR}/red_a128.png"
	"${DAWN_TEST_FIXTURES_DIR}/text-after-idat.png"
	"${DAWN_TEST_FIXTURES_DIR}/red_a128.svg"
	"${DAWN_TEST_FIXTURES_DIR}/rgbw_2x2.png"
	"${DAWN_TEST_FIXTURES_DIR}/rgbw_2x2.svg"
	"${DAWN_TEST_FIXTURES_DIR}/white.png"
)
add_custom_command(
	OUTPUT ${DAWN_TEST_FIXTURE_FILES}
	COMMAND "$<TARGET_FILE:gen-fixtures>" "${DAWN_TEST_FIXTURES_DIR}"
	DEPENDS gen-fixtures
	COMMENT "Generating test fixtures in the build tree"
	VERBATIM
)
add_custom_target(test-fixtures DEPENDS ${DAWN_TEST_FIXTURE_FILES})

add_executable(test-channels test-channels.cpp)
if (NOT CMAKE_CROSSCOMPILING)
	add_dependencies(test-channels test-fixtures)
endif()
target_link_libraries(test-channels PRIVATE libdn)
target_compile_definitions(test-channels PRIVATE
	DAWN_TEST_FIXTURES_DIR="${DAWN_TEST_FIXTURES_DIR}"
)

add_test(NAME channels COMMAND test-channels)
set_tests_properties(channels PROPERTIES
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

if (DAWN_WITH_DNRS)
	add_executable(test-dnrs test-dnrs.cpp)
	target_link_libraries(test-dnrs PRIVATE libdn)
	add_test(NAME dnrs COMMAND test-dnrs)
endif()

if (APPLE AND DAWN_WITH_IMAGEIO)
	add_executable(test-imageio test-imageio.cpp)
	add_dependencies(test-imageio test-fixtures)
	target_link_libraries(test-imageio PRIVATE libdn)
	target_compile_definitions(test-imageio PRIVATE
		DAWN_TEST_FIXTURES_DIR="${DAWN_TEST_FIXTURES_DIR}"
	)
	add_test(NAME imageio COMMAND test-imageio)
endif()

add_executable(test-jpeg-crop test-jpeg-crop.cpp)
if (NOT CMAKE_CROSSCOMPILING)
	add_dependencies(test-jpeg-crop test-fixtures)
endif()
target_link_libraries(test-jpeg-crop PRIVATE libdn)
target_compile_definitions(test-jpeg-crop PRIVATE
	DAWN_TEST_FIXTURES_DIR="${DAWN_TEST_FIXTURES_DIR}")
add_test(NAME jpeg_crop COMMAND test-jpeg-crop)

add_executable(test-icns test-icns.cpp)
target_link_libraries(test-icns PRIVATE libdn)
add_test(NAME icns COMMAND test-icns)

add_executable(bench-open bench-open.cpp)
target_link_libraries(bench-open PRIVATE libdn)

add_executable(test-xdg test-xdg.cpp "${PROJECT_SOURCE_DIR}/dn/xdg.cpp")
target_include_directories(test-xdg PRIVATE
	"${PROJECT_SOURCE_DIR}/dn"
	"${PROJECT_BINARY_DIR}"
)
target_link_libraries(test-xdg PRIVATE Qt6::Gui)
add_test(NAME xdg COMMAND test-xdg)

add_executable(test-thumbnail-cache
	test-thumbnail-cache.cpp
	"${PROJECT_SOURCE_DIR}/dn/thumbnail-cache.cpp")
target_include_directories(test-thumbnail-cache PRIVATE
	"${PROJECT_SOURCE_DIR}/dn")
target_link_libraries(test-thumbnail-cache PRIVATE
	libdn
	Qt6::Gui
	PkgConfig::WEBP
	PkgConfig::WEBPDEMUX
	PkgConfig::WEBPMUX)
add_test(NAME thumbnail_cache COMMAND test-thumbnail-cache)

add_executable(test-thumbnailer
	test-thumbnailer.cpp
	"${PROJECT_SOURCE_DIR}/dn/thumbnailer.cpp"
	"${PROJECT_SOURCE_DIR}/dn/thumbnail-cache.cpp")
target_include_directories(test-thumbnailer PRIVATE
	"${PROJECT_SOURCE_DIR}/dn"
	"${PROJECT_SOURCE_DIR}/libdn")
target_link_libraries(test-thumbnailer PRIVATE
	libdn
	Qt6::Gui
	Threads::Threads
	PkgConfig::WEBP
	PkgConfig::WEBPDEMUX
	PkgConfig::WEBPMUX)
add_test(NAME thumbnailer COMMAND test-thumbnailer)

# Translation needs a catalogue to find, and gettext settles the language
# once per process, so each case gets a process of its own.
if (MSGFMT_EXECUTABLE)
	set(gettext_locale_dir "${CMAKE_CURRENT_BINARY_DIR}/locale")
	set(gettext_mo "${gettext_locale_dir}/xx/LC_MESSAGES/dawn.mo")
	add_custom_command(OUTPUT "${gettext_mo}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${gettext_locale_dir}/xx/LC_MESSAGES"
		COMMAND "${MSGFMT_EXECUTABLE}" --check -o "${gettext_mo}"
			"${CMAKE_CURRENT_SOURCE_DIR}/test-gettext.po"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test-gettext.po"
		COMMENT "Compiling the test catalogue"
		VERBATIM)

	add_executable(test-gettext test-gettext.cpp "${gettext_mo}")
	target_link_libraries(test-gettext PRIVATE libdn)
	target_compile_definitions(test-gettext PRIVATE
		DAWN_TEST_LOCALE_DIR="${gettext_locale_dir}")

	add_test(NAME gettext_fallback COMMAND test-gettext fallback)
	set_tests_properties(gettext_fallback PROPERTIES ENVIRONMENT "LANGUAGE=")
	foreach (case translated context plural library)
		add_test(NAME gettext_${case} COMMAND test-gettext ${case})
		set_tests_properties(gettext_${case} PROPERTIES ENVIRONMENT
			"LANGUAGE=xx")
	endforeach()

	# A system with no locale but "C" leaves gettext nothing to select with.
	set_tests_properties(gettext_fallback gettext_translated gettext_context
		gettext_plural gettext_library PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Accessibility is only ever proven from the outside, over the same bus and
# through the same client library a screen reader uses.  libatspi is a test
# dependency and nothing else: dn links none of it.
option(DAWN_WITH_ACCESSIBILITY_TESTS
	"Require what the external accessibility test needs" OFF)
if (DAWN_WITH_ACCESSIBILITY AND UNIX AND NOT APPLE)
	pkg_check_modules(ATSPI IMPORTED_TARGET atspi-2)
	# Every object libatspi hands out is a GObject, but atspi-2.pc requires
	# only glib, so referencing and unreferencing them needs this as well.
	pkg_check_modules(GOBJECT IMPORTED_TARGET gobject-2.0)

	# Overflow coverage resizes the window through Weston, because Qt's
	# AT-SPI Component.SetSize is a stub and a production Value interface on
	# the frame is not a resize API.  The test asserts that resize, so this
	# is not optional to it: without the plugin there is no test to run.
	# weston.pc carries the release, and libweston is versioned by its major.
	pkg_check_modules(WESTON IMPORTED_TARGET weston)
	if (WESTON_FOUND)
		string(REGEX MATCH "^[0-9]+" WESTON_ABI "${WESTON_VERSION}")
		pkg_check_modules(LIBWESTON IMPORTED_TARGET "libweston-${WESTON_ABI}")
	endif()
	pkg_check_modules(WAYLAND_SERVER IMPORTED_TARGET wayland-server)

	if (ATSPI_FOUND AND GOBJECT_FOUND AND WESTON_FOUND AND LIBWESTON_FOUND
			AND WAYLAND_SERVER_FOUND)
		add_executable(test-accessible test-accessible.cpp)
		target_include_directories(test-accessible PRIVATE
			"${PROJECT_BINARY_DIR}")
		target_link_libraries(test-accessible PRIVATE
			PkgConfig::ATSPI
			PkgConfig::GOBJECT)
		add_dependencies(test-accessible dn)
		if (NOT CMAKE_CROSSCOMPILING)
			add_dependencies(test-accessible test-fixtures)
		endif()

		add_library(weston-a11y-resize MODULE weston-a11y-resize.c)
		target_link_libraries(weston-a11y-resize PRIVATE
			PkgConfig::WESTON
			PkgConfig::LIBWESTON
			PkgConfig::WAYLAND_SERVER)
		set_target_properties(weston-a11y-resize PROPERTIES
			PREFIX ""
			C_STANDARD 11
			C_STANDARD_REQUIRED ON)
		add_dependencies(test-accessible weston-a11y-resize)

		add_test(NAME accessible
			COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-accessible.sh"
				"$<TARGET_FILE:test-accessible>"
				"$<TARGET_FILE:dn>"
				"${DAWN_TEST_FIXTURES_DIR}")
		# The launcher skips when the session it needs cannot be built; once
		# dn is up, anything going wrong at all is a failure.
		set_tests_properties(accessible PROPERTIES SKIP_RETURN_CODE 77)
		if (DAWN_WITH_ACCESSIBILITY_TESTS)
			set_tests_properties(accessible PROPERTIES
				ENVIRONMENT "DN_A11Y_STRICT=1")
		endif()
	elseif (DAWN_WITH_ACCESSIBILITY_TESTS)
		message(FATAL_ERROR "DAWN_WITH_ACCESSIBILITY_TESTS=ON but atspi-2, "
			"gobject-2.0, or the weston plugin headers were not found")
	endif()
elseif (DAWN_WITH_ACCESSIBILITY_TESTS)
	message(FATAL_ERROR "DAWN_WITH_ACCESSIBILITY_TESTS=ON without a Linux "
		"accessibility build")
endif()

add_executable(test-lxdr test-lxdr.cpp)
target_link_libraries(test-lxdr PRIVATE libdn)
add_test(NAME lxdr COMMAND test-lxdr)

add_executable(test-ipc test-ipc.cpp)
target_link_libraries(test-ipc PRIVATE libdn)
add_test(NAME ipc COMMAND test-ipc)

add_executable(test-shm test-shm.cpp)
target_link_libraries(test-shm PRIVATE libdn)
add_test(NAME shm COMMAND test-shm)

# The main desktop file has its MimeType= rewritten during the build.
find_program(DAWN_DESKTOP_FILE_VALIDATE desktop-file-validate)
if (DAWN_DESKTOP_FILE_VALIDATE)
	foreach(desktop
		name.janouch.Dawn.desktop
		name.janouch.Dawn.Browse.desktop
		name.janouch.Dawn.Commander.desktop
		name.janouch.Dawn.CropJpeg.desktop
	)
		add_test(NAME ${desktop} COMMAND "${DAWN_DESKTOP_FILE_VALIDATE}"
			"${PROJECT_SOURCE_DIR}/packaging/${desktop}")
	endforeach()
endif()

# The screenshot lives in the repository, and validation must not fetch it.
find_program(DAWN_APPSTREAMCLI appstreamcli)
if (DAWN_APPSTREAMCLI)
	add_test(NAME metainfo COMMAND "${DAWN_APPSTREAMCLI}"
		validate --no-net --explain "${dawn_metainfo}")
endif()
