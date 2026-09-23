//
// load-imageio.mm: image loading via macOS ImageIO
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// It would be preferable if macOS didn't differ from other platforms
// in format support, though this is basically its equivalent of Glycin
// or GdkPixbuf.

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstring>
#include <memory>
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
imageio_delay(CFDictionaryRef properties, uint64_t *loops, bool *bump)
{
	const struct {
		CFStringRef container, unclamped, clamped, loops;
		bool bump;
	} animations[] = {
		{kCGImagePropertyGIFDictionary, kCGImagePropertyGIFUnclampedDelayTime,
			kCGImagePropertyGIFDelayTime, kCGImagePropertyGIFLoopCount, true},
		{kCGImagePropertyPNGDictionary, kCGImagePropertyAPNGUnclampedDelayTime,
			kCGImagePropertyAPNGDelayTime, kCGImagePropertyAPNGLoopCount, true},
		{kCGImagePropertyWebPDictionary, kCGImagePropertyWebPUnclampedDelayTime,
			kCGImagePropertyWebPDelayTime, kCGImagePropertyWebPLoopCount, true},
		{kCGImagePropertyHEICSDictionary,
			kCGImagePropertyHEICSUnclampedDelayTime,
			kCGImagePropertyHEICSDelayTime, kCGImagePropertyHEICSLoopCount,
			false},
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
		if (bump)
			*bump = animation.bump;
		return int64_t(seconds * 1000 + 0.5);
	}
	return -1;
}

// Files that carry no profile of their own get CG's sRGB from ImageIO,
// and passing that off as embedded would hide that it is a guess.  Clear it,
// let finish_image() invent sRGB, and admit to it in profile_assumed.
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

// --- Gain maps ---------------------------------------------------------------

// Only one-component 8-bit maps have been seen, 'L008' as CoreVideo calls it.
static unique_ptr<GainMap>
imageio_gain_map_pixels(CFDictionaryRef info, const GainMap &metadata)
{
	auto data =
		(CFDataRef) CFDictionaryGetValue(info, kCGImageAuxiliaryDataInfoData);
	auto description = (CFDictionaryRef) CFDictionaryGetValue(
		info, kCGImageAuxiliaryDataInfoDataDescription);
	double width = 0, height = 0, stride = 0, format = 0;
	if (!data || !description ||
		!imageio_number(description, CFSTR("Width"), &width) ||
		!imageio_number(description, CFSTR("Height"), &height) ||
		!imageio_number(description, CFSTR("BytesPerRow"), &stride) ||
		!imageio_number(description, CFSTR("PixelFormat"), &format) ||
		uint32_t(format) != fourcc('L', '0', '0', '8') || width < 1 ||
		height < 1 || width > kMaxDimension || height > kMaxDimension ||
		stride < width ||
		double(CFDataGetLength(data)) < stride * (height - 1) + width)
		return nullptr;

	ImagePtr pixels = image_new(uint32_t(width), uint32_t(height));
	if (!pixels)
		return nullptr;

	const uint8_t *src = CFDataGetBytePtr(data);
	for (uint32_t y = 0; y < pixels->height; y++) {
		uint16_t *d = row_u16(*pixels, y);
		const uint8_t *s = src + size_t(y) * size_t(stride);
		for (uint32_t x = 0; x < pixels->width; x++, d += 4)
			d[0] = d[1] = d[2] = d[3] = uint16_t(s[x] * 257);
	}
	return make_gain_map(*pixels, metadata, true);
}

