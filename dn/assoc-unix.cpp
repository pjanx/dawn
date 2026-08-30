//
// assoc-unix.cpp: XDG MIME Applications Open With (filename types, no GIO)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "assoc.hpp"

#include "xdg.hpp"

#include <libdn.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProcess>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

// FIXME: Duplicated in main.cpp.
// It should actually be derived from the active QGuiApplication.
constexpr auto kSelfDesktop = QLatin1String("dn.desktop");

string
read_text_file(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};

	return file.readAll().toStdString();
}

bool
write_text_file(const QString &path, string_view text)
{
	QFileInfo info(path);
	if (!QDir().mkpath(info.absolutePath()))
		return false;

	// One file carries the user's associations for every application,
	// so it must never be left truncated.
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	if (file.write(text.data(), qsizetype(text.size())) !=
		qsizetype(text.size()))
		return false;
	return file.commit();
}

bool
parse_bool(string_view value)
{
	const QString v = QString::fromUtf8(value.data(), qsizetype(value.size()))
						  .trimmed()
						  .toLower();
	return v == QLatin1String("true") || v == QLatin1String("1");
}

vector<QString>
split_semicolons(string_view value)
{
	vector<QString> out;
	const QString decoded =
		QString::fromUtf8(value.data(), qsizetype(value.size()));
	for (const QString &part : decoded.split(u';', Qt::SkipEmptyParts)) {
		const QString item = part.trimmed();
		if (!item.isEmpty())
			out.push_back(item);
	}
	return out;
}

QString
normalize_desktop_id(QString id)
{
	id = id.trimmed();
	if (id.isEmpty())
		return {};

	if (!id.endsWith(QLatin1String(".desktop")))
		id += QLatin1String(".desktop");
	return id;
}

vector<QString>
current_desktops()
{
	vector<QString> out;
	const QString env = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
	for (const QString &part : env.split(u':', Qt::SkipEmptyParts)) {
		const QString desk = part.trimmed();
		if (!desk.isEmpty())
			out.push_back(desk);
	}
	return out;
}

vector<QString>
locale_candidates()
{
	vector<QString> raw;
	const QString language = qEnvironmentVariable("LANGUAGE");
	if (!language.isEmpty()) {
		for (const QString &part : language.split(u':', Qt::SkipEmptyParts))
			raw.push_back(part);
	}

	raw.push_back(qEnvironmentVariable("LC_ALL"));
	raw.push_back(qEnvironmentVariable("LC_MESSAGES"));
	raw.push_back(qEnvironmentVariable("LANG"));

	vector<QString> out;
	auto add = [&](const QString &s) {
		if (!s.isEmpty() && find(out.begin(), out.end(), s) == out.end())
			out.push_back(s);
	};
	for (QString loc : raw) {
		loc = loc.trimmed();
		if (loc.isEmpty() || loc == QLatin1String("C") ||
			loc == QLatin1String("POSIX"))
			continue;
		const int dot = loc.indexOf(u'.');
		if (dot >= 0)
			loc = loc.left(dot);
		loc.replace(u'-', u'_');
		QString modifier;
		const int at = loc.indexOf(u'@');
		if (at >= 0) {
			modifier = loc.mid(at);
			loc = loc.left(at);
		}
		QString lang = loc;
		QString country;
		const int us = loc.indexOf(u'_');
		if (us >= 0) {
			country = loc.mid(us);
			lang = loc.left(us);
		}
		if (!country.isEmpty() && !modifier.isEmpty())
			add(lang + country + modifier);
		if (!country.isEmpty())
			add(lang + country);
		if (!modifier.isEmpty())
			add(lang + modifier);
		add(lang);
	}
	return out;
}

using IniGroup = dawn::detail::IniGroup;
using IniFile = dawn::detail::IniFile;

vector<QString>
mimeapps_list_paths()
{
	vector<QString> paths;
	const vector<QString> desktops = current_desktops();
	auto add_dir = [&](const QString &dir, bool applications) {
		const QString base = applications
			? QDir(dir).filePath(QStringLiteral("applications"))
			: dir;
		for (const QString &desk : desktops) {
			paths.push_back(QDir(base).filePath(
				desk.toLower() + QStringLiteral("-mimeapps.list")));
		}
		paths.push_back(QDir(base).filePath(QStringLiteral("mimeapps.list")));
	};
	for (const QString &dir : xdg_config_dirs())
		add_dir(dir, false);
	for (const QString &dir : xdg_data_dirs())
		add_dir(dir, true);
	return paths;
}

