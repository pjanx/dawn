//
// hint.hpp: Vimperator-style f hint overlay
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit.hpp"

#include <QString>

#include <vector>

namespace dn
{

struct Browser;
struct Page;

struct Hint : Popup {
	Page *page = nullptr;

	Hint();
	void open(Kit &kit);
	void close(Kit &kit) override;
	void place(Kit &kit) override;
	void prepare(Kit &kit) override;
	void paint(Kit &kit) const override;
	bool captures_keys() const override { return true; }
	bool key(Kit &kit, int key, unsigned mods) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;

private:
	struct Target {
		QString label;
		Rect at{};
		Rect chip{};
		Button *button = nullptr;
		Browser *browser = nullptr;
		int file_i = -1;
	};

	std::vector<Target> targets_;
	QString typed_;

	void collect();
	void assign_labels();
	void refresh_rects();
	void layout_chips(const Kit &kit);
	[[nodiscard]] bool matches(const Target &t) const;
	void fire(Kit &kit, Target t);
};

}  // namespace dn
