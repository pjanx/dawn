//
// kit-files.hpp: the file chooser
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit-chrome.hpp"

namespace dn
{

// Concrete chooser rows, shared with the accessibility adapter.
struct FileList;
struct FileRow;

struct FileEntry {
	QString path;
	QString name;
	QString size;  // Empty for a directory.
	QString modified;
	uint64_t bytes = 0;
	int64_t mtime = 0;
	bool dir = false;
};

struct FileRows : ScrollColumn {
	FileList *list = nullptr;
	FileRow *selected = nullptr;

	Widget *tab_stop() override;
	void select(Kit &kit, FileRow *row);
};

struct FileRow : Button {
	FileList *list = nullptr;
	FileEntry entry;
	bool elided_ = false;

	FileRow();
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void prepare(Kit &kit) override;
	void paint(Kit &kit) const override;
	[[nodiscard]] QString tip() const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool activate(Kit &kit) override;
	bool double_click(Kit &kit, float x, float y, Qt::MouseButton button,
		unsigned mods) override;
};

// A filter and an explicit default save suffix.  Static data: the globs are
// one ';'-separated string so a caller can spell a table inline.
struct FileType {
	const char *label;   ///< N_("Lossless WebP (*.webp)")
	const char *globs;   ///< "*.webp", "*.icc;*.icm", "*"
	const char *suffix;  ///< ".webp", or null; never parsed from globs.
};

struct FileDialogSetup {
	bool save = false;
	QString directory;
	QString name;         ///< Suggested filename; Save only.
	QString explanation;  ///< Optional note below the heading.
	std::span<const FileType> types;
	/// Empty return means written and the dialog closes; otherwise it stays
	/// open showing the message -- the same contract dialog_entry has.
	std::function<QString(Kit &kit, const QString &path, int type)> on_accept;
};

void dialog_files(Kit &kit, FileDialogSetup setup);

}  // namespace dn
