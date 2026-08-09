add_executable(gen_fixtures gen-fixtures.cpp)
target_link_libraries(gen_fixtures PRIVATE
	PkgConfig::ZLIB
	${DN_LCMS2_TARGET}
)


set(DN_TEST_FIXTURES_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(DN_TEST_FIXTURE_FILES
	"${DN_TEST_FIXTURES_DIR}/black.png"
	"${DN_TEST_FIXTURES_DIR}/cmyk-lab.icc"
	"${DN_TEST_FIXTURES_DIR}/blue.bmp"
	"${DN_TEST_FIXTURES_DIR}/blue.jpg"
	"${DN_TEST_FIXTURES_DIR}/blue.png"
	"${DN_TEST_FIXTURES_DIR}/blue.svg"
	"${DN_TEST_FIXTURES_DIR}/blue.tga"
	"${DN_TEST_FIXTURES_DIR}/blue.tif"
	"${DN_TEST_FIXTURES_DIR}/blue.webp"
	"${DN_TEST_FIXTURES_DIR}/blue16.png"
	"${DN_TEST_FIXTURES_DIR}/display-p3-red_vs_srgb-red.png"
	"${DN_TEST_FIXTURES_DIR}/green.bmp"
	"${DN_TEST_FIXTURES_DIR}/green.jpg"
	"${DN_TEST_FIXTURES_DIR}/green.png"
	"${DN_TEST_FIXTURES_DIR}/green.svg"
	"${DN_TEST_FIXTURES_DIR}/green.tga"
	"${DN_TEST_FIXTURES_DIR}/green.tif"
	"${DN_TEST_FIXTURES_DIR}/green.webp"
	"${DN_TEST_FIXTURES_DIR}/green16.png"
	"${DN_TEST_FIXTURES_DIR}/red.bmp"
	"${DN_TEST_FIXTURES_DIR}/red.jpg"
	"${DN_TEST_FIXTURES_DIR}/red.png"
	"${DN_TEST_FIXTURES_DIR}/red.svg"
	"${DN_TEST_FIXTURES_DIR}/red.tga"
	"${DN_TEST_FIXTURES_DIR}/red.tif"
	"${DN_TEST_FIXTURES_DIR}/red.webp"
	"${DN_TEST_FIXTURES_DIR}/red16.png"
	"${DN_TEST_FIXTURES_DIR}/red_a128.png"
	"${DN_TEST_FIXTURES_DIR}/text-after-idat.png"
	"${DN_TEST_FIXTURES_DIR}/red_a128.svg"
	"${DN_TEST_FIXTURES_DIR}/rgbw_2x2.png"
	"${DN_TEST_FIXTURES_DIR}/rgbw_2x2.svg"
	"${DN_TEST_FIXTURES_DIR}/white.png"
)
add_custom_command(
	OUTPUT ${DN_TEST_FIXTURE_FILES}
	COMMAND "$<TARGET_FILE:gen_fixtures>" "${DN_TEST_FIXTURES_DIR}"
	DEPENDS gen_fixtures
	COMMENT "Generating test fixtures in the build tree"
	VERBATIM
)
add_custom_target(test_fixtures DEPENDS ${DN_TEST_FIXTURE_FILES})

add_executable(test_channels test-channels.cpp)
if(NOT CMAKE_CROSSCOMPILING)
	add_dependencies(test_channels test_fixtures)
endif()
target_link_libraries(test_channels PRIVATE dawn::libdn)
target_compile_definitions(test_channels PRIVATE
	DN_TEST_FIXTURES_DIR="${DN_TEST_FIXTURES_DIR}"
)

add_test(NAME channels COMMAND test_channels)
set_tests_properties(channels PROPERTIES
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(bench_open bench-open.cpp)
target_link_libraries(bench_open PRIVATE dawn::libdn)

add_executable(test_xdg test-xdg.cpp "${CMAKE_SOURCE_DIR}/dn/xdg.cpp")
target_include_directories(test_xdg PRIVATE
	"${CMAKE_SOURCE_DIR}/dn"
	"${PROJECT_BINARY_DIR}"
)
target_link_libraries(test_xdg PRIVATE Qt6::Gui)
add_test(NAME xdg COMMAND test_xdg)

add_executable(test_thumbnail_cache
	test-thumbnail-cache.cpp
	"${CMAKE_SOURCE_DIR}/dn/thumbnail-cache.cpp")
target_include_directories(test_thumbnail_cache PRIVATE
	"${CMAKE_SOURCE_DIR}/dn")
target_link_libraries(test_thumbnail_cache PRIVATE
	dawn::libdn
	Qt6::Gui
	PkgConfig::WEBP
	PkgConfig::WEBPDEMUX
	PkgConfig::WEBPMUX)
add_test(NAME thumbnail_cache COMMAND test_thumbnail_cache)

add_executable(test_thumbnailer
	test-thumbnailer.cpp
	"${CMAKE_SOURCE_DIR}/dn/thumbnailer.cpp")
target_include_directories(test_thumbnailer PRIVATE
	"${CMAKE_SOURCE_DIR}/dn"
	"${CMAKE_SOURCE_DIR}/libdn")
target_link_libraries(test_thumbnailer PRIVATE
	dawn::libdn
	Qt6::Gui
	Threads::Threads)
add_test(NAME thumbnailer COMMAND test_thumbnailer)

add_executable(test_lxdr test-lxdr.cpp)
target_link_libraries(test_lxdr PRIVATE dawn::libdn)
add_test(NAME lxdr COMMAND test_lxdr)

if(NOT WIN32 AND NOT APPLE)
	add_executable(test_ipc test-ipc.cpp)
	target_link_libraries(test_ipc PRIVATE dawn::libdn)
	add_test(NAME ipc COMMAND test_ipc)
endif()
