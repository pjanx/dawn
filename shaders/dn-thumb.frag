#version 450
#extension GL_GOOGLE_include_directive : require

#define DN_FILTER 1
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in vec4 vAtlasRect;
layout(location = 3) flat in vec2 vDestSize;

layout(location = 0) out vec4 fColor;

layout(push_constant) uniform Push {
	vec2 scale;
	vec2 translate;
	vec4 odd;
	vec4 even;
	vec2 origin;
	float checker_size;
} pc;

// Thumbnails are both filtered and composited in encoded values.
vec4 fetch_entry(ivec2 p, ivec2 lo, ivec2 hi)
{
	return texelFetch(sTexture, clamp(p, lo, hi), 0);
}

void main()
{
	ivec2 lo = ivec2(vAtlasRect.xy);
	ivec2 end = ivec2(vAtlasRect.zw);
	ivec2 source_size = max(end - lo, ivec2(1));
	ivec2 hi = lo + source_size - ivec2(1);
	vec2 extent = max(vAtlasRect.zw - vAtlasRect.xy, vec2(1e-12));
	vec2 local = clamp((vUV - vAtlasRect.xy) / extent, vec2(0.0), vec2(1.0));
	vec2 pos = local * vec2(source_size) - vec2(0.5);
	vec2 scale = max(vDestSize / vec2(source_size), vec2(1e-6));
	int transfer = 3;  // Atlas pixels are in this window's display profile.

	vec4 c;
	if (all(equal(source_size, ivec2(round(vDestSize))))) {
		c = fetch_entry(lo + ivec2(clamp(floor(pos + vec2(0.5)),
			vec2(0.0), vec2(source_size - 1))), lo, hi);
	} else {
		vec2 kernel_scale = min(scale, vec2(1.0));
		vec2 radius = vec2(1.0) / kernel_scale;
		ivec2 first = ivec2(floor(pos - radius)) + ivec2(1);
		ivec2 last = ivec2(ceil(pos + radius)) - ivec2(1);
		c = vec4(0.0);
		float weights = 0.0;
		for (int y = first.y; y <= last.y; ++y) {
			float wy = filter_weight((pos.y - float(y)) * kernel_scale.y);
			for (int x = first.x; x <= last.x; ++x) {
				float wx = filter_weight((pos.x - float(x)) * kernel_scale.x);
				float w = wx * wy;
				c += fetch_entry(lo + ivec2(x, y), lo, hi) * w;
				weights += w;
			}
		}
		c /= max(weights, 1e-6);
	}

	float a = clamp(c.a, 0.0, 1.0);
	vec3 rgb = clamp(c.rgb, vec3(0.0), vec3(a));
	vec2 tile = floor((gl_FragCoord.xy - pc.origin) / pc.checker_size);
	vec3 bg = (int(tile.x + tile.y) & 1) == 0 ? pc.even.rgb : pc.odd.rgb;
	bg = encode_rgb(bg, transfer);
	// Tile opacity follows the composition.
	fColor = vColor * vec4(decode_rgb(rgb + (1.0 - a) * bg, transfer), 1.0);
}
