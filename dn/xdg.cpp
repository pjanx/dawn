//
// xdg.cpp: shared-mime-info subclasses + globs (no GLib)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "xdg.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

QString
xdg_home_dir(const char *var, const char *default_rel)
{
	const QString env = qEnvironmentVariable(var);
	if (!env.isEmpty() && QDir::isAbsolutePath(env))
		return QDir::cleanPath(env);
	QString home = qEnvironmentVariable("HOME");
	if (home.isEmpty())
		home = QDir::homePath();
	return QDir::cleanPath(QDir(home).filePath(QString::fromUtf8(default_rel)));
}

vector<QString>
split_search_path(const QString &value)
{
	vector<QString> out;
	const QChar sep =
#ifdef Q_OS_WIN
		u';';
#else
		u':';
#endif
	for (const QString &part : value.split(sep, Qt::SkipEmptyParts)) {
		const QString dir = QDir::cleanPath(part.trimmed());
		if (!dir.isEmpty() && QDir::isAbsolutePath(dir))
			out.push_back(dir);
	}
	return out;
}

void
append_unique(vector<QString> &dirs, const QString &dir)
{
	if (dir.isEmpty() || !QDir::isAbsolutePath(dir))
		return;
	const QString clean = QDir::cleanPath(dir);
	for (const QString &existing : dirs) {
		if (existing == clean)
			return;
	}
	dirs.push_back(clean);
}

QString
read_text_file(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	QString text = QString::fromUtf8(file.readAll());
	text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
	text.replace(u'\r', u'\n');
	return text;
}

struct MimeGlob {
	QString type;
	QString glob;
	QRegularExpression glob_re;  ///< glob converted
	int weight = 50;
};

struct MimeDb {
	// superclass → subclasses (is-a)
	unordered_map<QString, unordered_set<QString>> subclasses;
	vector<MimeGlob> globs;
};

void
read_mime_subclasses(const QString &path, MimeDb &db)
{
	const QString text = read_text_file(path);
	if (text.isEmpty() && !QFileInfo::exists(path))
		return;
	for (const QString &raw : text.split(u'\n')) {
		const QString line = raw.trimmed();
		if (line.isEmpty() || line.startsWith(u'#'))
			continue;
		const QStringList parts = line.split(u' ', Qt::SkipEmptyParts);
		if (parts.size() < 2)
			continue;
		const QString &subclass = parts[0];
		const QString &superclass = parts[1];
		if (subclass.startsWith(u'#'))
			continue;
		db.subclasses[superclass].insert(subclass);
	}
}

bool
read_mime_globs(const QString &path, bool is_globs2, MimeDb &db)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;

	QString text = QString::fromUtf8(file.readAll());
	text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
	text.replace(u'\r', u'\n');
	for (const QString &raw : text.split(u'\n')) {
		const QString line = raw.trimmed();
		if (line.isEmpty() || line.startsWith(u'#'))
			continue;

		const QStringList f = line.split(u':');
		const int type_i = is_globs2 ? 1 : 0;
		if (f.size() < type_i + 2)
			continue;

		const QString type = f[type_i];
		const QString glob = f[type_i + 1];
		if (type.isEmpty() || glob.isEmpty() ||
			glob == QLatin1String("__NOGLOBS__"))
			continue;

		// :cs (case-sensitive) is ignored; weight is unused for filtering.
		MimeGlob g;
		g.type = type;
		g.glob = glob.toLower();
		g.glob_re = QRegularExpression::fromWildcard(g.glob, Qt::CaseInsensitive);
		if (is_globs2)
			g.weight = f[0].toInt();
		db.globs.push_back(std::move(g));
	}
	return true;
}

const MimeDb &
mime_db()
{
	static const MimeDb db = [] {
		MimeDb loaded;
		for (const QString &dir : xdg_data_dirs()) {
			read_mime_subclasses(
				QDir(dir).filePath(QStringLiteral("mime/subclasses")), loaded);
		}
		for (const QString &dir : xdg_data_dirs()) {
			const QString path2 =
				QDir(dir).filePath(QStringLiteral("mime/globs2"));
			const QString path1 =
				QDir(dir).filePath(QStringLiteral("mime/globs"));
			if (!read_mime_globs(path2, true, loaded))
				read_mime_globs(path1, false, loaded);
		}
		return loaded;
	}();
	return db;
}

