cmake_minimum_required(VERSION 3.28)
project(dawn
	VERSION 0.1.0
	DESCRIPTION "Colour-managed image browser"
	LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A plain build-root path would collide with CMake-produced subdirs.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options(-Wall -Wextra -Wpedantic
		-Wno-unused-parameter -Wno-missing-field-initializers)
endif()

if(WIN32 AND CMAKE_CROSSCOMPILING)
	set(DN_MSYS2_PREFIX "${PROJECT_SOURCE_DIR}/ucrt64")
	list(APPEND CMAKE_PREFIX_PATH "${DN_MSYS2_PREFIX}")
	list(APPEND CMAKE_FIND_ROOT_PATH "${DN_MSYS2_PREFIX}")

	# Relativize prefixes, and bar pkg-config from looking up host libraries
	set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PROJECT_SOURCE_DIR}")
	set(DN_MSYS2_PCPATH
		"${DN_MSYS2_PREFIX}/share/pkgconfig:${DN_MSYS2_PREFIX}/lib/pkgconfig")
	set(ENV{PKG_CONFIG_PATH} "${DN_MSYS2_PCPATH}")
	set(ENV{PKG_CONFIG_LIBDIR} "${DN_MSYS2_PCPATH}")

	# Qt tries to find qtpaths without an .exe file extension
	# (see Qt6CoreMacros.cmake).  This ultimately means broken windeployqt.
	set(QT_NO_QTPATHS_DEPLOYMENT_WARNING ON)

	# You may need to set WINEPATH because the MSYS2 rcc.exe may be executed.
	# Potentially set AUTORCC_EXECUTABLE manually to something usable.
endif()

if(APPLE)
	# Build tree: bin/dn.app/Contents/MacOS vs bin/libdn.dylib.
	# Install: MacOS/dn, Frameworks/libdn.dylib (macdeployqt leaves
	# @rpath/libdn.dylib; Homebrew dylibs become @executable_path).
	set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/../Frameworks")
else()
	set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()

# Dependency fetching is centralised here in the root CMakeLists.txt.
set(CMAKE_MODULE_PATH
	"${PROJECT_SOURCE_DIR}/cmake"
	"${PROJECT_SOURCE_DIR}/submodules/liberty/cmake"
)

