# macOS app bundle: Qt Frameworks/PlugIns + fiv-like Resources/share.
# MACOSX_BUNDLE is set on dn next to qt_add_executable.

set(_dn_bundle "$<TARGET_BUNDLE_DIR_NAME:dn>")
install(TARGETS dn BUNDLE DESTINATION .)
#install(TARGETS dnthumbd RUNTIME DESTINATION "${_dn_bundle}/Contents/MacOS")
install(TARGETS libdn LIBRARY DESTINATION "${_dn_bundle}/Contents/Frameworks")

set(_dn_mime_dir)
pkg_get_variable(_dn_smi_datadir shared-mime-info datadir)
if(_dn_smi_datadir AND EXISTS "${_dn_smi_datadir}/mime/globs2"
		AND EXISTS "${_dn_smi_datadir}/mime/subclasses")
	set(_dn_mime_dir "${_dn_smi_datadir}/mime")
endif()
if(NOT _dn_mime_dir)
	foreach(_dn_root IN ITEMS "$ENV{HOMEBREW_PREFIX}" /opt/homebrew /usr/local)
		if(_dn_root AND EXISTS "${_dn_root}/share/mime/globs2"
				AND EXISTS "${_dn_root}/share/mime/subclasses")
			set(_dn_mime_dir "${_dn_root}/share/mime")
			break()
		endif()
	endforeach()
endif()
install(FILES
	"${_dn_mime_dir}/globs2"
	"${_dn_mime_dir}/subclasses"
	DESTINATION "${_dn_bundle}/Contents/Resources/share/mime")

# macdeployqt otherwise copies imageformats/virtualkeyboard/styles and
# pulls QtSvg, QtPdf, QtQuick, QtWidgets, QtNetwork with them.
install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QCocoaIntegrationPlugin
	LIBRARY DESTINATION "${_dn_bundle}/Contents/PlugIns/platforms"
	RUNTIME DESTINATION "${_dn_bundle}/Contents/PlugIns/platforms")

# MoltenVK is an ICD; macdeployqt only copies the linked loader.
# Homebrew lib/libMoltenVK.dylib is a Cellar symlink; install(FILES) would
# copy the link and the relocated bundle would not find MoltenVK.
find_library(DN_MOLTENVK_LIBRARY NAMES MoltenVK REQUIRED
	HINTS "$ENV{HOMEBREW_PREFIX}/lib" /opt/homebrew/lib /usr/local/lib)
get_filename_component(DN_MOLTENVK_LIBRARY "${DN_MOLTENVK_LIBRARY}" REALPATH)
install(FILES "${DN_MOLTENVK_LIBRARY}"
	DESTINATION "${_dn_bundle}/Contents/Frameworks")
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
	DESTINATION "${_dn_bundle}/Contents/Resources/vulkan/icd.d")
unset(_dn_bundle)

qt_generate_deploy_app_script(TARGET dn OUTPUT_SCRIPT _dn_deploy_dn
	NO_PLUGINS)
install(SCRIPT "${_dn_deploy_dn}")
# Homebrew Qt QTBUG-127075: run macdeployqt twice.
install(SCRIPT "${_dn_deploy_dn}")
