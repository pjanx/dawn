#version 450

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aAtlasRect;
layout(location = 4) in vec2 aDestSize;
layout(location = 5) in float aTransfer;

layout(push_constant) uniform uPushConstant {
	vec2 scale;
	vec2 translate;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out vec4 vAtlasRect;
layout(location = 3) flat out vec2 vDestSize;
layout(location = 4) flat out float vTransfer;

void
main()
{
	vUV = aUV;
	vColor = aColor;
	vAtlasRect = aAtlasRect;
	vDestSize = aDestSize;
	vTransfer = aTransfer;
	gl_Position = vec4(aPos * pc.scale + pc.translate, 0.0, 1.0);
}
