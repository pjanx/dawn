//
// kit-files.cpp: the file chooser
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <libdn/gettext.hpp>

#include "kit-files.hpp"
#include "url.hpp"

#include <QDateTime>
#include <QDir>
#include <QDirListing>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace std;

namespace dn
{

// --- Listing -----------------------------------------------------------------

// Compile each filter once for the lifetime of the chooser.
static vector<QRegularExpression>
compile_globs(const char *globs)
{
	vector<QRegularExpression> out;
	if (!globs)
		return out;
	for (const QString &glob :
		QString::fromUtf8(globs).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
		out.push_back(
			QRegularExpression::fromWildcard(glob, Qt::CaseInsensitive));
		out.back().optimize();
	}
	return out;
}

static bool
matches_globs(span<const QRegularExpression> globs, const QString &name)
{
	if (globs.empty())
		return true;
	for (const QRegularExpression &glob : globs) {
		if (glob.match(name).hasMatch())
			return true;
	}
	return false;
}

// Directories are always shown: they are how one navigates, not content.
static vector<FileEntry>
scan(const QString &dir, span<const QRegularExpression> globs, bool hidden)
{
	vector<FileEntry> out;
	for (const auto &ent :
		QDirListing(dir, QDirListing::IteratorFlag::IncludeHidden)) {
		const QFileInfo info = ent.fileInfo();
		const QString name = info.fileName();
		if (!hidden && name.startsWith(QLatin1Char('.')))
			continue;

		FileEntry file;
		file.dir = info.isDir();
		if (!file.dir && (!info.isFile() || !matches_globs(globs, name)))
			continue;
		file.path = info.absoluteFilePath();
		file.name = name;
		file.mtime = info.lastModified().toMSecsSinceEpoch();
		file.modified =
			QLocale().toString(info.lastModified(), QLocale::ShortFormat);
		if (!file.dir) {
			file.bytes = uint64_t(max<qint64>(0, info.size()));
			file.size = QLocale().formattedDataSize(qint64(file.bytes));
		}
		out.push_back(std::move(file));
	}
	return out;
}

// --- The list ----------------------------------------------------------------

constexpr float kMinNameW = 120.f;
constexpr int kVisibleRows = 12;

// Which column the header sorts by; the order they are drawn in.
enum : int { ColName, ColSize, ColModified, ColCount };

// The header and rows share column widths.
struct FileList : Container {
	Button *heads[ColCount] = {};
	FileRows *rows = nullptr;
	int col_w[ColCount] = {};
	function<void(Kit &, const FileEntry &)> on_select;
	function<void(Kit &, const FileEntry &)> on_activate;

	FileList();
	void set_entries(Kit &kit, vector<FileEntry> entries);
	void sort_by(Kit &kit, int column);
	[[nodiscard]] const FileEntry *current(const Kit &kit) const;

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	bool key(Kit &kit, const Key &ev) override;

	int head_h_ = 0;
	int sort_ = ColName;
	bool sort_desc_ = false;

