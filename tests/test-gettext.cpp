//
// test-gettext.cpp: message catalogue lookup
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-gettext.h>

#include "libdn/libdn.h"
#include "test.hpp"

#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

using namespace std;

// CTest reads this as "this case had nothing to run against".
constexpr int kSkip = 77;

// gettext disables LANGUAGE under a "C" locale, which "C.UTF-8" counts as.
static bool
is_c_locale(const char *name)
{
	return name[0] == 'C' && (!name[1] || name[1] == '.');
}

// So a real locale has to be found first.  Which one it is does not matter:
// LANGUAGE picks the catalogue, and the codeset binding settles the encoding.
// Both the environment and the C library get told, because glibc reads the
// locale it has while GNU libintl reads the variable.
static bool
adopt_any_locale()
{
	// What the machine is already set up for, first; the rest are names
	// generated often enough that one of them tends to exist.
	const char *current = setlocale(LC_MESSAGES, "");
	for (const char *name : {current, "en_US.UTF-8", "en_US.utf8", "en_US",
			 "en_GB.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8"}) {
		if (!name || !*name || is_c_locale(name) || !setlocale(LC_ALL, name))
			continue;
#ifdef _WIN32
		_putenv_s("LC_ALL", name);
#else
		setenv("LC_ALL", name, 1);
#endif
		return true;
	}
	return false;
}

static void
test_fallback()
{
	CHECK(!strcmp(_("Dawn"), "Dawn"));
	CHECK(!strcmp(C_("verb", "Mirror"), "Mirror"));
	CHECK(!strcmp(P_("%d file", "%d files", 1), "%d file"));
	CHECK(!strcmp(P_("%d file", "%d files", 3), "%d files"));
}

static void
test_translated()
{
	CHECK(!strcmp(_("Dawn"), "Úsvit"));

	// What no translator has got to yet stays readable.
	CHECK(!strcmp(_("Not in the catalogue"), "Not in the catalogue"));
}

static void
test_context()
{
	CHECK(!strcmp(C_("verb", "Mirror"), "zrcadlit"));
	CHECK(!strcmp(C_("noun", "Mirror"), "zrcadlo"));

	// A context only ever narrows: without one there is nothing to find.
	CHECK(!strcmp(_("Mirror"), "Mirror"));
}

static void
test_plural()
{
	CHECK(!strcmp(P_("%d file", "%d files", 1), "%d soubor"));
	CHECK(!strcmp(P_("%d file", "%d files", 3), "%d soubory"));
	CHECK(!strcmp(P_("%d file", "%d files", 7), "%d souborů"));
}

// libdn speaks through the same catalogue, without having chosen it.
static void
test_library_error()
{
	const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef, 0, 1, 2, 3};
	// One loader, so that what is left over is its own diagnostic, and one
	// this build is certain to have.
	const string only[] = {"ICNS"};
	dawn::OpenContext ctx;
	ctx.loaders = only;
	dawn::Error error;
	CHECK(!dawn::open_from_data(garbage, ctx, &error));
	CHECK(error.message == "toto není obrázek ICNS");
}

int
main(int argc, char **argv)
{
	// Language selection is process-wide and settled by the first lookup,
	// so every case runs as its own process, told which one it is.
	const string_view name = argc > 1 ? argv[1] : "";

	// Without a locale there is no catalogue to consult, and only the case
	// that expects none can still say anything.
	if (!adopt_any_locale() && name != "fallback") {
		fprintf(stderr, "%s: this system has no locale but \"C\"\n", argv[0]);
		return kSkip;
	}

	dawn::gettext_init();

	// The package catalogues gettext_init() bound are of no use here.
	bindtextdomain(DAWN_TEXTDOMAIN, DAWN_TEST_LOCALE_DIR);

	for (const test::Case &c : {
			 test::Case{"fallback", test_fallback},
			 test::Case{"translated", test_translated},
			 test::Case{"context", test_context},
			 test::Case{"plural", test_plural},
			 test::Case{"library", test_library_error},
		 }) {
		if (name == c.name)
			return test::run({c});
	}

	fprintf(stderr, "usage: %s {fallback|translated|context|plural|library}\n",
		argv[0]);
	return 1;
}
