# One big CMakeLists.txt because otherwise clutter spreads too easily.
cmake_minimum_required(VERSION 3.28)
project(Dawn
	VERSION 0.1.0
	DESCRIPTION "Colour-managed image browser"
	HOMEPAGE_URL "https://git.janouch.name/p/dawn"
	LANGUAGES C CXX
)
if (APPLE)
	enable_language(OBJCXX)
endif()

set(DAWN_NAMESPACE "name.janouch.Dawn")
set(CPACK_PACKAGE_VENDOR "The Dawn Authors")
set(CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A plain build-root path would collide with CMake-produced subdirs.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

if (WIN32 AND CMAKE_CROSSCOMPILING)
	# XXX: Building from within MSYS2 might need to have DAWN_MSYS2_PREFIX
	# preset in the cache.
	set(DAWN_MSYS2_PREFIX "${PROJECT_BINARY_DIR}/ucrt64")
	list(APPEND CMAKE_PREFIX_PATH "${DAWN_MSYS2_PREFIX}")
	list(APPEND CMAKE_FIND_ROOT_PATH "${DAWN_MSYS2_PREFIX}")

	# Relativize prefixes, and bar pkg-config from looking up host libraries
	set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PROJECT_BINARY_DIR}")
	set(DAWN_MSYS2_PCPATH
		"${DAWN_MSYS2_PREFIX}/share/pkgconfig:${DAWN_MSYS2_PREFIX}/lib/pkgconfig")
	set(ENV{PKG_CONFIG_PATH} "${DAWN_MSYS2_PCPATH}")
	set(ENV{PKG_CONFIG_LIBDIR} "${DAWN_MSYS2_PCPATH}")

	# Qt tries to find qtpaths without an .exe file extension
	# (see Qt6CoreMacros.cmake).  This ultimately means broken windeployqt.
	set(QT_NO_QTPATHS_DEPLOYMENT_WARNING ON)
endif()

if (APPLE)
	# Homebrew targets the host system.  With LSMinimumSystemVersion,
	# Launch Services explains the refusal; dyld would not.
	if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
		execute_process(COMMAND sw_vers -productVersion
			OUTPUT_VARIABLE dawn_macos_version
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		if (NOT dawn_macos_version MATCHES "^([0-9]+)")
			message(FATAL_ERROR "Cannot tell what macOS version this is")
		endif()
		set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_MATCH_1}.0")
	endif()

	# Build tree: bin/dn.app/Contents/MacOS vs bin/libdn.dylib.
	# Install: MacOS/dn, Frameworks/libdn.dylib (macdeployqt leaves
	# @rpath/libdn.dylib; Homebrew dylibs become @executable_path).
	set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/../Frameworks")
else()
	set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()

set(CMAKE_MODULE_PATH
	"${PROJECT_SOURCE_DIR}/cmake"
	"${PROJECT_SOURCE_DIR}/submodules/liberty/cmake"
)

# --- Dependencies -------------------------------------------------------------

find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Intl REQUIRED)
# Maybe we need a newer version, just not to build.
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui OPTIONAL_COMPONENTS WaylandClient)
find_program(AWK NAMES awk REQUIRED)
include(lxdr-generate)
include(embed-shader)

set(DN_WITH_WAYLAND "${Qt6WaylandClient_FOUND}")
if (DN_WITH_WAYLAND)
	pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
	# For QPlatformNativeInterface: Qt exposes no other way to the toplevel.
	# Debian's qt6-base-private-dev adds Qt6::GuiPrivate while loading Qt6Gui,
	# but deliberately ships no standalone Qt6GuiPrivateConfig.cmake.
	if (NOT TARGET Qt6::GuiPrivate)
		find_package(Qt6 6.8 REQUIRED COMPONENTS GuiPrivate)
	endif()
endif()

pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(WEBPDEMUX REQUIRED IMPORTED_TARGET libwebpdemux)
pkg_check_modules(WEBPMUX REQUIRED IMPORTED_TARGET libwebpmux)
pkg_check_modules(LIBPNG IMPORTED_TARGET libpng)
# Qt6 SVG cannot render our icons correctly, so we hard depend on resvg.
pkg_check_modules(RESVG REQUIRED IMPORTED_TARGET resvg)

macro(dawn_optional_pkg stem doc module)
	pkg_check_modules(${stem} IMPORTED_TARGET ${module})
	option(DAWN_WITH_${stem} "${doc}" ${${stem}_FOUND})
	if(DAWN_WITH_${stem} AND NOT ${stem}_FOUND)
		message(FATAL_ERROR "DAWN_WITH_${stem}=ON but ${module} was not found")
	endif()
endmacro()

dawn_optional_pkg(LIBHEIF "HEIF/AVIF support" libheif)
dawn_optional_pkg(LIBJXL "JPEG XL support" libjxl)
dawn_optional_pkg(LIBRAW "LibRaw support" libraw)
dawn_optional_pkg(LIBRSVG "SVG via librsvg" librsvg-2.0)
dawn_optional_pkg(LIBTIFF "TIFF support" libtiff-4)
dawn_optional_pkg(LIBWMF "WMF support" libwmf)
dawn_optional_pkg(OPENJPEG "JPEG 2000 support" libopenjp2)
dawn_optional_pkg(POPPLER "PDF via Poppler" poppler-cpp)
dawn_optional_pkg(XCURSOR "Xcursor support" xcursor)

find_program(CARGO_EXECUTABLE cargo)
if (CARGO_EXECUTABLE)
	set(CARGO_FOUND TRUE)