	void sort_rows();
	void sync_heads();
	void step(Kit &kit, int to);
	[[nodiscard]] int focused(const Kit &kit) const;
};

FileRow::FileRow()
{
	this->flat = true;
}

// Never grow: in a column that would split the listing's height between the
// rows instead of giving each of them the font's own.  The width comes from
// the column arranging them, which hands every child its full inner width.
Size
FileRow::measure_content(Kit &kit, int max_w, int)
{
	const int h = kit.px(kFramePadY) * 2 +
		kit.text_height(QStringLiteral("Ag"), 0, false);
	return {max_w < kUnlim ? max_w : 0, max(h, kit.icon_px())};
}

// A directory holds more rows than there is any point in shaping or drawing;
// the scrolled column clips them, and this is what keeps the cost clipped too.
static bool
offscreen(const Widget *w, const Kit &kit)
{
	return !w->shown() ||
		visible_rect(w, {0, 0, kit.host_w_, kit.host_h_}).empty();
}

static TextCache::Text &
file_name_layout(const Kit &kit, const FileRow &row)
{
	const int pad = kit.px(kFramePadX) * 2 + kit.icon_px() + kit.px(4.f);
	return row.text_cache_.get(
		kit, row.text, max(1, row.list->col_w[ColName] - pad), 1, false, false);
}

void
FileRow::prepare(Kit &kit)
{
	if (offscreen(this, kit))
		return;

	const auto &cached = file_name_layout(kit, *this);
	this->elided_ = cached.layout && cached.layout->text() != this->text;
}

QString
FileRow::tip() const
{
	return this->elided_ ? this->text : QString();
}

void
FileRow::paint(Kit &kit) const
{
	if (offscreen(this, kit))
		return;

	if (this->list->rows->selected == this)
		kit.draw_fill(this->r, kit.colours_[ColourPress]);
	else if (kit.hot_ == this)
		kit.draw_fill(this->r, kit.colours_[ColourHover]);

	const int pad = kit.px(kFramePadX);
	const int icon = kit.icon_px();
	const int th = kit.text_height(QStringLiteral("Ag"), 0, false);
	const int ty = this->r.y + (this->r.h - th) / 2;
	Colour fg = kit.colours_[ColourInk];
	fg.a *= kit.ink_alpha();
	kit.draw_icon(this->r.x + pad, this->r.y + (this->r.h - icon) / 2, icon,
		this->icon, fg);

	kit.emit_layout(float(this->r.x + pad + icon + kit.px(4.f)), float(ty),
		file_name_layout(kit, *this), fg, -1);

	// Both are quantities, and read down a right-aligned column.
	int x = this->r.x + this->list->col_w[ColName];
	for (int i = ColSize; i < ColCount; i++) {
		const QString &text =
			i == ColSize ? this->entry.size : this->entry.modified;
		kit.emit_text(
			float(x + this->list->col_w[i] - pad - kit.text_width(text, false)),
			float(ty), text, fg, false);
		x += this->list->col_w[i];
	}

	if (kit.focus_ == this && kit.focus_visible_)
		kit.focus_ring(this->r);
}

// A single click only selects; opening is Return, a double click, or the
// platform's Press -- and all three of those arrive as activate(), exactly
// as they do on a file in the browser.
bool
FileRow::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (!Button::press(kit, x, y, button))
		return false;

	this->list->rows->select(kit, this);
	return true;
}

bool
FileRow::release(Kit &kit, float, float, Qt::MouseButton button)
{
	return button == Qt::LeftButton && kit.pressed_ == this;
}

// Callbacks can replace this row: retain its value and owner first.
bool
FileRow::activate(Kit &kit)
{
	FileList *owner = this->list;
	const FileEntry entry = this->entry;
	kit.set_focus(this, kit.focus_visible_);
	owner->rows->select(kit, this);
	if (owner->on_activate)
		owner->on_activate(kit, entry);
	return true;
}

bool
FileRow::double_click(Kit &kit, float, float, Qt::MouseButton button, unsigned)
{
	return button == Qt::LeftButton && activate(kit);
}

FileList::FileList()
{
	static const char *const kLabels[ColCount] = {
		N_("Name"), N_("Size"), N_("Modified")};
	for (int i = 0; i < ColCount; i++) {
		auto head = make_unique<Button>();
		head->flat = true;
		head->bold = true;
		head->focus_on_press = false;
		head->text = QString::fromUtf8(_(kLabels[i]));
		head->on_click = [this, i](Kit &kit) { sort_by(kit, i); };
		this->heads[i] = head.get();
		add_child(std::move(head), size_t(-1));
	}

	auto rows = make_unique<FileRows>();
	rows->grow = true;
	rows->follow_focus = true;
	rows->list = this;
	this->rows = rows.get();
	add_child(std::move(rows), size_t(-1));
	sync_heads();
}

