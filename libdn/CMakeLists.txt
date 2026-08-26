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
	ipc.cpp
	libdn.cpp
	load-jpeg.cpp
	load-tiff-ep.cpp
	load-webp.cpp
	load-wuffs.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(libdn
	PUBLIC
		${DAWN_LCMS2_TARGET}
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
if (DAWN_WITH_GDKPIXBUF)
	target_sources(libdn PRIVATE load-gdkpixbuf.cpp)
	target_link_libraries(libdn PRIVATE PkgConfig::GDKPIXBUF)
endif()

# Target is libdn; SONAME/file is libdn.so (avoid liblibdn.so).
# MinGW would otherwise emit libdn.dll; the exe looks for dn.dll beside itself.
set_target_properties(libdn PROPERTIES OUTPUT_NAME dn)
if (WIN32)
	set_target_properties(libdn PROPERTIES PREFIX "")
endif()
add_library(dawn::libdn ALIAS libdn)

if (WIN32)
	target_sources(libdn PRIVATE ipc-windows.cpp ipc-instance.cpp)
	target_link_libraries(libdn PRIVATE advapi32)
elseif (NOT APPLE)
	target_sources(libdn PRIVATE ipc-unix.cpp ipc-instance.cpp)
endif()

# Generated LXDR header; ipc.cpp compile-checks it against Encoder/Decoder.
lxdr_generate(
	${PROJECT_BINARY_DIR}/generated/ipc/instance.lxdr.hpp
	dn::ipc::instance
	Instance
	${PROJECT_SOURCE_DIR}/ipc/common.lxdr
	${PROJECT_SOURCE_DIR}/ipc/instance.lxdr
)
add_custom_target(dn-tiff-tables
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/tiff-tables.h)
add_dependencies(libdn dn-tiff-tables lxdr-instance_lxdr_hpp)

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
