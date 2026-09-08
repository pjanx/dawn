use super::*;

use std::ffi::CStr;

struct Decoder(*mut dnrs_decoder);

impl Drop for Decoder {
	fn drop(&mut self) {
		unsafe { dnrs_decoder_free(self.0) };
	}
}

fn error_message(error: *mut dnrs_error) -> String {
	assert!(!error.is_null());
	let message = unsafe {
		CStr::from_ptr(dnrs_error_message(error))
			.to_string_lossy()
			.into_owned()
	};
	unsafe { dnrs_error_free(error) };
	message
}

fn open(data: &[u8], first_frame_only: bool) -> Decoder {
	let mut error = ptr::null_mut();
	let decoder = unsafe {
		dnrs_decoder_new(
			data.as_ptr(),
			data.len(),
			first_frame_only,
			&mut error,
		)
	};
	if decoder.is_null() {
		panic!("{}", error_message(error));
	}
	assert!(error.is_null());
	Decoder(decoder)
}

fn document_info(decoder: &Decoder) -> dnrs_document_info {
	let mut info = unsafe { std::mem::zeroed() };
	let mut error = ptr::null_mut();
	assert!(unsafe { dnrs_decoder_get_info(decoder.0, &mut info, &mut error) });
	assert!(error.is_null());
	info
}

fn next_page(decoder: &Decoder) -> dnrs_page_info {
	let mut info = unsafe { std::mem::zeroed() };
	let mut error = ptr::null_mut();
	assert!(unsafe {
		dnrs_decoder_next_page(decoder.0, &mut info, &mut error)
	});
	assert!(error.is_null());
	info
}

fn next_frame(decoder: &Decoder) -> dnrs_frame {
	let mut frame = unsafe { std::mem::zeroed() };
	let mut error = ptr::null_mut();
	assert!(unsafe {
		dnrs_decoder_next_frame(decoder.0, &mut frame, &mut error)
	});
	assert!(error.is_null());
	frame
}

fn png(
	color: png::ColorType,
	depth: png::BitDepth,
	pixels: &[u8],
	text: bool,
	exif: bool,
) -> Vec<u8> {
	let mut data = Vec::new();
	{
		let mut info = png::Info::with_size(2, 1);
		info.color_type = color;
		info.bit_depth = depth;
		if exif {
			info.exif_metadata = Some(
				b"II\x2a\0\x08\0\0\0\x01\0\x12\x01\x03\0\x01\0\0\0\x06\0\0\0\0\0\0\0"
					.as_slice()
					.into(),
			);
		}
		let encoder = png::Encoder::with_info(&mut data, info).unwrap();
		let mut writer = encoder.write_header().unwrap();
		writer.write_image_data(pixels).unwrap();
		if text {
			let chunk = png::text_metadata::TEXtChunk::new("prompt", "hello");
			writer.write_text_chunk(&chunk).unwrap();
		}
		writer.finish().unwrap();
	}
	data
}

#[test]
fn abi_rejects_bad_calls_and_input() {
	let mut error = ptr::null_mut();
	let decoder =
		unsafe { dnrs_decoder_new(ptr::null(), 1, false, &mut error) };
	assert!(decoder.is_null());
	assert_eq!(error_message(error), "null input pointer");

	let junk = b"this is not an image";
	error = ptr::null_mut();
	let decoder = unsafe {
		dnrs_decoder_new(junk.as_ptr(), junk.len(), false, &mut error)
	};
	assert!(decoder.is_null());
	assert_eq!(error_message(error), "unrecognised image data");

	let oversized = br#"#define huge_width 65536
#define huge_height 1
static unsigned char huge_bits[] = { 0x00 };
"#;
	error = ptr::null_mut();
	let decoder = unsafe {
		dnrs_decoder_new(oversized.as_ptr(), oversized.len(), false, &mut error)
	};
	assert!(decoder.is_null());
	assert!(!error.is_null());
	unsafe { dnrs_error_free(error) };

	let data = png(
		png::ColorType::Rgba,
		png::BitDepth::Eight,
		&[1, 2, 3, 4, 5, 6, 7, 8],
		false,
		false,
	);
	let decoder = open(&data, false);
	let mut frame = unsafe { std::mem::zeroed() };
	error = ptr::null_mut();
	assert!(!unsafe {
		dnrs_decoder_next_frame(decoder.0, &mut frame, &mut error)
	});
	assert_eq!(error_message(error), "next_frame called before next_page");
}