struct AssocSets {
	vector<QString> defaults;
	vector<QString> added;
	unordered_set<QString> removed;
};

void
append_unique(vector<QString> &list, const QString &id)
{
	if (id.isEmpty())
		return;
	if (find(list.begin(), list.end(), id) != list.end())
		return;
	list.push_back(id);
}

void
apply_mimeapps(AssocSets &acc, const IniFile &ini, const QString &type)
{
	const string type_utf8 = type.toUtf8().toStdString();
	for (const IniGroup &group : ini.groups) {
		if (group.name == "Default Applications") {
			for (const QString &id : split_semicolons(
					 dawn::detail::ini_get(group, type_utf8)))
				append_unique(acc.defaults, normalize_desktop_id(id));
		} else if (group.name == "Added Associations") {
			for (const QString &id : split_semicolons(
					 dawn::detail::ini_get(group, type_utf8))) {
				const QString nid = normalize_desktop_id(id);
				if (!acc.removed.contains(nid))
					append_unique(acc.added, nid);
			}
		} else if (group.name == "Removed Associations") {
			for (const QString &id : split_semicolons(
					 dawn::detail::ini_get(group, type_utf8))) {
				const QString nid = normalize_desktop_id(id);
				if (find(acc.added.begin(), acc.added.end(), nid) ==
					acc.added.end())
					acc.removed.insert(nid);
			}
		}
	}
}

AssocSets
associations_for_type(const QString &type)
{
	AssocSets acc;
	for (const QString &path : mimeapps_list_paths()) {
		if (!QFileInfo::exists(path))
			continue;
		apply_mimeapps(
			acc, dawn::detail::ini_parse(read_text_file(path)), type);
	}
	return acc;
}

vector<QString>
cache_ids_for_type(const QString &type)
{
	vector<QString> ids;
	for (const QString &dir : xdg_data_dirs()) {
		const QString path =
			QDir(dir).filePath(QStringLiteral("applications/mimeinfo.cache"));
		if (!QFileInfo::exists(path))
			continue;

		const IniFile ini = dawn::detail::ini_parse(read_text_file(path));
		const string type_utf8 = type.toUtf8().toStdString();
		for (const IniGroup &group : ini.groups) {
			if (group.name != "MIME Cache")
				continue;
			for (const QString &id : split_semicolons(
					 dawn::detail::ini_get(group, type_utf8)))
				append_unique(ids, normalize_desktop_id(id));
		}
	}
	return ids;
}

QString
desktop_path_for_id(const QString &id)
{
	vector<QString> names;
	names.push_back(id);
	QString alt = id;
	for (int i = 0; i < alt.size(); ++i) {
		if (alt[i] == u'-') {
			alt[i] = u'/';
			names.push_back(alt);
		}
	}
	for (const QString &dir : xdg_data_dirs()) {
		for (const QString &name : names) {
			const QString path =
				QDir(dir).filePath(QStringLiteral("applications/") + name);
			if (QFileInfo::exists(path))
				return path;
		}
	}
	return {};
}

struct Desktop {
	QString id;
	QString path;
	QString name;
	QString icon;
	QString exec;
	QString try_exec;
	vector<QString> only_show_in;
	vector<QString> not_show_in;
	bool hidden = false;
	bool application = true;
};

QString
localized_value(const IniGroup &entry, const QString &key)
{
	const QString prefix = key + QLatin1String("[");
	unordered_map<QString, QString> localized;
	QString fallback;
	for (const auto &kv : entry.keys) {
		const QString item_key = QString::fromStdString(kv.first);
		if (item_key == key) {
			if (fallback.isEmpty())
				fallback = QString::fromStdString(
					dawn::detail::desktop_unescape(kv.second));
			continue;
		}
		if (!item_key.startsWith(prefix) || !item_key.endsWith(u']'))
			continue;
		const QString loc = item_key.mid(
			prefix.size(), item_key.size() - prefix.size() - 1);
		localized.insert({loc, QString::fromStdString(
								 dawn::detail::desktop_unescape(kv.second))});
	}
	for (const QString &loc : locale_candidates()) {
		const auto it = localized.find(loc);
		if (it != localized.end())
			return it->second;
	}
	return fallback;
}

