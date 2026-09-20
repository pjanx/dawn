#version 450

layout(location = 0) in ivec4 aBox;
layout(location = 1) in vec4 aUV;
layout(location = 2) in vec4 aTop;
layout(location = 3) in vec4 aBottom;
layout(location = 4) in float aContrast;

layout(push_constant) uniform uPushConstant {
	vec2 scale;
	vec2 translate;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out vec4 vAtlasRect;
layout(location = 3) flat out vec2 vDestSize;
layout(location = 4) flat out float vContrast;

void
main()
{
	const vec2 corners[6] = vec2[](
		vec2(0, 0), vec2(1, 0), vec2(1, 1),
		vec2(0, 0), vec2(1, 1), vec2(0, 1));
	vec2 corner = corners[gl_VertexIndex];
	vec2 pos = mix(vec2(aBox.xy), vec2(aBox.zw), corner);
	vUV = mix(aUV.xy, aUV.zw, corner);
	vColor = mix(aTop, aBottom, corner.y);
	vContrast = aContrast;
	vAtlasRect = aUV;
	vDestSize = abs(vec2(aBox.zw) - vec2(aBox.xy));
	gl_Position = vec4(pos * pc.scale + pc.translate, 0.0, 1.0);
}
