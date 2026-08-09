//
// fullscreen.vert: fullscreen triangle vertex shader
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#version 450

//
// Fullscreen triangle (-1,-1), (3,-1), (-1,3): covers all of NDC with three
// vertices and no vertex buffer. No outputs — the fragment shader derives
// everything it needs from gl_FragCoord.
//
void main()
{
	vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
