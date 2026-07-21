#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	int source_mip;
	int refinement_steps;
	mat4 projection;
	mat4 inv_projection;
} params;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
#ifdef MODE_WARP_COLOR
layout(rgb10_a2, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#elif defined(MODE_WARP_DEPTH)
layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#elif defined(MODE_SOLVE) || defined(MODE_WARP_VELOCITY)
layout(rg16f, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#else
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#endif

#ifdef MODE_WARP_VELOCITY
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 source_uv = texelFetch(depth_texture, pixel, 0).rg;
	imageStore(destination, pixel, textureLod(source, source_uv, 0.0));
}
#endif

vec2 project_view(vec3 p) {
	vec4 clip = params.projection * vec4(p, 1.0);
	return clip.xy / clip.w * 0.5 + 0.5;
}

#ifdef MODE_PROJECT
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.size);
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 view = view_h.xyz / view_h.w;
	float displacement = texelFetch(source, pixel, 0).r;
	vec3 displaced = view + normalize(view) * displacement;
	vec4 displaced_clip = params.projection * vec4(displaced, 1.0);
	float target_depth = displacement == 0.0 ? depth : displaced_clip.z / displaced_clip.w;
	imageStore(destination, pixel, vec4(project_view(displaced) - uv, target_depth, 1.0));
}
#endif

#ifdef MODE_DOWNSAMPLE
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	ivec2 p = pixel * 2;
	ivec2 source_max = textureSize(source, params.source_mip) - 1;
	vec2 v = texelFetch(source, p, params.source_mip).rg;
	v += texelFetch(source, min(p + ivec2(1, 0), source_max), params.source_mip).rg;
	v += texelFetch(source, min(p + ivec2(0, 1), source_max), params.source_mip).rg;
	v += texelFetch(source, min(p + ivec2(1, 1), source_max), params.source_mip).rg;
	imageStore(destination, pixel, vec4(v * 0.25, 0.0, 0.0));
}
#endif

#ifdef MODE_SOLVE
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 source_uv = uv;
	for (int mip = params.source_mip; mip >= 0; mip--) {
		// Linear sampling is the barycenter of the four surrounding scatter vectors.
		for (int iteration = 0; iteration < params.refinement_steps; iteration++) {
			vec2 scatter = textureLod(source, source_uv, float(mip)).rg;
			source_uv = clamp(uv - scatter, vec2(0.0), vec2(1.0));
		}
	}
	imageStore(destination, pixel, vec4(source_uv, 0.0, 0.0));
}
#endif

#ifdef MODE_WARP_COLOR
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 source_uv = texelFetch(depth_texture, pixel, 0).rg;
	imageStore(destination, pixel, textureLod(source, source_uv, 0.0));
}
#endif

#ifdef MODE_WARP_DEPTH
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 source_uv = texelFetch(depth_texture, pixel, 0).rg;
	float target_depth = textureLod(source, source_uv, 0.0).b;
	imageStore(destination, pixel, vec4(target_depth));
}
#endif
