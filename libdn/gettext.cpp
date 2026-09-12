//
// gettext.cpp: message catalogue startup
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>
#include <dawn-gettext.h>

#include <clocale>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

using namespace std;

namespace dawn
{

#ifdef _WIN32

// Windows packages and macOS bundles are relocatable, so their catalogues can
// only be found relative to the executable, the way the user guide already is.
// The wide path matters here: bindtextdomain() would have to squeeze an
// installation directory through the ANSI codepage first.
static wstring
package_locale_dir()
{
	wchar_t buf[MAX_PATH] = {};
	const DWORD len = GetModuleFileNameW(nullptr, buf, DWORD(size(buf)));
	if (!len || len == size(buf))
		return {};

	wstring path(buf, len);
	const size_t slash = path.find_last_of(L"\\/");
	if (slash == wstring::npos)
		return {};
	path.resize(slash + 1);
	return path + L"share\\locale";
}

#else

static string
executable_path()
{
	char buf[PATH_MAX] = {};
#ifdef __APPLE__
	uint32_t size = sizeof buf;
	if (_NSGetExecutablePath(buf, &size))
		return {};
	return buf;
#else
	// Other Unices need their own means of asking, and go without.
	const ssize_t len = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (len < 0)
		return {};
	return string(buf, size_t(len));
#endif
}

// The build tree keeps catalogues beside the binaries, as it does the
// user guide.
static string
package_locale_dir()
{
	string path = executable_path();
	const size_t slash = path.find_last_of('/');
	if (slash == string::npos)
		return {};
	path.resize(slash + 1);
#ifdef __APPLE__
	return path + "../Resources/share/locale";
#else
	return path + "../share/locale";
#endif
}

static string
locale_dir()
{
#ifdef __APPLE__
	// The bundle is one relocatable thing, installed or not.
	return package_locale_dir();
#else
	// A Unix installation puts its catalogues wherever the packager said,
	// which is what DAWN_LOCALEDIR records; asking the executable instead
	// would ignore a CMAKE_INSTALL_LOCALEDIR pointing anywhere else.  Only
	// an uninstalled build tree has no such directory to find.
	error_code ec;
	if (filesystem::is_directory(DAWN_LOCALEDIR, ec))
		return DAWN_LOCALEDIR;
	return package_locale_dir();
#endif
}

#endif

void
gettext_init()
{
	// GNU libintl derives the language from the environment on Unix, and from
	// the user's interface preferences on macOS and Windows.  Both are what
	// the user asked for, including a deliberate LC_ALL=C.
	setlocale(LC_ALL, "");

	// Configuration files, IPC and shader sources are all written and parsed
	// C-style; only what the user reads follows the locale.
	setlocale(LC_NUMERIC, "C");

#ifdef _WIN32
	const wstring dir = package_locale_dir();
	if (dir.empty())
		bindtextdomain(DAWN_TEXTDOMAIN, DAWN_LOCALEDIR);
	else
		wbindtextdomain(DAWN_TEXTDOMAIN, dir.c_str());
#else
	const string dir = locale_dir();
	bindtextdomain(DAWN_TEXTDOMAIN, dir.empty() ? DAWN_LOCALEDIR : dir.c_str());
#endif
	bind_textdomain_codeset(DAWN_TEXTDOMAIN, "UTF-8");
}

string
format_message(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	va_list measure;
	va_copy(measure, args);
	const int len = vsnprintf(nullptr, 0, format, measure);
	va_end(measure);

	string message;
	if (len > 0) {
		message.resize(size_t(len));
		vsnprintf(message.data(), size_t(len) + 1, format, args);
	}
	va_end(args);
	return message;
}

}  // namespace dawn
