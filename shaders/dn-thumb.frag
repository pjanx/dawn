#version 450
#extension GL_GOOGLE_include_directive : require

#define DN_FILTER 1
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in vec4 vAtlasRect;
layout(location = 3) flat in vec2 vDestSize;
layout(location = 4) flat in float vTransfer;

layout(location = 0) out vec4 fColor;

vec4 fetch_entry(ivec2 p, ivec2 lo, ivec2 hi, int transfer)
{
	vec4 encoded = texelFetch(sTexture, clamp(p, lo, hi), 0);
	return associated_to_linear(encoded, transfer, encoded.a >= 1.0);
}

void main()
{
	ivec2 texture_size = textureSize(sTexture, 0);
	ivec2 lo = ivec2(round(vAtlasRect.xy * vec2(texture_size)));
	ivec2 end = ivec2(round(vAtlasRect.zw * vec2(texture_size)));
	ivec2 source_size = max(end - lo, ivec2(1));
	ivec2 hi = lo + source_size - ivec2(1);
	vec2 extent = max(vAtlasRect.zw - vAtlasRect.xy, vec2(1e-12));
	vec2 local = clamp((vUV - vAtlasRect.xy) / extent, vec2(0.0), vec2(1.0));
	vec2 pos = local * vec2(source_size) - vec2(0.5);
	vec2 scale = max(vDestSize / vec2(source_size), vec2(1e-6));
	int transfer = int(round(vTransfer));

	vec4 linear;
	if (all(equal(source_size, ivec2(round(vDestSize))))) {
		linear = fetch_entry(lo + ivec2(clamp(floor(pos + vec2(0.5)),
			vec2(0.0), vec2(source_size - 1))), lo, hi, transfer);
	} else {
		vec2 kernel_scale = min(scale, vec2(1.0));
		vec2 radius = vec2(1.0) / kernel_scale;
		ivec2 first = ivec2(floor(pos - radius)) + ivec2(1);
		ivec2 last = ivec2(ceil(pos + radius)) - ivec2(1);
		linear = vec4(0.0);
		float weights = 0.0;
		for (int y = first.y; y <= last.y; ++y) {
			float wy = filter_weight((pos.y - float(y)) * kernel_scale.y);
			for (int x = first.x; x <= last.x; ++x) {
				float wx = filter_weight((pos.x - float(x)) * kernel_scale.x);
				float w = wx * wy;
				linear += fetch_entry(lo + ivec2(x, y), lo, hi, transfer) * w;
				weights += w;
			}
		}
		linear /= max(weights, 1e-6);
	}

	float a = clamp(linear.a, 0.0, 1.0);
	vec3 rgb = clamp(linear.rgb, vec3(0.0), vec3(a));
	vec3 encoded = a > 0.0
		? encode_rgb(rgb / a, transfer) * a : vec3(0.0);
	fColor = vColor * vec4(encoded, a);
}
