#version 460

// The finished frame, through the game's gamma table: on a PC, the video card's ramp.

layout(binding = 0) uniform sampler2D scene;
layout(binding = 1) uniform sampler2D gammaTable;

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

float gammaOf(float value, int channel) {
	return texture(gammaTable, vec2(value * (255.0 / 256.0) + 0.5 / 256.0, 0.5))[channel];
}

void main() {
	const vec3 color = texture(scene, vTexCoord).rgb;
	outColor = vec4(gammaOf(color.r, 0), gammaOf(color.g, 1), gammaOf(color.b, 2), 1.0);
}