void
FileList::sync_heads()
{
	for (int i = 0; i < ColCount; i++) {
		this->heads[i]->icon = i != this->sort_ ? nullptr
			: this->sort_desc_ ? "view-sort-descending-symbolic"
							   : "view-sort-ascending-symbolic";
	}
}

void
FileList::set_entries(Kit &kit, vector<FileEntry> entries)
{
	const QString keep =
		this->rows->selected ? this->rows->selected->entry.path : QString();
	const bool had_focus = kit.focus_ && kit.focus_->parent_ == this->rows;
	const QString focused =
		had_focus ? ((FileRow *) kit.focus_)->entry.path : QString();
	this->rows->selected = nullptr;
	this->rows->erase_children(kit, 0);
	for (FileEntry &entry : entries) {
		auto row = make_unique<FileRow>();
		row->list = this;
		row->text = entry.name;
		row->icon = entry.dir ? "folder-symbolic" : nullptr;
		row->entry = std::move(entry);
		if (row->entry.path == keep)
			this->rows->selected = row.get();
		FileRow *raw = row.get();
		this->rows->add_child(std::move(row), size_t(-1));
		if (raw->entry.path == focused)
			kit.reseat_focus(raw);
	}
	sort_rows();
	if (had_focus && !kit.focus_)
		kit.reseat_focus(this->rows->tab_stop());
	this->rows->scroll_.offset = 0;
}

void
FileList::sort_by(Kit &, int column)
{
	if (column == this->sort_)
		this->sort_desc_ = !this->sort_desc_;
	else
		this->sort_ = column;
	sync_heads();
	sort_rows();
}

// Sorting moves ownership, not the widgets or their focus/selection identity.
void
FileList::sort_rows()
{
	sort(this->rows->kids.begin(), this->rows->kids.end(),
		[this](const auto &left, const auto &right) {
			const FileEntry &a = ((const FileRow *) left.get())->entry;
			const FileEntry &b = ((const FileRow *) right.get())->entry;
			if (a.dir != b.dir)
				return a.dir;
			int cmp = 0;
			if (this->sort_ == ColSize && a.bytes != b.bytes)
				cmp = a.bytes < b.bytes ? -1 : 1;
			else if (this->sort_ == ColModified && a.mtime != b.mtime)
				cmp = a.mtime < b.mtime ? -1 : 1;
			if (cmp == 0)
				cmp = a.name.localeAwareCompare(b.name);
			return this->sort_desc_ ? cmp > 0 : cmp < 0;
		});
	this->rows->invalidate_measure();
}

void
FileRows::select(Kit &kit, FileRow *row)
{
	this->selected = row;
	if (row && this->list->on_select) {
		// A path commit may replace this row during the callback.
		const FileEntry entry = row->entry;
		this->list->on_select(kit, entry);
	}
}

Widget *
FileRows::tab_stop()
{
	return this->selected ? this->selected : child(0);
}

int
FileList::focused(const Kit &kit) const
{
	const auto &kids = this->rows->kids;
	const auto it = find_if(kids.begin(), kids.end(),
		[&kit](const auto &row) { return row.get() == kit.focus_; });
	return it == kids.end() ? -1 : int(it - kids.begin());
}

const FileEntry *
FileList::current(const Kit &kit) const
{
	if (const FileRow *row = this->rows->selected)
		return &row->entry;
	const int i = focused(kit);
	return i < 0 ? nullptr : &((FileRow *) this->rows->child(size_t(i)))->entry;
}

void
FileList::step(Kit &kit, int to)
{
	const int n = int(this->rows->kids.size());
	if (!n)
		return;
	auto *row = (FileRow *) this->rows->child(size_t(clamp(to, 0, n - 1)));
	kit.set_focus(row, true);
	this->rows->select(kit, row);
}

