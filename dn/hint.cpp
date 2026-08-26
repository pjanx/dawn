//
// hint.cpp: Vimperator-style f hint overlay
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "hint.hpp"

#include "browser.hpp"
#include "chrome.hpp"

#include <QChar>
#include <Qt>

#include <algorithm>

using namespace std;

namespace dn
{
namespace
{

constexpr char kChars[] = "SADFJKLEWCMPGH";
// TODO(p): The fuck, derive it.
constexpr int kNChars = 14;
constexpr float kChipPadX = 4.0f;
constexpr float kChipPadY = 2.0f;

Colour
col(const Colour &c, float alpha = 1.0f)
{
	return {c.r, c.g, c.b, c.a * alpha};
}

Rect
intersection(Rect a, Rect b)
{
	const float x = max(a.x, b.x);
	const float y = max(a.y, b.y);
	return {x, y, max(0.0f, min(a.x + a.w, b.x + b.w) - x),
		max(0.0f, min(a.y + a.h, b.y + b.h) - y)};
}

Rect
visible_rect(const Widget *w, Rect host)
{
	if (!w || !w->shown() || w->r.w <= 0.0f || w->r.h <= 0.0f)
		return {};
	Rect visible = intersection(w->r, host);
	for (const Widget *p = w->parent_; p; p = p->parent_) {
		if (!p->shown())
			return {};
		if (p->clips_children())
			visible = intersection(visible, p->r);
	}
	return visible;
}

void
collect_buttons(Widget *w, Rect host, vector<Button *> &out)
{
	if (!w || !w->shown())
		return;
	if (dynamic_cast<Popup *>(w) || dynamic_cast<Browser *>(w) ||
		dynamic_cast<Splitter *>(w))
		return;
	if (auto *b = dynamic_cast<Button *>(w)) {
		const Rect visible = visible_rect(b, host);
		if (b->enabled_ && b->hittable && b->on_click && visible.w > 0.0f &&
			visible.h > 0.0f)
			out.push_back(b);
		return;
	}
	const size_t n = w->child_count();
	for (size_t i = 0; i < n; ++i)
		collect_buttons(w->child(i), host, out);
}

QString
label_at(int i, int len)
{
	QString s(len, QLatin1Char('A'));
	int x = i;
	for (int k = len - 1; k >= 0; --k) {
		s[k] = QLatin1Char(kChars[x % kNChars]);
		x /= kNChars;
	}
	return s;
}

int
label_len(int n)
{
	if (n <= 0)
		return 1;
	int len = 1;
	int cap = kNChars;
	while (cap < n && len < 8) {
		++len;
		cap *= kNChars;
	}
	return len;
}

bool
modifier_only(int key)
{
	return key == Qt::Key_Shift || key == Qt::Key_Control ||
		key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}

}  // namespace

Hint::Hint()
{
	this->hittable = true;
	this->visible = false;
	this->fill = Fill::None;
}

void
Hint::open(Kit &kit)
{
	this->typed_.clear();
	Popup::open(kit);
	collect();
	assign_labels();
	place(kit);
}

void
Hint::close(Kit &kit)
{
	this->typed_.clear();
	this->targets_.clear();
	Popup::close(kit);
}

void
Hint::place(Kit &kit)
{
	this->visible = true;
	this->r = {0.0f, 0.0f, kit.host_w_, kit.host_h_};
	refresh_rects();
	layout_chips(kit);
}

void
Hint::prepare(Kit &kit)
{
	for (const Target &t : this->targets_) {
		if (matches(t))
			kit.cache_text(t.label, true);
	}
}

void
Hint::paint(Kit &kit) const
{
	if (!shown())
		return;
	kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
		this->r.y + this->r.h, col(kit.colours_[ColourInk], 0.1f));
	const float th = kit.text_height(QStringLiteral("Ag"), 0.0f, true);
	for (const Target &t : this->targets_) {
		if (!matches(t) || t.chip.w <= 0.0f)
			continue;
		const Rect c = t.chip;
		kit.list_.add_rect_filled(
			c.x, c.y, c.x + c.w, c.y + c.h, kit.colours_[ColourHint]);
		kit.list_.add_rect_stroke(
			c.x, c.y, c.x + c.w, c.y + c.h, kit.colours_[ColourInk]);
		const QString rest = t.label.mid(this->typed_.size());
		float tx = c.x + kChipPadX;
		const float ty = c.y + max(0.0f, (c.h - th) * 0.5f);
		if (!this->typed_.isEmpty()) {
			kit.emit_text(tx, ty, this->typed_,
				col(kit.colours_[ColourInk], 0.25f), true);
			tx += kit.text_width(this->typed_, true);
		}
		if (!rest.isEmpty())
			kit.emit_text(tx, ty, rest, kit.colours_[ColourInk], true);
	}
}

