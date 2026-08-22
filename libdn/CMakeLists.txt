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
add_custom_target(dn_tiff_tables
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/tiff-tables.h)

set(DN_WUFFS_DIR ${PROJECT_SOURCE_DIR}/submodules/wuffs-mirror-release-c)

set(DN_LOADER_SOURCES "")
set(DN_OPTIONAL_LIBS "")
if(DN_WITH_JPEG_QS)
	list(APPEND DN_OPTIONAL_LIBS jpegqs)
endif()
if(DN_WITH_LIBRAW)
	list(APPEND DN_LOADER_SOURCES load-libraw.cpp)
	list(APPEND DN_OPTIONAL_LIBS PkgConfig::LIBRAW)
endif()
list(APPEND DN_LOADER_SOURCES load-resvg.cpp)
if(DN_WITH_LIBRSVG)
	list(APPEND DN_LOADER_SOURCES load-librsvg.cpp)
	list(APPEND DN_OPTIONAL_LIBS PkgConfig::LIBRSVG)
endif()
if(DN_WITH_XCURSOR)
	list(APPEND DN_LOADER_SOURCES load-xcursor.cpp)
	list(APPEND DN_OPTIONAL_LIBS PkgConfig::XCURSOR)
endif()
if(DN_WITH_LIBHEIF)
	list(APPEND DN_LOADER_SOURCES load-heif.cpp)
	list(APPEND DN_OPTIONAL_LIBS PkgConfig::LIBHEIF)
endif()
if(DN_WITH_LIBTIFF)
	list(APPEND DN_LOADER_SOURCES load-tiff.cpp)
	list(APPEND DN_OPTIONAL_LIBS PkgConfig::LIBTIFF)
endif()
if(DN_WITH_GDKPIXBUF)
	list(APPEND DN_LOADER_SOURCES load-gdkpixbuf.cpp)
	list(APPEND DN_OPTIONAL_LIBS PkgConfig::GDKPIXBUF)
endif()

add_library(libdn SHARED
	libdn.cpp
	ipc.cpp
	load-wuffs.cpp
	load-jpeg.cpp
	load-webp.cpp
	load-tiff-ep.cpp
	${DN_LOADER_SOURCES}
)
# Target is libdn; SONAME/file is libdn.so (avoid liblibdn.so).
# MinGW would otherwise emit libdn.dll; the exe looks for dn.dll beside itself.
set_target_properties(libdn PROPERTIES OUTPUT_NAME dn)
if(WIN32)
	set_target_properties(libdn PROPERTIES PREFIX "")
endif()
add_library(dawn::libdn ALIAS libdn)

if(NOT WIN32 AND NOT APPLE)
	target_sources(libdn PRIVATE ipc-unix.cpp ipc-instance.cpp)
endif()

# Generated LXDR header; ipc.cpp compile-checks it against Encoder/Decoder.
dn_lxdr_generate(
	${PROJECT_BINARY_DIR}/generated/ipc/instance.lxdr.hpp
	dn::ipc::instance
	Instance
	${PROJECT_SOURCE_DIR}/ipc/common.lxdr
	${PROJECT_SOURCE_DIR}/ipc/instance.lxdr
)
add_dependencies(libdn dn_tiff_tables dn_lxdr_instance_lxdr_hpp)

target_include_directories(libdn
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}
		${DN_WUFFS_DIR}/release/c
)

find_package(Threads REQUIRED)
target_link_libraries(libdn
	PUBLIC
		${DN_LCMS2_TARGET}
		Vulkan::Vulkan
	PRIVATE
		Threads::Threads
		PkgConfig::JPEG
		PkgConfig::WEBP
		PkgConfig::WEBPDEMUX
		PkgConfig::RESVG
		${DN_OPTIONAL_LIBS}
)


# Vulkan scale shaders
dawn_embed_shader(libdn fullscreen.vert fullscreen_vert)
dawn_embed_shader(libdn scale-h.frag scale_h_bilinear
	-DDN_FILTER=1)
dawn_embed_shader(libdn scale-v.frag scale_v_bilinear
	-DDN_FILTER=1)
dawn_embed_shader(libdn scale-2d.frag scale_2d_nearest
	-DDN_FILTER=0)
dawn_embed_shader(libdn scale-2d.frag scale_2d_bilinear
	-DDN_FILTER=1)
dawn_embed_shader(libdn scale-2d.frag scale_2d_nohalo
	-DDN_FILTER=2)
dawn_embed_shader(libdn thumb-scale-h.comp thumb_scale_h)
dawn_embed_shader(libdn thumb-scale-v.comp thumb_scale_v)
dawn_embed_shader(libdn thumb-reduce.comp thumb_reduce)

target_sources(libdn PRIVATE
	scale-engine.cpp
	scale-scaler.cpp
	thumb-scaler.cpp
	vk-device.cpp
)