endif()
option(DAWN_WITH_DNRS "Rust image loader" ${CARGO_FOUND})
if (DAWN_WITH_DNRS AND NOT CARGO_FOUND)
	message(FATAL_ERROR "DAWN_WITH_DNRS=ON but cargo was not found")
endif()

# jxrlib ships no pkg-config file, and its headers include one another
# by bare name, so the whole package directory has to go on the include path.
find_path(JXRLIB_INCLUDE_DIR JXRGlue.h PATH_SUFFIXES jxrlib)
find_library(JXRLIB_GLUE_LIBRARY jxrglue)
find_library(JXRLIB_CORE_LIBRARY jpegxr)
if (JXRLIB_INCLUDE_DIR AND JXRLIB_GLUE_LIBRARY AND JXRLIB_CORE_LIBRARY)
	set(JXRLIB_FOUND TRUE)
endif()
option(DAWN_WITH_JXRLIB "JPEG XR support" ${JXRLIB_FOUND})
if (DAWN_WITH_JXRLIB AND NOT JXRLIB_FOUND)
	message(FATAL_ERROR "DAWN_WITH_JXRLIB=ON but jxrlib was not found")
endif()

if (APPLE)
	# These don't make much sense to disable, but it's more regular with them.
	option(DAWN_WITH_IMAGEIO "ImageIO loader" ON)
	option(DAWN_WITH_CGPDF "Core Graphics PDF page rendering" ON)
endif()

include(FetchContent)

# The GPL fast float plugin improves performance a ton,
# but distributions usually omit it.
option(DAWN_BUNDLED_LCMS2
	"Use a fast bundled lcms2 instead of the system one" ON)
set(DAWN_WITH_LCMS2_FAST_FLOAT FALSE)
set(DAWN_WITH_LCMS2_THREADED FALSE)

add_library(dawn-lcms2 INTERFACE)
if (NOT DAWN_BUNDLED_LCMS2)
	pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)
	target_link_libraries(dawn-lcms2 INTERFACE PkgConfig::LCMS2)

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
	if (Lcms2FastFloat_FOUND)
		set(DAWN_WITH_LCMS2_FAST_FLOAT TRUE)
		target_link_libraries(dawn-lcms2 INTERFACE
			"${LCMS2_FAST_FLOAT_LIB}")
		target_include_directories(dawn-lcms2 INTERFACE
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

	set(DAWN_WITH_LCMS2_FAST_FLOAT TRUE)
	set(DAWN_WITH_LCMS2_THREADED TRUE)
	set_target_properties(lcms2 lcms2_fast_float lcms2_threaded PROPERTIES
		POSITION_INDEPENDENT_CODE ON)
	# Upstream keeps plugin includes PRIVATE.
	target_include_directories(lcms2_fast_float PUBLIC
		$<BUILD_INTERFACE:${lcms2_SOURCE_DIR}/plugins/fast_float/include>)
	target_include_directories(lcms2_threaded PUBLIC
		$<BUILD_INTERFACE:${lcms2_SOURCE_DIR}/plugins/threaded/include>)

	# FIXME: LibRaw may transitively link to system lcms2.
	# What gives?  Clear in LINK_INTERFACE_LIBRARIES?
	target_link_libraries(dawn-lcms2 INTERFACE
		lcms2 lcms2_fast_float lcms2_threaded)

	# Have CTest not fail.
	set_target_properties(testcms PROPERTIES EXCLUDE_FROM_ALL FALSE)
endif()

option(DAWN_WITH_JPEG_QS "Bundled JPEG Quant Smooth" ON)
if (DAWN_WITH_JPEG_QS)
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
	if (NOT TARGET jpegqs)
		message(FATAL_ERROR "Bundled jpegqs did not create target 'jpegqs'")
	endif()
endif()

option(DAWN_WITH_WIP "Expose unfinished subapplications" OFF)

# --- Build configuration ------------------------------------------------------

# CMAKE_INSTALL_FULL_LOCALEDIR goes into dawn-config.h.
include(GNUInstallDirs)

configure_file(
	"${PROJECT_SOURCE_DIR}/dawn-config.h.in"
	"${PROJECT_BINARY_DIR}/dawn-config.h")

include_directories("${PROJECT_SOURCE_DIR}")
include(CTest)

# Keep diagnostics after bundled dependencies so that they don't apply to them.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options(-Wall -Wextra -Wpedantic
		-Wconversion -Wformat=2 -Wundef -Wcast-align
		-Wno-unused-parameter -Wno-missing-field-initializers)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		add_compile_options(-Wshadow=local)
	else()
		add_compile_options(-Wshadow-uncaptured-local)
	endif()
	add_compile_options(
		$<$<COMPILE_LANGUAGE:CXX>:-Wextra-semi>
		$<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
		$<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>)
endif()

# --- libdn --------------------------------------------------------------------

