//
// scale-v.frag: separable vertical scale pass
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#version 450
#extension GL_GOOGLE_include_directive : require

//
// Separable vertical pass (bilinear, via DN_FILTER).
//
// Samples the H-pass mid 2D-array. Same kernel_scale rule as the H pass so
// magnification and minification share one path.
//

#include "common.glsl"

layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2DArray u_horiz;

layout(push_constant) uniform Push {
	vec2 viewport;
	float scale;
	int transfer;
	ivec2 image_size;
	ivec2 grid;
	ivec2 mid_grid;
	ivec2 mid_pad;
	ivec2 layer_origin; /* unused */
	vec2 pan;
	float angle; /* unused */
	float bg_r, bg_g, bg_b;
	float checker_r, checker_g, checker_b;
} pc;

vec4 fetch_horiz(ivec2 p, ivec2 disp)
{
	/* p.x = screen column, p.y = display row */
	p.x = clamp(p.x, 0, int(pc.viewport.x) - 1);
	p.y = clamp(p.y, 0, disp.y - 1);
	return texelFetch(u_horiz, mid_coord(p, pc.mid_pad, pc.mid_grid), 0);
}

void main()
{
	int transfer = unpack_transfer(pc.transfer);
	int orient = unpack_orient(pc.transfer);
	ivec2 disp = display_size(pc.image_size, orient);

	vec2 img = (gl_FragCoord.xy - pc.viewport * 0.5) / pc.scale +
		   vec2(disp) * 0.5 + pc.pan;

	if (any(lessThan(img, vec2(0.0))) ||
	    any(greaterThanEqual(img, vec2(disp))))
		discard;

	int col = int(gl_FragCoord.x);
	float pos_y = img.y - 0.5;
	float kernel_scale = min(pc.scale, 1.0);
	float radius = kSupport / kernel_scale;

	int first = int(floor(pos_y - radius)) + 1;
	int last = int(ceil(pos_y + radius)) - 1;

	vec4 sum = vec4(0.0);
	float weight_sum = 0.0;
	for (int y = first; y <= last; ++y) {
		float w = filter_weight((pos_y - float(y)) * kernel_scale);
		sum += fetch_horiz(ivec2(col, y), disp) * w;
		weight_sum += w;
	}
	vec4 c = sum / max(weight_sum, 1e-6);
	out_color = finish_scale(c, transfer, unpack_checker(pc.transfer) != 0,
				 unpack_composite(pc.transfer) != 0,
				 unpack_linear_blend(pc.transfer) != 0,
				 vec3(pc.bg_r, pc.bg_g, pc.bg_b),
				 vec3(pc.checker_r, pc.checker_g, pc.checker_b));
}
