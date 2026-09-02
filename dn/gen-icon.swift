// gen-icon.swift: generate a program icon for dn in the Apple icon format
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//
// As an odd regression, AppKit may be necessary for JIT linking.
import AppKit

// NSGraphicsContext mostly just weirdly wraps over Quartz,
// so we do it all in Quartz directly.
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

// Apple uses something that's close to a "quintic superellipse" in their icons,
// but doesn't quite match. Either way, it looks better than rounded rectangles.
func addSquircle(context: CGContext, bounds: CGRect) {
	context.move(to: CGPoint(x: bounds.maxX, y: bounds.midY))
	for theta in stride(from: 0.0, to: .pi * 2, by: .pi / 1e4) {
		let x = pow(abs(cos(theta)), 2 / 5.0) * bounds.width / 2
			* CGFloat(signOf: cos(theta), magnitudeOf: 1) + bounds.midX
		let y = pow(abs(sin(theta)), 2 / 5.0) * bounds.height / 2
			* CGFloat(signOf: sin(theta), magnitudeOf: 1) + bounds.midY
		context.addLine(to: CGPoint(x: x, y: y))
	}
	context.closePath()
}

func drawIcon(scale: CGFloat) -> CGImage? {
	let size = CGSizeMake(1024, 1024)

	let colorspace = CGColorSpaceCreateDeviceRGB()
	let context = CGContext(data: nil,
		width: Int(size.width * scale), height: Int(size.height * scale),
		bitsPerComponent: 8, bytesPerRow: 0, space: colorspace,
		bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
	context.scaleBy(x: scale, y: scale)

	let bounds = CGRectMake(100, 100, size.width - 200, size.height - 200)
	addSquircle(context: context, bounds: bounds)
	let squircle = context.path!

	// Gradients don't draw shadows, so draw it separately.
	context.saveGState()
	context.setShadow(offset: CGSizeMake(0, -12).applying(context.ctm),
		blur: 28 * scale, color: CGColor(gray: 0, alpha: 0.5))
	context.setFillColor(CGColor(red: 1, green: 0x7ap-8, blue: 0, alpha: 1))
	context.fillPath()
	context.restoreGState()

	// Quartz Y is up: start is the bottom of the squircle (orange),
	// end is the top (yellow #ffee00).
	context.saveGState()
	context.addPath(squircle)
	context.clip()
	context.drawLinearGradient(
		CGGradient(colorsSpace: colorspace, colors: [
			CGColor(red: 1, green: 0x00p-8, blue: 0, alpha: 1),
			CGColor(red: 1, green: 0xffp-8, blue: 0, alpha: 1)
		] as CFArray, locations: [0, 1])!,
		start: CGPointMake(0, 100), end: CGPointMake(0, size.height - 100),
		options: CGGradientDrawingOptions(rawValue: 0))
	context.restoreGState()

	return context.makeImage()
}

if CommandLine.arguments.count != 2 {
	print("Usage: \(CommandLine.arguments.first!) OUTPUT.icns")
	exit(EXIT_FAILURE)
}

let filename = CommandLine.arguments[1]

let macOSSizes: Array<CGFloat> = [16, 32, 128, 256, 512]
let icns = CGImageDestinationCreateWithURL(
	URL(fileURLWithPath: filename) as CFURL,
	UTType.icns.identifier as CFString, macOSSizes.count * 2, nil)!
for size in macOSSizes {
	CGImageDestinationAddImage(icns, drawIcon(scale: size / 1024.0)!, nil)
	CGImageDestinationAddImage(icns, drawIcon(scale: size / 1024.0 * 2)!, [
			kCGImagePropertyDPIWidth: 144,
			kCGImagePropertyDPIHeight: 144,
		] as CFDictionary)
}
if !CGImageDestinationFinalize(icns) {
	print("ICNS finalization failed.")
	exit(EXIT_FAILURE)
}
