//
// kit-browser.hpp: directory browser
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit-chrome.hpp"
#include "kit.hpp"
#include "sheet.hpp"
#include "thumbnailer.hpp"
#include "types.hpp"

#include <libdn.h>

#include <QString>
#include <QUrl>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dn
{

constexpr float kBrowseSidebarPts = 192.f;

enum class SortField : uint8_t { Name, Time };
enum class BrowserView : uint8_t { Tile, Grid };

struct BrowseSetup {
	SortField sort = SortField::Name;
	bool sort_desc = false;
	bool filter_files = true;
};

struct Browser : Widget {
	struct File {
		std::string path;
		std::string name;
		int64_t mtime = 0;
		uint64_t size = 0;
		uint32_t image_w = 0;
		uint32_t image_h = 0;
		int ram_w = 0;
		int ram_h = 0;
		std::vector<uint16_t> ram;
		bool ram_interim = false;
		bool ram_pending = false;
		bool cache_bypass = false;
		bool regen_failed = false;
		dawn::Transfer transfer = dawn::Transfer::Srgb;
		Sheet::Packed gpu;
		bool failed = false;
		Rect tile{};
		Rect cell{};
		QString cap_text;
		Rect cap{};
	};
	struct DirRow {
		std::string path;
		std::string name;
		std::string tip;
		const char *icon = nullptr;
		bool current = false;
	};
	struct PlaceItem {
		Button *button = nullptr;
		std::string path;
	};
	struct CachedSize {
		int64_t mtime = 0;
		uint64_t size = 0;
		uint32_t w = 0;
		uint32_t h = 0;
	};

	Kit &kit_;
	Thumbnailer &thumbnailer_;
	uint64_t thumbnail_client_ = 0;
	Page *page_ = nullptr;
	ScrollColumn *places_ = nullptr;
	std::vector<PlaceItem> place_items_;

	// Enumeration below is std::filesystem; this is the identity above it.
	QUrl dir_url_;
	std::shared_ptr<dawn::Cmm> cmm_;
	std::shared_ptr<dawn::Profile> screen_profile_;

	bool show_names_ = false;
	// The toolbar search field, whose text narrows the listing; it is
	// deliberately per-window, and not part of BrowseSetup.
	Entry *search_ = nullptr;
	BrowseSetup setup_;
	BrowserView view_ = BrowserView::Tile;
	int thumb_size_ = 256;
	bool places_dirty_ = true;

	Scroll scroll_;

	struct GridRow {
		int first = 0;
		int count = 0;
		int y = 0;
		int h = 0;
	};
	std::vector<GridRow> rows_;
	int cursor_ = -1;
	float cursor_x_ = 0;
	bool cursor_x_dirty_ = false;
	int layout_cursor_ = -1;
	float layout_cell_x_ = 0;
	int layout_w_ = 0;
	int mid_file_ = -1;

	bool can_prev_dir_ = false;
	bool can_next_dir_ = false;
	bool can_parent_dir_ = false;
	std::vector<File> files_;
	std::vector<DirRow> side_dirs_;
	std::unordered_map<std::string, CachedSize> size_cache_;
	std::unordered_map<std::string, Thumbnailer::Priority> thumb_inflight_;

	struct HistEntry {
		QUrl url;
		float side_scroll = 0;
	};
	std::vector<HistEntry> hist_back_;
	std::vector<HistEntry> hist_forward_;

	Sheet sheet_{Sheet::kSize, false};
	uint64_t thumb_gen_ = 0;

	Browser(Kit &kit, Thumbnailer &thumbnailer);
	~Browser() override;

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	[[nodiscard]] bool focusable() const override;
	[[nodiscard]] Qt::CursorShape cursor() const override;
	[[nodiscard]] QString tip() const override;
	[[nodiscard]] Rect tip_anchor() const override { return {}; }

	void init();
	void destroy();
	void set_host(float width_pts, float height_pts, float dpr);
	void open_dir(const QUrl &url, bool record = true);
	void rescan();
	bool hist_back();
	bool hist_forward();
	void hist_clear_forward();
	[[nodiscard]] bool hist_can_back() const;
	[[nodiscard]] bool hist_can_forward() const;
	void select_file(const QUrl &url);
	void file_gone(const QUrl &url);
	[[nodiscard]] QUrl file_url(int index) const;
	[[nodiscard]] BrowseSetup browse_setup() const { return this->setup_; }
	void set_screen_profile(
		std::shared_ptr<dawn::Cmm> cmm, std::shared_ptr<dawn::Profile> profile);
	void present(Page &ui);
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	Scroll *scrollbar() override { return &this->scroll_; }
	bool motion(Kit &kit, float x, float y) override;
	bool scroll(Kit &kit, float x, float y, int delta) override;
	bool pan(Kit &kit, float x, float y, float dx, float dy) override;
	bool key(Kit &kit, const Key &ev) override;
	bool double_click(Kit &kit, float x, float y, Qt::MouseButton button,
		unsigned mods) override;
	[[nodiscard]] int wake_ms() const override;
	[[nodiscard]] bool thumbs_busy() const;
};

std::unique_ptr<Page> make_browser_page(
	Kit &kit, const HostActions &host, Thumbnailer &thumbnailer, Browser **out);

}  // namespace dn
