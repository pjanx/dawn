# Windows ZIP: copy MinGW prefix DLLs, then prune. The C++ runtime is the
# MSYS2 ucrt64 one; do not overlay the host toolchain's copies.

install(TARGETS dn libdn dnthumbd
	RUNTIME DESTINATION .)

install(DIRECTORY "${DN_MINGW_PREFIX}/bin/"
	DESTINATION .
	FILES_MATCHING
		PATTERN "*.dll"
		PATTERN "vulkan-1.dll" EXCLUDE)

if(NOT EXISTS "${DN_MINGW_PREFIX}/bin/wperl.exe"
		OR NOT EXISTS "${DN_MINGW_PREFIX}/bin/exiftool"
		OR NOT EXISTS "${DN_MINGW_PREFIX}/lib/perl5")
	message(FATAL_ERROR
		"Bundled Perl or ExifTool missing at ${DN_MINGW_PREFIX}\n"
		"Run: sh cmake/Win64Depends.sh ${CMAKE_BINARY_DIR}")
endif()
install(FILES
	"${DN_MINGW_PREFIX}/bin/wperl.exe"
	"${DN_MINGW_PREFIX}/bin/exiftool"
	DESTINATION .)
install(DIRECTORY "${DN_MINGW_PREFIX}/lib/perl5/"
	DESTINATION lib/perl5)

# QPA is dlopend; the cleanup walker will not see it from the exe.
if(TARGET Qt6::QWindowsIntegrationPlugin)
	install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QWindowsIntegrationPlugin
		RUNTIME DESTINATION platforms
		LIBRARY DESTINATION platforms)
else()
	set(_dn_qwindows
		"${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/platforms/qwindows.dll")
	if(NOT EXISTS "${_dn_qwindows}")
		message(FATAL_ERROR "qwindows.dll not found at ${_dn_qwindows}")
	endif()
	install(FILES "${_dn_qwindows}"
		DESTINATION platforms)
	unset(_dn_qwindows)
endif()

# Not a DLL; dump above does not copy it. SwiftShader + MSVC CRT come from
# ucrt64/bin via Win64Depends.sh and the directory install.
if(NOT EXISTS "${DN_MINGW_PREFIX}/bin/vk_swiftshader_icd.json")
	message(FATAL_ERROR
		"vk_swiftshader_icd.json missing at ${DN_MINGW_PREFIX}/bin\n"
		"Run: sh cmake/Win64Depends.sh ${CMAKE_BINARY_DIR}")
endif()
install(FILES "${DN_MINGW_PREFIX}/bin/vk_swiftshader_icd.json"
	DESTINATION .)

if(NOT EXISTS "${DN_MINGW_PREFIX}/share/mime/globs2"
		OR NOT EXISTS "${DN_MINGW_PREFIX}/share/mime/subclasses")
	message(FATAL_ERROR
		"share/mime/globs2 or subclasses missing at ${DN_MINGW_PREFIX}\n"
		"Run: sh cmake/Win64Depends.sh ${CMAKE_BINARY_DIR}")
endif()
install(FILES
	"${DN_MINGW_PREFIX}/share/mime/globs2"
	"${DN_MINGW_PREFIX}/share/mime/subclasses"
	DESTINATION share/mime)

install(SCRIPT "${CMAKE_SOURCE_DIR}/cmake/Win32Cleanup.cmake")

set(CPACK_PACKAGE_VENDOR "dawn")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)
