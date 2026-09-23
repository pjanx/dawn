//
// dn-present.frag: encode linear composition, then optionally dither
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#version 450
#extension GL_GOOGLE_include_directive : require
#define DN_FILTER 0
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_compose;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push {
	float levels;  // 0 disables dithering; otherwise 2^bpc - 1
	uint premultiplied;
	uint srgb_attachment;
	// Extended presentation passes linear light on, in the platform's
	// primaries, with SDR white at `white`.
	uint extended;
	vec4 matrix[3];  // Columns, from display linear RGB
	float white;
} pc;

float
bayer8(vec2 p)
{
	ivec2 i = ivec2(p) & 7;
	int diagonal = i.x ^ i.y;
	int b = ((diagonal & 1) << 5) | ((i.x & 1) << 4) |
		((diagonal & 2) << 2) | ((i.x & 2) << 1) |
		((diagonal & 4) >> 1) | ((i.x & 4) >> 2);
	return (float(b) + 0.5) / 64.0;
}

void
main()
{
	vec4 c = texelFetch(u_compose, ivec2(gl_FragCoord.xy), 0);
	if (pc.extended != 0) {
		// Linear premultiplied values pass through the matrix as they are.
		mat3 m = mat3(pc.matrix[0].xyz, pc.matrix[1].xyz, pc.matrix[2].xyz);
		c.rgb = m * c.rgb * pc.white;
		if (pc.premultiplied == 0)
			c.rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
		out_color = c;
		return;
	}
	c.rgb = c.a > 0.0 ? profile_curve(c.rgb / c.a, true) : vec3(0.0);
	if (pc.premultiplied != 0)
		c.rgb *= c.a;
	if (pc.levels > 0.0)
		c.rgb = floor(c.rgb * pc.levels + bayer8(gl_FragCoord.xy)) / pc.levels;
	if (pc.srgb_attachment != 0)
		c.rgb = srgb_to_linear(c.rgb);
	out_color = vec4(c.rgb, c.a);
}
