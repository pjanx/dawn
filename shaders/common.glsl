//
// common.glsl: shared scale fragment helpers
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// Shared by scale_*.frag — included via glslang -I.

const int TRANSFER_LINEAR = 0;
const int TRANSFER_SRGB = 1;
const int TRANSFER_ADOBE_RGB = 2;

int unpack_transfer(int packed)
{
	return packed & 255;
}

int unpack_orient(int packed)
{
	int o = (packed >> 8) & 255;
	if (o < 1 || o > 8)
		return 1;
	return o;
}

int unpack_checker(int packed)
{
	return (packed >> 16) & 1;
}

int unpack_composite(int packed)
{
	return (packed >> 17) & 1;
}

int unpack_opaque(int packed)
{
	return (packed >> 18) & 1;
}

#ifndef DN_COMPUTE
// 20px squares. even = toolbar_bottom, odd = well (same pairing as
// browser draw_checker). Colours are already linear.
vec3 checker(vec3 odd, vec3 even)
{
	vec2 xy = gl_FragCoord.xy / 20.0;
	if ((int(floor(xy.x) + floor(xy.y)) & 1) == 0)
		return even;
	return odd;
}
#endif

ivec2 display_size(ivec2 src, int o)
{
	if (o == 5 || o == 6 || o == 7 || o == 8)
		return src.yx;
	return src;
}

// Display texel → source texel. Matches fiv's cairo pattern matrix
// (floor of display→source at pixel centre).
ivec2 oriented_to_source(ivec2 p, int o, ivec2 src)
{
	int W = src.x;
	int H = src.y;
	if (o == 2)
		return ivec2(W - 1 - p.x, p.y);
	if (o == 3)
		return ivec2(W - 1 - p.x, H - 1 - p.y);
	if (o == 4)
		return ivec2(p.x, H - 1 - p.y);
	if (o == 5)
		return ivec2(p.y, p.x);
	if (o == 6)
		return ivec2(p.y, H - 1 - p.x);
	if (o == 7)
		return ivec2(W - 1 - p.y, H - 1 - p.x);
	if (o == 8)
		return ivec2(W - 1 - p.y, p.x);
	return p;
}

#define DN_FILTER_NEAREST 0
#define DN_FILTER_BILINEAR 1
#define DN_FILTER_NOHALO 2

#if DN_FILTER == DN_FILTER_BILINEAR
const float kSupport = 1.0;
float filter_weight(float x) { return max(0.0, 1.0 - abs(x)); }
#endif

vec3 srgb_to_linear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
		   greaterThan(c, vec3(0.04045)));
}

vec3 linear_to_srgb(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
		   greaterThan(c, vec3(0.0031308)));
}

vec3 gamma22_to_linear(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(2.2));
}

vec3 linear_to_gamma22(vec3 c)
{
	return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

vec3 decode_rgb(vec3 c, int transfer)
{
	if (transfer == TRANSFER_SRGB)
		return srgb_to_linear(c);
	if (transfer == TRANSFER_ADOBE_RGB)
		return gamma22_to_linear(c);
	return c;
}

vec3 encode_rgb(vec3 c, int transfer)
{
	if (transfer == TRANSFER_SRGB)
		return linear_to_srgb(c);
	if (transfer == TRANSFER_ADOBE_RGB)
		return linear_to_gamma22(c);
	return c;
}

// Tile-array coordinate for an in-bounds texel. A single-tile image, which
// is everything short of maxImageDimension2D, skips two integer divisions
// per tap.
ivec3 tile_coord(ivec2 p, ivec2 size, ivec2 grid)
{
	if (grid.x == 1 && grid.y == 1)
		return ivec3(p, 0);
	ivec2 base = max((size + grid - 1) / grid, ivec2(1));
	int col = min(p.x / base.x, grid.x - 1);
	int row = min(p.y / base.y, grid.y - 1);
	return ivec3(p - ivec2(col, row) * base, row * grid.x + col);
}

// Same for the H-pass mid buffer, where the tile size is given.
ivec3 mid_coord(ivec2 p, ivec2 pad, ivec2 grid)
{
	if (grid.x == 1 && grid.y == 1)
		return ivec3(p, 0);
	ivec2 base = max(pad, ivec2(1));
	int col = min(p.x / base.x, grid.x - 1);
	int row = min(p.y / base.y, grid.y - 1);
	return ivec3(p - ivec2(col, row) * base, row * grid.x + col);
}

// Tiles carry associated (premultiplied) alpha in the destination
// encoding, because libdn premultiplies after colour management. Recover
// the straight colour before the EOTF and re-associate, so filters see
// linear premultiplied values. `opaque` is uniform across the draw.
vec4 associated_to_linear(vec4 t, int transfer, bool opaque)
{
	if (opaque)
		return vec4(decode_rgb(t.rgb, transfer), 1.0);
	if (t.a <= 0.0)
		return vec4(0.0);
	return vec4(decode_rgb(clamp(t.rgb / t.a, 0.0, 1.0), transfer) * t.a, t.a);
}

#ifndef DN_COMPUTE
// Alpha is resolved here, not by the fixed-function blend: that blend runs
// on encoded values, which reads a soft edge as a halo (premultiplied RGB
// encoded without dividing out alpha) and a shadow as mud (background
// mixed in the wrong space). Compositing needs no reciprocal, so knowing
// what is behind the image is also the cheaper path.
vec4 finish_scale(vec4 lin_premul, int transfer, bool checkerboard,
		  bool composite, vec3 bg_linear, vec3 checker_linear)
{
	float a = clamp(lin_premul.a, 0.0, 1.0);
	// Nohalo takes minmod slopes per channel, so RGB can outrun alpha.
	vec3 rgb = clamp(lin_premul.rgb, vec3(0.0), vec3(a));
	if (checkerboard)
		bg_linear = checker(bg_linear, checker_linear);
	else if (!composite)
		return vec4(
			a > 0.0 ? encode_rgb(rgb / a, transfer) * a : vec3(0.0), a);
	return vec4(encode_rgb(bg_linear * (1.0 - a) + rgb, transfer), 1.0);
}
#endif
