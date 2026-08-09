//
// scale-2d.frag: non-separable scale (free rotate)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#version 450
#extension GL_GOOGLE_include_directive : require

//
// Samples source tiles in the rotated display frame. Used when the view
// angle is not axis-aligned; the H→V mid buffer is in the wrong basis.
// Also the only path for Nearest and NoHalo zoom.
//
// Convolution kernels match the separable path. Radius is capped so a
// tiny scale cannot open a 2D neighbourhood of hundreds of taps.
//

#include "common.glsl"

layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2DArray u_tiles;

layout(push_constant) uniform Push {
	vec2 viewport;
	float scale;
	int transfer;
	ivec2 image_size;
	ivec2 grid;
	ivec2 mid_grid;
	ivec2 mid_pad;
	ivec2 layer_origin;
	vec2 pan;
	float angle;
	float bg_r, bg_g, bg_b;
	float checker_r, checker_g, checker_b;
} pc;

vec4 fetch_image(ivec2 p)
{
	return texelFetch(u_tiles,
			  tile_coord(clamp(p, ivec2(0), pc.image_size - 1),
				     pc.image_size, pc.grid),
			  0);
}

#if DN_FILTER == DN_FILTER_NOHALO
vec4 fetch_linear(ivec2 p, int orient, int transfer, bool opaque)
{
	return associated_to_linear(
		fetch_image(oriented_to_source(p, orient, pc.image_size)),
		transfer, opaque);
}

// Nohalo level 1 (Robidoux et al., C3S2E 2009): minmod slopes,
// double-density planes, bilinear. Used for zoom; minify is bilinear.
vec4 nohalo_minmod(vec4 a, vec4 b)
{
	return mix(vec4(0.0), mix(b, a, lessThanEqual(abs(a), abs(b))),
		   greaterThanEqual(a * b, vec4(0.0)));
}