QString
localized_name(const IniGroup &entry)
{
	// GLib's display name, which users see next to ours in other file
	// managers, prefers this GNOME extension: "Mozilla Firefox" over
	// "Firefox". Unlike GLib, don't let an empty value blank the name out.
	const QString full =
		localized_value(entry, QStringLiteral("X-GNOME-FullName"));
	if (!full.isEmpty())
		return full;
	return localized_value(entry, QStringLiteral("Name"));
}

bool
try_exec_ok(const QString &try_exec)
{
	if (try_exec.isEmpty())
		return true;

	const QFileInfo info(try_exec);
	if (info.isAbsolute())
		return info.isFile() && info.isExecutable();

	const QString path = qEnvironmentVariable("PATH");
	for (const QString &dir : path.split(u':', Qt::SkipEmptyParts)) {
		const QFileInfo cand(QDir(dir).filePath(try_exec));
		if (cand.isFile() && cand.isExecutable())
			return true;
	}
	return false;
}

bool
shown_on_desktop(const Desktop &d)
{
	const vector<QString> desks = current_desktops();
	if (!d.only_show_in.empty()) {
		bool ok = false;
		for (const QString &desk : desks) {
			if (find(d.only_show_in.begin(), d.only_show_in.end(), desk) !=
				d.only_show_in.end()) {
				ok = true;
				break;
			}
		}
		if (!ok)
			return false;
	}
	for (const QString &desk : desks) {
		if (find(d.not_show_in.begin(), d.not_show_in.end(), desk) !=
			d.not_show_in.end())
			return false;
	}
	return true;
}

Desktop
load_desktop(const QString &id)
{
	Desktop d;
	d.id = id;
	d.path = desktop_path_for_id(id);
	if (d.path.isEmpty())
		return d;

	const IniFile ini = dawn::detail::ini_parse(read_text_file(d.path));
	const IniGroup *entry = nullptr;
	for (const IniGroup &group : ini.groups) {
		if (group.name == "Desktop Entry") {
			entry = &group;
			break;
		}
	}
	if (!entry)
		return d;

	const QString type = QString::fromStdString(
		dawn::detail::ini_get(*entry, "Type"))
						 .trimmed();
	d.application = type.isEmpty() || type == QLatin1String("Application");
	d.name = localized_name(*entry);
	d.icon = QString::fromStdString(dawn::detail::desktop_unescape(
		dawn::detail::ini_get(*entry, "Icon")));
	d.exec = QString::fromStdString(dawn::detail::desktop_unescape(
		dawn::detail::ini_get(*entry, "Exec")));
	d.try_exec = QString::fromStdString(dawn::detail::desktop_unescape(
		dawn::detail::ini_get(*entry, "TryExec")));
	d.hidden = parse_bool(dawn::detail::ini_get(*entry, "Hidden"));
	d.only_show_in =
		split_semicolons(dawn::detail::ini_get(*entry, "OnlyShowIn"));
	d.not_show_in =
		split_semicolons(dawn::detail::ini_get(*entry, "NotShowIn"));
	return d;
}

const Desktop *
desktop_by_id(const QString &id)
{
	static unordered_map<QString, Desktop> cache;
	if (id.isEmpty())
		return nullptr;
	auto it = cache.find(id);
	if (it == cache.end())
		it = cache.insert({id, load_desktop(id)}).first;
	if (it->second.path.isEmpty() || !it->second.application)
		return nullptr;
	return &it->second;
}

bool
listable(const Desktop &d)
{
	if (d.id == kSelfDesktop)
		return false;
	if (d.hidden || d.exec.isEmpty() || !shown_on_desktop(d))
		return false;
	if (!try_exec_ok(d.try_exec))
		return false;
	return true;
}

Handler
to_app(const Desktop &d)
{
	Handler a;
	a.id = d.id;
	a.name = d.name.isEmpty() ? d.id : d.name;
	a.icon = d.icon;
	return a;
}

vector<QString>
filename_types(const QString &path)
{
	// Content sniff (mime/magic / QMimeDatabase::mimeTypeForFile) is a later
	// follow-up. Directories are inode/directory from stat (GIO
	// get_content_type). Regular files use filename globs only.
	if (QFileInfo(path).isDir())
		return {QStringLiteral("inode/directory")};
	return types_for_filename(path);
}

