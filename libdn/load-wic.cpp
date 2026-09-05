//
// load-wic.cpp: image loading via Windows Imaging Component
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// WIC is deliberately last in the Windows loader chain.  It's not particularly
// useful due to other loaders mostly being capable of doing the same job:
//
// Duplicated:    BMP, GIF, JPEG, PNG, WebP (Wuffs), AVIF/HEIC* (libheif),
//                DDS, ICO (Glycin), TIFF (LibTIFF), WebP* (libwebp),
//                raw photos* (LibRaw)
// Unique to WIC: JPEG XR

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <propidl.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

// mingw-w64 writes __declspec(dllimport) ahead of extern "C" in this header,
// which GCC remarks upon for every declaration in it.
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
#include <propvarutil.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std;
using Microsoft::WRL::ComPtr;

namespace dawn
{

#ifdef __IWICBitmapSourceTransform_INTERFACE_DEFINED__
static constexpr GUID kPixelFormatDepth = {0x4c9c9f45, 0x1d89, 0x4e31,
	{0x9b, 0xc7, 0x69, 0x34, 0x3a, 0x0d, 0xca, 0x69}};
static constexpr GUID kPixelFormatGain = {0xa884022a, 0xaf13, 0x4c16,
	{0xb7, 0x46, 0x61, 0x9b, 0xf6, 0x18, 0xb8, 0x78}};
#endif

namespace
{

class ComApartment
{
	HRESULT hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

public:
	~ComApartment()
	{
		if (SUCCEEDED(hr_))
			CoUninitialize();
	}

