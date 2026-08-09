# Overlay for jpeg-quantsmooth (copied into the FetchContent source tree).
# Port of fiv/subprojects/packagefiles/libjpegqs/meson.build.
#
# One static archive (not several link_with deps): the selector and SIMD
# variants reference each other, and separate .a files lose do_quantsmooth
# when linked into libdn.so (GNU ld single-pass archives).

cmake_minimum_required(VERSION 3.20)
project(jpegqs LANGUAGES C)

if(NOT TARGET PkgConfig::JPEG)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
endif()

find_library(JPEGQS_LIBM m)

set(_jpegqs_defs WITH_LOG)
set(_jpegqs_libs PkgConfig::JPEG)
if(JPEGQS_LIBM)
	list(APPEND _jpegqs_libs "${JPEGQS_LIBM}")
endif()

# Do not put the quantsmooth source dir on the include path: it ships a
# jconfig.h that shadows the system jpeg-turbo one under multiarch layouts.

function(dn_jpegqs_object name)
	cmake_parse_arguments(A "" "" "DEFS;FLAGS" ${ARGN})
	add_library(jpegqs-${name} OBJECT libjpegqs.c)
	target_compile_definitions(jpegqs-${name} PRIVATE ${_jpegqs_defs} ${A_DEFS})
	if(A_FLAGS)
		target_compile_options(jpegqs-${name} PRIVATE ${A_FLAGS})
	endif()
	target_link_libraries(jpegqs-${name} PUBLIC ${_jpegqs_libs})
	set_target_properties(jpegqs-${name} PROPERTIES
		POSITION_INDEPENDENT_CODE ON)
endfunction()

set(_jpegqs_objects "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
	dn_jpegqs_object(avx512
		DEFS SIMD_SELECT SIMD_NAME=avx512 SIMD_AVX512
		FLAGS -mavx512f -mfma)
	dn_jpegqs_object(avx2
		DEFS SIMD_SELECT SIMD_NAME=avx2 SIMD_AVX2
		FLAGS -mavx2 -mfma)
	dn_jpegqs_object(sse2
		DEFS SIMD_SELECT SIMD_NAME=sse2 SIMD_SSE2
		FLAGS -msse2)
	dn_jpegqs_object(base
		DEFS SIMD_SELECT SIMD_NAME=base SIMD_BASE)
	dn_jpegqs_object(select DEFS SIMD_SELECT)
	set(_jpegqs_objects
		$<TARGET_OBJECTS:jpegqs-base>
		$<TARGET_OBJECTS:jpegqs-sse2>
		$<TARGET_OBJECTS:jpegqs-avx2>
		$<TARGET_OBJECTS:jpegqs-avx512>
		$<TARGET_OBJECTS:jpegqs-select>)
else()
	dn_jpegqs_object(nosimd DEFS NO_SIMD)
	set(_jpegqs_objects $<TARGET_OBJECTS:jpegqs-nosimd>)
endif()

add_library(jpegqs STATIC ${_jpegqs_objects})
set_target_properties(jpegqs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(jpegqs PUBLIC ${_jpegqs_libs})
target_include_directories(jpegqs PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