vector<QString>
ancestor_types(const vector<QString> &types)
{
	vector<QString> ancestors;
	QMimeDatabase mime;
	for (const QString &type : types) {
		const QMimeType mt = mime.mimeTypeForName(type);
		if (!mt.isValid())
			continue;
		for (const QString &a : mt.allAncestors()) {
			if (find(types.begin(), types.end(), a) != types.end())
				continue;
			append_unique(ancestors, a);
		}
	}
	return ancestors;
}

void
merge_assoc(AssocSets &into, const AssocSets &from)
{
	for (const QString &id : from.defaults)
		append_unique(into.defaults, id);
	for (const QString &id : from.added)
		append_unique(into.added, id);
	into.removed.insert(from.removed.begin(), from.removed.end());
	for (const QString &id : into.added)
		into.removed.erase(id);
}

bool
usable_id(const QString &id, const unordered_set<QString> &removed)
{
	if (id.isEmpty() || id == kSelfDesktop || removed.contains(id))
		return false;
	const Desktop *d = desktop_by_id(id);
	return d && listable(*d);
}

vector<QString>
split_exec(const QString &exec)
{
	vector<QString> args;
	QString cur;
	bool in_quote = false;
	for (int i = 0; i < exec.size(); ++i) {
		const QChar c = exec[i];
		if (in_quote) {
			if (c == u'\\' && i + 1 < exec.size()) {
				const QChar n = exec[++i];
				if (n == u's')
					cur += u' ';
				else if (n == u'n')
					cur += u'\n';
				else if (n == u't')
					cur += u'\t';
				else if (n == u'r')
					cur += u'\r';
				else
					cur += n;
			} else if (c == u'"') {
				in_quote = false;
			} else {
				cur += c;
			}
		} else if (c == u'"') {
			in_quote = true;
		} else if (c.isSpace()) {
			if (!cur.isEmpty()) {
				args.push_back(cur);
				cur.clear();
			}
		} else if (c == u'\\' && i + 1 < exec.size()) {
			cur += exec[++i];
		} else {
			cur += c;
		}
	}
	if (!cur.isEmpty())
		args.push_back(cur);
	return args;
}

QStringList
expand_exec(const Desktop &d, const QString &path)
{
	const QString url = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath())
							.toString(QUrl::FullyEncoded);
	const QString abs = QFileInfo(path).absoluteFilePath();
	QStringList out;
	bool saw_file = false;
	for (const QString &arg : split_exec(d.exec)) {
		if (arg == QLatin1String("%i")) {
			if (!d.icon.isEmpty()) {
				out.push_back(QStringLiteral("--icon"));
				out.push_back(d.icon);
			}
			continue;
		}
		QString built;
		for (int i = 0; i < arg.size(); ++i) {
			if (arg[i] != u'%' || i + 1 >= arg.size()) {
				built += arg[i];
				continue;
			}
			const QChar code = arg[++i];
			switch (code.unicode()) {
			case u'f':
			case u'F':
				built += abs;
				saw_file = true;
				break;
			case u'u':
			case u'U':
				built += url;
				saw_file = true;
				break;
			case u'c':
				built += d.name;
				break;
			case u'k':
				built += d.path;
				break;
			case u'%':
				built += u'%';
				break;
			case u'i':
			case u'd':
			case u'D':
			case u'n':
			case u'N':
			case u'v':
			case u'm':
				break;
			default:
				break;
			}
		}
		out.push_back(built);
	}
	if (!saw_file && !abs.isEmpty())
		out.push_back(abs);
	return out;
}

QString
user_mimeapps_path()
{
	const vector<QString> dirs = xdg_config_dirs();
	if (dirs.empty())
		return {};
	return QDir(dirs.front()).filePath(QStringLiteral("mimeapps.list"));
}

QString
prepend_id(const QString &value, const QString &id)
{
	vector<QString> ids;
	append_unique(ids, id);
	for (const QString &existing :
		split_semicolons(value.toUtf8().toStdString()))
		append_unique(ids, normalize_desktop_id(existing));
	QString out;
	for (const QString &item : ids) {
		out += item;
		out += u';';
	}
	return out;
}

}  // namespace

Handler
default_for(const QString &path)
{
	const vector<QString> types = filename_types(path);
	AssocSets acc;
	for (const QString &type : types)
		merge_assoc(acc, associations_for_type(type));
	for (const QString &id : acc.defaults) {
		if (!usable_id(id, acc.removed))
			continue;
		return to_app(*desktop_by_id(id));
	}
	return {};
}

