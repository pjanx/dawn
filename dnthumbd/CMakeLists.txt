add_executable(dnthumbd
	main.cpp
	orient.cpp
	encode-webp.cpp
)

target_link_libraries(dnthumbd PRIVATE
	dawn::libdn
	PkgConfig::WEBP
)

target_include_directories(dnthumbd PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${PROJECT_SOURCE_DIR}/libdn
)