void
add_applying_transitive_closure(const QString &element,
	const unordered_map<QString, unordered_set<QString>> &relation,
	unordered_set<QString> &output)
{
	if (output.contains(element))
		return;
	output.insert(element);
	// TODO(p): Iterate over all aliases of `element` in addition to
	// any direct match (and rename this no-longer-generic function).
	const auto it = relation.find(element);
	if (it == relation.end())
		return;
	for (const QString &sub : it->second)
		add_applying_transitive_closure(sub, relation, output);
}

}  // namespace

vector<QString>
xdg_data_dirs()
{
	vector<QString> dirs;
	append_unique(dirs, xdg_home_dir("XDG_DATA_HOME", ".local/share"));

	// One package-relative datadir ahead of XDG_DATA_DIRS.
#if defined Q_OS_MACOS || defined Q_OS_WIN
	if (QCoreApplication::instance()) {
		const QString app_dir = QCoreApplication::applicationDirPath();
		if (!app_dir.isEmpty()) {
#if defined Q_OS_MACOS
			append_unique(dirs,
				QDir(app_dir).absoluteFilePath(
					QStringLiteral("../Resources/share")));
#else
			append_unique(
				dirs, QDir(app_dir).absoluteFilePath(QStringLiteral("share")));
#endif
		}
	}
#endif

	QString data_dirs = qEnvironmentVariable("XDG_DATA_DIRS");
	if (data_dirs.isEmpty()) {
#ifdef Q_OS_MACOS
		data_dirs =
			QStringLiteral("/opt/homebrew/share:/usr/local/share:/usr/share");
#else
		data_dirs = QStringLiteral("/usr/local/share:/usr/share");
#endif
	}
	for (const QString &dir : split_search_path(data_dirs))
		append_unique(dirs, dir);
	return dirs;
}

vector<QString>
xdg_config_dirs()
{
	vector<QString> dirs;
	append_unique(dirs, xdg_home_dir("XDG_CONFIG_HOME", ".config"));

	QString config_dirs = qEnvironmentVariable("XDG_CONFIG_DIRS");
	if (config_dirs.isEmpty())
		config_dirs = QStringLiteral("/etc/xdg");
	for (const QString &dir : split_search_path(config_dirs))
		append_unique(dirs, dir);
	return dirs;
}

vector<QString>
extract_mime_globs(const vector<string> &media_types)
{
	const MimeDb &db = mime_db();
	unordered_set<QString> supported;
	for (const string &type : media_types)
		add_applying_transitive_closure(
			QString::fromStdString(type), db.subclasses, supported);

	unordered_set<QString> globs;
	for (const MimeGlob &g : db.globs) {
		if (supported.contains(g.type))
			globs.insert(g.glob);
	}
	vector<QString> out;
	out.reserve(globs.size());
	for (const QString &g : globs)
		out.push_back(g);
	return out;
}

vector<QString>
types_for_filename(const QString &path)
{
	if (path.isEmpty())
		return {};
	const QString name = QFileInfo(path).fileName().toLower();
	if (name.isEmpty())
		return {};

	const MimeDb &db = mime_db();
	unordered_map<QString, int> best_weight;
	for (const MimeGlob &g : db.globs) {
		if (!g.glob_re.match(name).hasMatch())
			continue;
		auto it = best_weight.find(g.type);
		if (it == best_weight.end())
			best_weight.insert({g.type, g.weight});
		else if (g.weight > it->second)
			it->second = g.weight;
	}

	vector<QString> out;
	out.reserve(best_weight.size());
	for (const auto &kv : best_weight)
		out.push_back(kv.first);
	sort(out.begin(), out.end(), [&](const QString &a, const QString &b) {
		const int wa = best_weight.at(a);
		const int wb = best_weight.at(b);
		if (wa != wb)
			return wa > wb;
		return a < b;
	});
	return out;
}

}  // namespace dn
