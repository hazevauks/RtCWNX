#version 460

// One triangle that covers the screen, for showing the finished frame.

layout(location = 0) out vec2 vTexCoord;

void main() {
	const vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID & 2) * 2.0 - 1.0);
	gl_Position = vec4(p, 0.0, 1.0);
	vTexCoord = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
}
