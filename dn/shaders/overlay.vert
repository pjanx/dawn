#version 450

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform uPushConstant {
	vec2 scale;
	vec2 translate;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void
main()
{
	vUV = aUV;
	vColor = aColor;
	gl_Position = vec4(aPos * pc.scale + pc.translate, 0.0, 1.0);
}