#[test]
fn png_rgba8_metadata_and_iteration() {
	let data = png(
		png::ColorType::Rgba,
		png::BitDepth::Eight,
		&[0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80],
		true,
		true,
	);
	let decoder = open(&data, false);
	let document = document_info(&decoder);
	assert_eq!(document.page_count, 1);
	assert!(!document.exif.data.is_null());
	assert!(document.exif.length >= 26);
	assert_eq!(document.text_length, 1);
	let text = unsafe { &*document.text };
	assert_eq!(unsafe { CStr::from_ptr(text.key) }.to_bytes(), b"prompt");
	assert_eq!(unsafe { CStr::from_ptr(text.value) }.to_bytes(), b"hello");

	let page = next_page(&decoder);
	assert_eq!((page.width, page.height, page.frame_count), (2, 1, 1));
	assert_eq!(page.orientation, 6);
	let mut frame = next_frame(&decoder);
	assert_eq!(frame.format as u32, dnrs_pixel_format::Rgba8 as u32);
	assert_eq!(frame.stride, 8);
	assert_eq!(frame.length, 8);
	assert_eq!(
		unsafe { std::slice::from_raw_parts(frame.data, frame.length) },
		&[0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80]
	);
	unsafe { dnrs_frame_clear(&mut frame) };
	assert!(frame.data.is_null());

	let mut error = ptr::null_mut();
	assert!(!unsafe {
		dnrs_decoder_next_frame(decoder.0, &mut frame, &mut error)
	});
	assert!(error.is_null());
}

#[test]
fn png_rgba16_is_little_endian() {
	let data = png(
		png::ColorType::Rgba,
		png::BitDepth::Sixteen,
		&[
			0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x01, 0x02, 0x03,
			0x04, 0x05, 0x06, 0x07, 0x08,
		],
		false,
		false,
	);
	let decoder = open(&data, false);
	next_page(&decoder);
	let mut frame = next_frame(&decoder);
	assert_eq!(frame.format as u32, dnrs_pixel_format::Rgba16Le as u32);
	assert_eq!(
		unsafe { std::slice::from_raw_parts(frame.data, frame.length) },
		&[
			0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a, 0xf0, 0xde, 0x02, 0x01, 0x04,
			0x03, 0x06, 0x05, 0x08, 0x07,
		]
	);
	unsafe { dnrs_frame_clear(&mut frame) };
}

#[test]
fn gif_animation_and_first_frame_only() {
	use image::codecs::gif::{GifEncoder, Repeat};
	use image::{Delay, Frame, RgbaImage};

	let mut data = Vec::new();
	{
		let mut encoder = GifEncoder::new(&mut data);
		encoder.set_repeat(Repeat::Finite(3)).unwrap();
		for (pixel, delay) in [([255, 0, 0, 255], 20), ([0, 0, 255, 255], 30)] {
			let image = RgbaImage::from_raw(1, 1, pixel.to_vec()).unwrap();
			encoder
				.encode_frame(Frame::from_parts(
					image,
					0,
					0,
					Delay::from_numer_denom_ms(delay, 1),
				))
				.unwrap();
		}
	}

	let decoder = open(&data, false);
	let page = next_page(&decoder);
	assert_eq!(page.frame_count, 2);
	assert_eq!(page.loop_count, 3);
	let mut first = next_frame(&decoder);
	let mut second = next_frame(&decoder);
	assert_eq!(first.duration_ms, 20);
	assert_eq!(second.duration_ms, 30);
	unsafe {
		dnrs_frame_clear(&mut first);
		dnrs_frame_clear(&mut second);
	}

	let decoder = open(&data, true);
	assert_eq!(next_page(&decoder).frame_count, 1);
}

#[test]
fn tiff_pages_are_separate() {
	use tiff::encoder::{colortype, TiffEncoder};

	let mut cursor = Cursor::new(Vec::new());
	{
		let mut encoder = TiffEncoder::new(&mut cursor).unwrap();
		encoder
			.write_image::<colortype::RGB8>(1, 1, &[255, 0, 0])
			.unwrap();
		encoder
			.write_image::<colortype::RGB8>(1, 1, &[0, 0, 255])
			.unwrap();
	}
	let decoder = open(&cursor.into_inner(), false);
	assert_eq!(document_info(&decoder).page_count, 2);
	for expected in [[255, 0, 0], [0, 0, 255]] {
		assert_eq!(next_page(&decoder).frame_count, 1);
		let mut frame = next_frame(&decoder);
		assert_eq!(frame.format as u32, dnrs_pixel_format::Rgb8 as u32);
		assert_eq!(
			unsafe { std::slice::from_raw_parts(frame.data, frame.length) },
			&expected
		);
		unsafe { dnrs_frame_clear(&mut frame) };
	}
}