find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Qt6 6.11 REQUIRED COMPONENTS Gui OPTIONAL_COMPONENTS WaylandClient)
find_program(AWK NAMES awk REQUIRED)
include(lxdr-generate)
find_program(GLSLANG_VALIDATOR
	NAMES glslangValidator glslang
	HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
	REQUIRED
)
set(DN_WITH_WAYLAND "${Qt6WaylandClient_FOUND}")
if (DN_WITH_WAYLAND)
	pkg_check_modules(DN_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
endif()

pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(WEBPDEMUX REQUIRED IMPORTED_TARGET libwebpdemux)
pkg_check_modules(WEBPMUX REQUIRED IMPORTED_TARGET libwebpmux)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
# Qt6 SVG cannot render our icons correctly, so we hard depend on resvg.
pkg_check_modules(RESVG REQUIRED IMPORTED_TARGET resvg)

macro(dn_optional_pkg stem doc module)
	pkg_check_modules(${stem} IMPORTED_TARGET ${module})
	option(DN_WITH_${stem} "${doc}" ${${stem}_FOUND})
	if(DN_WITH_${stem} AND NOT ${stem}_FOUND)
		message(FATAL_ERROR "DN_WITH_${stem}=ON but ${module} was not found")
	endif()
endmacro()

dn_optional_pkg(LIBRSVG "SVG via librsvg" librsvg-2.0)
dn_optional_pkg(XCURSOR "Xcursor support" xcursor)
dn_optional_pkg(LIBHEIF "HEIF/AVIF support" libheif)
dn_optional_pkg(LIBTIFF "TIFF support" libtiff-4)
dn_optional_pkg(GDKPIXBUF "gdk-pixbuf fallback" gdk-pixbuf-2.0)
dn_optional_pkg(LIBRAW "LibRaw support" libraw)

include(FetchContent)

# The GPL fast float plugin improves performance a ton,
# but distributions usually omit it.
option(DN_BUNDLED_LCMS2 "Use a fast bundled lcms2 instead of the system one" ON)
set(DN_WITH_LCMS2_FAST_FLOAT FALSE)
set(DN_WITH_LCMS2_THREADED FALSE)

add_library(dn_lcms2 INTERFACE)
set(DN_LCMS2_TARGET dn_lcms2)
if(NOT DN_BUNDLED_LCMS2)
	pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)
	target_link_libraries(dn_lcms2 INTERFACE PkgConfig::LCMS2)

	# Either lcms2 has been built to include the plugin, or it hasn't.
	find_path(LCMS2_FAST_FLOAT_INCLUDE_DIR lcms2_fast_float.h
		HINTS ${LCMS2_INCLUDE_DIRS}
		NO_DEFAULT_PATH)
	find_library(LCMS2_FAST_FLOAT_LIB NAMES lcms2_fast_float
		HINTS ${LCMS2_LIBRARY_DIRS}
		NO_DEFAULT_PATH)
	
	include(FindPackageHandleStandardArgs)
	find_package_handle_standard_args(Lcms2FastFloat
		REQUIRED_VARS LCMS2_FAST_FLOAT_LIB LCMS2_FAST_FLOAT_INCLUDE_DIR)
	if(Lcms2FastFloat_FOUND)
		set(DN_WITH_LCMS2_FAST_FLOAT TRUE)
		target_link_libraries(dn_lcms2 INTERFACE
			"${LCMS2_FAST_FLOAT_LIB}")
		target_include_directories(dn_lcms2 INTERFACE
			"${LCMS2_FAST_FLOAT_INCLUDE_DIR}")
	endif()