	bool usable() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
	HRESULT result() const { return hr_; }
};

}  // namespace

static string
hr_message(const char *what, HRESULT hr)
{
	char number[16] = {};
	snprintf(number, sizeof number, "0x%08lx", (unsigned long) hr);
	return string(what) + ": Windows error " + number;
}

static void
set_hr_error(Error *error, const char *what, HRESULT hr)
{
	set_error(error, hr_message(what, hr));
}

static bool
create_factory(ComPtr<IWICImagingFactory> &factory, Error *error)
{
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
		CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
		reinterpret_cast<void **>(factory.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) {
		set_hr_error(error, "cannot create WIC factory", hr);
		return false;
	}
	return true;
}

static optional<uint64_t>
metadata_uint(IWICMetadataQueryReader *reader, const wchar_t *name)
{
	if (!reader)
		return nullopt;

	PROPVARIANT value{};
	if (FAILED(reader->GetMetadataByName(name, &value)))
		return nullopt;

	ULONGLONG number = 0;
	optional<uint64_t> out;
	if (SUCCEEDED(PropVariantToUInt64(value, &number)))
		out = number;
	PropVariantClear(&value);
	return out;
}

static Orientation
wic_orientation(IWICMetadataQueryReader *reader)
{
	// The first is a format-independent metadata policy expression. The others
	// cover codecs which expose only their native metadata hierarchy.
	const wchar_t *queries[] = {
		L"System.Photo.Orientation",
		L"/app1/ifd/{ushort=274}",
		L"/ifd/{ushort=274}",
	};
	for (const wchar_t *query : queries) {
		auto value = metadata_uint(reader, query);
		if (value && *value >= 1 && *value <= 8)
			return Orientation(*value);
	}
	return Orientation::Unknown;
}

template <typename Owner>
static vector<uint8_t>
wic_icc(IWICImagingFactory *factory, Owner *owner)
{
	UINT count = 0;
	if (!owner || FAILED(owner->GetColorContexts(0, nullptr, &count)) || !count)
		return {};

	vector<ComPtr<IWICColorContext>> contexts(count);
	vector<IWICColorContext *> raw(count);
	for (UINT i = 0; i < count; i++) {
		if (FAILED(factory->CreateColorContext(
				contexts[i].ReleaseAndGetAddressOf())))
			return {};
		raw[i] = contexts[i].Get();
	}
	UINT actual = 0;
	if (FAILED(owner->GetColorContexts(count, raw.data(), &actual)))
		return {};

	for (UINT i = 0; i < min(count, actual); i++) {
		WICColorContextType type = WICColorContextUninitialized;
		if (FAILED(raw[i]->GetType(&type)) || type != WICColorContextProfile)
			continue;

		UINT size = 0;
		if (FAILED(raw[i]->GetProfileBytes(0, nullptr, &size)) || !size)
			continue;

		vector<uint8_t> profile(size);
		UINT got = 0;
		if (SUCCEEDED(raw[i]->GetProfileBytes(size, profile.data(), &got)) &&
			got <= size) {
			profile.resize(got);
			return profile;
		}
	}
	return {};
}

static bool
valid_dimensions(UINT width, UINT height)
{
	return width && height && width <= kMaxDimension && height <= kMaxDimension;
}

static ImagePtr
decode_bitmap(IWICImagingFactory *factory, IWICBitmapSource *source,
	span<const uint8_t> icc, const OpenContext &ctx, Error *error)
{
	UINT width = 0, height = 0;
	HRESULT hr = source->GetSize(&width, &height);
	if (FAILED(hr)) {
		set_hr_error(error, "cannot read WIC image dimensions", hr);
		return nullptr;
	}
	if (!valid_dimensions(width, height)) {
		set_error(error, "invalid or overflowing WIC image dimensions");
		return nullptr;
	}

	ComPtr<IWICFormatConverter> converter;
	if (FAILED(hr = factory->CreateFormatConverter(
				   converter.ReleaseAndGetAddressOf()))) {
		set_hr_error(error, "cannot create WIC format converter", hr);
		return nullptr;
	}

	bool deep = true;
	hr = converter->Initialize(source, GUID_WICPixelFormat64bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) {
		deep = false;
		converter.Reset();
		if (FAILED(hr = factory->CreateFormatConverter(
					   converter.ReleaseAndGetAddressOf())) ||
			FAILED(hr = converter->Initialize(source,
					   GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
					   nullptr, 0, WICBitmapPaletteTypeCustom))) {
			set_hr_error(error, "cannot convert WIC pixels to BGRA", hr);
			return nullptr;
		}
	}

	ImagePtr image = image_new(width, height);
	if (!image) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	const size_t stride = size_t(width) * (deep ? 8 : 4);
	const size_t size = stride * height;
	if (stride > numeric_limits<UINT>::max() ||
		size > numeric_limits<UINT>::max()) {
		set_error(error, "WIC pixel buffer is too large");
		return nullptr;
	}

	{
		detail::StageClock clk(&OpenTiming::decode_ms);
		if (deep) {
			hr = converter->CopyPixels(
				nullptr, UINT(stride), UINT(size), image->data.data());
		} else {
			vector<uint8_t> pixels(size);
			hr = converter->CopyPixels(
				nullptr, UINT(stride), UINT(size), pixels.data());
			if (SUCCEEDED(hr))
				widen_bgra8_to_bgra16(*image, pixels.data(), stride);
		}
	}
	if (FAILED(hr)) {
		set_hr_error(error, "cannot copy WIC pixels", hr);
		return nullptr;
	}

	shared_ptr<Profile> source_profile;
	if (!icc.empty()) {
		image->icc.assign(icc.begin(), icc.end());
		auto cmm = cmm_or_default(ctx);
		source_profile = cmm->get_profile(image->icc);
		if (source_profile &&
			profile_chromaticities(source_profile.get()).model !=
				ColorModel::Rgb) {
			// IWICFormatConverter has already produced RGB samples. A non-RGB
			// source profile no longer describes those samples.
			add_warning(ctx,
				"WIC converted a non-RGB colour space without preserving its "
				"profile");
			image->icc.clear();
			source_profile.reset();
		}
	}
	if (source_profile)
		image->effective_profile = source_profile;
	ensure_working_premul(
		*image, ctx, source_profile.get(), /*input_premul=*/false);
	return image;
}

static ImagePtr
decode_frame(IWICImagingFactory *factory, IWICBitmapDecoder *decoder,
	IWICBitmapFrameDecode *frame, const OpenContext &ctx, Error *error)
{
	vector<uint8_t> icc = wic_icc(factory, frame);
	if (icc.empty())
		icc = wic_icc(factory, decoder);

	ImagePtr image = decode_bitmap(factory, frame, icc, ctx, error);
	if (image) {
		ComPtr<IWICMetadataQueryReader> reader;
		(void) frame->GetMetadataQueryReader(reader.ReleaseAndGetAddressOf());
		image->orientation = wic_orientation(reader.Get());
	}
	return image;
}

#ifdef __IWICBitmapSourceTransform_INTERFACE_DEFINED__
static ImagePtr
load_heif_representation(IWICBitmapFrameDecode *frame, const GUID &format,
	const char *name, const OpenContext &ctx)
{
	ComPtr<IWICBitmapSourceTransform> transform;
	if (FAILED(frame->QueryInterface(IID_IWICBitmapSourceTransform,
			reinterpret_cast<void **>(transform.ReleaseAndGetAddressOf()))))
		return nullptr;

	WICPixelFormatGUID closest = format;
	if (FAILED(transform->GetClosestPixelFormat(&closest)) ||
		!IsEqualGUID(closest, format))
		return nullptr;

	UINT width = 0, height = 0;
	if (FAILED(frame->GetSize(&width, &height)) ||
		!valid_dimensions(width, height))
		return nullptr;

	const size_t size = size_t(width) * height;
	if (size > numeric_limits<UINT>::max())
		return nullptr;

	vector<uint8_t> plane(size);
	WICPixelFormatGUID requested = format;
	if (FAILED(transform->CopyPixels(nullptr, width, height, &requested,
			WICBitmapTransformRotate0, width, UINT(size), plane.data())))
		return nullptr;

	ImagePtr image = image_new(width, height);
	if (!image)
		return nullptr;

	for (uint32_t y = 0; y < image->height; y++) {
		uint16_t *dst = row_u16(*image, y);
		const uint8_t *src = plane.data() + size_t(y) * width;
		for (uint32_t x = 0; x < image->width; x++) {
			uint16_t v = uint16_t(src[x] * 257u);
			dst[0] = dst[1] = dst[2] = v;
			dst[3] = 65535;
			dst += 4;
		}
	}
	image->text["WIC representation"] = name;
	ensure_working_premul(*image, ctx, nullptr, /*input_premul=*/false);
	return image;
}

static void
append_heif_representations(ImagePtr &head, ImagePtr &tail,
	IWICBitmapFrameDecode *frame, const OpenContext &ctx)
{
	append_page(head, tail,
		load_heif_representation(frame, kPixelFormatDepth, "depth", ctx));
	append_page(head, tail,
		load_heif_representation(frame, kPixelFormatGain, "gain", ctx));
}
#endif

static ImagePtr
load_dds(IWICImagingFactory *factory, IWICBitmapDecoder *decoder,
	const OpenContext &ctx, Error *error)
{
	ComPtr<IWICDdsDecoder> dds;
	HRESULT hr = decoder->QueryInterface(IID_IWICDdsDecoder,
		reinterpret_cast<void **>(dds.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return nullptr;

	WICDdsParameters params{};
	if (FAILED(hr = dds->GetParameters(&params))) {
		set_hr_error(error, "cannot read WIC DDS parameters", hr);
		return nullptr;
	}
	if (!params.ArraySize || !params.MipLevels) {
		set_error(error, "empty WIC DDS image");
		return nullptr;
	}

	ImagePtr head, tail;
	for (UINT array = 0; array < params.ArraySize; array++) {
		for (UINT mip = 0; mip < params.MipLevels; mip++) {
			UINT slices = params.Dimension == WICDdsTexture3D
				? max(1u, params.Depth >> min(mip, 31u))
				: 1u;
			for (UINT slice = 0; slice < slices; slice++) {
				ComPtr<IWICBitmapFrameDecode> frame;
				if (FAILED(hr = dds->GetFrame(array, mip, slice,
							   frame.ReleaseAndGetAddressOf()))) {
					add_warning(
						ctx, hr_message("cannot get WIC DDS subimage", hr));
					continue;
				}

				Error suberror;
				ImagePtr image =
					decode_frame(factory, decoder, frame.Get(), ctx, &suberror);
				if (!image) {
					if (!head) {
						set_error(error, std::move(suberror.message));
						return nullptr;
					}
					add_warning(ctx, suberror.message);
					continue;
				}

				image->text["DDS array"] = to_string(array);
				image->text["DDS mip level"] = to_string(mip);
				image->text["DDS slice"] = to_string(slice);
				append_page(head, tail, std::move(image));
				if (ctx.first_frame_only)
					return head;
			}
		}
	}
	return head;
}

static ImagePtr
load_pages(IWICImagingFactory *factory, IWICBitmapDecoder *decoder,
	const GUID &container, const OpenContext &ctx, Error *error)
{
	if (IsEqualGUID(container, GUID_ContainerFormatDds)) {
		ImagePtr dds = load_dds(factory, decoder, ctx, error);
		if (dds || (error && *error))
			return dds;
	}

	UINT count = 0;
	HRESULT hr = decoder->GetFrameCount(&count);
	if (FAILED(hr) || !count) {
		if (FAILED(hr))
			set_hr_error(error, "cannot enumerate WIC image frames", hr);
		else
			set_error(error, "empty WIC image");
		return nullptr;
	}

	// WIC exposes raw GIF frames, so animation would require disposal and
	// compositing machinery. The dedicated GIF loader handles that; this
	// fallback intentionally returns only WIC's first frame.
	const bool gif = IsEqualGUID(container, GUID_ContainerFormatGif);

	ImagePtr head, tail;
	for (UINT i = 0; i < count; i++) {
		ComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(hr = decoder->GetFrame(i, frame.ReleaseAndGetAddressOf()))) {
			if (!head) {
				set_hr_error(error, "cannot get WIC image frame", hr);
				return nullptr;
			}
			add_warning(
				ctx, hr_message("cannot get later WIC image frame", hr));
			continue;
		}

		Error suberror;
		ImagePtr image =
			decode_frame(factory, decoder, frame.Get(), ctx, &suberror);
		if (!image) {
			if (!head) {
				set_error(error, std::move(suberror.message));
				return nullptr;
			}
			add_warning(ctx, suberror.message);
			continue;
		}
		append_page(head, tail, std::move(image));
		if (ctx.first_frame_only || gif)
			break;

#ifdef __IWICBitmapSourceTransform_INTERFACE_DEFINED__
		if (IsEqualGUID(container, GUID_ContainerFormatHeif))
			append_heif_representations(head, tail, frame.Get(), ctx);
#endif
	}
	return head;
}

static string
wide_to_utf8(wstring_view value)
{
	if (value.empty())
		return {};

	int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		int(value.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};

	string out(size_t(size), '\0');
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
			int(value.size()), out.data(), size, nullptr, nullptr))
		return {};
	return out;
}

