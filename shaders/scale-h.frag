//
// scale-h.frag: separable horizontal scale pass
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#version 450
#extension GL_GOOGLE_include_directive : require

//
// Separable horizontal pass (bilinear, via DN_FILTER).
//
// Writes one layer of the mid 2D-array (viewport tiles × source-row tiles).
// kernel_scale = min(scale, 1): magnification keeps the classic support
// neighbourhood; minification widens the kernel in source space.
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
} pc;

vec4 fetch_image(ivec2 p)
{
	return texelFetch(u_tiles,
			  tile_coord(clamp(p, ivec2(0), pc.image_size - 1),
				     pc.image_size, pc.grid),
			  0);
}

void main()
{
	int transfer = unpack_transfer(pc.transfer);
	int orient = unpack_orient(pc.transfer);
	bool opaque = unpack_opaque(pc.transfer) != 0;
	ivec2 disp = display_size(pc.image_size, orient);

	int lx = int(gl_FragCoord.x);
	int ly = int(gl_FragCoord.y);
	int row = pc.layer_origin.y + ly;
	if (row < 0 || row >= disp.y) {
		out_color = vec4(0.0);
		return;
	}

	float screen_x = float(pc.layer_origin.x + lx) + 0.5;
	float src_x = (screen_x - pc.viewport.x * 0.5) / pc.scale +
		      float(disp.x) * 0.5 + pc.pan.x;
	float pos_x = src_x - 0.5;
	float kernel_scale = min(pc.scale, 1.0);
	float radius = kSupport / kernel_scale;

	int first = int(floor(pos_x - radius)) + 1;
	int last = int(ceil(pos_x + radius)) - 1;

	vec4 sum = vec4(0.0);
	float weight_sum = 0.0;
	for (int x = first; x <= last; ++x) {
		float w = filter_weight((pos_x - float(x)) * kernel_scale);
		vec4 t = associated_to_working(
			fetch_image(oriented_to_source(ivec2(x, row), orient,
						       pc.image_size)),
			pc.transfer, opaque);
		sum += t * w;
		weight_sum += w;
	}
	out_color = sum / max(weight_sum, 1e-6);
}