else()
	set(LCMS2_BUILD_SHARED OFF CACHE BOOL "" FORCE)
	set(LCMS2_BUILD_STATIC ON CACHE BOOL "" FORCE)
	set(LCMS2_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(LCMS2_WITH_FASTFLOAT ON CACHE BOOL "" FORCE)
	set(LCMS2_WITH_THREADED_PLUGIN ON CACHE BOOL "" FORCE)

	# Bundled static lib; do not ship headers/.a with the app package.
	FetchContent_Declare(lcms2
		URL https://github.com/mm2/Little-CMS/archive/refs/tags/lcms2.19.1.tar.gz
		URL_HASH SHA256=267705e278e2f7c2fb886c259dadcbaeb2be52748bcbc71c79f08aacacb7a709
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		EXCLUDE_FROM_ALL
	)
	FetchContent_MakeAvailable(lcms2)

	set(DN_WITH_LCMS2_FAST_FLOAT TRUE)
	set(DN_WITH_LCMS2_THREADED TRUE)
	set_target_properties(lcms2 lcms2_fast_float lcms2_threaded PROPERTIES
		POSITION_INDEPENDENT_CODE ON)
	# Upstream keeps plugin includes PRIVATE.
	target_include_directories(lcms2_fast_float PUBLIC
		$<BUILD_INTERFACE:${lcms2_SOURCE_DIR}/plugins/fast_float/include>)
	target_include_directories(lcms2_threaded PUBLIC
		$<BUILD_INTERFACE:${lcms2_SOURCE_DIR}/plugins/threaded/include>)
	target_link_libraries(dn_lcms2 INTERFACE
		lcms2 lcms2_fast_float lcms2_threaded)
endif()

option(DN_WITH_JPEG_QS "Bundled JPEG Quant Smooth" ON)
if(DN_WITH_JPEG_QS)
	FetchContent_Declare(jpegqs
		URL https://github.com/ilyakurdyukov/jpeg-quantsmooth/archive/refs/tags/1.20260122.tar.gz
		URL_HASH SHA256=7dcbaa7d994511a03dba845aa218a25f9c8e70c0bcb0800ff61ae3d92dd50c3d
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		PATCH_COMMAND
			${CMAKE_COMMAND} -E copy
				"${PROJECT_SOURCE_DIR}/cmake/jpegqs/CMakeLists.txt"
				<SOURCE_DIR>/CMakeLists.txt
		COMMAND
			${CMAKE_COMMAND} -E copy_directory
				"${PROJECT_SOURCE_DIR}/cmake/jpegqs/include"
				<SOURCE_DIR>/include
	)
	FetchContent_MakeAvailable(jpegqs)
	if(NOT TARGET jpegqs)
		message(FATAL_ERROR "Bundled jpegqs did not create target 'jpegqs'")
	endif()
endif()

# TODO(p): This will want to be single instance everywhere.
if(UNIX AND NOT APPLE)
	set(DN_WITH_SINGLE_INSTANCE TRUE)
else()
	set(DN_WITH_SINGLE_INSTANCE FALSE)
endif()

file(SHA256 "${PROJECT_SOURCE_DIR}/ipc/common.lxdr" DN_IPC_LXDR_H1)
file(SHA256 "${PROJECT_SOURCE_DIR}/ipc/instance.lxdr" DN_IPC_LXDR_H2)
string(SHA256 DN_IPC_LXDR_COMBINED "${DN_IPC_LXDR_H1}${DN_IPC_LXDR_H2}")
string(SUBSTRING "${DN_IPC_LXDR_COMBINED}" 0 12 DN_IPC_LXDR_PREFIX)
set(DN_IPC_BUILD_ID "dawn ${PROJECT_VERSION} ${DN_IPC_LXDR_PREFIX}")

configure_file(
	"${PROJECT_SOURCE_DIR}/dawn-config.h.in"
	"${PROJECT_BINARY_DIR}/dawn-config.h"
)

add_subdirectory(libdn)
add_subdirectory(dnthumbd)
add_subdirectory(dn)

if(WIN32)
	install(TARGETS dn libdn dnthumbd
		RUNTIME DESTINATION .)
	install(DIRECTORY "${DN_MSYS2_PREFIX}/bin/"
		DESTINATION .
		FILES_MATCHING
			PATTERN "*.dll"
			PATTERN "vulkan-1.dll" EXCLUDE)

	# This installation is pretty big, with a ton of files, thus it's optional.
	install(FILES
		"${DN_MSYS2_PREFIX}/bin/wperl.exe"
		"${DN_MSYS2_PREFIX}/bin/exiftool"
		DESTINATION .
		OPTIONAL)
	install(DIRECTORY
		"${DN_MSYS2_PREFIX}/lib/perl5/"
		DESTINATION lib/perl5
		OPTIONAL)

	# TODO(p): Does this need to be branched?
	if(TARGET Qt6::QWindowsIntegrationPlugin)
		install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QWindowsIntegrationPlugin
			RUNTIME DESTINATION platforms
			LIBRARY DESTINATION platforms)
	else()
		install(FILES
			"${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/platforms/qwindows.dll"
			DESTINATION platforms)
	endif()

	install(FILES
		"${DN_MSYS2_PREFIX}/bin/vk_swiftshader_icd.json"
		DESTINATION .)
	install(FILES
		"${DN_MSYS2_PREFIX}/share/mime/globs2"
		"${DN_MSYS2_PREFIX}/share/mime/subclasses"
		DESTINATION share/mime)

	# Prune unused libraries.
	install(SCRIPT "${PROJECT_SOURCE_DIR}/cmake/msys2-cleanup.cmake")
endif()
if(APPLE)
	include(cmake/macos-install.cmake)
endif()

include(CTest)
if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

set(CPACK_PACKAGE_VENDOR "dawn")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)
