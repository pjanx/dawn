//
// load-ora.cpp: OpenRaster and Krita image loading
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn-loaders.h"
#include "libdn.h"

#include <algorithm>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std;

namespace dawn
{

// Both formats are ZIP archives keeping a flattened composite as a PNG, which
// is all we take: blending layers ourselves would be a different program.
// Krita omits that composite from .krz files and from autosaves, so we fall
// back on the smaller previews it writes for file managers.

enum {
	kSignatureLocal = 0x04034B50,
	kSignatureCentral = 0x02014B50,
	kSignatureEocd = 0x06054B50,
	kSignatureEocd64 = 0x06064B50,
	kSignatureLocator64 = 0x07064B50,
};

enum {
	kMethodStore = 0,
	kMethodDeflate = 8,
};

/// The ZIP64 extra field, holding whatever fixed fields have overflowed.
constexpr uint16_t kExtraZip64 = 0x0001;

/// Sizes and offsets that overflow their fixed field carry this instead.
constexpr uint32_t kOverflow = 0xFFFFFFFF;

/// No preview worth showing is anywhere near this large.
constexpr uint64_t kMaxEntrySize = 256 << 20;

// --- Reading -----------------------------------------------------------------

static uint16_t
le16(const uint8_t *p)
{
	return uint16_t(uint32_t(p[1]) << 8 | p[0]);
}

static uint32_t
le32(const uint8_t *p)
{
	return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 |
		uint32_t(p[0]);
}

static uint64_t
le64(const uint8_t *p)
{
	return uint64_t(le32(p + 4)) << 32 | le32(p);
}

// --- Central directory -------------------------------------------------------

struct ZipEntry {
	string name;
	uint16_t method = 0;
	uint64_t compressed_size = 0;
	uint64_t uncompressed_size = 0;
	uint64_t local_header_offset = 0;
};

/// The end of central directory record is last, but a comment may follow it,
/// so it needs to be searched for, and its length field confirms the find.
static const uint8_t *
find_eocd(span<const uint8_t> data)
{
	size_t limit = min(data.size(), size_t(0xFFFF) + 22);
	for (size_t back = 22; back <= limit; back++) {
		const uint8_t *p = data.data() + data.size() - back;
		if (le32(p) == kSignatureEocd && le16(p + 20) == back - 22)
			return p;
	}
	return nullptr;
}

struct Directory {
	uint64_t entries = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
};

static bool
find_directory(span<const uint8_t> data, Directory *out)
{
	const uint8_t *eocd = find_eocd(data);
	if (!eocd)
		return false;

	out->entries = le16(eocd + 10);
	out->size = le32(eocd + 12);
	out->offset = le32(eocd + 16);
	if (out->entries != 0xFFFF && out->size != kOverflow &&
		out->offset != kOverflow)
		return true;

	// Anything that overflowed is in a ZIP64 record, found through a locator
	// that immediately precedes the record we have just matched.
	if (size_t(eocd - data.data()) < 20 ||
		le32(eocd - 20) != kSignatureLocator64)
		return false;

	uint64_t at = le64(eocd - 20 + 8);
	if (data.size() < 56 || at > data.size() - 56 ||
		le32(data.data() + at) != kSignatureEocd64)
		return false;

	out->entries = le64(data.data() + at + 32);
	out->size = le64(data.data() + at + 40);
	out->offset = le64(data.data() + at + 48);
	return true;
}

/// Replace overflowed fixed fields with what a ZIP64 extra field says.
/// They only appear if they were needed, and always in this order.
static void
apply_zip64(span<const uint8_t> extra, ZipEntry *entry)
{
	size_t offset = 0;
	while (extra.size() - offset >= 4) {
		uint16_t id = le16(extra.data() + offset);
		uint16_t length = le16(extra.data() + offset + 2);
		offset += 4;
		if (length > extra.size() - offset)
			return;

		const uint8_t *p = extra.data() + offset;
		const uint8_t *end = p + length;
		offset += length;
		if (id != kExtraZip64)
			continue;

		if (entry->uncompressed_size == kOverflow && end - p >= 8) {
			entry->uncompressed_size = le64(p);
			p += 8;
		}
		if (entry->compressed_size == kOverflow && end - p >= 8) {
			entry->compressed_size = le64(p);
			p += 8;
		}
		if (entry->local_header_offset == kOverflow && end - p >= 8)
			entry->local_header_offset = le64(p);
		return;
	}
}

static bool
read_directory(span<const uint8_t> data, vector<ZipEntry> *out)
{
	Directory d;
	if (!find_directory(data, &d) || d.offset > data.size() ||
		d.size > data.size() - d.offset)
		return false;

	span<const uint8_t> directory = data.subspan(d.offset, d.size);
	size_t at = 0;
	for (uint64_t i = 0; i < d.entries; i++) {
		if (directory.size() - at < 46 ||
			le32(directory.data() + at) != kSignatureCentral)
			return false;

		const uint8_t *p = directory.data() + at;
		size_t name_length = le16(p + 28);
		size_t extra_length = le16(p + 30);
		size_t total = 46 + name_length + extra_length + le16(p + 32);
		if (directory.size() - at < total)
			return false;

		ZipEntry entry;
		entry.name.assign((const char *) p + 46, name_length);
		entry.method = le16(p + 10);
		entry.compressed_size = le32(p + 20);
		entry.uncompressed_size = le32(p + 24);
		entry.local_header_offset = le32(p + 42);
		apply_zip64({p + 46 + name_length, extra_length}, &entry);
		out->push_back(std::move(entry));
		at += total;
	}
	return true;
}

// --- Members -----------------------------------------------------------------

static const ZipEntry *
find_entry(const vector<ZipEntry> &entries, string_view name)
{
	for (const ZipEntry &entry : entries)
		if (entry.name == name)
			return &entry;
	return nullptr;
}

static bool
read_entry(
	span<const uint8_t> data, const ZipEntry &entry, vector<uint8_t> *out)
{
	if (entry.uncompressed_size > kMaxEntrySize || data.size() < 30 ||
		entry.local_header_offset > data.size() - 30)
		return false;

	// Only the local header's two length fields can be trusted; its sizes may
	// be left for a trailing data descriptor, which is why we came here from
	// the central directory in the first place.
	const uint8_t *p = data.data() + entry.local_header_offset;
	if (le32(p) != kSignatureLocal)
		return false;

	uint64_t start =
		entry.local_header_offset + 30 + le16(p + 26) + le16(p + 28);
	if (start > data.size() || entry.compressed_size > data.size() - start)
		return false;

	span<const uint8_t> body = data.subspan(start, entry.compressed_size);
	if (entry.method == kMethodStore) {
		if (entry.compressed_size != entry.uncompressed_size)
			return false;

		try {
			out->assign(body.begin(), body.end());
		} catch (const bad_alloc &) {
			return false;
		}
		return true;
	}
	if (entry.method != kMethodDeflate)
		return false;

	try {
		out->resize(entry.uncompressed_size);
	} catch (const bad_alloc &) {
		return false;
	}
	return detail::inflate_raw(body, *out);
}

// --- Public entry point ------------------------------------------------------

ImagePtr
detail::load_ora(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	if (data.size() < 4 || le32(data.data()) != kSignatureLocal) {
		set_error(error, "not a ZIP archive");
		return nullptr;
	}

	vector<ZipEntry> entries;
	if (!read_directory(data, &entries)) {
		set_error(error, "malformed ZIP archive");
		return nullptr;
	}

	// Both formats open with a stored "mimetype" member, which is the only
	// thing telling them apart from any other ZIP archive.  Krita puts
	// "application/x-krita" in .krz files as well.
	vector<uint8_t> type;
	const ZipEntry *mimetype = find_entry(entries, "mimetype");
	string_view id;
	if (mimetype && read_entry(data, *mimetype, &type))
		id = string_view((const char *) type.data(), type.size());
	if (id != "image/openraster" && id != "application/x-krita") {
		set_error(error, "not an OpenRaster or Krita image");
		return nullptr;
	}

	// In descending order of fidelity, the latter two being previews meant
	// for file managers, which is all that .krz and autosaves ever carry.
	static const char *kComposites[] = {
		"mergedimage.png",
		"preview.png",
		"Thumbnails/thumbnail.png",
	};

	vector<uint8_t> png;
	const char *found = nullptr;
	for (const char *name : kComposites) {
		const ZipEntry *entry = find_entry(entries, name);
		if (entry && read_entry(data, *entry, &png)) {
			found = name;
			break;
		}
	}
	if (!found) {
		set_error(error, "the archive carries no composite image");
		return nullptr;
	}
	if (found != kComposites[0])
		add_warning(ctx, string(found) + " is a reduced-size preview");

	return detail::load_wuffs(png, ctx, error);
}

}  // namespace dawn
