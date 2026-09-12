//
// dawn-gettext.h: message translation macros
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <dawn-config.h>

#include <libintl.h>

#include <string>

/// Dawn ships one catalogue for the application and the library alike.
/// Every lookup names it explicitly, so that libdn keeps working inside
/// a host that has bound a text domain of its own.
#define DAWN_TEXTDOMAIN "dawn"

/// Translate a literal.
#define _(s) dgettext(DAWN_TEXTDOMAIN, (s))
/// Mark a literal for extraction, to be translated with _() where it is used.
#define N_(s) (s)
/// Translate a literal that needs disambiguating from an identical one.
#define C_(context, s) dawn_pgettext(context "\004" s, (s))
/// Translate a countable literal.
#define P_(s, p, n) dngettext(DAWN_TEXTDOMAIN, (s), (p), (n))

/// C_() backend.  A miss returns the argument itself, glued context and all.
static inline const char *
dawn_pgettext(const char *msgctxtid, const char *msgid)
{
	const char *translation = dgettext(DAWN_TEXTDOMAIN, msgctxtid);
	return translation == msgctxtid ? msgid : translation;
}

namespace dawn
{

/// Pick the user's language, bind the Dawn catalogue, and have gettext answer
/// in UTF-8.  Executables call this before they build any translated string
/// or start any worker thread.  libdn never touches the process locale on its
/// own, so a host that skips this step simply gets the English originals.
void gettext_init();

/// Build a message out of a translated format string.  A format string is
/// only ever a variable because it has been through gettext, which tells the
/// compiler as much, so the usual checking still applies at the call site.
std::string format_message(const char *format, ...) DAWN_FORMAT(1, 2);

}  // namespace dawn
