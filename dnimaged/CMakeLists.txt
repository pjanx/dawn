add_executable(dnimaged
	main.cpp
)

target_link_libraries(dnimaged PRIVATE
	dawn::libdn
)

target_include_directories(dnimaged PRIVATE
	${PROJECT_SOURCE_DIR}/libdn
)