#[test]
fn image_extras_xbm_and_xpm_are_sniffed() {
	let xbm = br#"#define dot_width 1
#define dot_height 1
static unsigned char dot_bits[] = { 0x01 };
"#;
	let decoder = open(xbm, false);
	assert_eq!(next_page(&decoder).width, 1);
	let mut frame = next_frame(&decoder);
	assert_eq!(frame.format as u32, dnrs_pixel_format::Gray8 as u32);
	unsafe { dnrs_frame_clear(&mut frame) };

	let xpm = br#"/* XPM */
static char *dot[] = {
"1 1 1 1",
"a c #ff0000",
"a"
};
"#;
	let decoder = open(xpm, false);
	assert_eq!(next_page(&decoder).width, 1);
	let mut frame = next_frame(&decoder);
	assert_eq!(frame.format as u32, dnrs_pixel_format::Rgba16Le as u32);
	assert_eq!(
		unsafe { std::slice::from_raw_parts(frame.data, frame.length) },
		&[255, 255, 0, 0, 0, 0, 255, 255]
	);
	unsafe { dnrs_frame_clear(&mut frame) };
}

fn verify_encoded_image(data: &[u8]) {
	let decoder = open(data, false);
	assert_eq!(document_info(&decoder).page_count, 1);
	let page = next_page(&decoder);
	assert_eq!((page.width, page.height, page.frame_count), (1, 1, 1));
	let mut frame = next_frame(&decoder);
	assert!(!frame.data.is_null());
	assert!(frame.length > 0);
	unsafe { dnrs_frame_clear(&mut frame) };
}

#[test]
fn image_rs_still_formats_are_decodable() {
	use image::{ExtendedColorType, ImageEncoder};

	let rgb = [0x12, 0x34, 0x56];
	let rgba = [0x12, 0x34, 0x56, 0x78];
	let rgba16 = [0x1234_u16, 0x5678, 0x9abc, 0xdef0]
		.into_iter()
		.flat_map(u16::to_ne_bytes)
		.collect::<Vec<_>>();
	let rgb32f = [0.25_f32, 0.5, 0.75]
		.into_iter()
		.flat_map(f32::to_ne_bytes)
		.collect::<Vec<_>>();
	let rgba32f = [0.25_f32, 0.5, 0.75, 1.0]
		.into_iter()
		.flat_map(f32::to_ne_bytes)
		.collect::<Vec<_>>();

	let mut data = Vec::new();
	image::codecs::bmp::BmpEncoder::new(&mut data)
		.write_image(&rgb, 1, 1, ExtendedColorType::Rgb8)
		.unwrap();
	verify_encoded_image(&data);

	data.clear();
	image::codecs::farbfeld::FarbfeldEncoder::new(&mut data)
		.write_image(&rgba16, 1, 1, ExtendedColorType::Rgba16)
		.unwrap();
	verify_encoded_image(&data);

	data.clear();
	image::codecs::hdr::HdrEncoder::new(&mut data)
		.write_image(&rgb32f, 1, 1, ExtendedColorType::Rgb32F)
		.unwrap();
	verify_encoded_image(&data);

	data.clear();
	image::codecs::ico::IcoEncoder::new(&mut data)
		.write_image(&rgba, 1, 1, ExtendedColorType::Rgba8)
		.unwrap();
	verify_encoded_image(&data);

	data.clear();
	image::codecs::jpeg::JpegEncoder::new_with_quality(&mut data, 100)
		.write_image(&rgb, 1, 1, ExtendedColorType::Rgb8)
		.unwrap();
	verify_encoded_image(&data);

	let mut exr = Cursor::new(Vec::new());
	image::codecs::openexr::OpenExrEncoder::new(&mut exr)
		.write_image(&rgba32f, 1, 1, ExtendedColorType::Rgba32F)
		.unwrap();
	verify_encoded_image(exr.get_ref());

	data.clear();
	image::codecs::pnm::PnmEncoder::new(&mut data)
		.write_image(&rgb, 1, 1, ExtendedColorType::Rgb8)
		.unwrap();
	verify_encoded_image(&data);

	data.clear();
	image::codecs::qoi::QoiEncoder::new(&mut data)
		.write_image(&rgba, 1, 1, ExtendedColorType::Rgba8)
		.unwrap();
	verify_encoded_image(&data);

	data.clear();
	image::codecs::tga::TgaEncoder::new(&mut data)
		.write_image(&rgb, 1, 1, ExtendedColorType::Rgb8)
		.unwrap();
	verify_encoded_image(&data);

	data.clear();
	image::codecs::webp::WebPEncoder::new_lossless(&mut data)
		.write_image(&rgba, 1, 1, ExtendedColorType::Rgba8)
		.unwrap();
	verify_encoded_image(&data);
}

