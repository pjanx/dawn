//
// load-imageio.mm: image loading via macOS ImageIO
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// It would be preferrable if macOS didn't differ from other platforms
// in format support, though this is basically its equivalent of Glycin
// or GdkPixbuf.

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstring>
#include <string>
#include <vector>

using namespace std;

namespace dawn
{

// --- Properties --------------------------------------------------------------

static bool
imageio_number(CFDictionaryRef dictionary, CFStringRef key, double *out)
{
	CFTypeRef value = CFDictionaryGetValue(dictionary, key);
	if (!value || CFGetTypeID(value) != CFNumberGetTypeID())
		return false;
	return CFNumberGetValue((CFNumberRef) value, kCFNumberDoubleType, out);
}

// ImageIO exposes pages and animation frames as the same flat index space,
// and there is no format-independent delay key--so ask all four containers
// that have one.  Returns milliseconds, or -1 when this index is a page.
static int64_t
imageio_delay(CFDictionaryRef properties, uint64_t *loops)
{
	const struct {
		CFStringRef container, unclamped, clamped, loops;
	} animations[] = {
		{kCGImagePropertyGIFDictionary, kCGImagePropertyGIFUnclampedDelayTime,
			kCGImagePropertyGIFDelayTime, kCGImagePropertyGIFLoopCount},
		{kCGImagePropertyPNGDictionary, kCGImagePropertyAPNGUnclampedDelayTime,
			kCGImagePropertyAPNGDelayTime, kCGImagePropertyAPNGLoopCount},
		{kCGImagePropertyWebPDictionary, kCGImagePropertyWebPUnclampedDelayTime,
			kCGImagePropertyWebPDelayTime, kCGImagePropertyWebPLoopCount},
		{kCGImagePropertyHEICSDictionary,
			kCGImagePropertyHEICSUnclampedDelayTime,
			kCGImagePropertyHEICSDelayTime, kCGImagePropertyHEICSLoopCount},
	};

	for (const auto &animation : animations) {
		CFTypeRef value = CFDictionaryGetValue(properties, animation.container);
		if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID())
			continue;

		// The clamped variant floors short delays; we want what the file says.
		CFDictionaryRef dictionary = (CFDictionaryRef) value;
		double seconds = 0;
		if (!imageio_number(dictionary, animation.unclamped, &seconds) &&
			!imageio_number(dictionary, animation.clamped, &seconds))
			continue;

		double count = 0;
		if (loops && imageio_number(dictionary, animation.loops, &count) &&
			count > 0)
			*loops = uint64_t(count);
		return int64_t(seconds * 1000 + 0.5);
	}
	return -1;
}

// Files that carry no profile of their own get CG's sRGB from ImageIO,
// and passing that off as embedded would hide that it is a guess.  Clear it,
// let ensure_working_premul() invent sRGB, and admit to it in profile_assumed.
// Compare bytes rather than trust the absent profile name: a space CG derives
// from PNG gAMA/cHRM is nameless as well, yet real.
static void
imageio_drop_invented_srgb(Image &image)
{
	CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	if (!srgb)
		return;

	CFDataRef expected = CGColorSpaceCopyICCData(srgb);
	CGColorSpaceRelease(srgb);
	if (!expected)
		return;

	if (size_t(CFDataGetLength(expected)) == image.icc.size() &&
		!memcmp(CFDataGetBytePtr(expected), image.icc.data(), image.icc.size()))
		image.icc.clear();
	CFRelease(expected);
}

// ImageIO parses metadata into dictionaries rather than handing back
// original Exif or XMP packets, so Image::exif and Image::xmp stay empty.
// This is the only piece of metadata we can pass on.
static Orientation
imageio_orientation(CFDictionaryRef properties)
{
	double value = 0;
	if (!imageio_number(properties, kCGImagePropertyOrientation, &value) ||
		value < 1 || value > 8)
		return Orientation::Unknown;
	return Orientation(int(value));
}

// --- Pixels ------------------------------------------------------------------

// Core Graphics converts from the image's colour space to the context's,
// so giving the context the source's own space makes drawing an identity,
// and leaves all colour management to lcms2, as with every other loader.
// Anything that cannot be one--Gray, CMYK, Indexed, Lab, extended range--gets
// a real conversion to sRGB instead, which is then no longer an assumption.
static CGColorSpaceRef
imageio_target_space(
	CGImageRef cg, const OpenContext &ctx, CFDataRef *icc, Error *error)
{
	CGColorSpaceRef source = CGImageGetColorSpace(cg);
	if (source && CGColorSpaceGetModel(source) == kCGColorSpaceModelRGB &&
		!CGColorSpaceUsesExtendedRange(source) &&
		(*icc = CGColorSpaceCopyICCData(source)))
		return CGColorSpaceRetain(source);

	// Display P3 would narrow the loss without removing it, and this is
	// a fallback loader--revisit if anyone actually views EXR files in anger.
	if (source && CGColorSpaceUsesExtendedRange(source))
		add_warning(ctx, "extended range colours clipped to sRGB");

	CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	if (!srgb) {
		set_error(error, "cannot create an sRGB colour space");
		return nullptr;
	}
	*icc = CGColorSpaceCopyICCData(srgb);
	return srgb;
}

