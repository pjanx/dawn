add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tiff-tables.h
	COMMAND ${AWK} -f ${CMAKE_CURRENT_SOURCE_DIR}/tiff-tables.awk
		${CMAKE_CURRENT_SOURCE_DIR}/tiff-tables.db
		> ${CMAKE_CURRENT_BINARY_DIR}/tiff-tables.h
	DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/tiff-tables.awk
		${CMAKE_CURRENT_SOURCE_DIR}/tiff-tables.db
	VERBATIM
)
add_library(libdn SHARED
	gettext.cpp
	ipc.cpp
	libdn.cpp
	load-icns.cpp
	load-jpeg.cpp
	load-ora.cpp
	load-psd.cpp
	load-tiff-ep.cpp
	load-webp.cpp
	load-wuffs.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(libdn
	PUBLIC
		${DAWN_LCMS2_TARGET}
		Intl::Intl
		Vulkan::Vulkan
	PRIVATE
		Threads::Threads
		PkgConfig::JPEG
		PkgConfig::RESVG
		PkgConfig::WEBP
		PkgConfig::WEBPDEMUX
)

if (DAWN_WITH_JPEG_QS)
	target_link_libraries(libdn PRIVATE jpegqs)
endif()
if (DAWN_WITH_LIBRAW)
	target_sources(libdn PRIVATE load-libraw.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::LIBRAW)
endif()
if (DAWN_WITH_LIBWMF)
	target_sources(libdn PRIVATE load-libwmf.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::LIBWMF)
endif()
if (DAWN_WITH_POPPLER)
	target_sources(libdn PRIVATE load-poppler.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::POPPLER)
endif()
target_sources(libdn PRIVATE load-resvg.cpp)
if (DAWN_WITH_LIBRSVG)
	target_sources(libdn PRIVATE load-librsvg.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::LIBRSVG)
endif()
if (DAWN_WITH_XCURSOR)
	target_sources(libdn PRIVATE load-xcursor.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::XCURSOR)
endif()
if (DAWN_WITH_LIBHEIF)
	target_sources(libdn PRIVATE load-heif.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::LIBHEIF)
endif()
if (DAWN_WITH_LIBJXL)
	target_sources(libdn PRIVATE load-jxl.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::LIBJXL)
endif()
if (DAWN_WITH_OPENJPEG)
	target_sources(libdn PRIVATE load-openjpeg.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::OPENJPEG)
endif()
if (DAWN_WITH_LIBTIFF)
	target_sources(libdn PRIVATE load-tiff.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::LIBTIFF)
endif()
if (DAWN_WITH_JXRLIB)
	target_sources(libdn PRIVATE load-jxr.cpp)
	target_include_directories(libdn SYSTEM PRIVATE "${JXRLIB_INCLUDE_DIR}")
	target_link_libraries(libdn PRIVATE
		"${JXRLIB_GLUE_LIBRARY}" "${JXRLIB_CORE_LIBRARY}")
endif()
if (DAWN_WITH_DNRS)
	target_sources(libdn PRIVATE load-dnrs.cpp)
	target_link_libraries(libdn PRIVATE libdnrs)
	if (WIN32)
		# Rust's MinGW standard library calls these Native API helpers.
		target_link_libraries(libdn PRIVATE ntdll)
	endif()

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

# Target is libdn; SONAME/file is libdn.so (avoid liblibdn.so).
# MinGW would otherwise emit libdn.dll; the exe looks for dn.dll beside itself.
set_target_properties(libdn PROPERTIES OUTPUT_NAME dn)
if (WIN32)
	set_target_properties(libdn PROPERTIES PREFIX "")
endif()
add_library(dawn::libdn ALIAS libdn)

if (WIN32)
	target_sources(libdn PRIVATE
		config-windows.cpp
		ipc-instance.cpp
		ipc-loop-windows.cpp
		ipc-rpc.cpp
		ipc-shm-windows.cpp
		ipc-windows.cpp
	)
	target_link_libraries(libdn PRIVATE advapi32)
elseif (APPLE)
	enable_language(OBJCXX)
	find_library(DAWN_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
	target_sources(libdn PRIVATE
		config-macos.mm
		ipc-instance.cpp
		ipc-loop-unix.cpp
		ipc-rpc.cpp
		ipc-shm-unix.cpp
		ipc-unix.cpp
	)
	target_link_libraries(libdn PRIVATE "${DAWN_COREFOUNDATION_FRAMEWORK}")

	if (DAWN_WITH_IMAGEIO OR DAWN_WITH_CGPDF)
		find_library(DAWN_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
		target_link_libraries(libdn PRIVATE "${DAWN_COREGRAPHICS_FRAMEWORK}")
	endif()

	if (DAWN_WITH_IMAGEIO)
		find_library(DAWN_IMAGEIO_FRAMEWORK ImageIO REQUIRED)
		target_sources(libdn PRIVATE load-imageio.mm)
		target_link_libraries(libdn PRIVATE "${DAWN_IMAGEIO_FRAMEWORK}")
	endif()

	if (DAWN_WITH_CGPDF)
		target_sources(libdn PRIVATE load-cgpdf.mm)
	endif()
elseif (NOT APPLE)
	target_sources(libdn PRIVATE
		config-unix.cpp
		ipc-instance.cpp
		ipc-loop-unix.cpp
		ipc-rpc.cpp
		ipc-shm-unix.cpp
		ipc-unix.cpp
	)
endif()

# Generated LXDR headers; ipc-instance.cpp binds instance to the RPC core,
# and tests/test-lxdr.cpp checks them against Encoder/Decoder.  The types
# every service shares are generated once, into the enclosing namespace.
lxdr_generate(
	${PROJECT_BINARY_DIR}/generated/ipc/common.lxdr.hpp
	dawn::ipc
	Common
	${PROJECT_SOURCE_DIR}/ipc/common.lxdr
)
lxdr_generate(
	${PROJECT_BINARY_DIR}/generated/ipc/instance.lxdr.hpp
	dawn::ipc::instance
	Instance
	${PROJECT_SOURCE_DIR}/ipc/instance.lxdr
	EXTERN ${PROJECT_SOURCE_DIR}/ipc/common.lxdr
)
lxdr_generate(
	${PROJECT_BINARY_DIR}/generated/ipc/imaged.lxdr.hpp
	dawn::ipc::imaged
	Imaged
	${PROJECT_SOURCE_DIR}/ipc/imaged.lxdr
	EXTERN ${PROJECT_SOURCE_DIR}/ipc/common.lxdr
)
lxdr_generate(
	${PROJECT_BINARY_DIR}/generated/ipc/thumbd.lxdr.hpp
	dawn::ipc::thumbd
	Thumbd
	${PROJECT_SOURCE_DIR}/ipc/thumbd.lxdr
	EXTERN ${PROJECT_SOURCE_DIR}/ipc/common.lxdr
)
add_custom_target(dn-tiff-tables
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/tiff-tables.h)
add_dependencies(libdn
	dn-tiff-tables
	lxdr-common_lxdr_hpp
	lxdr-imaged_lxdr_hpp
	lxdr-instance_lxdr_hpp
	lxdr-thumbd_lxdr_hpp
)

target_include_directories(libdn
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}
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

target_sources(libdn PRIVATE
	scale-engine.cpp
	scale-scaler.cpp
	thumb-scaler.cpp
	vk-device.cpp
)
