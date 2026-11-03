// OpenGL's fixed function fragment stage, as far as the renderer uses it. Built once
// for each texture environment it uses, each with and without the portal's clip
// plane: cmake/RtcwShaders.cmake puts the #version and the #defines before this.
// Alpha testing is the GPU's own.

layout(std140, binding = 0) uniform Fragment {
	vec4 fogColor;
};

#if defined(TEX) || defined(TEX_REPLACE) || defined(MULTI_MODULATE) || defined(MULTI_ADD) || defined(MULTI_DECAL)
layout(binding = 0) uniform sampler2D texture0;
#endif
#if defined(MULTI_MODULATE) || defined(MULTI_ADD) || defined(MULTI_DECAL)
layout(binding = 1) uniform sampler2D texture1;
#endif

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord0;
layout(location = 2) in vec2 vTexCoord1;
layout(location = 3) in float vFog;
layout(location = 4) in float vClip;

layout(location = 0) out vec4 outColor;

void main() {
#ifdef CLIP
	if (vClip < 0.0) {
		discard;
	}
#endif

#if defined(TEX_REPLACE)
	vec4 color = texture(texture0, vTexCoord0);
#elif defined(COLOR)
	vec4 color = vColor;
#else
	vec4 color = texture(texture0, vTexCoord0) * vColor;
#endif

#if defined(MULTI_MODULATE)
	color *= texture(texture1, vTexCoord1);
#elif defined(MULTI_ADD)
	const vec4 second = texture(texture1, vTexCoord1);
	color = vec4(color.rgb + second.rgb, color.a * second.a);
#elif defined(MULTI_DECAL)
	const vec4 second = texture(texture1, vTexCoord1);
	color = vec4(mix(color.rgb, second.rgb, second.a), color.a);
#endif

	outColor = vec4(mix(fogColor.rgb, color.rgb, vFog), color.a);
}