bool
FileList::key(Kit &kit, const Key &ev)
{
	if (ev.mods || this->rows->kids.empty())
		return false;

	const int n = int(this->rows->kids.size());
	const int at = focused(kit);
	// A page of rows, not of pixels: the focus is what moves.
	const int page =
		max(1, int(this->rows->r.h / max(1, this->rows->child(0)->r.h)));
	switch (ev.key) {
	case Qt::Key_Up:
		step(kit, at < 0 ? n - 1 : at - 1);
		return true;
	case Qt::Key_Down:
		step(kit, at < 0 ? 0 : at + 1);
		return true;
	case Qt::Key_PageUp:
		step(kit, at < 0 ? 0 : at - page);
		return true;
	case Qt::Key_PageDown:
		step(kit, at < 0 ? n - 1 : at + page);
		return true;
	case Qt::Key_Home:
		step(kit, 0);
		return true;
	case Qt::Key_End:
		step(kit, n - 1);
		return true;
	default:
		return false;
	}
}

Size
FileList::measure_content(Kit &kit, int max_w, int)
{
	this->head_h_ = 0;
	for (Button *head : this->heads)
		this->head_h_ =
			max(this->head_h_, head->measure(kit, kUnlim, kUnlim).h);

	const int row_h = kit.px(kFramePadY) * 2 +
		kit.text_height(QStringLiteral("Ag"), 0, false);
	return {max_w < kUnlim ? max_w : kit.px(kMinNameW * 3.f),
		kit.hairline() * 2 + this->head_h_ +
			max(row_h, kit.icon_px()) * kVisibleRows};
}

void
FileList::arrange_content(Kit &kit, Rect alloc)
{
	this->r = alloc;

	// Everything lives within the border this draws itself, so that the
	// border never lands on the outermost pixel of a row, or of its ring.
	const int hair = kit.hairline();
	const Rect in = alloc.inset(hair, hair);

	// Size and Modified take their widest formatted value; Name gets the
	// rest, minus the strip the scrollbar draws over.
	const int pad = kit.px(kFramePadX) * 2;
	int size_w = this->heads[ColSize]->measure(kit, kUnlim, kUnlim).w;
	int time_w = this->heads[ColModified]->measure(kit, kUnlim, kUnlim).w;
	for (const auto &child : this->rows->kids) {
		const FileEntry &entry = ((const FileRow *) child.get())->entry;
		size_w = max(size_w, kit.text_width(entry.size, false) + pad);
		time_w = max(time_w, kit.text_width(entry.modified, false) + pad);
	}
	const int inner = max(0, in.w - kit.px(kScrollBarW));
	this->col_w[ColSize] = size_w;
	this->col_w[ColModified] = time_w;
	this->col_w[ColName] = max(kit.px(kMinNameW), inner - size_w - time_w);

	int x = in.x;
	for (int i = 0; i < ColCount; i++) {
		this->heads[i]->arrange(kit, {x, in.y, this->col_w[i], this->head_h_});
		x += this->col_w[i];
	}
	this->rows->arrange(
		kit, {in.x, in.y + this->head_h_, in.w, max(0, in.h - this->head_h_)});
}

// The listing is a view onto the filesystem, and reads as one: the same
// ground the browser and the viewer put their content on.  The header stays
// on the dialog's own, which is what tells the two apart.  The border is
// this widget's rather than a panel's around it, because a panel draws its
// stroke after its children and would land on top of the rows.
void
FileList::paint(Kit &kit) const
{
	if (!shown())
		return;

	kit.draw_fill(this->rows->r, kit.colours_[ColourWell]);
	paint_children(kit);
	kit.draw_border(this->r, kit.colours_[ColourDivider], kit.hairline());
}

// --- The dialog --------------------------------------------------------------

namespace
{

// The content owns navigation and acceptance; the list owns presentation.
struct Chooser : Column {
	Dialog *dialog = nullptr;
	FileDialogSetup setup;
	vector<vector<QRegularExpression>> globs;

	Entry *path = nullptr;
	Entry *name = nullptr;
	Combo *type = nullptr;
	FileList *list = nullptr;
	Label *warning = nullptr;
	// Checked by default: dotfiles are noise until they are asked for.
	Button *hide = nullptr;

