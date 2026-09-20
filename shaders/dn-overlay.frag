#version 450

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 4) flat in float vContrast;

layout(location = 0) out vec4 fColor;

void
main()
{
	vec4 mask = texture(sTexture, vUV / vec2(textureSize(sTexture, 0)));
	mask += vContrast * mask * (1.0 - mask);
	fColor = vColor * mask;
}
