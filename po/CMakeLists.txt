# Dawn keeps one message catalogue for the application and the library alike.
# Extraction and compilation run on the host, which is what a Windows cross
# build has; only libintl itself comes from the target.
find_program(XGETTEXT_EXECUTABLE xgettext)
find_program(MSGMERGE_EXECUTABLE msgmerge)
find_program(MSGFMT_EXECUTABLE msgfmt)

# Both lists are read at configure time, so editing one has to re-run it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/LINGUAS"
	"${CMAKE_CURRENT_SOURCE_DIR}/POTFILES")

set(pot "${CMAKE_CURRENT_SOURCE_DIR}/dawn.pot")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/LINGUAS" linguas REGEX "^[^#]")
list(JOIN linguas " " linguas)
separate_arguments(linguas UNIX_COMMAND "${linguas}")
# POTFILES names every platform, so that a Linux machine can still extract
# what only macOS or Windows compiles.  xgettext has no scanner for .mm,
# and its C++ one reads Objective-C++ well enough for string literals.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/POTFILES" potfiles REGEX "^[^#]")

# Desktop entries are a language of their own to xgettext, and their keys
# say what to extract; a source name is never one of them.
set(sources ${potfiles})
list(FILTER sources EXCLUDE REGEX "[.]desktop$")
set(desktops ${potfiles})
list(FILTER desktops INCLUDE REGEX "[.]desktop$")
list(JOIN sources "\n" source_list)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/POTFILES.sources" "${source_list}\n")

# Catalogues live where the rest of the package data does, so that the build
# tree, the bundle and the installation all resolve them the same way.
set(DAWN_LOCALE_DIR "${PROJECT_BINARY_DIR}/share/locale" PARENT_SCOPE)
set(locale_dir "${PROJECT_BINARY_DIR}/share/locale")
file(MAKE_DIRECTORY "${locale_dir}")

if (XGETTEXT_EXECUTABLE AND MSGMERGE_EXECUTABLE)
	set(merge_commands)
	foreach (lang IN LISTS linguas)
		list(APPEND merge_commands COMMAND "${MSGMERGE_EXECUTABLE}"
			--quiet --update --backup=none "${lang}.po" "${pot}")
	endforeach()

	# Both runs write the same header, the second one over the first.
	set(about
		--sort-by-file
		"--package-name=${PROJECT_NAME}"
		"--package-version=${PROJECT_VERSION}"
		"--copyright-holder=${CPACK_PACKAGE_VENDOR}"
		"--msgid-bugs-address=${PROJECT_HOMEPAGE_URL}"
		"--directory=${PROJECT_SOURCE_DIR}"
		"--output=${pot}")

	add_custom_target(update-po
		COMMAND "${XGETTEXT_EXECUTABLE}"
			--language=C++ --from-code=UTF-8 --qt
			--keyword=_ --keyword=N_ --keyword=C_:1c,2 --keyword=P_:1,2
			--add-comments=TRANSLATORS ${about}
			"--files-from=${CMAKE_CURRENT_BINARY_DIR}/POTFILES.sources"
		COMMAND "${XGETTEXT_EXECUTABLE}"
			--language=Desktop --join-existing
			--keyword --keyword=GenericName --keyword=X-GNOME-FullName
			${about} ${desktops}
		${merge_commands}
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		COMMENT "Extracting messages and updating translations"
		VERBATIM)
endif()

# Ordinary builds only compile what translators have already committed.
set(catalogues)
foreach (lang IN LISTS linguas)
	set(mo "${locale_dir}/${lang}/LC_MESSAGES/dawn.mo")
	if (NOT MSGFMT_EXECUTABLE)
		message(FATAL_ERROR "msgfmt is needed to compile ${lang}.po")
	endif()
	add_custom_command(OUTPUT "${mo}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${locale_dir}/${lang}/LC_MESSAGES"
		COMMAND "${MSGFMT_EXECUTABLE}" --check
			-o "${mo}" "${CMAKE_CURRENT_SOURCE_DIR}/${lang}.po"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${lang}.po"
		COMMENT "Compiling ${lang}.po"
		VERBATIM)
	list(APPEND catalogues "${mo}")
endforeach()
set(DAWN_CATALOGUES ${catalogues} PARENT_SCOPE)

# Desktop entries take their translations from the PO files directly.
set(translations ${linguas})
list(TRANSFORM translations PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM translations APPEND ".po")
set(DAWN_TRANSLATIONS ${translations} PARENT_SCOPE)

add_custom_target(catalogues ALL DEPENDS ${catalogues})

# The macOS bundle carries its own copy, put there by the dn target.
if (WIN32)
	install(DIRECTORY "${locale_dir}/" DESTINATION share/locale)
elseif (NOT APPLE)
	install(DIRECTORY "${locale_dir}/"
		DESTINATION "${CMAKE_INSTALL_LOCALEDIR}")
endif()