static ImagePtr
load_imageio_image(CGImageRef cg, const OpenContext &ctx, Error *error)
{
	size_t width = CGImageGetWidth(cg), height = CGImageGetHeight(cg);
	if (!width || !height || width > kMaxDimension || height > kMaxDimension) {
		set_error(error, "image dimensions overflow");
		return nullptr;
	}

	CFDataRef icc = nullptr;
	CGColorSpaceRef space = imageio_target_space(cg, ctx, &icc, error);
	if (!space)
		return nullptr;

	// Only EXR, HDR, PSD and 16-bit TIFF ever need the deep path.
	bool deep = CGImageGetBitsPerComponent(cg) > 8;
	size_t bits = deep ? 16 : 8;
	CGBitmapInfo info = CGBitmapInfo(kCGImageAlphaPremultipliedLast) |
		(deep ? kCGBitmapByteOrder16Little : kCGBitmapByteOrder32Big);

	size_t stride = width * 4 * (bits / 8);
	vector<uint8_t> pixels(stride * height);
	CGContextRef context = CGBitmapContextCreate(
		pixels.data(), width, height, bits, stride, space, info);
	if (context) {
		CGContextSetBlendMode(context, kCGBlendModeCopy);
		CGContextDrawImage(
			context, CGRectMake(0, 0, double(width), double(height)), cg);
		CGContextRelease(context);
	}
	CGColorSpaceRelease(space);
	if (!context) {
		if (icc)
			CFRelease(icc);
		set_error(error, "cannot create a bitmap context");
		return nullptr;
	}

	ImagePtr image = image_new(uint32_t(width), uint32_t(height));
	if (!image) {
		if (icc)
			CFRelease(icc);
		set_error(error, "image allocation failure");
		return nullptr;
	}

	// Core Graphics always premultiplies, which is what ensure_working_premul()
	// is then told about by our caller.
	if (deep)
		pack_rgba16le_to_bgra16(
			*image, (const uint16_t *) pixels.data(), stride, 16);
	else
		pack_rgba8_to_bgra16(*image, pixels.data(), stride);

	if (icc) {
		const uint8_t *bytes = CFDataGetBytePtr(icc);
		image->icc.assign(bytes, bytes + CFDataGetLength(icc));
		CFRelease(icc);
	}
	return image;
}

// --- Loader ------------------------------------------------------------------

static ImagePtr
load_imageio_indexes(CGImageSourceRef source, CFDictionaryRef options,
	const OpenContext &ctx, Error *error)
{
	if (CGImageSourceGetStatus(source) != kCGImageStatusComplete) {
		set_error(error, "incomplete or unsupported ImageIO image");
		return nullptr;
	}

	ImagePtr head, tail;
	bool animated = false;
	uint64_t loops = 0;
	size_t count = CGImageSourceGetCount(source);
	for (size_t i = 0; i < count; i++) {
		CFDictionaryRef properties =
			CGImageSourceCopyPropertiesAtIndex(source, i, options);
		int64_t duration =
			properties ? imageio_delay(properties, i ? nullptr : &loops) : -1;
		if (!i)
			animated = duration >= 0;

		Error suberror;
		ImagePtr image;
		CGImageRef cg = CGImageSourceCreateImageAtIndex(source, i, options);
		if (!cg) {
			set_error(&suberror, "ImageIO decoding error");
		} else {
			image = load_imageio_image(cg, ctx, &suberror);
			CGImageRelease(cg);
		}
		if (image && properties) {
			image->orientation = imageio_orientation(properties);
			if (!CFDictionaryContainsKey(
					properties, kCGImagePropertyProfileName))
				imageio_drop_invented_srgb(*image);
		}
		if (properties)
			CFRelease(properties);

		if (!image) {
			if (!head) {
				set_error(error, std::move(suberror.message));
				return nullptr;
			}
			add_warning(ctx, suberror.message);
			break;
		}

		if (animated) {
			image->frame_duration = max(duration, int64_t(0));
			append_frame(head, tail, std::move(image));
		} else {
			// ICNS and ICO expose their size variants this way,
			// just like our own ICNS loader does.
			append_page(head, tail, std::move(image));
		}
		if (ctx.first_frame_only)
			break;
	}
	if (!head) {
		set_error(error, "empty ImageIO image");
		return nullptr;
	}

	head->loops = loops;
	for (Image *page = head.get(); page; page = page->page_next.get())
		ensure_working_premul_pages(*page, ctx, nullptr, /*input_premul=*/true);
	return head;
}