#[test]
fn dds_dxt1_is_decodable() {
	fn push_u32(data: &mut Vec<u8>, value: u32) {
		data.extend_from_slice(&value.to_le_bytes());
	}

	let mut data = b"DDS ".to_vec();
	push_u32(&mut data, 124);
	push_u32(&mut data, 0x1007);
	push_u32(&mut data, 4);
	push_u32(&mut data, 4);
	push_u32(&mut data, 8);
	push_u32(&mut data, 0);
	push_u32(&mut data, 0);
	for _ in 0..11 {
		push_u32(&mut data, 0);
	}
	push_u32(&mut data, 32);
	push_u32(&mut data, 4);
	data.extend_from_slice(b"DXT1");
	for _ in 0..5 {
		push_u32(&mut data, 0);
	}
	push_u32(&mut data, 0x1000);
	for _ in 0..4 {
		push_u32(&mut data, 0);
	}
	data.extend_from_slice(&[0x00, 0xf8, 0, 0, 0, 0, 0, 0]);

	let decoder = open(&data, false);
	let page = next_page(&decoder);
	assert_eq!((page.width, page.height), (4, 4));
	let mut frame = next_frame(&decoder);
	assert_eq!(frame.format as u32, dnrs_pixel_format::Rgb8 as u32);
	assert_eq!(
		unsafe { std::slice::from_raw_parts(frame.data, 3) },
		&[255, 0, 0]
	);
	unsafe { dnrs_frame_clear(&mut frame) };
}

#[cfg(feature = "jpeg2000")]
#[test]
fn jpeg2000_is_decodable() {
	// A 1x1 red JP2 encoded by OpenJPEG 2.5.4.
	let data = [
		0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a,
		0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x6a, 0x70, 0x32, 0x20,
		0x00, 0x00, 0x00, 0x00, 0x6a, 0x70, 0x32, 0x20, 0x00, 0x00, 0x00, 0x2d,
		0x6a, 0x70, 0x32, 0x68, 0x00, 0x00, 0x00, 0x16, 0x69, 0x68, 0x64, 0x72,
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x0f, 0x07,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x63, 0x6f, 0x6c, 0x72, 0x01, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x93, 0x6a, 0x70, 0x32,
		0x63, 0xff, 0x4f, 0xff, 0x51, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0f, 0x01, 0x01, 0x0f, 0x01,
		0x01, 0x0f, 0x01, 0x01, 0xff, 0x52, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01,
		0x01, 0x00, 0x04, 0x04, 0x00, 0x01, 0xff, 0x5c, 0x00, 0x04, 0x40, 0x80,
		0xff, 0x64, 0x00, 0x25, 0x00, 0x01, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65,
		0x64, 0x20, 0x62, 0x79, 0x20, 0x4f, 0x70, 0x65, 0x6e, 0x4a, 0x50, 0x45,
		0x47, 0x20, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x20, 0x32, 0x2e,
		0x35, 0x2e, 0x34, 0xff, 0x90, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x1b, 0x00, 0x01, 0xff, 0x93, 0xcf, 0xfc, 0x30, 0x08, 0x07, 0xb7, 0x80,
		0xdf, 0xf8, 0x90, 0x10, 0x01, 0x3f, 0xff, 0xd9,
	];
	verify_encoded_image(&data);
}

#[test]
fn every_png_truncation_is_an_error() {
	let data = png(
		png::ColorType::Rgba,
		png::BitDepth::Eight,
		&[1, 2, 3, 4, 5, 6, 7, 8],
		false,
		false,
	);
	for length in 0..data.len() {
		let mut error = ptr::null_mut();
		let decoder = unsafe {
			dnrs_decoder_new(data.as_ptr(), length, false, &mut error)
		};
		assert!(decoder.is_null(), "accepted PNG prefix of {length} bytes");
		assert!(!error.is_null());
		unsafe { dnrs_error_free(error) };
	}
}