	// The directory the listing is actually of, as an absolute native path.
	QString dir;

	void warn(const QString &message) const;
	void set_dir(Kit &kit, const QString &next);
	void go_parent(Kit &kit);
	QString make_dir(Kit &kit, const QString &folder);
	bool commit_path(Kit &kit);
	void rescan(Kit &kit);
	void retype(Kit &kit);
	void accept(Kit &kit);
	void accept_path(Kit &kit, const QString &target);
	void write(Kit &kit, const QString &target, int type);
	[[nodiscard]] int type_index() const;
};

}  // namespace

void
Chooser::warn(const QString &message) const
{
	this->warning->set_text(message);
	this->warning->set_visible(!message.isEmpty());
}

int
Chooser::type_index() const
{
	if (!this->type || this->setup.types.empty())
		return 0;
	return clamp(this->type->current, 0, int(this->setup.types.size()) - 1);
}

void
Chooser::rescan(Kit &kit)
{
	span<const QRegularExpression> active;
	if (!this->globs.empty())
		active = this->globs[size_t(type_index())];
	this->list->set_entries(kit, scan(this->dir, active, !this->hide->active));
}

void
Chooser::set_dir(Kit &kit, const QString &next)
{
	const QFileInfo info(next);
	if (next.isEmpty() || !info.isDir()) {
		warn(QString::fromUtf8(_("Not a directory")));
		this->path->set_text(kit, this->dir);
		return;
	}

	warn({});
	this->dir = QDir::cleanPath(info.absoluteFilePath());
	this->path->set_text(kit, this->dir);
	rescan(kit);
}

void
Chooser::go_parent(Kit &kit)
{
	commit_path(kit);
	set_dir(kit, QDir(this->dir).absoluteFilePath(QStringLiteral("..")));
}

// Relative to where we are; what was made is then shown.
QString
Chooser::make_dir(Kit &kit, const QString &folder)
{
	const QString trimmed = folder.trimmed();
	if (trimmed.isEmpty())
		return QString::fromUtf8(_("No name given"));
	if (!QDir(this->dir).mkdir(trimmed))
		return QString::fromUtf8(_("Could not create the directory"));

	rescan(kit);
	return {};
}

// Nothing typed since the last commit needs one; anything else is resolved
// the same way a command-line argument would be.
bool
Chooser::commit_path(Kit &kit)
{
	if (this->path->text == this->dir)
		return false;

	const QString was = this->dir;
	set_dir(kit, url_to_path(url_from_user_input(this->path->text, this->dir)));
	return this->dir != was;
}

// A leading dot names a hidden file; only a later dot starts a suffix.
static QString
save_name(QString name, const char *suffix, bool replace)
{
	name = QDir::fromNativeSeparators(name);
	if (!suffix)
		return name;
	const qsizetype slash = name.lastIndexOf(QLatin1Char('/'));
	const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
	if (dot > slash + 1) {
		if (!replace)
			return name;
		name.truncate(dot);
	}
	return name + QString::fromUtf8(suffix);
}

void
Chooser::retype(Kit &kit)
{
	if (!commit_path(kit))
		rescan(kit);
	if (this->name && !this->name->text.isEmpty())
		this->name->set_text(kit,
			save_name(this->name->text,
				this->setup.types[size_t(type_index())].suffix, true));
}

void
Chooser::write(Kit &kit, const QString &target, int type)
{
	const QString message = this->setup.on_accept
		? this->setup.on_accept(kit, target, type)
		: QString();
	if (message.isEmpty())
		this->dialog->close(kit);
	else
		warn(message);
}