// Apple's pre-ISO map, as its cameras put in HEIC and JPEG.  From macOS 15 on,
// ImageIO also reports ISO 21496-1 maps, but describes their metadata its own
// way, which nothing here has been checked against, so those are left alone.
API_AVAILABLE(macos(11.0))
static void
imageio_gain_map(CGImageSourceRef source, size_t index,
	CFDictionaryRef properties, Image &image, const OpenContext &ctx)
{
	CFDictionaryRef info = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
		source, index, kCGImageAuxiliaryDataTypeHDRGainMap);
	if (!info)
		return;

	string xmp;
	auto metadata = (CGImageMetadataRef) CFDictionaryGetValue(
		info, kCGImageAuxiliaryDataInfoMetadata);
	CFDataRef packet =
		metadata ? CGImageMetadataCreateXMPData(metadata, nullptr) : nullptr;
	if (packet) {
		xmp.assign((const char *) CFDataGetBytePtr(packet),
			size_t(CFDataGetLength(packet)));
		CFRelease(packet);
	}

	// ImageIO hands out no Exif, but it does parse Apple's maker note.
	double maker33 = NAN, maker48 = 0;
	CFTypeRef maker =
		CFDictionaryGetValue(properties, kCGImagePropertyMakerAppleDictionary);
	if (maker && CFGetTypeID(maker) == CFDictionaryGetTypeID()) {
		(void) imageio_number((CFDictionaryRef) maker, CFSTR("33"), &maker33);
		(void) imageio_number((CFDictionaryRef) maker, CFSTR("48"), &maker48);
	}

	const double headroom =
		apple_gain_map_headroom_from_maker(xmp, maker33, maker48);
	if (headroom > 0) {
		const GainMap map = apple_gain_map(headroom);
		if (gain_map_applies(map, ctx))
			image.gain_map = imageio_gain_map_pixels(info, map);
	}
	CFRelease(info);
}

// --- Pixels ------------------------------------------------------------------

// Core Graphics converts from the image's colour space to the context's,
// so giving the context the source's own space makes drawing an identity,
// and leaves all colour management to lcms2, as with every other loader.
// Anything that cannot be one--Gray, CMYK, Indexed, Lab--gets a real
// conversion to sRGB instead, which is then no longer an assumption.
static CGColorSpaceRef
imageio_target_space(CGImageRef cg, CFDataRef *icc, Error *error)
{
	CGColorSpaceRef source = CGImageGetColorSpace(cg);
	if (source && CGColorSpaceGetModel(source) == kCGColorSpaceModelRGB &&
		(*icc = CGColorSpaceCopyICCData(source)))
		return CGColorSpaceRetain(source);

	CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	if (!srgb) {
		set_error(error, _("cannot create an sRGB colour space"));
		return nullptr;
	}
	*icc = CGColorSpaceCopyICCData(srgb);
	return srgb;
}

