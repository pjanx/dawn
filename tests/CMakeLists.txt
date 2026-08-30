add_executable(gen-fixtures gen-fixtures.cpp)
target_link_libraries(gen-fixtures PRIVATE
	PkgConfig::ZLIB
	${DAWN_LCMS2_TARGET}
)

pkg_check_modules(LIBPNG IMPORTED_TARGET libpng)
if (LIBPNG_FOUND)
	add_executable(gen-sample gen-sample.c)
	target_link_libraries(gen-sample PRIVATE
		PkgConfig::LIBPNG
		${DAWN_LCMS2_TARGET}
	)
endif()

set(DAWN_TEST_FIXTURES_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(DAWN_TEST_FIXTURE_FILES
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
if(NOT CMAKE_CROSSCOMPILING)
	add_dependencies(test-channels test-fixtures)
endif()
target_link_libraries(test-channels PRIVATE dawn::libdn)
target_compile_definitions(test-channels PRIVATE
	DAWN_TEST_FIXTURES_DIR="${DAWN_TEST_FIXTURES_DIR}"
)

add_test(NAME channels COMMAND test-channels)
set_tests_properties(channels PROPERTIES
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(bench-open bench-open.cpp)
target_link_libraries(bench-open PRIVATE dawn::libdn)

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
	dawn::libdn
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
	dawn::libdn
	Qt6::Gui
	Threads::Threads
	PkgConfig::WEBP
	PkgConfig::WEBPDEMUX
	PkgConfig::WEBPMUX)
add_test(NAME thumbnailer COMMAND test-thumbnailer)

add_executable(test-lxdr test-lxdr.cpp)
target_link_libraries(test-lxdr PRIVATE dawn::libdn)
add_test(NAME lxdr COMMAND test-lxdr)

if(NOT APPLE)
	add_executable(test-ipc test-ipc.cpp)
	target_link_libraries(test-ipc PRIVATE dawn::libdn)
	add_test(NAME ipc COMMAND test-ipc)
endif()

# dn.desktop is rewritten by dn-update-desktop-files on install.
find_program(DAWN_DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DAWN_DESKTOP_FILE_VALIDATE)
	foreach(desktop dn.desktop dn-browse.desktop)
		add_test(NAME ${desktop} COMMAND "${DAWN_DESKTOP_FILE_VALIDATE}"
			"${PROJECT_SOURCE_DIR}/dn/${desktop}")
	endforeach()
endif()
