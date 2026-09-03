//
// config-unix.cpp: XDG configuration file backend
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std;
namespace fs = filesystem;

namespace dawn
{
namespace detail
{

string
trim(string_view value)
{
	const size_t first = value.find_first_not_of(" \t");
	if (first == string_view::npos)
		return {};
	const size_t last = value.find_last_not_of(" \t");
	return string(value.substr(first, last - first + 1));
}

string
desktop_unescape(string_view value)
{
	string out;
	out.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] != '\\' || i + 1 == value.size()) {
			out += value[i];
			continue;
		}
		switch (value[++i]) {
		case 'n': out += '\n'; break;
		case 'r': out += '\r'; break;
		case 't': out += '\t'; break;
		case 's': out += ' '; break;
		default: out += value[i]; break;
		}
	}
	return out;
}

string
desktop_escape(string_view value)
{
	string out;
	out.reserve(value.size());
	for (const char c : value) {
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out += c; break;
		}
	}
	return out;
}

IniFile
ini_parse(string_view text)
{
	IniFile ini;
	IniGroup *group = nullptr;
	for (size_t offset = 0; offset <= text.size();) {
		const size_t end = text.find_first_of("\r\n", offset);
		string line(text.substr(offset,
			end == string::npos ? string::npos : end - offset));
		const string stripped = trim(line);
		if (stripped.empty() || stripped[0] == '#') {
			if (!group)
				ini.preamble.push_back(line);
		} else if (stripped.size() >= 2 && stripped.front() == '[' &&
			stripped.back() == ']' && stripped.find('=') == string::npos) {
			ini.groups.push_back(
				{stripped.substr(1, stripped.size() - 2), {}});
			group = &ini.groups.back();
		} else if (group) {
			const size_t equals = line.find('=');
			if (equals != string::npos)
				group->keys.emplace_back(
					trim(string_view(line).substr(0, equals)),
					string(string_view(line).substr(equals + 1)));
		}
		if (end == string::npos)
			break;
		offset = end +
			(text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n'
				? 2
				: 1);
	}
	return ini;
}

string
ini_serialize(const IniFile &ini)
{
	string out;
	for (const string &line : ini.preamble)
		out += line + '\n';
	for (const IniGroup &group : ini.groups) {
		out += '[' + group.name + "]\n";
		for (const auto &[key, value] : group.keys)
			out += key + '=' + value + '\n';
	}
	return out;
}

string
ini_get(const IniGroup &group, string_view key)
{
	for (const auto &[name, value] : group.keys)
		if (name == key)
			return value;
	return {};
}

void
ini_set(IniGroup &group, string_view key, string_view value)
{
	for (auto &[name, stored] : group.keys)
		if (name == key) {
			stored = value;
			return;
		}
	group.keys.emplace_back(key, value);
}

}  // namespace detail

using detail::IniFile;
using detail::IniGroup;

static void
fail(Error *error, string message)
{
	if (error)
		*error = {Error::Code::Io, std::move(message)};
}

static optional<pair<string, string>>
split_key(string_view key)
{
	const size_t slash = key.rfind('/');
	if (slash == string_view::npos || slash == 0 || slash + 1 == key.size())
		return nullopt;
	return pair<string, string>{string(key.substr(0, slash)),
		string(key.substr(slash + 1))};
}

static fs::path
config_path(Error *error)
{
	if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
		fs::path base(xdg);
		if (base.is_absolute())
			return base / DAWN_NAMESPACE / "dawn.conf";
	}
	if (const char *home = getenv("HOME"); home && *home)
		return fs::path(home) / ".config" / DAWN_NAMESPACE / "dawn.conf";
	fail(error, "cannot locate the user configuration directory");
	return {};
}

static optional<IniFile>
load_ini(const fs::path &path, Error *error)
{
	error_code ec;
	if (!fs::exists(path, ec)) {
		if (ec)
			fail(error, "cannot inspect configuration file: " + ec.message());
		else
			return IniFile{};
		return nullopt;
	}
	ifstream input(path, ios::binary);
	if (!input) {
		fail(error, "cannot open configuration file");
		return nullopt;
	}
	string text((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
	if (input.bad()) {
		fail(error, "cannot read configuration file");
		return nullopt;
	}
	return detail::ini_parse(text);
}

optional<string>
config_get(string_view key, Error *error)
{
	if (error)
		*error = {};
	const auto parts = split_key(key);
	if (!parts) {
		fail(error, "invalid configuration key");
		return nullopt;
	}
	const fs::path path = config_path(error);
	if (path.empty())
		return nullopt;
	const optional<IniFile> ini = load_ini(path, error);
	if (!ini)
		return nullopt;
	for (const IniGroup &group : ini->groups) {
		if (group.name != parts->first)
			continue;
		for (const auto &[name, value] : group.keys)
			if (name == parts->second)
				return detail::desktop_unescape(value);
	}
	return nullopt;
}

bool
config_set(string_view key, string_view value, Error *error)
{
	if (error)
		*error = {};
	const auto parts = split_key(key);
	if (!parts) {
		fail(error, "invalid configuration key");
		return false;
	}
	const fs::path path = config_path(error);
	if (path.empty())
		return false;
	optional<IniFile> ini = load_ini(path, error);
	if (!ini)
		return false;
	IniGroup *wanted = nullptr;
	for (IniGroup &group : ini->groups)
		if (group.name == parts->first) {
			wanted = &group;
			break;
		}
	if (!wanted) {
		ini->groups.push_back({parts->first, {}});
		wanted = &ini->groups.back();
	}
	detail::ini_set(*wanted, parts->second, detail::desktop_escape(value));

	error_code ec;
	fs::create_directories(path.parent_path(), ec);
	if (ec) {
		fail(error, "cannot create configuration directory: " + ec.message());
		return false;
	}
	const fs::path temporary = path.string() + ".new";
	const string data = detail::ini_serialize(*ini);
	{
		ofstream output(temporary, ios::binary | ios::trunc);
		if (!output || !output.write(data.data(), streamsize(data.size())) ||
			!output.flush()) {
			fail(error, "cannot write configuration file");
			return false;
		}
	}
	fs::rename(temporary, path, ec);
	if (ec) {
		fail(error, "cannot replace configuration file: " + ec.message());
		return false;
	}
	return true;
}

}  // namespace dawn