bool
Hint::key(Kit &kit, int key, unsigned mods)
{
	const unsigned extra = mods &
		unsigned(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	if (modifier_only(key) && extra == 0)
		return true;
	if (key == Qt::Key_Escape) {
		close(kit);
		return true;
	}
	if (key == Qt::Key_Backspace && extra == 0) {
		if (!this->typed_.isEmpty())
			this->typed_.chop(1);
		return true;
	}
	if (extra == 0 && key >= Qt::Key_A && key <= Qt::Key_Z) {
		const QChar ch = QLatin1Char(char('A' + (key - Qt::Key_A)));
		const QString next = this->typed_ + ch;
		bool any = false;
		const Target *exact = nullptr;
		int exact_n = 0;
		for (const Target &t : this->targets_) {
			if (!t.label.startsWith(next))
				continue;
			any = true;
			if (t.label == next) {
				exact = &t;
				++exact_n;
			}
		}
		if (!any)
			return true;
		this->typed_ = next;
		if (exact_n == 1 && exact)
			fire(kit, *exact);
		return true;
	}
	close(kit);
	return true;
}

bool
Hint::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button != Qt::LeftButton) {
		close(kit);
		kit.pressed_ = nullptr;
		return true;
	}
	for (const Target &t : this->targets_) {
		if (!matches(t))
			continue;
		if (t.chip.contains(x, y) || t.at.contains(x, y)) {
			fire(kit, t);
			kit.pressed_ = nullptr;
			return true;
		}
	}
	close(kit);
	kit.pressed_ = nullptr;
	return true;
}

bool
Hint::release(Kit &, float, float, Qt::MouseButton)
{
	return true;
}

bool
Hint::motion(Kit &, float, float)
{
	return true;
}

void
Hint::collect()
{
	this->targets_.clear();
	if (!this->page)
		return;
	Rect host = this->page->r;
	if (host.w <= 0.0f || host.h <= 0.0f)
		host = {0.0f, 0.0f, 1.0e8f, 1.0e8f};
	vector<Button *> buttons;
	collect_buttons(this->page, host, buttons);
	for (Button *b : buttons) {
		Target t;
		t.button = b;
		t.at = visible_rect(b, host);
		this->targets_.push_back(t);
	}
	auto *browser = dynamic_cast<Browser *>(this->page->content);
	if (!browser)
		return;
	const Rect well = browser->r;
	for (int i = 0; i < int(browser->files_.size()); ++i) {
		const Browser::File &f = browser->files_[size_t(i)];
		if (f.tile.w <= 0.0f || f.tile.h <= 0.0f)
			continue;
		const Rect visible = intersection(f.tile, well);
		if (visible.w <= 0.0f || visible.h <= 0.0f)
			continue;
		Target t;
		t.browser = browser;
		t.file_i = i;
		t.at = visible;
		this->targets_.push_back(t);
	}
}

void
Hint::assign_labels()
{
	const int n = int(this->targets_.size());
	const int len = label_len(n);
	for (int i = 0; i < n; ++i)
		this->targets_[size_t(i)].label = label_at(i, len);
}

void
Hint::refresh_rects()
{
	vector<Target> keep;
	keep.reserve(this->targets_.size());
	for (Target &t : this->targets_) {
		if (t.button) {
			const Rect visible = visible_rect(t.button, this->page->r);
			if (!t.button->enabled_ || visible.w <= 0.0f || visible.h <= 0.0f)
				continue;
			t.at = visible;
			keep.push_back(t);
			continue;
		}
		if (!t.browser || t.file_i < 0 ||
			t.file_i >= int(t.browser->files_.size()))
			continue;
		const Browser::File &f = t.browser->files_[size_t(t.file_i)];
		const Rect visible = intersection(f.tile, t.browser->r);
		if (visible.w <= 0.0f || visible.h <= 0.0f)
			continue;
		t.at = visible;
		keep.push_back(t);
	}
	this->targets_ = std::move(keep);
}

void
Hint::layout_chips(const Kit &kit)
{
	const float th = kit.text_height(QStringLiteral("Ag"), 0.0f, true);
	const float ch = th + kChipPadY * 2.0f;
	for (Target &t : this->targets_) {
		const float tw = kit.text_width(t.label, true);
		const float cw = tw + kChipPadX * 2.0f;
		t.chip = kit.snap_rect({t.at.x, t.at.y, cw, ch});
	}
}

bool
Hint::matches(const Target &t) const
{
	return t.label.startsWith(this->typed_);
}

void
Hint::fire(Kit &kit, Target t)
{
	Button *button = t.button;
	Browser *browser = t.browser;
	const int file_i = t.file_i;
	close(kit);
	if (button) {
		button->activate(kit);
		return;
	}
	if (!browser || file_i < 0 || file_i >= int(browser->files_.size()))
		return;
	const QUrl url = browser->file_url(file_i);
	browser->select_file(url);
	if (browser->page_ && browser->page_->host &&
		browser->page_->host->activate)
		browser->page_->host->activate(url);
}

}  // namespace dn
