//
// libdnrs: image-rs and other decoders exposed through a small C ABI
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#![allow(non_camel_case_types)]

use std::ffi::{c_char, CStr, CString};
use std::io::{BufRead, Cursor, Seek};
use std::mem;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::OnceLock;

use image::{
	AnimationDecoder, DynamicImage, ImageDecoder, ImageFormat, ImageReader,
};
#[cfg(feature = "raw")]
use libopenraw::{metadata::Value, Bitmap};

const MAX_DIMENSION: u32 = 65_535;
const MAX_DECODE_ALLOC: u64 = 512 * 1024 * 1024;
const MAX_TEXT_LENGTH: usize = 2 * 1024 * 1024;

#[repr(C)]
pub struct dnrs_blob {
	data: *const u8,
	length: usize,
}

#[repr(C)]
pub struct dnrs_text {
	key: *const c_char,
	value: *const c_char,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum dnrs_pixel_format {
	Gray8,
	GrayAlpha8,
	Rgb8,
	Rgba8,
	Gray16Le,
	GrayAlpha16Le,
	Rgb16Le,
	Rgba16Le,
}

#[repr(C)]
pub struct dnrs_document_info {
	codec: *const c_char,
	page_count: u32,
	icc: dnrs_blob,
	exif: dnrs_blob,
	xmp: dnrs_blob,
	text: *const dnrs_text,
	text_length: usize,
}

#[repr(C)]
pub struct dnrs_page_info {
	width: u32,
	height: u32,
	frame_count: u32,
	orientation: u32,
	loop_count: u64,
}

#[repr(C)]
pub struct dnrs_frame {
	width: u32,
	height: u32,
	stride: usize,
	format: dnrs_pixel_format,
	data: *mut u8,
	length: usize,
	duration_ms: u64,
}

pub struct dnrs_error {
	message: CString,
}

struct Frame {
	width: u32,
	height: u32,
	stride: usize,
	format: dnrs_pixel_format,
	pixels: Vec<u8>,
	duration_ms: u64,
}

struct Page {
	width: u32,
	height: u32,
	orientation: u32,
	loop_count: u64,
	frames: Vec<Frame>,
}

#[derive(Default)]
struct Metadata {
	icc: Vec<u8>,
	exif: Vec<u8>,
	xmp: Vec<u8>,
	orientation: u32,
	text: Vec<(String, String)>,
}

pub struct dnrs_decoder {
	codec: CString,
	icc: Vec<u8>,
	exif: Vec<u8>,
	xmp: Vec<u8>,
	_text_storage: Vec<(CString, CString)>,
	text: Vec<dnrs_text>,
	pages: Vec<Page>,
	page_index: Option<usize>,
	frame_index: usize,
}

fn image_limits() -> image::Limits {
	let mut limits = image::Limits::default();
	limits.max_image_width = Some(MAX_DIMENSION);
	limits.max_image_height = Some(MAX_DIMENSION);
	limits.max_alloc = Some(MAX_DECODE_ALLOC);
	limits
}

fn set_decoder_limits(decoder: &mut impl ImageDecoder) -> Result<(), String> {
	decoder
		.set_limits(image_limits())
		.map_err(|error| error.to_string())
}

static MIME_TYPES: &[&CStr] = &[
	c"image/bmp",
	c"image/gif",
	c"image/jpeg",
	c"image/png",
	c"image/qoi",
	c"image/tiff",
	c"image/vnd.microsoft.icon",
	c"image/vnd.radiance",
	c"image/webp",
	c"image/x-dds",
	c"image/x-exr",
	c"image/x-farbfeld",
	c"image/x-portable-anymap",
	c"image/x-tga",
	c"image/x-win-bitmap",
	c"image/x-xbitmap",
	c"image/x-xpixmap",
	#[cfg(feature = "jpeg2000")]
	c"image/jp2",
	#[cfg(feature = "jpeg2000")]
	c"image/x-jp2-codestream",
	#[cfg(feature = "raw")]
	c"image/x-dcraw",
];

fn set_error(out: *mut *mut dnrs_error, message: impl ToString) {
	if out.is_null() {
		return;
	}
	let message = message.to_string().replace('\0', " ");
	unsafe {
		*out = Box::into_raw(Box::new(dnrs_error {
			message: CString::new(message).expect("NULs were removed"),
		}));
	}
}

fn ffi_result<T>(
	error: *mut *mut dnrs_error,
	default: T,
	f: impl FnOnce() -> Result<T, String>,
) -> T {
	if !error.is_null() {
		unsafe { *error = ptr::null_mut() };
	}
	match catch_unwind(AssertUnwindSafe(f)) {
		Ok(Ok(value)) => value,
		Ok(Err(message)) => {
			set_error(error, message);
			default
		}
		Err(_) => {
			set_error(error, "decoder panic");
			default
		}
	}
}

fn bytes_per_pixel(format: dnrs_pixel_format) -> usize {
	match format {
		dnrs_pixel_format::Gray8 => 1,
		dnrs_pixel_format::GrayAlpha8 => 2,
		dnrs_pixel_format::Rgb8 => 3,
		dnrs_pixel_format::Rgba8 => 4,
		dnrs_pixel_format::Gray16Le => 2,
		dnrs_pixel_format::GrayAlpha16Le => 4,
		dnrs_pixel_format::Rgb16Le => 6,
		dnrs_pixel_format::Rgba16Le => 8,
	}
}

fn make_frame(
	width: u32,
	height: u32,
	format: dnrs_pixel_format,
	pixels: Vec<u8>,
	duration_ms: u64,
) -> Result<Frame, String> {
	if width == 0 || width > MAX_DIMENSION {
		return Err("invalid image width".into());
	}
	if height == 0 || height > MAX_DIMENSION {
		return Err("invalid image height".into());
	}
	let stride = (width as usize)
		.checked_mul(bytes_per_pixel(format))
		.ok_or("image dimensions overflow")?;
	let expected = stride
		.checked_mul(height as usize)
		.ok_or("image dimensions overflow")?;
	if pixels.len() != expected {
		return Err("decoder returned a truncated frame".into());
	}
	Ok(Frame {
		width,
		height,
		stride,
		format,
		pixels,
		duration_ms,
	})
}

fn frame_from_dynamic(
	image: DynamicImage,
	duration_ms: u64,
) -> Result<Frame, String> {
	let (width, height) = (image.width(), image.height());
	let (format, pixels) = match image {
		DynamicImage::ImageLuma8(v) => (dnrs_pixel_format::Gray8, v.into_raw()),
		DynamicImage::ImageLumaA8(v) => {
			(dnrs_pixel_format::GrayAlpha8, v.into_raw())
		}
		DynamicImage::ImageRgb8(v) => (dnrs_pixel_format::Rgb8, v.into_raw()),
		DynamicImage::ImageRgba8(v) => (dnrs_pixel_format::Rgba8, v.into_raw()),
		DynamicImage::ImageLuma16(v) => {
			(dnrs_pixel_format::Gray16Le, u16_to_le(v.into_raw()))
		}
		DynamicImage::ImageLumaA16(v) => {
			(dnrs_pixel_format::GrayAlpha16Le, u16_to_le(v.into_raw()))
		}
		DynamicImage::ImageRgb16(v) => {
			(dnrs_pixel_format::Rgb16Le, u16_to_le(v.into_raw()))
		}
		DynamicImage::ImageRgba16(v) => {
			(dnrs_pixel_format::Rgba16Le, u16_to_le(v.into_raw()))
		}
		other => (
			dnrs_pixel_format::Rgba16Le,
			u16_to_le(other.to_rgba16().into_raw()),
		),
	};
	make_frame(width, height, format, pixels, duration_ms)
}

fn u16_to_le(samples: Vec<u16>) -> Vec<u8> {
	let mut bytes = Vec::with_capacity(samples.len() * 2);
	for sample in samples {
		bytes.extend_from_slice(&sample.to_le_bytes());
	}
	bytes
}

fn animation_frames<I>(
	frames: I,
	first_frame_only: bool,
) -> Result<Vec<Frame>, String>
where
	I: IntoIterator<Item = image::ImageResult<image::Frame>>,
{
	let mut decoded = Vec::new();
	let mut total = 0_u64;
	for frame in
		frames
			.into_iter()
			.take(if first_frame_only { 1 } else { usize::MAX })
	{
		let frame = frame.map_err(|e| e.to_string())?;
		let (numer, denom) = frame.delay().numer_denom_ms();
		let duration = if denom == 0 {
			0
		} else {
			(u64::from(numer) + u64::from(denom) / 2) / u64::from(denom)
		};
		let frame = frame_from_dynamic(
			DynamicImage::ImageRgba8(frame.into_buffer()),
			duration,
		)?;
		push_frame(&mut decoded, &mut total, frame)?;
	}
	Ok(decoded)
}

fn push_frame(
	frames: &mut Vec<Frame>,
	total: &mut u64,
	frame: Frame,
) -> Result<(), String> {
	*total = total
		.checked_add(frame.pixels.len() as u64)
		.ok_or_else(|| "decoded image size overflow".to_string())?;
	if *total > MAX_DECODE_ALLOC {
		return Err("decoded image exceeds allocation limit".into());
	}
	frames.push(frame);
	Ok(())
}

fn collect_metadata(
	decoder: &mut impl ImageDecoder,
) -> Result<Metadata, String> {
	Ok(Metadata {
		icc: decoder
			.icc_profile()
			.map_err(|error| error.to_string())?
			.unwrap_or_default(),
		exif: decoder
			.exif_metadata()
			.map_err(|error| error.to_string())?
			.unwrap_or_default(),
		xmp: decoder
			.xmp_metadata()
			.map_err(|error| error.to_string())?
			.unwrap_or_default(),
		orientation: u32::from(
			decoder
				.orientation()
				.map_err(|error| error.to_string())?
				.to_exif(),
		),
		text: Vec::new(),
	})
}

fn decode_frame<D: ImageDecoder>(
	mut decoder: D,
) -> Result<(Frame, Metadata), String> {
	set_decoder_limits(&mut decoder)?;
	let metadata = collect_metadata(&mut decoder)?;
	let image =
		DynamicImage::from_decoder(decoder).map_err(|e| e.to_string())?;
	Ok((frame_from_dynamic(image, 0)?, metadata))
}

fn decode_still<R: BufRead + Seek>(
	mut reader: ImageReader<R>,
) -> Result<(Frame, Metadata), String> {
	reader.limits(image_limits());
	decode_frame(reader.into_decoder().map_err(|e| e.to_string())?)
}

fn decoder_from_still<D: ImageDecoder>(
	codec: &str,
	decoder: D,
) -> Result<dnrs_decoder, String> {
	let (frame, metadata) = decode_frame(decoder)?;
	decoder_from_frames(codec.into(), vec![frame], 0, metadata)
}

fn collect_png_text(data: &[u8]) -> Result<Vec<(String, String)>, String> {
	let mut decoder = png::Decoder::new(Cursor::new(data));
	decoder.set_limits(png::Limits {
		bytes: usize::try_from(MAX_DECODE_ALLOC).unwrap_or(usize::MAX),
	});
	let mut reader = decoder.read_info().map_err(|error| error.to_string())?;
	let info = reader.info();
	if info.width == 0
		|| info.height == 0
		|| info.width > MAX_DIMENSION
		|| info.height > MAX_DIMENSION
	{
		return Err("invalid image dimensions".into());
	}
	reader.finish().map_err(|error| error.to_string())?;

	let info = reader.info();
	let mut text = Vec::new();
	for chunk in &info.uncompressed_latin1_text {
		text.push((chunk.keyword.clone(), chunk.text.clone()));
	}
	for chunk in &info.compressed_latin1_text {
		let mut chunk = chunk.clone();
		chunk
			.decompress_text_with_limit(MAX_TEXT_LENGTH)
			.map_err(|error| error.to_string())?;
		text.push((
			chunk.keyword.clone(),
			chunk.get_text().map_err(|error| error.to_string())?,
		));
	}
	for chunk in &info.utf8_text {
		if chunk.keyword == "XML:com.adobe.xmp" {
			continue;
		}
		let mut chunk = chunk.clone();
		chunk
			.decompress_text_with_limit(MAX_TEXT_LENGTH)
			.map_err(|error| error.to_string())?;
		text.push((
			chunk.keyword.clone(),
			chunk.get_text().map_err(|error| error.to_string())?,
		));
	}
	Ok(text)
}

fn tiff_select_page(page: &mut [u8], offset: u64) -> Result<(), String> {
	if page.len() < 16 {
		return Err("invalid TIFF header".into());
	}
	match page.get(0..4) {
		Some([b'I', b'I', 42, 0]) => {
			let offset = u32::try_from(offset)
				.map_err(|_| "invalid TIFF page offset")?;
			page[4..8].copy_from_slice(&offset.to_le_bytes());
		}
		Some([b'M', b'M', 0, 42]) => {
			let offset = u32::try_from(offset)
				.map_err(|_| "invalid TIFF page offset")?;
			page[4..8].copy_from_slice(&offset.to_be_bytes());
		}
		Some([b'I', b'I', 43, 0]) => {
			page[8..16].copy_from_slice(&offset.to_le_bytes())
		}
		Some([b'M', b'M', 0, 43]) => {
			page[8..16].copy_from_slice(&offset.to_be_bytes())
		}
		_ => return Err("invalid TIFF header".into()),
	}
	Ok(())
}

fn decode_tiff_pages(
	data: &[u8],
	first_frame_only: bool,
) -> Result<Vec<(Frame, Metadata)>, String> {
	let mut scanner = tiff::decoder::Decoder::new(Cursor::new(data))
		.map_err(|error| error.to_string())?;

	// image-rs only ever decodes the first IFD, so retarget the header of
	// one working copy at each page in turn.
	let mut page = data.to_vec();
	let mut pages = Vec::new();
	let mut total = 0_u64;
	loop {
		let (width, height) =
			scanner.dimensions().map_err(|error| error.to_string())?;
		if width == 0
			|| height == 0
			|| width > MAX_DIMENSION
			|| height > MAX_DIMENSION
		{
			return Err("invalid image dimensions".into());
		}
		let offset = scanner
			.ifd_pointer()
			.ok_or_else(|| "missing TIFF page offset".to_string())?
			.0;
		tiff_select_page(&mut page, offset)?;
		let decoded = decode_still(ImageReader::with_format(
			Cursor::new(page.as_slice()),
			ImageFormat::Tiff,
		))?;
		total = total
			.checked_add(decoded.0.pixels.len() as u64)
			.ok_or_else(|| "decoded image size overflow".to_string())?;
		if total > MAX_DECODE_ALLOC {
			return Err("decoded image exceeds allocation limit".into());
		}
		pages.push(decoded);
		if first_frame_only || !scanner.more_images() {
			break;
		}
		scanner.next_image().map_err(|error| error.to_string())?;
	}
	Ok(pages)
}

fn decode_animation<I>(
	frames: I,
	loops: image::metadata::LoopCount,
	metadata: Metadata,
	first_frame_only: bool,
) -> Result<(Vec<Frame>, Metadata, u64), String>
where
	I: IntoIterator<Item = image::ImageResult<image::Frame>>,
{
	let loops = match loops {
		image::metadata::LoopCount::Infinite => 0,
		image::metadata::LoopCount::Finite(n) => u64::from(n.get()),
	};
	Ok((animation_frames(frames, first_frame_only)?, metadata, loops))
}

fn decode_still_frames(
	data: &[u8],
	format: ImageFormat,
) -> Result<(Vec<Frame>, Metadata, u64), String> {
	let reader = ImageReader::with_format(Cursor::new(data), format);
	let (frame, metadata) = decode_still(reader)?;
	Ok((vec![frame], metadata, 0))
}

fn decode_image_rs(
	data: &[u8],
	format: ImageFormat,
	first_frame_only: bool,
) -> Result<dnrs_decoder, String> {
	let codec = format!("image-rs/{format:?}");
	let (frames, mut metadata, loops) = match format {
		ImageFormat::Gif => {
			let mut decoder =
				image::codecs::gif::GifDecoder::new(Cursor::new(data))
					.map_err(|e| e.to_string())?;
			set_decoder_limits(&mut decoder)?;
			let metadata = collect_metadata(&mut decoder)?;
			let loops = decoder.loop_count();
			decode_animation(
				decoder.into_frames(),
				loops,
				metadata,
				first_frame_only,
			)?
		}
		ImageFormat::Png => {
			let mut decoder =
				image::codecs::png::PngDecoder::new(Cursor::new(data))
					.map_err(|e| e.to_string())?;
			set_decoder_limits(&mut decoder)?;
			let metadata = collect_metadata(&mut decoder)?;
			if decoder.is_apng().map_err(|e| e.to_string())? {
				let apng = decoder.apng().map_err(|e| e.to_string())?;
				let loops = apng.loop_count();
				decode_animation(
					apng.into_frames(),
					loops,
					metadata,
					first_frame_only,
				)?
			} else {
				decode_still_frames(data, format)?
			}
		}
		ImageFormat::WebP => {
			let mut decoder =
				image::codecs::webp::WebPDecoder::new(Cursor::new(data))
					.map_err(|e| e.to_string())?;
			set_decoder_limits(&mut decoder)?;
			let metadata = collect_metadata(&mut decoder)?;
			if decoder.has_animation() {
				let loops = decoder.loop_count();
				decode_animation(
					decoder.into_frames(),
					loops,
					metadata,
					first_frame_only,
				)?
			} else {
				decode_still_frames(data, format)?
			}
		}
		ImageFormat::Tiff => {
			return decoder_from_pages(
				codec,
				decode_tiff_pages(data, first_frame_only)?,
			)
		}
		_ => decode_still_frames(data, format)?,
	};
	if format == ImageFormat::Png {
		metadata.text = collect_png_text(data)?;
	}
	decoder_from_frames(codec, frames, loops, metadata)
}

#[cfg(feature = "jpeg2000")]
fn decode_jpeg2000(data: &[u8]) -> Result<dnrs_decoder, String> {
	let decoder =
		hayro_jpeg2000::integration::Jp2Decoder::new(Cursor::new(data))
			.map_err(|e| e.to_string())?;
	decoder_from_still("hayro-jpeg2000", decoder)
}

#[cfg(feature = "raw")]
fn frame_from_rendered(rendered: &impl Bitmap) -> Result<Frame, String> {
	let (format, pixels) = if let Some(data) = rendered.data16() {
		(dnrs_pixel_format::Rgb16Le, u16_to_le(data.to_vec()))
	} else if let Some(data) = rendered.data8() {
		(dnrs_pixel_format::Rgb8, data.to_vec())
	} else {
		return Err("libopenraw returned no pixels".into());
	};
	make_frame(rendered.width(), rendered.height(), format, pixels, 0)
}

#[cfg(feature = "raw")]
fn decode_raw(
	rawfile: &libopenraw::RawFileHandle,
) -> Result<dnrs_decoder, String> {
	// libopenraw has no allocation-limit API. Reject implausible dimensions
	// from the container metadata before asking it to unpack the raw plane.
	for key in [
		"Exif.Image.ImageWidth",
		"Exif.Image.ImageLength",
		"Exif.Photo.PixelXDimension",
		"Exif.Photo.PixelYDimension",
	] {
		if let Some(dimension) = rawfile
			.metadata_value(key)
			.and_then(|value| value.integer())
		{
			if dimension == 0 || dimension > MAX_DIMENSION {
				return Err("invalid raw image dimensions".into());
			}
		}
	}

	let xmp = rawfile
		.metadata_value("Exif.Image.ApplicationNotes")
		.and_then(|value| match value {
			Value::Bytes(bytes) => Some(bytes),
			_ => None,
		})
		.unwrap_or_default();
	let raw = rawfile.raw_data(false).map_err(|e| e.to_string())?;
	if raw.width() == 0
		|| raw.height() == 0
		|| raw.width() > MAX_DIMENSION
		|| raw.height() > MAX_DIMENSION
	{
		return Err("invalid raw image dimensions".into());
	}

	// Developing camera samples is format-specific processing rather than
	// display policy. libopenraw produces tagged sRGB values here; libdn
	// still owns conversion from that source space to the display target.
	let rendered = raw
		.rendered_image(&libopenraw::RenderingOptions::default())
		.map_err(|e| e.to_string())?;
	let metadata = Metadata {
		xmp,
		..Metadata::default()
	};
	let frames = vec![frame_from_rendered(&rendered)?];
	let mut decoder =
		decoder_from_frames("libopenraw".into(), frames, 0, metadata)?;
	decoder.pages[0].orientation = rawfile.orientation();
	Ok(decoder)
}

fn decode_image(
	data: Vec<u8>,
	first_frame_only: bool,
) -> Result<dnrs_decoder, String> {
	image_extras::register();

	if let Ok(format) = image::guess_format(&data) {
		return decode_image_rs(&data, format, first_frame_only);
	}
	#[cfg(feature = "jpeg2000")]
	if data.starts_with(&[0xff, 0x4f, 0xff, 0x51])
		|| data.starts_with(&[0, 0, 0, 12, b'j', b'P', b' ', b' '])
	{
		return decode_jpeg2000(&data);
	}
	if let Ok(decoder) =
		image::codecs::tga::TgaDecoder::new(Cursor::new(data.as_slice()))
	{
		return decoder_from_still("image-rs/Tga", decoder);
	}
	// XPM registers a signature with image, while XBM is C source with no
	// magic number and has to be tried explicitly.
	if let Ok(reader) =
		ImageReader::new(Cursor::new(data.as_slice())).with_guessed_format()
	{
		if let Ok((frame, metadata)) = decode_still(reader) {
			return decoder_from_frames(
				"image-extras".into(),
				vec![frame],
				0,
				metadata,
			);
		}
	}
	if let Ok(decoder) =
		image_extras::xbm::XbmDecoder::new(Cursor::new(data.as_slice()))
	{
		return decoder_from_still("image-extras/XBM", decoder);
	}
	#[cfg(feature = "raw")]
	if let Ok(rawfile) = libopenraw::rawfile_from_memory(data, None) {
		return decode_raw(&rawfile);
	}
	Err("unrecognised image data".into())
}

fn decoder_from_pages(
	codec: String,
	decoded: Vec<(Frame, Metadata)>,
) -> Result<dnrs_decoder, String> {
	let mut first = None;
	let mut pages = Vec::new();
	for (frame, metadata) in decoded {
		pages.push(Page {
			width: frame.width,
			height: frame.height,
			orientation: metadata.orientation,
			loop_count: 0,
			frames: vec![frame],
		});
		first.get_or_insert(metadata);
	}
	let first = first.ok_or("the image has no pages")?;
	make_decoder(codec, pages, first)
}

fn decoder_from_frames(
	codec: String,
	frames: Vec<Frame>,
	loop_count: u64,
	metadata: Metadata,
) -> Result<dnrs_decoder, String> {
	let first = frames
		.first()
		.ok_or_else(|| "the image has no frames".to_string())?;
	let page = Page {
		width: first.width,
		height: first.height,
		orientation: metadata.orientation,
		loop_count,
		frames,
	};
	make_decoder(codec, vec![page], metadata)
}

fn make_decoder(
	codec: String,
	pages: Vec<Page>,
	metadata: Metadata,
) -> Result<dnrs_decoder, String> {
	let text_storage = metadata
		.text
		.into_iter()
		.map(|(key, value)| {
			let key = CString::new(key.replace('\0', " "))
				.map_err(|_| "invalid metadata key")?;
			let value = CString::new(value.replace('\0', " "))
				.map_err(|_| "invalid metadata value")?;
			Ok((key, value))
		})
		.collect::<Result<Vec<_>, String>>()?;
	let text = text_storage
		.iter()
		.map(|(key, value)| dnrs_text {
			key: key.as_ptr(),
			value: value.as_ptr(),
		})
		.collect();
	Ok(dnrs_decoder {
		codec: CString::new(codec).map_err(|_| "invalid codec name")?,
		icc: metadata.icc,
		exif: metadata.exif,
		xmp: metadata.xmp,
		_text_storage: text_storage,
		text,
		pages,
		page_index: None,
		frame_index: 0,
	})
}

/// # Safety
///
/// `length`, when non-null, must point to writable memory.
#[no_mangle]
pub unsafe extern "C" fn dnrs_mime_types(
	length: *mut usize,
) -> *const *const c_char {
	static TYPES: OnceLock<Vec<usize>> = OnceLock::new();
	let types = TYPES.get_or_init(|| {
		MIME_TYPES.iter().map(|s| s.as_ptr() as usize).collect()
	});
	if !length.is_null() {
		unsafe { *length = types.len() };
	}
	types.as_ptr().cast()
}

/// # Safety
///
/// `data` must point to `length` readable bytes, unless `length` is zero.
/// `error`, when non-null, must point to writable memory.
#[no_mangle]
pub unsafe extern "C" fn dnrs_decoder_new(
	data: *const u8,
	length: usize,
	first_frame_only: bool,
	error: *mut *mut dnrs_error,
) -> *mut dnrs_decoder {
	ffi_result(error, ptr::null_mut(), || {
		if data.is_null() && length != 0 {
			return Err("null input pointer".into());
		}
		if length > isize::MAX as usize {
			return Err("input is too large".into());
		}
		let input = if length == 0 {
			Vec::new()
		} else {
			unsafe { std::slice::from_raw_parts(data, length) }.to_vec()
		};
		decode_image(input, first_frame_only)
			.map(|decoder| Box::into_raw(Box::new(decoder)))
	})
}

/// # Safety
///
/// All non-null arguments must point to live objects of their declared type.
#[no_mangle]
pub unsafe extern "C" fn dnrs_decoder_get_info(
	decoder: *const dnrs_decoder,
	info: *mut dnrs_document_info,
	error: *mut *mut dnrs_error,
) -> bool {
	ffi_result(error, false, || {
		let decoder = unsafe { decoder.as_ref() }.ok_or("null decoder")?;
		let info = unsafe { info.as_mut() }.ok_or("null document info")?;
		*info = dnrs_document_info {
			codec: decoder.codec.as_ptr(),
			page_count: u32::try_from(decoder.pages.len()).unwrap_or(u32::MAX),
			icc: dnrs_blob {
				data: decoder.icc.as_ptr(),
				length: decoder.icc.len(),
			},
			exif: dnrs_blob {
				data: decoder.exif.as_ptr(),
				length: decoder.exif.len(),
			},
			xmp: dnrs_blob {
				data: decoder.xmp.as_ptr(),
				length: decoder.xmp.len(),
			},
			text: decoder.text.as_ptr(),
			text_length: decoder.text.len(),
		};
		Ok(true)
	})
}

/// # Safety
///
/// All non-null arguments must point to live objects of their declared type.
#[no_mangle]
pub unsafe extern "C" fn dnrs_decoder_next_page(
	decoder: *mut dnrs_decoder,
	info: *mut dnrs_page_info,
	error: *mut *mut dnrs_error,
) -> bool {
	ffi_result(error, false, || {
		let decoder = unsafe { decoder.as_mut() }.ok_or("null decoder")?;
		let next = decoder.page_index.map_or(0, |n| n + 1);
		let Some(page) = decoder.pages.get(next) else {
			return Ok(false);
		};
		let info = unsafe { info.as_mut() }.ok_or("null page info")?;
		*info = dnrs_page_info {
			width: page.width,
			height: page.height,
			frame_count: u32::try_from(page.frames.len()).unwrap_or(u32::MAX),
			orientation: page.orientation,
			loop_count: page.loop_count,
		};
		decoder.page_index = Some(next);
		decoder.frame_index = 0;
		Ok(true)
	})
}

/// # Safety
///
/// All non-null arguments must point to live objects of their declared type.
#[no_mangle]
pub unsafe extern "C" fn dnrs_decoder_next_frame(
	decoder: *mut dnrs_decoder,
	out: *mut dnrs_frame,
	error: *mut *mut dnrs_error,
) -> bool {
	ffi_result(error, false, || {
		let decoder = unsafe { decoder.as_mut() }.ok_or("null decoder")?;
		let index = decoder.frame_index;
		let page = decoder
			.page_index
			.and_then(|n| decoder.pages.get_mut(n))
			.ok_or("next_frame called before next_page")?;
		let Some(frame) = page.frames.get_mut(index) else {
			return Ok(false);
		};
		let out = unsafe { out.as_mut() }.ok_or("null frame")?;
		// Iteration only ever moves forward, so give up our copy.
		let pixels = mem::take(&mut frame.pixels).into_boxed_slice();
		let length = pixels.len();
		let data = Box::into_raw(pixels).cast::<u8>();
		*out = dnrs_frame {
			width: frame.width,
			height: frame.height,
			stride: frame.stride,
			format: frame.format,
			data,
			length,
			duration_ms: frame.duration_ms,
		};
		decoder.frame_index += 1;
		Ok(true)
	})
}

/// # Safety
///
/// `error`, when non-null, must point to a live `dnrs_error`.
#[no_mangle]
pub unsafe extern "C" fn dnrs_error_message(
	error: *const dnrs_error,
) -> *const c_char {
	unsafe { error.as_ref() }.map_or(ptr::null(), |e| e.message.as_ptr())
}

/// # Safety
///
/// `frame`, when non-null, must point to a frame returned by libdnrs.
#[no_mangle]
pub unsafe extern "C" fn dnrs_frame_clear(frame: *mut dnrs_frame) {
	if let Some(frame) = unsafe { frame.as_mut() } {
		if !frame.data.is_null() {
			unsafe {
				drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
					frame.data,
					frame.length,
				)));
			}
		}
		frame.data = ptr::null_mut();
		frame.length = 0;
	}
}

/// # Safety
///
/// `decoder`, when non-null, must have been returned by libdnrs and not freed.
#[no_mangle]
pub unsafe extern "C" fn dnrs_decoder_free(decoder: *mut dnrs_decoder) {
	if !decoder.is_null() {
		unsafe { drop(Box::from_raw(decoder)) };
	}
}

/// # Safety
///
/// `error`, when non-null, must have been returned by libdnrs and not freed.
#[no_mangle]
pub unsafe extern "C" fn dnrs_error_free(error: *mut dnrs_error) {
	if !error.is_null() {
		unsafe { drop(Box::from_raw(error)) };
	}
}

#[cfg(test)]
mod tests;