static void
append_mime_list(vector<string> &out, wstring_view list)
{
	size_t start = 0;
	while (start < list.size()) {
		size_t end = list.find(L',', start);
		if (end == wstring_view::npos)
			end = list.size();
		while (start < end && iswspace(list[start]))
			start++;
		while (end > start && iswspace(list[end - 1]))
			end--;
		string mime = wide_to_utf8(list.substr(start, end - start));
		if (!mime.empty())
			out.push_back(std::move(mime));
		start = end + 1;
	}
}

ImagePtr
detail::load_wic(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	if (data.size() > numeric_limits<DWORD>::max()) {
		set_error(error, "input is too large for WIC");
		return nullptr;
	}

	ComApartment apartment;
	if (!apartment.usable()) {
		set_hr_error(
			error, "cannot initialize COM for WIC", apartment.result());
		return nullptr;
	}

	ComPtr<IWICImagingFactory> factory;
	if (!create_factory(factory, error))
		return nullptr;

	ComPtr<IWICStream> stream;
	HRESULT hr = factory->CreateStream(stream.ReleaseAndGetAddressOf());
	if (SUCCEEDED(hr))
		hr = stream->InitializeFromMemory(
			const_cast<BYTE *>(data.data()), DWORD(data.size()));
	if (FAILED(hr)) {
		set_hr_error(error, "cannot create WIC input stream", hr);
		return nullptr;
	}

	ComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromStream(stream.Get(), nullptr,
		WICDecodeMetadataCacheOnDemand, decoder.ReleaseAndGetAddressOf());
	if (FAILED(hr)) {
		set_hr_error(error, "unsupported or unrecognized WIC image", hr);
		return nullptr;
	}

	GUID container{};
	if (FAILED(hr = decoder->GetContainerFormat(&container))) {
		set_hr_error(error, "cannot identify WIC image container", hr);
		return nullptr;
	}
	return load_pages(factory.Get(), decoder.Get(), container, ctx, error);
}