ImagePtr
detail::load_imageio(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// The caller's buffer outlives this call, and ImageIO's own cache would
	// only fight the thumbnailer's memory accounting.
	CFDataRef wrapper = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
		data.data(), CFIndex(data.size()), kCFAllocatorNull);
	if (!wrapper) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	const void *keys[] = {kCGImageSourceShouldCache};
	const void *values[] = {kCFBooleanFalse};
	CFDictionaryRef options =
		CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CGImageSourceRef source = CGImageSourceCreateWithData(wrapper, options);
	CFRelease(wrapper);

	ImagePtr image;
	if (source) {
		image = load_imageio_indexes(source, options, ctx, error);
		CFRelease(source);
	} else {
		set_error(error, "not an ImageIO-decodable image");
	}

	CFRelease(options);
	return image;
}

// --- Advertised types --------------------------------------------------------

vector<string>
detail::imageio_media_types()
{
	// What ImageIO decodes is a runtime property.
	// The media types the API may give us don't match shared-mime-info exactly,
	// so here is our best attempt at mapping them.
	static const struct {
		const char *uti, *mime;
	} kTypes[] = {
		{"com.adobe.photoshop-image", "image/vnd.adobe.photoshop"},
		{"com.adobe.raw-image", "image/x-adobe-dng"},
		{"com.apple.icns", "image/x-icns"},
		{"com.apple.pict", "image/x-pict"},
		{"com.canon.cr2-raw-image", "image/x-canon-cr2"},
		{"com.canon.cr3-raw-image", "image/x-canon-cr3"},
		{"com.canon.crw-raw-image", "image/x-canon-crw"},
		{"com.canon.tif-raw-image", "image/tiff"},
		{"com.compuserve.gif", "image/gif"},
		{"com.epson.raw-image", "image/x-epson-erf"},
		{"com.fuji.raw-image", "image/x-fuji-raf"},
		{"com.hasselblad.3fr-raw-image", "image/x-hasselblad-3fr"},
		{"com.hasselblad.fff-raw-image", "image/x-hasselblad-fff"},
		{"com.ilm.openexr-image", "image/x-exr"},
		{"com.kodak.raw-image", "image/x-kodak-dcr"},
		{"com.konicaminolta.raw-image", "image/x-minolta-mrw"},
		{"com.leafamerica.raw-image", "image/x-leaf-mos"},
		{"com.leica.raw-image", "image/x-panasonic-rw"},
		{"com.leica.rwl-raw-image", "image/x-panasonic-rw2"},
		{"com.microsoft.bmp", "image/bmp"},
		{"com.microsoft.cur", "image/x-win-bitmap"},
		{"com.microsoft.dds", "image/vnd.ms-dds"},
		{"com.microsoft.ico", "image/vnd.microsoft.icon"},
		{"com.nikon.nrw-raw-image", "image/x-nikon-nrw"},
		{"com.nikon.raw-image", "image/x-nikon-nef"},
		// Apple splits Olympus three ways; shared-mime-info has one .orf.
		{"com.olympus.or-raw-image", "image/x-olympus-orf"},
		{"com.olympus.raw-image", "image/x-olympus-orf"},
		{"com.olympus.sr-raw-image", "image/x-olympus-orf"},
		{"com.panasonic.raw-image", "image/x-panasonic-rw"},
		{"com.panasonic.rw2-raw-image", "image/x-panasonic-rw2"},
		{"com.pentax.raw-image", "image/x-pentax-pef"},
		{"com.phaseone.raw-image", "image/x-phaseone-iiq"},
		{"com.samsung.raw-image", "image/x-samsung-srw"},
		{"com.sgi.sgi-image", "image/x-sgi"},
		{"com.sony.arw-raw-image", "image/x-sony-arw"},
		{"com.sony.raw-image", "image/x-sony-srf"},
		{"com.sony.sr2-raw-image", "image/x-sony-sr2"},
		{"com.truevision.tga-image", "image/x-tga"},
		{"org.khronos.astc", "image/astc"},
		{"org.khronos.ktx", "image/ktx"},
		{"org.khronos.ktx2", "image/ktx2"},
		{"org.webmproject.webp", "image/webp"},
		{"public.avci", "image/avci"},
		{"public.avif", "image/avif"},
		{"public.heic", "image/heif"},
		{"public.heif", "image/heif"},
		{"public.jpeg", "image/jpeg"},
		{"public.jpeg-2000", "image/jp2"},
		{"public.jpeg-xl", "image/jxl"},
		{"public.pbm", "image/x-portable-bitmap"},
		{"public.png", "image/png"},
		{"public.radiance", "image/vnd.radiance"},
		{"public.tiff", "image/tiff"},
	};

	vector<string> types{"image/x-dcraw"};
	CFArrayRef identifiers = CGImageSourceCopyTypeIdentifiers();
	if (!identifiers)
		return types;

	for (CFIndex i = 0; i < CFArrayGetCount(identifiers); i++) {
		CFStringRef identifier =
			(CFStringRef) CFArrayGetValueAtIndex(identifiers, i);
		char uti[256] = {};
		if (!CFStringGetCString(
				identifier, uti, sizeof uti, kCFStringEncodingUTF8))
			continue;

		for (const auto &entry : kTypes)
			if (!strcmp(entry.uti, uti))
				types.push_back(entry.mime);
	}
	CFRelease(identifiers);
	return types;
}

}  // namespace dawn
