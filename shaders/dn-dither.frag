//
// dither.frag: ordered dither from 16-bit compose to 8-bit swapchain
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#version 450

layout(set = 0, binding = 0) uniform sampler2D u_compose;
layout(location = 0) out vec4 out_color;

float
bayer8(vec2 p)
{
	ivec2 i = ivec2(p) & 7;
	int b = ((i.x & 1) << 5) | ((i.y & 1) << 4) | ((i.x & 2) << 2) |
		((i.y & 2) << 1) | ((i.x & 4) >> 1) | ((i.y & 4) >> 2);
	return (float(b) + 0.5) / 64.0;
}

void
main()
{
	vec4 c = texelFetch(u_compose, ivec2(gl_FragCoord.xy), 0);
	c.rgb += (bayer8(gl_FragCoord.xy) - 0.5) / 255.0;
	out_color = vec4(c.rgb, c.a);
}
