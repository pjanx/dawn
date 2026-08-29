//
// tiffer.h: TIFF reading utilities
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// --- Utilities ---------------------------------------------------------------

static uint64_t
tiffer_u64be(const uint8_t *p)
{
	return (uint64_t) p[0] << 56 | (uint64_t) p[1] << 48 |
		(uint64_t) p[2] << 40 | (uint64_t) p[3] << 32 | (uint64_t) p[4] << 24 |
		p[5] << 16 | p[6] << 8 | p[7];
}

static uint32_t
tiffer_u32be(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint16_t
tiffer_u16be(const uint8_t *p)
{
	return (uint16_t) p[0] << 8 | p[1];
}

static uint64_t
tiffer_u64le(const uint8_t *p)
{
	return (uint64_t) p[7] << 56 | (uint64_t) p[6] << 48 |
		(uint64_t) p[5] << 40 | (uint64_t) p[4] << 32 | (uint64_t) p[3] << 24 |
		p[2] << 16 | p[1] << 8 | p[0];
}

static uint32_t
tiffer_u32le(const uint8_t *p)
{
	return (uint32_t) p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
}

static uint16_t
tiffer_u16le(const uint8_t *p)
{
	return (uint16_t) p[1] << 8 | p[0];
}

// --- TIFF --------------------------------------------------------------------
// libtiff is a mess, and the format is not particularly complicated.
// Exiv2 is senselessly copylefted, and cannot do much.
// libexif is only marginally better.
// ExifTool is too user-oriented.

struct un {
	uint64_t (*u64)(const uint8_t *);
	uint32_t (*u32)(const uint8_t *);
	uint16_t (*u16)(const uint8_t *);
};

static struct un tiffer_unbe = {tiffer_u64be, tiffer_u32be, tiffer_u16be};
static struct un tiffer_unle = {tiffer_u64le, tiffer_u32le, tiffer_u16le};

struct tiffer {
	struct un *un;
	const uint8_t *begin, *p, *end;
	uint16_t remaining_fields;
};

static bool
tiffer_u32(struct tiffer *self, uint32_t *u)
{
	if (self->end - self->p < 4)
		return false;

	*u = self->un->u32(self->p);
	self->p += 4;
	return true;
}

static bool
tiffer_u16(struct tiffer *self, uint16_t *u)
{
	if (self->end - self->p < 2)
		return false;

	*u = self->un->u16(self->p);
	self->p += 2;
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
tiffer_init(struct tiffer *self, const uint8_t *tiff, size_t len)
{
	self->un = NULL;
	self->begin = self->p = tiff;
	self->end = tiff + len;
	self->remaining_fields = 0;

	const uint8_t le[4] = {'I', 'I', 42, 0}, be[4] = {'M', 'M', 0, 42};

	if (tiff + 8 > self->end)
		return false;
	else if (!memcmp(tiff, le, sizeof le))
		self->un = &tiffer_unle;
	else if (!memcmp(tiff, be, sizeof be))
		self->un = &tiffer_unbe;
	else
		return false;

	self->p = tiff + 4;
	// The first IFD needs to be read by caller explicitly,
	// even though it's required to be present by TIFF 6.0.
	return true;
}

/// Read the next IFD in a sequence.
static bool
tiffer_next_ifd(struct tiffer *self)
{
	// All fields from any previous IFD need to be read first.
	if (self->remaining_fields)
		return false;

	uint32_t ifd_offset = 0;
	if (!tiffer_u32(self, &ifd_offset))
		return false;

	// There is nothing more to read, this chain has terminated.
	if (!ifd_offset)
		return false;

	// Note that TIFF 6.0 requires there to be at least one entry,
	// but there is no need for us to check it.
	self->p = self->begin + ifd_offset;
	return tiffer_u16(self, &self->remaining_fields);
}

static size_t
tiffer_length(const struct tiffer *self)
{
	return self->begin > self->end ? 0 : self->end - self->begin;
}

/// Initialize a derived TIFF reader for a subIFD at the given location.
static bool
tiffer_subifd(
	const struct tiffer *self, uint32_t offset, struct tiffer *subreader)
{
	if (tiffer_length(self) < offset)
		return false;

	*subreader = *self;
	subreader->p = subreader->begin + offset;
	return tiffer_u16(subreader, &subreader->remaining_fields);
}

enum tiffer_type {
	TIFFER_BYTE = 1,
	TIFFER_ASCII,
	TIFFER_SHORT,
	TIFFER_LONG,
	TIFFER_RATIONAL,
	TIFFER_SBYTE,
	TIFFER_UNDEFINED,
	TIFFER_SSHORT,
	TIFFER_SLONG,
	TIFFER_SRATIONAL,
	TIFFER_FLOAT,
	TIFFER_DOUBLE,
	// This last type from TIFF Technical Note 1 isn't really used much.
	TIFFER_IFD,
};

static size_t
tiffer_value_size(enum tiffer_type type)
{
	switch (type) {
	case TIFFER_BYTE:
	case TIFFER_SBYTE:
	case TIFFER_ASCII:
	case TIFFER_UNDEFINED:
		return 1;
	case TIFFER_SHORT:
	case TIFFER_SSHORT:
		return 2;
	case TIFFER_LONG:
	case TIFFER_SLONG:
	case TIFFER_FLOAT:
	case TIFFER_IFD:
		return 4;
	case TIFFER_RATIONAL:
	case TIFFER_SRATIONAL:
	case TIFFER_DOUBLE:
		return 8;
	default:
		return 0;
	}
}

/// A lean iterator for values within entries.
struct tiffer_entry {
	uint16_t tag;
	enum tiffer_type type;
	// For {S,}BYTE, ASCII, UNDEFINED, use these fields directly.
	const uint8_t *p;
	uint32_t remaining_count;
};

static bool
tiffer_next_value(struct tiffer_entry *entry)
{
	if (!entry->remaining_count)
		return false;

	entry->p += tiffer_value_size(entry->type);
	entry->remaining_count--;
	return true;
}

static bool
tiffer_integer(
	const struct tiffer *self, const struct tiffer_entry *entry, int64_t *out)
{
	if (!entry->remaining_count)
		return false;

	// Somewhat excessively lenient, intended for display.
	// TIFF 6.0 only directly suggests that a reader is should accept
	// any of BYTE/SHORT/LONG for unsigned integers.
	switch (entry->type) {
	case TIFFER_BYTE:
	case TIFFER_ASCII:
	case TIFFER_UNDEFINED:
		*out = *entry->p;
		return true;
	case TIFFER_SBYTE:
		*out = (int8_t) *entry->p;
		return true;
	case TIFFER_SHORT:
		*out = self->un->u16(entry->p);
		return true;
	case TIFFER_SSHORT:
		*out = (int16_t) self->un->u16(entry->p);
		return true;
	case TIFFER_LONG:
	case TIFFER_IFD:
		*out = self->un->u32(entry->p);
		return true;
	case TIFFER_SLONG:
		*out = (int32_t) self->un->u32(entry->p);
		return true;
	default:
		return false;
	}
}

static bool
tiffer_rational(const struct tiffer *self, const struct tiffer_entry *entry,
	int64_t *numerator, int64_t *denominator)
{
	if (!entry->remaining_count)
		return false;

	// Somewhat excessively lenient, intended for display.
	switch (entry->type) {
	case TIFFER_RATIONAL:
		*numerator = self->un->u32(entry->p);
		*denominator = self->un->u32(entry->p + 4);
		return true;
	case TIFFER_SRATIONAL:
		*numerator = (int32_t) self->un->u32(entry->p);
		*denominator = (int32_t) self->un->u32(entry->p + 4);
		return true;
	default:
		if (tiffer_integer(self, entry, numerator)) {
			*denominator = 1;
			return true;
		}
		return false;
	}
}

static bool
tiffer_real(
	const struct tiffer *self, const struct tiffer_entry *entry, double *out)
{
	if (!entry->remaining_count)
		return false;

	// Somewhat excessively lenient, intended for display.
	// Assuming the host architecture uses IEEE 754.
	switch (entry->type) {
		int64_t numerator, denominator;
	case TIFFER_FLOAT:
		*out = *(float *) entry->p;
		return true;
	case TIFFER_DOUBLE:
		*out = *(double *) entry->p;
		return true;
	default:
		if (tiffer_rational(self, entry, &numerator, &denominator)) {
			*out = (double) numerator / denominator;
			return true;
		}
		return false;
	}
}

static bool
tiffer_next_entry(struct tiffer *self, struct tiffer_entry *entry)
{
	if (!self->remaining_fields)
		return false;

	uint16_t type = 0xFFFF;
	entry->type = (enum tiffer_type) 0xFFFF;
	if (!tiffer_u16(self, &entry->tag) || !tiffer_u16(self, &type) ||
		!tiffer_u32(self, &entry->remaining_count))
		return false;

	// Short values may and will be inlined, rather than pointed to.
	size_t values_size =
		tiffer_value_size((enum tiffer_type) type) * entry->remaining_count;
	uint32_t offset = 0;
	if (values_size <= sizeof offset) {
		entry->p = self->p;
		self->p += sizeof offset;
	} else if (tiffer_u32(self, &offset) && tiffer_length(self) >= offset) {
		entry->p = self->begin + offset;
	} else {
		return false;
	}

	// All entries are pre-checked not to overflow.
	if (values_size > PTRDIFF_MAX ||
		self->end - entry->p < (ptrdiff_t) values_size)
		return false;

	// Setting it at the end may provide an indication while debugging.
	entry->type = (enum tiffer_type) type;
	self->remaining_fields--;
	return true;
}