add_library(libdn SHARED
	libdn/gettext.cpp
	libdn/ipc.cpp
	libdn/ipc-instance.cpp
	libdn/ipc-rpc.cpp
	libdn/libdn.cpp
	libdn/load-icns.cpp
	libdn/load-jpeg.cpp
	libdn/load-ora.cpp
	libdn/load-psd.cpp
	libdn/load-resvg.cpp
	libdn/load-tiff-ep.cpp
	libdn/load-webp.cpp
	libdn/load-wuffs.cpp
	libdn/scale-engine.cpp
	libdn/scale-scaler.cpp
	libdn/thumb-scaler.cpp
	libdn/vk-device.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(libdn
	PUBLIC
		dawn-lcms2
		Intl::Intl
		Vulkan::Vulkan
	PRIVATE
		Threads::Threads
		PkgConfig::JPEG
		PkgConfig::RESVG
		PkgConfig::WEBP
		PkgConfig::WEBPDEMUX
)

if (WIN32)
	target_sources(libdn PRIVATE
		libdn/config-windows.cpp
		libdn/ipc-loop-windows.cpp
		libdn/ipc-shm-windows.cpp
		libdn/ipc-windows.cpp
	)
	target_link_libraries(libdn PRIVATE advapi32)
elseif (APPLE)
	target_sources(libdn PRIVATE
		libdn/config-macos.mm
		libdn/ipc-loop-unix.cpp
		libdn/ipc-shm-unix.cpp
		libdn/ipc-unix.cpp
	)
else()
	target_sources(libdn PRIVATE
		libdn/config-unix.cpp
		libdn/ipc-loop-unix.cpp
		libdn/ipc-shm-unix.cpp
		libdn/ipc-unix.cpp
	)
endif()

macro(dawn_loader stem suffix)
	if (DAWN_WITH_${stem})
		target_sources(libdn PRIVATE libdn/load-${suffix}.cpp)
		target_link_libraries(libdn PRIVATE PkgConfig::${stem})
	endif()
endmacro()

dawn_loader(LIBHEIF heif)
dawn_loader(LIBJXL jxl)
dawn_loader(LIBRAW libraw)
dawn_loader(LIBRSVG librsvg)
dawn_loader(LIBTIFF tiff)
dawn_loader(LIBWMF libwmf)
dawn_loader(OPENJPEG openjpeg)
dawn_loader(POPPLER poppler)
dawn_loader(XCURSOR xcursor)

if (DAWN_WITH_JPEG_QS)
	target_link_libraries(libdn PRIVATE jpegqs)
endif()
if (DAWN_WITH_JXRLIB)
	target_sources(libdn PRIVATE libdn/load-jxr.cpp)
	target_include_directories(libdn SYSTEM PRIVATE "${JXRLIB_INCLUDE_DIR}")
	target_link_libraries(libdn PRIVATE
		"${JXRLIB_GLUE_LIBRARY}" "${JXRLIB_CORE_LIBRARY}")
endif()
if (DAWN_WITH_DNRS)
	add_subdirectory(libdnrs)

	target_sources(libdn PRIVATE libdn/load-dnrs.cpp)
	target_link_libraries(libdn PRIVATE libdnrs)

	# Rust would otherwise export rust_eh_personality, which resvg exports
	# as well, and ELF interposition would cross the two runtimes.
	set(exclude "${CMAKE_STATIC_LIBRARY_PREFIX}dnrs${CMAKE_STATIC_LIBRARY_SUFFIX}")
	set(exclude "LINKER:--exclude-libs,${exclude}")
	include(CheckLinkerFlag)
	check_linker_flag(CXX "${exclude}" DAWN_HAVE_EXCLUDE_LIBS)
	if (DAWN_HAVE_EXCLUDE_LIBS)
		target_link_options(libdn PRIVATE "${exclude}")
	endif()
endif()

if (APPLE)
	find_library(DAWN_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
	find_library(DAWN_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
	find_library(DAWN_IMAGEIO_FRAMEWORK ImageIO REQUIRED)

	target_link_libraries(libdn PRIVATE
		"${DAWN_COREFOUNDATION_FRAMEWORK}" "${DAWN_COREGRAPHICS_FRAMEWORK}")

	if (DAWN_WITH_IMAGEIO)
		target_sources(libdn PRIVATE libdn/load-imageio.mm)
		target_link_libraries(libdn PRIVATE "${DAWN_IMAGEIO_FRAMEWORK}")
	endif()
	if (DAWN_WITH_CGPDF)
		target_sources(libdn PRIVATE libdn/load-cgpdf.mm)
	endif()
endif()

add_custom_command(
	OUTPUT "${PROJECT_BINARY_DIR}/tiff-tables.h"
	COMMAND ${AWK} -f "${PROJECT_SOURCE_DIR}/libdn/tiff-tables.awk"
		"${PROJECT_SOURCE_DIR}/libdn/tiff-tables.db"
		> "${PROJECT_BINARY_DIR}/tiff-tables.h"
	DEPENDS
		"${PROJECT_SOURCE_DIR}/libdn/tiff-tables.awk"
		"${PROJECT_SOURCE_DIR}/libdn/tiff-tables.db"
	VERBATIM
)
target_sources(libdn PRIVATE
	"${PROJECT_BINARY_DIR}/tiff-tables.h")

# Generated LXDR headers; ipc-instance.cpp binds instance to the RPC core,
# and tests/test-lxdr.cpp checks them against Encoder/Decoder.  The types
# every service shares are generated once, into the enclosing namespace.
lxdr_generate(
	${PROJECT_BINARY_DIR}/generated/ipc/common.lxdr.hpp
	dawn::ipc
	Common
	${PROJECT_SOURCE_DIR}/ipc/common.lxdr
)
target_sources(libdn PRIVATE
	"${PROJECT_BINARY_DIR}/generated/ipc/common.lxdr.hpp")
foreach (ipc Instance Imaged Thumbd)
	string(TOLOWER "${ipc}" lower)
	lxdr_generate(
		${PROJECT_BINARY_DIR}/generated/ipc/${lower}.lxdr.hpp
		dawn::ipc::${lower}
		${ipc}
		${PROJECT_SOURCE_DIR}/ipc/${lower}.lxdr
		EXTERN ${PROJECT_SOURCE_DIR}/ipc/common.lxdr
	)
	target_sources(libdn PRIVATE
		"${PROJECT_BINARY_DIR}/generated/ipc/${lower}.lxdr.hpp")
endforeach()

target_include_directories(libdn
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
	PRIVATE
		${PROJECT_SOURCE_DIR}/submodules/wuffs-mirror-release-c/release/c
)

# Vulkan scale shaders
dawn_embed_shader(libdn fullscreen.vert fullscreen_vert)
dawn_embed_shader(libdn scale-h.frag scale_h_bilinear   -DDN_FILTER=1)
dawn_embed_shader(libdn scale-v.frag scale_v_bilinear   -DDN_FILTER=1)
dawn_embed_shader(libdn scale-2d.frag scale_2d_nearest  -DDN_FILTER=0)
dawn_embed_shader(libdn scale-2d.frag scale_2d_bilinear -DDN_FILTER=1)
dawn_embed_shader(libdn scale-2d.frag scale_2d_nohalo   -DDN_FILTER=2)
dawn_embed_shader(libdn thumb-scale-h.comp thumb_scale_h)
dawn_embed_shader(libdn thumb-scale-v.comp thumb_scale_v)
dawn_embed_shader(libdn thumb-reduce.comp thumb_reduce)

set_target_properties(libdn PROPERTIES
	OUTPUT_NAME dn
	SOVERSION 0
)

# --- Other targets ------------------------------------------------------------

if (LIBPNG_FOUND)
	foreach (target dnimage dnthumb)
		add_executable(${target} tools/${target}.cpp tools/png-io.cpp)
		target_link_libraries(${target} PRIVATE libdn PkgConfig::LIBPNG)
	endforeach()
endif()

add_subdirectory(po)

add_executable(dnimaged dnimaged/main.cpp)
target_link_libraries(dnimaged PRIVATE libdn)

add_executable(dnthumbd dnthumbd/main.cpp)
target_link_libraries(dnthumbd PRIVATE libdn)

qt_add_executable(dn
	dn/action.cpp
	dn/app.cpp
	dn/display-profile.cpp
	dn/gpu.cpp
	dn/kit-browser.cpp
	dn/kit-chrome.cpp
	dn/kit-cie-diagram.cpp
	dn/kit-commander.cpp
	dn/kit.cpp
	dn/kit-crop-jpeg.cpp
	dn/kit-viewer.cpp
	dn/main.cpp
	dn/overlay.cpp
	dn/renderer.cpp
	dn/thumbnail-cache.cpp
	dn/thumbnailer.cpp
	dn/url.cpp
	dn/window.cpp
	dn/xdg.cpp
	dn/icons.qrc
)
set_target_properties(dn PROPERTIES AUTORCC ON)

target_link_libraries(dn PRIVATE
	libdn
	Qt6::Gui
	PkgConfig::WEBP
	PkgConfig::WEBPDEMUX
	PkgConfig::WEBPMUX
	PkgConfig::RESVG
	Vulkan::Vulkan
	Threads::Threads
)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
add_custom_command(
	OUTPUT "${PROJECT_BINARY_DIR}/generated/cmf-cie1931-2deg-1nm.h"
	COMMAND ${AWK} -f "${PROJECT_SOURCE_DIR}/dn/cmf-to-header.awk"
		"${PROJECT_SOURCE_DIR}/dn/ciexyz31_1.csv"
		"${PROJECT_BINARY_DIR}/generated/cmf-cie1931-2deg-1nm.h"
	DEPENDS
		"${PROJECT_SOURCE_DIR}/dn/cmf-to-header.awk"
		"${PROJECT_SOURCE_DIR}/dn/ciexyz31_1.csv"
	COMMENT "Generate CIE 1931 2° 1 nm CMF header"
	VERBATIM
)
target_sources(dn PRIVATE
	"${PROJECT_BINARY_DIR}/generated/cmf-cie1931-2deg-1nm.h")

dawn_embed_shader(dn dn-overlay.vert dn_overlay_vert)
dawn_embed_shader(dn dn-overlay.frag dn_overlay_frag)
dawn_embed_shader(dn dn-thumb.frag dn_thumb_frag)
dawn_embed_shader(dn dn-dither.frag dn_dither_frag)

if (WIN32)
	target_sources(dn PRIVATE
		dn/display-profile-windows.cpp
		dn/assoc-windows.cpp
	)
	target_link_libraries(dn PRIVATE
		gdi32 shell32 shlwapi ole32 advapi32 user32)
	set_target_properties(dn PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
elseif (APPLE)
	find_library(DN_APPKIT_FRAMEWORK AppKit REQUIRED)
	find_library(DN_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
	find_library(DN_CORESERVICES_FRAMEWORK CoreServices REQUIRED)
	find_library(DN_UNIFORMTYPES_FRAMEWORK UniformTypeIdentifiers REQUIRED)
	target_sources(dn PRIVATE
		dn/display-profile-macos.mm
		dn/assoc-macos.mm
		dn/app-menu-macos.mm
		dn/window-appearance-macos.mm
	)
	target_link_libraries(dn PRIVATE
		"${DN_APPKIT_FRAMEWORK}"
		"${DN_COREGRAPHICS_FRAMEWORK}"
		"${DN_CORESERVICES_FRAMEWORK}"
		"${DN_UNIFORMTYPES_FRAMEWORK}")
else()
	pkg_check_modules(COLORD REQUIRED IMPORTED_TARGET colord)
	target_sources(dn PRIVATE
		dn/display-profile-linux.cpp
		dn/assoc-unix.cpp
	)
	target_link_libraries(dn PRIVATE PkgConfig::COLORD)
endif()

if (NOT APPLE)
	target_sources(dn PRIVATE dn/instance.cpp)
endif()

# Application icon: SVG → PNG/ICO (xT) on non-Apple; Swift squircle ICNS (xM)
# on Darwin. Qt looks up IDI_ICON1 and nothing else.
set(dn_icon_svg "${PROJECT_SOURCE_DIR}/dn/dn.svg")
if (APPLE)
	find_program(DN_SWIFT_EXECUTABLE swift REQUIRED)
	set(MACOSX_BUNDLE_ICON_FILE dn.icns)
	set(dn_icon_icns "${PROJECT_BINARY_DIR}/${MACOSX_BUNDLE_ICON_FILE}")
	add_custom_command(OUTPUT "${dn_icon_icns}"
		COMMAND "${DN_SWIFT_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/dn/gen-icon.swift" "${dn_icon_icns}"
		DEPENDS "${PROJECT_SOURCE_DIR}/dn/gen-icon.swift"
		COMMENT "Generating dn application icon" VERBATIM)
	set_source_files_properties("${dn_icon_icns}" PROPERTIES
		MACOSX_PACKAGE_LOCATION Resources)
	target_sources(dn PRIVATE "${dn_icon_icns}")

	# Mirrors supported_media_types(), restricted to macOS types.
	# See /System/Library/CoreServices/CoreTypes.bundle/Contents/Info.plist
	set(utis
		com.apple.icns
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
	if (DAWN_WITH_DNRS)
		# The rest of what it adds, ImageIO knows about as well.
		list(APPEND utis
			com.microsoft.ico
			public.xbitmap-image
		)
	endif()
	if (DAWN_WITH_IMAGEIO)
		# Ask ImageIO through the very call the loader uses.
		# Hopefully it will match the system the program is run on as well.
		execute_process(
			COMMAND "${DN_SWIFT_EXECUTABLE}" -e [=[
import ImageIO
for type in CGImageSourceCopyTypeIdentifiers() as! [String] { print(type) }
]=]
			OUTPUT_VARIABLE imageio_utis
			COMMAND_ERROR_IS_FATAL ANY
		)
		separate_arguments(imageio_utis UNIX_COMMAND "${imageio_utis}")
		list(APPEND utis ${imageio_utis})
	endif()
	list(REMOVE_DUPLICATES utis)
	list(SORT utis)

	set(DN_MACOS_CONTENT_TYPES "")
	foreach (uti IN LISTS utis)
		string(APPEND DN_MACOS_CONTENT_TYPES
			"\n\t\t\t\t<string>${uti}</string>")
	endforeach()

	# Still a template afterwards: CMake fills ${MACOSX_BUNDLE_*} at generate
	# time, and Qt copies it once more in between.
	set(plist "${PROJECT_BINARY_DIR}/Info.plist.in")
	configure_file(dn/Info.plist.in "${plist}" @ONLY)

	# XXX: I actually want to name the binary dn but the bundle Dawn.
	set_target_properties(dn PROPERTIES
		RUNTIME_OUTPUT_NAME "${PROJECT_NAME}"
		MACOSX_BUNDLE ON
		MACOSX_BUNDLE_GUI_IDENTIFIER "${DAWN_NAMESPACE}"
		MACOSX_BUNDLE_ICON_FILE "${MACOSX_BUNDLE_ICON_FILE}"
		MACOSX_BUNDLE_INFO_PLIST "${plist}"
		MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
		MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
		MACOSX_BUNDLE_COPYRIGHT "${CPACK_PACKAGE_VENDOR}"
	)
else()
	include(IconUtils)
	set(dn_icon_base "${PROJECT_BINARY_DIR}/icons")
	set(dn_icon_png_list)
	if (WIN32)
		set(dn_icon_name dn)
	else()
		set(dn_icon_name "${DAWN_NAMESPACE}")
	endif()
	foreach (dn_icon_size 16 32 48 256)
		icon_to_png("${dn_icon_name}" "${dn_icon_svg}" ${dn_icon_size}
			"${dn_icon_base}" dn_icon_png)
		list(APPEND dn_icon_png_list "${dn_icon_png}")
	endforeach()

	if (WIN32)
		list(REMOVE_ITEM dn_icon_png_list "${dn_icon_png}")
		set(dn_icon_ico "${PROJECT_BINARY_DIR}/dn.ico")
		icon_for_win32("${dn_icon_ico}" "${dn_icon_png_list}" "${dn_icon_png}")

		set(dn_icon_rc "${PROJECT_BINARY_DIR}/dn.rc")
		# Qt specifically looks up IDI_ICON1 and nothing else.
		file(CONFIGURE OUTPUT "${dn_icon_rc}"
			CONTENT "IDI_ICON1 ICON \"dn.ico\"")
		set_property(SOURCE "${dn_icon_rc}"
			APPEND PROPERTY OBJECT_DEPENDS ${dn_icon_ico})
		target_sources(dn PRIVATE "${dn_icon_rc}")
	else()
		add_custom_target(icons ALL DEPENDS ${dn_icon_png_list})

		install(FILES "${dn_icon_svg}"
			DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
			RENAME "${DAWN_NAMESPACE}.svg")
		install(DIRECTORY "${dn_icon_base}"
			DESTINATION "${CMAKE_INSTALL_DATADIR}")

		# MimeType= follows the loader table compiled into dn, so ask it.
		# A cross build can only ask through an emulator; without one,
		# the checked-in list is installed as it is.
		set(desktop "${PROJECT_SOURCE_DIR}/dn/${DAWN_NAMESPACE}.desktop")
		if (NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
			set(generated
				"${PROJECT_BINARY_DIR}/${DAWN_NAMESPACE}.mime.desktop")
			add_custom_command(OUTPUT "${generated}"
				COMMAND ${CMAKE_COMMAND}
					"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
					-DDN=$<TARGET_FILE:dn>
					"-DINPUT=${desktop}" "-DOUTPUT=${generated}"
					-P "${PROJECT_SOURCE_DIR}/cmake/desktop-media-types.cmake"
				DEPENDS dn "${desktop}"
				COMMENT "Filling in desktop file media types"
				VERBATIM)
			set(desktop "${generated}")
		endif()

		set(desktops "${desktop}"
			"${PROJECT_SOURCE_DIR}/dn/${DAWN_NAMESPACE}.Browse.desktop")
		if (DAWN_WITH_WIP)
			list(APPEND desktops
				"${PROJECT_SOURCE_DIR}/dn/${DAWN_NAMESPACE}.CropJpeg.desktop"
				"${PROJECT_SOURCE_DIR}/dn/${DAWN_NAMESPACE}.Commander.desktop")
		endif()

		# What the launcher says is translated from the same catalogue as the
		# application, which is why the names in it are never invented here.
		set(entries)
		file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/applications")
		foreach (template IN LISTS desktops)
			get_filename_component(entry "${template}" NAME)
			string(REPLACE ".mime." "." entry "${entry}")
			set(output "${PROJECT_BINARY_DIR}/applications/${entry}")
			add_custom_command(OUTPUT "${output}"
				COMMAND "${MSGFMT_EXECUTABLE}" --desktop
					--keyword=Name --keyword=GenericName
					--keyword=X-GNOME-FullName
					"--template=${template}" -d "${PROJECT_SOURCE_DIR}/po"
					-o "${output}"
				DEPENDS "${template}" ${DAWN_TRANSLATIONS}
				COMMENT "Translating ${entry}"
				VERBATIM)
			list(APPEND entries "${output}")
		endforeach()
		add_custom_target(desktop-entries ALL DEPENDS ${entries})
		install(FILES ${entries}
			DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
	endif()
endif()

if (DN_WITH_WAYLAND)
	pkg_get_variable(dn_wayland_protocols_dir wayland-protocols pkgdatadir)
	if (NOT dn_wayland_protocols_dir)
		message(FATAL_ERROR "wayland-protocols pkgdatadir not found")
	endif()

	pkg_get_variable(dn_wayland_scanner wayland-scanner wayland_scanner)
	if (NOT dn_wayland_scanner)
		find_program(dn_wayland_scanner wayland-scanner REQUIRED)
	endif()

	set(dn_protocol_dir "${PROJECT_BINARY_DIR}/protocols")
	file(MAKE_DIRECTORY "${dn_protocol_dir}")
	set(dn_color_xml
		"${dn_wayland_protocols_dir}/staging/color-management/color-management-v1.xml")
	set(dn_color_header
		"${dn_protocol_dir}/color-management-v1-client-protocol.h")
	set(dn_color_source
		"${dn_protocol_dir}/color-management-v1-protocol.c")
	set(dn_xdg_xml
		"${dn_wayland_protocols_dir}/stable/xdg-shell/xdg-shell.xml")
	set(dn_xdg_header
		"${dn_protocol_dir}/xdg-shell-client-protocol.h")
	add_custom_command(
		OUTPUT "${dn_color_header}" "${dn_color_source}" "${dn_xdg_header}"
		COMMAND "${dn_wayland_scanner}" client-header
			"${dn_color_xml}" "${dn_color_header}"
		COMMAND "${dn_wayland_scanner}" private-code
			"${dn_color_xml}" "${dn_color_source}"
		COMMAND "${dn_wayland_scanner}" client-header
			"${dn_xdg_xml}" "${dn_xdg_header}"
		DEPENDS "${dn_color_xml}" "${dn_xdg_xml}"
		COMMENT "wayland-scanner protocols for dn"
		VERBATIM)

	target_sources(dn PRIVATE
		dn/wayland-color-bridge.cpp
		dn/wayland-window.cpp
		"${dn_color_header}"
		"${dn_color_source}"
		"${dn_xdg_header}"
	)
	target_include_directories(dn PRIVATE "${dn_protocol_dir}")
	target_link_libraries(dn PRIVATE PkgConfig::WAYLAND Qt6::GuiPrivate)
endif()

# User guide: build tree at share/docs/dawn (../share from bin/),
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
set(dawn_help_files
	"${DAWN_HELP_DIR}/dn.html"
	"${DAWN_HELP_DIR}/stylesheet.css")

if (APPLE)
	set_source_files_properties(${dawn_help_files}
		PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/share/doc/dawn")
	target_sources(dn PRIVATE ${dawn_help_files})
elseif (NOT WIN32)
	# This manual page generator should be good enough.
	set(ASCIIMAN "${PROJECT_SOURCE_DIR}/submodules/liberty/tools/asciiman.awk")
	foreach (page dn)
		set(page_output "${PROJECT_BINARY_DIR}/${page}.1")
		list(APPEND project_MAN_PAGES "${page_output}")
		add_custom_command(OUTPUT ${page_output}
			COMMAND env LC_ALL=C asciidoc-release-version=${PROJECT_VERSION}
				${AWK} -f ${ASCIIMAN} "${PROJECT_SOURCE_DIR}/docs/${page}.adoc"
				> ${page_output}
			DEPENDS "${PROJECT_SOURCE_DIR}/docs/${page}.adoc" ${ASCIIMAN}
			COMMENT "Generating man page for ${page}" VERBATIM)
	endforeach()

	add_custom_target(docs ALL DEPENDS ${project_MAN_PAGES})
endif()

# Message catalogues.  The bundle's Resources have to be complete already
# in the build tree; the installation copies the bundle as it finds it.
add_dependencies(dn catalogues)
if (APPLE AND DAWN_CATALOGUES)
	foreach (mo IN LISTS DAWN_CATALOGUES)
		file(RELATIVE_PATH rel "${DAWN_LOCALE_DIR}" "${mo}")
		get_filename_component(rel "${rel}" DIRECTORY)
		set_source_files_properties("${mo}" PROPERTIES
			MACOSX_PACKAGE_LOCATION "Resources/share/locale/${rel}")
	endforeach()
	target_sources(dn PRIVATE ${DAWN_CATALOGUES})
endif()

# AppKit-only identities beside Dawn.app.  Compile with swiftc directly so that
# this also works with Makefile generators, without enabling CMake's
# Swift language.
if (APPLE AND DAWN_WITH_WIP)
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
		set(slice "${PROJECT_BINARY_DIR}/dn-launcher-${arch}")
		add_custom_command(OUTPUT "${slice}"
			COMMAND "${DN_SWIFTC_EXECUTABLE}" -swift-version 5 -O
				-target "${arch}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}"
				-sdk "${dn_launcher_sdk}" -framework AppKit
				"${PROJECT_SOURCE_DIR}/dn/mode-launcher.swift" -o "${slice}"
			DEPENDS "${PROJECT_SOURCE_DIR}/dn/mode-launcher.swift"
			VERBATIM)
		list(APPEND dn_launcher_slices "${slice}")
	endforeach()

	set(dn_launcher "${PROJECT_BINARY_DIR}/dn-launcher")
	add_custom_command(OUTPUT "${dn_launcher}"
		COMMAND "${DN_LIPO_EXECUTABLE}"
			-create ${dn_launcher_slices} -output "${dn_launcher}"
		DEPENDS ${dn_launcher_slices} VERBATIM)

	foreach (dn_launch_id IN ITEMS CropJpeg Commander)
		if (dn_launch_id STREQUAL CropJpeg)
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
		file(MAKE_DIRECTORY
			"${bundle}/Contents/MacOS" "${bundle}/Contents/Resources")
		configure_file("${PROJECT_SOURCE_DIR}/dn/Launcher-Info.plist.in"
			"${bundle}/Contents/Info.plist" @ONLY)
		add_custom_command(OUTPUT "${bundle}/Contents/MacOS/dn-launcher"
				"${bundle}/Contents/Resources/dn.icns"
			COMMAND "${CMAKE_COMMAND}" -E copy
				"${dn_launcher}" "${bundle}/Contents/MacOS/dn-launcher"
			COMMAND "${CMAKE_COMMAND}" -E copy
				"${dn_icon_icns}" "${bundle}/Contents/Resources/dn.icns"
			DEPENDS "${dn_launcher}" "${dn_icon_icns}" VERBATIM)
		add_custom_target(dn-launcher-${dn_launch_id} ALL
			DEPENDS "${bundle}/Contents/MacOS/dn-launcher"
				"${bundle}/Contents/Resources/dn.icns")
		add_dependencies(dn-launcher-${dn_launch_id} dn)
		install(DIRECTORY "${bundle}" DESTINATION . USE_SOURCE_PERMISSIONS)
	endforeach()
endif()

# --- Tests --------------------------------------------------------------------

if (BUILD_TESTING)
	add_test(NAME dn-help COMMAND $<TARGET_FILE:dn> --help)
	add_subdirectory(tests)
endif()

# --- Installation -------------------------------------------------------------

if (WIN32)
	install(TARGETS dn libdn
		RUNTIME DESTINATION .)
	install(DIRECTORY "${DAWN_MSYS2_PREFIX}/bin/"
		DESTINATION .
		FILES_MATCHING
			PATTERN "*.dll"
			PATTERN "vulkan-1.dll" EXCLUDE)
	install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QWindowsIntegrationPlugin
		RUNTIME DESTINATION platforms
		LIBRARY DESTINATION platforms)

	install(FILES ${dawn_help_files}
		DESTINATION share/doc/dawn)
	install(FILES
		"${DAWN_MSYS2_PREFIX}/share/mime/globs2"
		"${DAWN_MSYS2_PREFIX}/share/mime/subclasses"
		DESTINATION share/mime)

	install(FILES
		"${DAWN_MSYS2_PREFIX}/bin/vk_swiftshader_icd.json"
		DESTINATION .)

	# This installation is pretty big, with a ton of files, thus it's optional.
	install(FILES
		"${DAWN_MSYS2_PREFIX}/bin/wperl.exe"
		"${DAWN_MSYS2_PREFIX}/bin/exiftool"
		DESTINATION .
		OPTIONAL)
	install(DIRECTORY
		"${DAWN_MSYS2_PREFIX}/lib/perl5/"
		DESTINATION lib/perl5
		OPTIONAL)

	# Prune unused libraries.
	install(SCRIPT "${PROJECT_SOURCE_DIR}/cmake/msys2-cleanup.cmake")
elseif (APPLE)
	# macOS app bundle: Qt Frameworks/PlugIns + fiv-like Resources/share.
	# MACOSX_BUNDLE is set on dn next to qt_add_executable.

	set(dn_bundle "$<TARGET_BUNDLE_DIR_NAME:dn>")
	install(TARGETS dn BUNDLE DESTINATION .)
	#install(TARGETS dnimaged dnthumbd
	#	RUNTIME DESTINATION "${dn_bundle}/Contents/MacOS")
	install(TARGETS libdn
		LIBRARY DESTINATION "${dn_bundle}/Contents/Frameworks")

	set(dn_mime_dir)
	pkg_get_variable(dn_smi_datadir shared-mime-info datadir)
	if (dn_smi_datadir AND EXISTS "${dn_smi_datadir}/mime/globs2"
			AND EXISTS "${dn_smi_datadir}/mime/subclasses")
		set(dn_mime_dir "${dn_smi_datadir}/mime")
	endif()
	if (NOT dn_mime_dir)
		foreach (dn_root IN ITEMS
			"$ENV{HOMEBREW_PREFIX}" /opt/homebrew /usr/local)
			if(dn_root AND EXISTS "${dn_root}/share/mime/globs2"
					AND EXISTS "${dn_root}/share/mime/subclasses")
				set(dn_mime_dir "${dn_root}/share/mime")
				break()
			endif()
		endforeach()
	endif()
	install(FILES
		"${dn_mime_dir}/globs2"
		"${dn_mime_dir}/subclasses"
		DESTINATION "${dn_bundle}/Contents/Resources/share/mime")

	# macdeployqt otherwise copies imageformats/virtualkeyboard/styles and
	# pulls QtSvg, QtPdf, QtQuick, QtWidgets, QtNetwork with them.
	install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QCocoaIntegrationPlugin
		LIBRARY DESTINATION "${dn_bundle}/Contents/PlugIns/platforms"
		RUNTIME DESTINATION "${dn_bundle}/Contents/PlugIns/platforms")

	# MoltenVK is an ICD; macdeployqt only copies the linked loader.
	# Homebrew lib/libMoltenVK.dylib is a Cellar symlink; install(FILES) would
	# copy the link and the relocated bundle would not find MoltenVK.
	find_library(DN_MOLTENVK_LIBRARY NAMES MoltenVK REQUIRED
		HINTS "$ENV{HOMEBREW_PREFIX}/lib" /opt/homebrew/lib /usr/local/lib)
	get_filename_component(
		DN_MOLTENVK_LIBRARY "${DN_MOLTENVK_LIBRARY}" REALPATH)
	install(FILES "${DN_MOLTENVK_LIBRARY}"
		DESTINATION "${dn_bundle}/Contents/Frameworks")
	file(WRITE "${CMAKE_BINARY_DIR}/MoltenVK_icd.json" [[{
		"file_format_version": "1.0.0",
		"ICD": {
			"library_path": "../../../Frameworks/libMoltenVK.dylib",
			"api_version": "1.4.0",
			"is_portability_driver": true
		}
	}
	]])
	install(FILES "${CMAKE_BINARY_DIR}/MoltenVK_icd.json"
		DESTINATION "${dn_bundle}/Contents/Resources/vulkan/icd.d")
	unset(dn_bundle)

	qt_generate_deploy_app_script(TARGET dn OUTPUT_SCRIPT dn_deploy NO_PLUGINS)
	install(SCRIPT "${dn_deploy}")
	# Homebrew Qt QTBUG-127075: run macdeployqt twice.
	install(SCRIPT "${dn_deploy}")
else()
	install(TARGETS dn libdn
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

	install(FILES ${dawn_help_files}
		DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dawn")
	foreach (page ${project_MAN_PAGES})
		string(REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
		install(FILES "${page}"
			DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
	endforeach()

	# Software centres want this, and Flatpak bundles only carry an icon
	# when an AppStream component points at one.
	configure_file(flatpak-metainfo.xml.in
		"${PROJECT_BINARY_DIR}/${DAWN_NAMESPACE}.metainfo.xml" @ONLY)
	install(FILES "${PROJECT_BINARY_DIR}/${DAWN_NAMESPACE}.metainfo.xml"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
endif()

# --- Packaging ----------------------------------------------------------------

set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)

# CPack's WIX generator needs the real WiX toolset (Windows or wine).
# We've already figured out msitools.
if (WIN32 AND CMAKE_CROSSCOMPILING)
	set(DAWN_ICON_ICO "${dn_icon_ico}")
	set(DAWN_MSI_WIP OFF)
	if (DAWN_WITH_WIP)
		set(DAWN_MSI_WIP ON)
	endif()
	set(dawn_wxs "${PROJECT_BINARY_DIR}/dawn.wxs")
	configure_file(dawn.wxs.in "${dawn_wxs}" @ONLY)

	set(dawn_msi
		"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}.msi")
	add_custom_target(msi
		COMMAND "${PROJECT_SOURCE_DIR}/msys2-package-msi.sh"
			"${CMAKE_SYSTEM_PROCESSOR}" "${dawn_msi}" "${dawn_wxs}"
			"${PROJECT_DESCRIPTION}" "${DAWN_MSI_WIP}"
		WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
		VERBATIM)
endif()
