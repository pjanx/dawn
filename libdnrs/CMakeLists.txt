# Cargo builds for the build host, and only an explicit target gets
# a directory of its own.  Dawn's Windows builds are MinGW, and cross-compiled.
set(default_target)
if (WIN32)
	set(default_target x86_64-pc-windows-gnu)
endif()

set(DAWN_DNRS_RUST_TARGET "${default_target}" CACHE STRING
	"Rust target triple for libdnrs; empty builds for the build host")
if (CMAKE_CROSSCOMPILING AND NOT DAWN_DNRS_RUST_TARGET)
	message(FATAL_ERROR "Cross-compiling libdnrs needs DAWN_DNRS_RUST_TARGET")
endif()

set(target_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
set(cargo_args build --release)
set(cargo_env "CARGO_TARGET_DIR=${target_dir}")
set(archive "${target_dir}")
if (DAWN_DNRS_RUST_TARGET)
	list(APPEND cargo_args --target "${DAWN_DNRS_RUST_TARGET}")
	string(APPEND archive "/${DAWN_DNRS_RUST_TARGET}")
endif()
set(archive "${archive}/release/${CMAKE_STATIC_LIBRARY_PREFIX}dnrs")
set(archive "${archive}${CMAKE_STATIC_LIBRARY_SUFFIX}")

if (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
	list(APPEND cargo_env
		"MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
if (CMAKE_CROSSCOMPILING)
	string(TOUPPER "${DAWN_DNRS_RUST_TARGET}" env_target)
	string(REPLACE "-" "_" env_target "${env_target}")
	string(TOLOWER "${env_target}" cc_env_target)

	# As with resvg: Rust comes with the standard library of its own host
	# alone, so any cross build has to make one from rust-src, and that is
	# a nightly feature.
	list(APPEND cargo_args -Zbuild-std)
	list(APPEND cargo_env
		RUSTC_BOOTSTRAP=1
		"CARGO_TARGET_${env_target}_LINKER=${CMAKE_C_COMPILER}"
		"CC_${cc_env_target}=${CMAKE_C_COMPILER}"
		"AR_${cc_env_target}=${CMAKE_AR}")
endif()

add_custom_command(
	OUTPUT "${archive}"
	COMMAND ${CMAKE_COMMAND} -E env ${cargo_env}
		${CARGO_EXECUTABLE} ${cargo_args}
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	DEPENDS lib.rs Cargo.toml libdnrs.h CMakeLists.txt
	COMMENT "Building libdnrs"
	JOB_SERVER_AWARE TRUE
	VERBATIM)

add_custom_target(libdnrs-build DEPENDS "${archive}")
add_library(libdnrs STATIC IMPORTED GLOBAL)
set_target_properties(libdnrs PROPERTIES
	IMPORTED_LOCATION "${archive}"
	INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
if (WIN32)
	# Rust's MinGW standard library calls these Native API helpers.
	target_link_libraries(libdnrs INTERFACE ntdll)
endif()
add_dependencies(libdnrs libdnrs-build)

if (BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
	add_test(NAME dnrs-rust
		COMMAND ${CMAKE_COMMAND} -E env ${cargo_env}
			${CARGO_EXECUTABLE} test --all-features)
	set_tests_properties(dnrs-rust PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