void
Chooser::accept(Kit &kit)
{
	commit_path(kit);

	if (!this->setup.save) {
		const FileEntry *entry = this->list->current(kit);
		if (!entry) {
			warn(QString::fromUtf8(_("No file selected")));
			return;
		}
		// Opening a directory is entering it, wherever it is asked for.
		if (entry->dir)
			set_dir(kit, entry->path);
		else
			accept_path(kit, entry->path);
		return;
	}

	QString file = this->name->text.trimmed();
	if (file.isEmpty()) {
		warn(QString::fromUtf8(_("No filename given")));
		return;
	}

	// The globs only said what to list; this is what the type saves as.
	const char *suffix = this->setup.types.empty()
		? nullptr
		: this->setup.types[size_t(type_index())].suffix;
	file = save_name(file, suffix, false);
	accept_path(kit, QDir(this->dir).absoluteFilePath(file));
}

void
Chooser::accept_path(Kit &kit, const QString &target)
{
	const int type = type_index();
	if (!this->setup.save || !QFileInfo::exists(target)) {
		write(kit, target, type);
		return;
	}

	// The question retains the destination and format it asks about.
	dialog_question(kit,
		QString::fromUtf8(_("%1 already exists. Overwrite it?"))
			.arg(QFileInfo(target).fileName()),
		N_("_Overwrite"),
		[this, target, type](Kit &inner) { write(inner, target, type); });
}

static unique_ptr<Button>
chooser_tool(const char *icon, const char *tip, function<void(Kit &)> on_click)
{
	auto button = make_unique<Button>();
	button->flat = true;
	button->focus_on_press = false;
	button->icon = icon;
	button->tip_text = QString::fromUtf8(_(tip));
	button->on_click = std::move(on_click);
	return button;
}

static unique_ptr<GutterRow>
chooser_field(const char *label, unique_ptr<Widget> control, Widget *buddy)
{
	auto text = make_unique<Label>();
	text->text = menu_label(label, &text->mnemonic);
	text->align = Align::End;
	text->buddy = buddy;

	auto row = make_unique<GutterRow>();
	row->gap = 8.f;
	row->add_child(std::move(text), size_t(-1));
	row->add_child(std::move(control), size_t(-1));
	return row;
}

// What the window would answer to, were it not shut out by the modal; only
// the one verb the chooser has any use for.
constexpr Action kChooserKeys[] = {Action::DirParent};

