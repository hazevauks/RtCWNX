#version 460

// OpenGL's fixed function vertex stage, as far as the renderer uses it. It does its
// texture coordinates and lighting on the CPU, so little is left: the transform, the
// color, the fog factor and the portal's clip plane.
//
// The uniform block is devVertexUniforms_t in source/renderer/dk_device.h.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(std140, binding = 0) uniform Transform {
	mat4 mvp;
	vec4 eye[3];		// the modelview's first three rows
	vec4 colorConst;	// color = array * colorArray.x + colorConst
	vec4 colorArray;
	vec4 fog;		// scale, end, density, mode: 0 none, 1 linear, 2 exp, 3 exp2
	vec4 clipPlane;		// eye space
};

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord0;
layout(location = 2) out vec2 vTexCoord1;
layout(location = 3) out float vFog;
layout(location = 4) out float vClip;

void main() {
	const vec4 p = vec4(inPosition, 1.0);
	gl_Position = mvp * p;

	vColor = inColor * colorArray.x + colorConst;
	vTexCoord0 = inTexCoord0;
	vTexCoord1 = inTexCoord1;

	const vec4 eyePosition = vec4(dot(p, eye[0]), dot(p, eye[1]), dot(p, eye[2]), 1.0);
	const float distance = abs(eyePosition.z);

	float f = 1.0;
	if (fog.w == 1.0) {
		f = (fog.y - distance) * fog.x;
	} else if (fog.w == 2.0) {
		f = exp(-fog.z * distance);
	} else if (fog.w == 3.0) {
		const float d = fog.z * distance;
		f = exp(-d * d);
	}
	vFog = clamp(f, 0.0, 1.0);

	vClip = dot(eyePosition, clipPlane);
}
