foreach(target dnimage dnthumb)
	add_executable(${target}
		${target}.cpp
		png-io.cpp
	)
	target_link_libraries(${target} PRIVATE
		dawn::libdn
		PkgConfig::LIBPNG
	)
	target_include_directories(${target} PRIVATE
		${PROJECT_SOURCE_DIR}/libdn
	)
endforeach()
