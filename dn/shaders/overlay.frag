#version 450

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 fColor;

void
main()
{
	fColor = vColor * texture(sTexture, vUV);
}