vec4 nohalo_sample(vec2 img, int orient, int transfer, bool opaque)
{
	vec2 pos = img - 0.5;
	ivec2 i0 = ivec2(floor(pos));
	vec2 t = pos - vec2(i0);

	// The 4x4 stencil less its corners, which no minmod reaches: the
	// 2x2 cell, then one neighbour beyond each of its four sides.
	vec4 v00 = fetch_linear(i0, orient, transfer, opaque);
	vec4 v10 = fetch_linear(i0 + ivec2(1, 0), orient, transfer, opaque);
	vec4 v01 = fetch_linear(i0 + ivec2(0, 1), orient, transfer, opaque);
	vec4 v11 = fetch_linear(i0 + ivec2(1, 1), orient, transfer, opaque);
	vec4 n0 = fetch_linear(i0 + ivec2(0, -1), orient, transfer, opaque);
	vec4 n1 = fetch_linear(i0 + ivec2(1, -1), orient, transfer, opaque);
	vec4 s0 = fetch_linear(i0 + ivec2(0, 2), orient, transfer, opaque);
	vec4 s1 = fetch_linear(i0 + ivec2(1, 2), orient, transfer, opaque);
	vec4 w0 = fetch_linear(i0 + ivec2(-1, 0), orient, transfer, opaque);
	vec4 w1 = fetch_linear(i0 + ivec2(-1, 1), orient, transfer, opaque);
	vec4 e0 = fetch_linear(i0 + ivec2(2, 0), orient, transfer, opaque);
	vec4 e1 = fetch_linear(i0 + ivec2(2, 1), orient, transfer, opaque);

	vec4 sx00 = nohalo_minmod(v10 - v00, v00 - w0);
	vec4 sy00 = nohalo_minmod(v01 - v00, v00 - n0);
	vec4 sx10 = nohalo_minmod(e0 - v10, v10 - v00);
	vec4 sy10 = nohalo_minmod(v11 - v10, v10 - n1);
	vec4 sx01 = nohalo_minmod(v11 - v01, v01 - w1);
	vec4 sy01 = nohalo_minmod(s0 - v01, v01 - v00);
	vec4 sx11 = nohalo_minmod(e1 - v11, v11 - v01);
	vec4 sy11 = nohalo_minmod(s1 - v11, v11 - v10);
	vec4 h_top = 0.5 * (v00 + 0.5 * sx00 + v10 - 0.5 * sx10);
	vec4 h_bot = 0.5 * (v01 + 0.5 * sx01 + v11 - 0.5 * sx11);
	vec4 v_lft = 0.5 * (v00 + 0.5 * sy00 + v01 - 0.5 * sy01);
	vec4 v_rgt = 0.5 * (v10 + 0.5 * sy10 + v11 - 0.5 * sy11);
	vec4 diag = 0.25 * (v00 + 0.5 * sx00 + 0.5 * sy00 + v10 - 0.5 * sx10 +
			    0.5 * sy10 + v01 + 0.5 * sx01 - 0.5 * sy01 + v11 -
			    0.5 * sx11 - 0.5 * sy11);
	vec2 u = t * 2.0;
	vec4 a, b, c, d;
	if (u.x < 1.0) {
		if (u.y < 1.0) {
			a = v00;
			b = h_top;
			c = v_lft;
			d = diag;
		} else {
			a = v_lft;
			b = diag;
			c = v01;
			d = h_bot;
			u.y -= 1.0;
		}
	} else {
		u.x -= 1.0;
		if (u.y < 1.0) {
			a = h_top;
			b = v10;
			c = diag;
			d = v_rgt;
		} else {
			a = diag;
			b = v_rgt;
			c = h_bot;
			d = v11;
			u.y -= 1.0;
		}
	}
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
#endif

void main()
{
	int transfer = unpack_transfer(pc.transfer);
	int orient = unpack_orient(pc.transfer);
	bool opaque = unpack_opaque(pc.transfer) != 0;
	ivec2 disp = display_size(pc.image_size, orient);

	vec2 from_c = gl_FragCoord.xy - pc.viewport * 0.5;
	float c = cos(-pc.angle);
	float s = sin(-pc.angle);
	vec2 unrot = vec2(c * from_c.x - s * from_c.y,
			  s * from_c.x + c * from_c.y);
	vec2 img = unrot / pc.scale + vec2(disp) * 0.5 + pc.pan;

	if (any(lessThan(img, vec2(0.0))) ||
	    any(greaterThanEqual(img, vec2(disp))))
		discard;

	vec4 color;
#if DN_FILTER == DN_FILTER_NEAREST
	color = associated_to_linear(
		fetch_image(oriented_to_source(ivec2(floor(img)), orient,
					      pc.image_size)),
		transfer, opaque);
#elif DN_FILTER == DN_FILTER_NOHALO
	color = nohalo_sample(img, orient, transfer, opaque);
#else
	vec2 pos = img - 0.5;
	float kernel_scale = min(pc.scale, 1.0);
	float radius = kSupport / kernel_scale;
	if (radius > 4.0) {
		radius = 4.0;
		kernel_scale = kSupport / radius;
	}

	int x0 = int(floor(pos.x - radius)) + 1;
	int x1 = int(ceil(pos.x + radius)) - 1;
	int y0 = int(floor(pos.y - radius)) + 1;
	int y1 = int(ceil(pos.y + radius)) - 1;

	vec4 sum = vec4(0.0);
	float weight_sum = 0.0;
	for (int y = y0; y <= y1; ++y) {
		float wy = filter_weight((pos.y - float(y)) * kernel_scale);
		for (int x = x0; x <= x1; ++x) {
			float w = wy *
				  filter_weight((pos.x - float(x)) * kernel_scale);
			vec4 t = associated_to_linear(
				fetch_image(oriented_to_source(
					ivec2(x, y), orient, pc.image_size)),
				transfer, opaque);
			sum += t * w;
			weight_sum += w;
		}
	}
	color = sum / max(weight_sum, 1e-6);
#endif
	out_color = finish_scale(color, transfer,
				 unpack_checker(pc.transfer) != 0,
				 unpack_composite(pc.transfer) != 0,
				 vec3(pc.bg_r, pc.bg_g, pc.bg_b),
				 vec3(pc.checker_r, pc.checker_g, pc.checker_b));
}
