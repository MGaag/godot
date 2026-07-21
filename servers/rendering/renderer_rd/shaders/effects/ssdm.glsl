#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	int source_mip;
	int refinement_steps;
	int coarsest_mip;
} params;

layout(set = 2, binding = 0, std140) uniform ProjectionData {
	mat4 projection;
	mat4 inv_projection;
} projection_data;

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
	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 source_uv = clamp(target_uv + texelFetch(depth_texture, pixel, 0).rg, vec2(0.0), vec2(1.0));
	imageStore(destination, pixel, textureLod(source, source_uv, 0.0));
}
#endif

vec2 project_view(vec3 p) {
	vec4 clip = projection_data.projection * vec4(p, 1.0);
	return clip.xy / clip.w * 0.5 + 0.5;
}

bool finite_vec2(vec2 value) {
	return !any(isnan(value)) && !any(isinf(value));
}

bool finite_vec4(vec4 value) {
	return !any(isnan(value)) && !any(isinf(value));
}

#ifdef MODE_PROJECT
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.size);
	float depth = texelFetch(depth_texture, pixel, 0).r;
	float displacement = texelFetch(source, pixel, 0).r;
	// Reverse-Z background has depth 0 and may reconstruct at infinity. Zero
	// displacement is identity and must never pass through projection math.
	if (displacement == 0.0 || depth <= 0.0 || isnan(displacement) || isinf(displacement)) {
		imageStore(destination, pixel, vec4(0.0, 0.0, depth, 1.0));
		return;
	}
	vec4 view_h = projection_data.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	if (!finite_vec4(view_h) || abs(view_h.w) < 1e-6) {
		imageStore(destination, pixel, vec4(0.0, 0.0, depth, 1.0));
		return;
	}
	vec3 view = view_h.xyz / view_h.w;
	// DISPLACEMENT is a world-unit distance along the view-plane normal. The
	// camera looks down -Z in view space, so +Z moves the sample toward it.
	// Moving along normalize(view) would only move along the camera ray and
	// therefore could never produce an XY screen-space displacement.
	vec3 displaced = view + vec3(0.0, 0.0, displacement);
	vec4 displaced_clip = projection_data.projection * vec4(displaced, 1.0);
	if (!finite_vec4(displaced_clip) || displaced_clip.w <= 1e-6) {
		imageStore(destination, pixel, vec4(0.0, 0.0, depth, 1.0));
		return;
	}
	vec2 scatter = displaced_clip.xy / displaced_clip.w * 0.5 + 0.5 - uv;
	float target_depth = displaced_clip.z / displaced_clip.w;
	if (!finite_vec2(scatter) || isnan(target_depth) || isinf(target_depth) || target_depth < 0.0 || target_depth > 1.0) {
		imageStore(destination, pixel, vec4(0.0, 0.0, depth, 1.0));
		return;
	}
	imageStore(destination, pixel, vec4(scatter, target_depth, 1.0));
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
vec2 bilerp(vec2 p00, vec2 p10, vec2 p01, vec2 p11, vec2 uv) {
	return mix(mix(p00, p10, uv.x), mix(p01, p11, uv.x), uv.y);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 source_uv = params.source_mip == params.coarsest_mip ? target_uv : target_uv + textureLod(depth_texture, target_uv, 0.0).rg;
	vec2 half_texel = 0.5 / vec2(params.size);
	for (int iteration = 0; iteration < params.refinement_steps; iteration++) {
		vec2 previous_source_uv = source_uv;
		vec2 q00 = clamp(source_uv + vec2(-half_texel.x, -half_texel.y), vec2(0.0), vec2(1.0));
		vec2 q10 = clamp(source_uv + vec2( half_texel.x, -half_texel.y), vec2(0.0), vec2(1.0));
		vec2 q01 = clamp(source_uv + vec2(-half_texel.x,  half_texel.y), vec2(0.0), vec2(1.0));
		vec2 q11 = clamp(source_uv + vec2( half_texel.x,  half_texel.y), vec2(0.0), vec2(1.0));
		vec2 t00 = q00 + textureLod(source, q00, float(params.source_mip)).rg;
		vec2 t10 = q10 + textureLod(source, q10, float(params.source_mip)).rg;
		vec2 t01 = q01 + textureLod(source, q01, float(params.source_mip)).rg;
		vec2 t11 = q11 + textureLod(source, q11, float(params.source_mip)).rg;
		vec2 local_uv = vec2(0.5);
		for (int inverse_iteration = 0; inverse_iteration < 3; inverse_iteration++) {
			vec2 residual = bilerp(t00, t10, t01, t11, local_uv) - target_uv;
			vec2 derivative_u = mix(t10 - t00, t11 - t01, local_uv.y);
			vec2 derivative_v = mix(t01 - t00, t11 - t10, local_uv.x);
			float determinant = derivative_u.x * derivative_v.y - derivative_u.y * derivative_v.x;
			if (abs(determinant) < 1e-10) {
				break;
			}
			local_uv -= vec2(
					residual.x * derivative_v.y - residual.y * derivative_v.x,
					derivative_u.x * residual.y - derivative_u.y * residual.x) /
					determinant;
		}
		source_uv = clamp(bilerp(q00, q10, q01, q11, local_uv), vec2(0.0), vec2(1.0));
		if (!finite_vec2(source_uv)) {
			source_uv = previous_source_uv;
			break;
		}
	}
	imageStore(destination, pixel, vec4(source_uv - target_uv, 0.0, 0.0));
}
#endif

#ifdef MODE_WARP_COLOR
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 source_uv = clamp(target_uv + texelFetch(depth_texture, pixel, 0).rg, vec2(0.0), vec2(1.0));
	imageStore(destination, pixel, textureLod(source, source_uv, 0.0));
}
#endif

#ifdef MODE_WARP_DEPTH
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) return;
	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 source_uv = clamp(target_uv + texelFetch(depth_texture, pixel, 0).rg, vec2(0.0), vec2(1.0));
	float target_depth = textureLod(source, source_uv, 0.0).b;
	imageStore(destination, pixel, vec4(target_depth));
}
#endif