vector<Handler>
recommended_for(const QString &path)
{
	const vector<QString> types = filename_types(path);
	AssocSets acc;
	vector<QString> cache_ids;
	for (const QString &type : types) {
		merge_assoc(acc, associations_for_type(type));
		for (const QString &id : cache_ids_for_type(type))
			append_unique(cache_ids, id);
	}

	const Handler def = default_for(path);
	vector<Handler> out;
	unordered_set<QString> seen;
	if (!def.id.isEmpty())
		seen.insert(def.id);
	auto push = [&](const QString &id) {
		if (seen.contains(id) || !usable_id(id, acc.removed))
			return;
		seen.insert(id);
		out.push_back(to_app(*desktop_by_id(id)));
	};
	for (const QString &id : acc.added)
		push(id);
	for (const QString &id : cache_ids)
		push(id);
	return out;
}

vector<Handler>
fallback_for(const QString &path)
{
	const vector<QString> types = filename_types(path);
	const vector<QString> ancestors = ancestor_types(types);
	AssocSets acc;
	for (const QString &type : types)
		merge_assoc(acc, associations_for_type(type));
	for (const QString &type : ancestors)
		merge_assoc(acc, associations_for_type(type));

	unordered_set<QString> seen;
	const Handler def = default_for(path);
	if (!def.id.isEmpty())
		seen.insert(def.id);
	for (const Handler &a : recommended_for(path))
		seen.insert(a.id);

	vector<Handler> out;
	for (const QString &type : ancestors) {
		for (const QString &id : cache_ids_for_type(type)) {
			if (seen.contains(id) || !usable_id(id, acc.removed))
				continue;
			seen.insert(id);
			out.push_back(to_app(*desktop_by_id(id)));
		}
	}
	return out;
}

bool
launch(const Handler &app, const QString &path)
{
	if (app.id.isEmpty() || path.isEmpty())
		return false;

	const Desktop *d = desktop_by_id(app.id);
	if (!d || d->exec.isEmpty())
		return false;
	const QStringList args = expand_exec(*d, path);
	if (args.isEmpty())
		return false;
	return QProcess::startDetached(args.front(), args.mid(1));
}

void
set_last_used(const Handler &app, const QString &path)
{
	if (app.id.isEmpty() || path.isEmpty())
		return;

	const QString id = normalize_desktop_id(app.id);
	if (id.isEmpty() || id == kSelfDesktop)
		return;
	const vector<QString> types = filename_types(path);
	if (types.empty())
		return;
	const QString dest = user_mimeapps_path();
	if (dest.isEmpty())
		return;

	IniFile ini = dawn::detail::ini_parse(read_text_file(dest));
	auto find_group = [&](string_view name) -> IniGroup * {
		for (IniGroup &group : ini.groups) {
			if (group.name == name)
				return &group;
		}
		return nullptr;
	};
	if (!find_group("Added Associations")) {
		IniGroup group;
		group.name = "Added Associations";
		ini.groups.push_back(std::move(group));
	}
	IniGroup *added = find_group("Added Associations");
	IniGroup *removed = find_group("Removed Associations");
	auto drop_id = [&](IniGroup &group, const QString &type) {
		const string type_utf8 = type.toUtf8().toStdString();
		vector<QString> kept;
		for (const QString &existing :
			split_semicolons(dawn::detail::ini_get(group, type_utf8))) {
			const QString nid = normalize_desktop_id(existing);
			if (nid != id)
				append_unique(kept, nid);
		}
		QString value;
		for (const QString &item : kept) {
			value += item;
			value += u';';
		}
		if (value.isEmpty()) {
			group.keys.erase(remove_if(group.keys.begin(), group.keys.end(),
								 [&](const pair<string, string> &kv) {
									 return kv.first == type_utf8;
								 }),
				group.keys.end());
		} else {
			dawn::detail::ini_set(
				group, type_utf8, value.toUtf8().toStdString());
		}
	};
	for (const QString &type : types) {
		const string type_utf8 = type.toUtf8().toStdString();
		const QString previous = QString::fromStdString(
			dawn::detail::ini_get(*added, type_utf8));
		dawn::detail::ini_set(*added, type_utf8,
			prepend_id(previous, id).toUtf8().toStdString());
		if (removed)
			drop_id(*removed, type);
	}
	write_text_file(dest, dawn::detail::ini_serialize(ini));
}

}  // namespace dn