vector<string>
detail::wic_media_types()
{
	vector<string> types;
	ComApartment apartment;
	if (!apartment.usable())
		return types;

	ComPtr<IWICImagingFactory> factory;
	if (!create_factory(factory, nullptr))
		return types;

	ComPtr<IEnumUnknown> enumerator;
	if (FAILED(factory->CreateComponentEnumerator(WICDecoder,
			WICComponentEnumerateDefault, enumerator.ReleaseAndGetAddressOf())))
		return types;

	while (true) {
		ComPtr<IUnknown> unknown;
		ULONG fetched = 0;
		HRESULT hr =
			enumerator->Next(1, unknown.ReleaseAndGetAddressOf(), &fetched);
		if (hr != S_OK || fetched != 1)
			break;

		ComPtr<IWICBitmapDecoderInfo> info;
		if (FAILED(unknown->QueryInterface(IID_IWICBitmapDecoderInfo,
				reinterpret_cast<void **>(info.ReleaseAndGetAddressOf()))))
			continue;

		UINT size = 0;
		if (FAILED(info->GetMimeTypes(0, nullptr, &size)) || size <= 1)
			continue;

		vector<wchar_t> value(size);
		UINT actual = 0;
		if (SUCCEEDED(info->GetMimeTypes(size, value.data(), &actual)) &&
			actual)
			append_mime_list(
				types, wstring_view(value.data(), min(size, actual) - 1));
	}
	return types;
}

}  // namespace dawn
