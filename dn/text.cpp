//
// text.cpp: Shared native text truncation policy
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "text.hpp"

#include <algorithm>

using namespace std;

namespace dn
{

static bool
fits(const TextLayout &layout, const TextOptions &options)
{
	if (int(layout.lines().size()) > options.max_lines)
		return false;
	for (const TextLine &line : layout.lines()) {
		if (line.advance > float(options.wrap_width) + .01f)
			return false;
	}
	return true;
}

unique_ptr<TextLayout>
TextBackend::layout(
	const QString &text, const TextOptions &options, string *error)
{
	auto source = layout_native(text, options, error);
	if (!source || options.max_lines <= 0 || options.wrap_width <= 0 ||
		fits(*source, options))
		return source;

	vector<int> cuts;
	if (!source->cut_positions(cuts, error))
		return {};
	// Start at the end of the last allowed line, or the first over-wide one.
	// Walking backwards avoids assuming shaping widths are monotonic.
	int limit = 0;
	for (size_t i = 0; i < source->lines().size(); i++) {
		const TextLine &line = source->lines()[i];
		limit = line.text_start + line.text_length;
		if (i + 1 >= size_t(options.max_lines) ||
			line.advance > float(options.wrap_width) + .01f)
			break;
	}

	const QString ellipsis(QChar(0x2026));
	auto minimum = layout_native(ellipsis, options, error);
	if (!minimum)
		return {};
	if (!fits(*minimum, options))
		return layout_native({}, options, error);

	auto cut = upper_bound(cuts.begin(), cuts.end(), limit);
	while (cut != cuts.begin()) {
		cut--;
		int end = *cut;
		while (end > 0 && text[end - 1].isSpace())
			end--;
		// Trimming must not split a native cluster, either.
		cut = upper_bound(cuts.begin(), cut + 1, end) - 1;
		if (!*cut)
			break;
		auto candidate =
			layout_native(text.left(*cut) + ellipsis, options, error);
		if (!candidate || fits(*candidate, options))
			return candidate;
	}
	return minimum;
}

}  // namespace dn