// Extended range, as EXR, Radiance and float TIFF come, is linear light with
// 1.0 at SDR white.  In BT.2020, little of scRGB stays negative.
static ImagePtr
load_imageio_hdr(CGImageRef cg, const OpenContext &ctx, Error *error)
{
	size_t width = CGImageGetWidth(cg), height = CGImageGetHeight(cg);
	CGColorSpaceRef space =
		CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
	if (!space) {
		set_error(error, _("failed to describe the colour space"));
		return nullptr;
	}

	vector<float> rgba(width * height * 4);
	CGContextRef context = CGBitmapContextCreate(rgba.data(), width, height, 32,
		width * 4 * sizeof(float), space,
		CGBitmapInfo(kCGImageAlphaPremultipliedLast) |
			kCGBitmapFloatComponents | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(space);
	if (!context) {
		set_error(error, _("cannot create a bitmap context"));
		return nullptr;
	}
	CGContextSetBlendMode(context, kCGBlendModeCopy);
	CGContextDrawImage(
		context, CGRectMake(0, 0, double(width), double(height)), cg);
	CGContextRelease(context);

	ImagePtr image = image_new(uint32_t(width), uint32_t(height));
	if (!image) {
		set_error(error, _("image allocation failure"));
		return nullptr;
	}
	if (!split_hdr(*image, ctx, rgba, true, kRec2020Primaries, error))
		return nullptr;
	return image;
}

static ImagePtr
load_imageio_image(CGImageRef cg, const OpenContext &ctx, Error *error)
{
	size_t width = CGImageGetWidth(cg), height = CGImageGetHeight(cg);
	if (!width || !height || width > kMaxDimension || height > kMaxDimension) {
		set_error(error, _("image dimensions overflow"));
		return nullptr;
	}

	CGColorSpaceRef source = CGImageGetColorSpace(cg);
	if (source && CGColorSpaceUsesExtendedRange(source))
		return load_imageio_hdr(cg, ctx, error);

	CFDataRef icc = nullptr;
	CGColorSpaceRef space = imageio_target_space(cg, &icc, error);
	if (!space)
		return nullptr;

	// Only EXR, HDR, PSD and 16-bit TIFF ever need the deep path.
	bool deep = CGImageGetBitsPerComponent(cg) > 8;
	size_t bits = deep ? 16 : 8;
	CGBitmapInfo info = CGBitmapInfo(kCGImageAlphaPremultipliedLast) |
		(deep ? kCGBitmapByteOrder16Little : kCGBitmapByteOrder32Big);

	size_t stride = width * 4 * (bits / 8);
	vector<uint16_t> pixels(stride * height / 2);
	auto raw = (uint8_t *) pixels.data();
	CGContextRef context =
		CGBitmapContextCreate(raw, width, height, bits, stride, space, info);
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
		set_error(error, _("cannot create a bitmap context"));
		return nullptr;
	}

	ImagePtr image = image_new(uint32_t(width), uint32_t(height));
	if (!image) {
		if (icc)
			CFRelease(icc);
		set_error(error, _("image allocation failure"));
		return nullptr;
	}

	// Core Graphics always premultiplies, which is what finish_image()
	// is then told about by our caller.
	if (deep)
		pack_rgba16le_to_bgra16(*image, pixels.data(), stride, 16);
	else
		pack_rgba8_to_bgra16(*image, raw, stride);

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
		set_error(error, _("incomplete or unsupported ImageIO image"));
		return nullptr;
	}

	ImagePtr head, tail;
	bool animated = false, bump = false;
	uint64_t loops = 0;
	size_t count = CGImageSourceGetCount(source);
	for (size_t i = 0; i < count; i++) {
		CFDictionaryRef properties =
			CGImageSourceCopyPropertiesAtIndex(source, i, options);
		int64_t duration = properties
			? imageio_delay(
				  properties, i ? nullptr : &loops, i ? nullptr : &bump)
			: -1;
		if (!i)
			animated = duration >= 0;

		Error suberror;
		ImagePtr image;
		bool hdr = false;
		CGImageRef cg = CGImageSourceCreateImageAtIndex(source, i, options);
		if (!cg) {
			set_error(&suberror, _("ImageIO decoding error"));
		} else {
			CGColorSpaceRef space = CGImageGetColorSpace(cg);
			hdr = space && CGColorSpaceUsesExtendedRange(space);
			image = load_imageio_image(cg, ctx, &suberror);
			CGImageRelease(cg);
		}
		if (image && properties) {
			image->orientation = imageio_orientation(properties);
			if (!CFDictionaryContainsKey(
					properties, kCGImagePropertyProfileName))
				imageio_drop_invented_srgb(*image);
			// A true HDR base ignores the file's map, as in HEIF.
			if (@available(macOS 11, *)) {
				if (!animated && !hdr && ctx.gain_maps)
					imageio_gain_map(source, i, properties, *image, ctx);
			}
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
			image->browser_animation_bump = bump;
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
		set_error(error, _("empty ImageIO image"));
		return nullptr;
	}

	// Only split_hdr() names a profile, for a base it leaves straight.
	head->loops = loops;
	for (Image *page = head.get(); page; page = page->page_next.get())
		finish_frames(*page, ctx, page->effective_profile.get(),
			/*input_premul=*/!page->effective_profile);
	return head;
}

ImagePtr
load_imageio(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// The caller's buffer outlives this call, and ImageIO's own cache would
	// only fight the thumbnailer's memory accounting.
	CFDataRef wrapper = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
		data.data(), CFIndex(data.size()), kCFAllocatorNull);
	if (!wrapper) {
		set_error(error, _("image allocation failure"));
		return nullptr;
	}

	const void *keys[] = {
		kCGImageSourceShouldCache, kCGImageSourceShouldAllowFloat};
	const void *values[] = {kCFBooleanFalse, kCFBooleanTrue};
	CFDictionaryRef options =
		CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CGImageSourceRef source = CGImageSourceCreateWithData(wrapper, options);
	CFRelease(wrapper);

	ImagePtr image;
	if (source) {
		image = load_imageio_indexes(source, options, ctx, error);
		CFRelease(source);
	} else {
		set_error(error, _("not an ImageIO-decodable image"));
	}

	CFRelease(options);
	return image;
}

// --- Advertised types --------------------------------------------------------

vector<string>
imageio_media_types()
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