void
dialog_files(Kit &kit, FileDialogSetup setup)
{
	Dialog &dialog = kit.new_dialog();
	dialog.max_w = 720.f;

	auto col = make_unique<Chooser>();
	Chooser *state = col.get();
	state->dialog = &dialog;
	for (const FileType &type : setup.types)
		state->globs.push_back(compile_globs(type.globs));
	state->setup = std::move(setup);

	col->gap = 8.f;

	// The first bold label is what the platform reads the dialog out as.
	auto heading = make_unique<Label>();
	heading->text =
		QString::fromUtf8(state->setup.save ? _("Save As") : _("Open"));
	heading->bold = true;
	col->add_child(std::move(heading), size_t(-1));
	if (!state->setup.explanation.isEmpty()) {
		auto note = make_unique<Label>();
		note->text = state->setup.explanation;
		note->wrap = true;
		note->dim = true;
		col->add_child(std::move(note), size_t(-1));
	}

	// Return and focus loss commit; dependent actions also commit explicitly.
	auto path = make_unique<Entry>();
	path->grow = true;
	path->on_commit = [state](Kit &k) { state->commit_path(k); };
	state->path = path.get();

	// The tools are a group of their own, set off from the field: packed
	// tight between themselves, and at arm's length from it.
	auto tools = make_unique<Row>();
	tools->gap = 2.f;
	tools->add_child(chooser_tool("go-up-symbolic", N_("Parent directory"),
						 [state](Kit &k) { state->go_parent(k); }),
		size_t(-1));
	tools->add_child(chooser_tool("arrows-circle-symbolic", N_("Refresh"),
						 [state](Kit &k) {
							 state->commit_path(k);
							 state->rescan(k);
						 }),
		size_t(-1));
	tools->add_child(chooser_tool("folder-plus-symbolic", N_("New folder"),
						 [state](Kit &k) {
							 state->commit_path(k);
							 dialog_entry(k, N_("New Folder"), N_("_Create"),
								 {}, [state](Kit &inner, const QString &name) {
									 return state->make_dir(inner, name);
								 });
						 }),
		size_t(-1));

	auto hide = chooser_tool(
		"filter-symbolic", N_("Hide hidden files"), [state](Kit &k) {
			state->hide->active = !state->hide->active;
			state->commit_path(k);
			state->rescan(k);
		});
	hide->active = true;
	state->hide = hide.get();
	tools->add_child(std::move(hide), size_t(-1));

	auto bar = make_unique<Row>();
	bar->gap = 8.f;
	bar->add_child(std::move(path), size_t(-1));
	bar->add_child(std::move(tools), size_t(-1));
	col->add_child(std::move(bar), size_t(-1));

	auto list = make_unique<FileList>();
	list->grow = true;
	state->list = list.get();
	col->add_child(std::move(list), size_t(-1));

	auto fields = make_unique<GutterColumn>();
	fields->gap = 4.f;
	if (state->setup.save) {
		auto name = make_unique<Entry>();
		name->text = state->setup.name;
		state->name = name.get();
		Entry *name_ref = name.get();
		fields->add_child(
			chooser_field(N_("File _name"), std::move(name), name_ref),
			size_t(-1));
	}
	if (!state->setup.types.empty()) {
		auto type = make_unique<Combo>();
		type->grow = true;
		for (const FileType &item : state->setup.types)
			type->items.push_back(QString::fromUtf8(_(item.label)));
		type->on_select = [state](Kit &k, int) { state->retype(k); };
		state->type = type.get();
		Combo *type_ref = type.get();
		fields->add_child(
			chooser_field(
				state->setup.save ? N_("Save as _type") : N_("Files of _type"),
				std::move(type), type_ref),
			size_t(-1));
	}
	col->add_child(std::move(fields), size_t(-1));

	auto warning = make_unique<Label>();
	warning->wrap = true;
	warning->visible = false;
	state->warning = warning.get();
	col->add_child(std::move(warning), size_t(-1));

	state->list->on_select = [state](Kit &k, const FileEntry &entry) {
		if (state->commit_path(k))
			return;
		if (state->name && !entry.dir)
			state->name->set_text(k, entry.name);
	};
	state->list->on_activate = [state](Kit &k, const FileEntry &entry) {
		if (state->commit_path(k) ||
			QFileInfo(entry.path).absolutePath() != state->dir)
			return;
		if (entry.dir)
			state->set_dir(k, entry.path);
		else if (state->setup.save)
			// on_select has just put it in the field; the suffix and the
			// overwrite question are still owed on it.
			state->accept(k);
		else
			state->accept_path(k, entry.path);
	};

	auto affirm = make_unique<Button>();
	affirm->text = menu_label(
		state->setup.save ? N_("_Save") : N_("_Open"), &affirm->mnemonic);
	affirm->pad_x = 16.f;
	affirm->on_click = [state](Kit &k) { state->accept(k); };

	auto cancel = make_unique<Button>();
	cancel->text = menu_label(N_("_Cancel"), &cancel->mnemonic);
	cancel->pad_x = 16.f;
	cancel->on_click = [&dialog](Kit &k) { dialog.close(k); };

	dialog.on_key = [state](Kit &k, const Key &ev) {
		if (match_key(kChooserKeys, ev.key, ev.mods) != Action::DirParent)
			return false;
		state->go_parent(k);
		return true;
	};

	dialog.show(
		kit, std::move(col), 560.f, std::move(affirm), std::move(cancel));

	// After show(), so that a failure has somewhere to put its message.
	QString start = state->setup.directory;
	if (start.isEmpty() || !QFileInfo(start).isDir())
		start = QDir::currentPath();
	state->set_dir(kit, start);
}

}  // namespace dn
