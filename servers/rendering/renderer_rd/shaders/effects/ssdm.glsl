#[compute]

#version 450

#VERSION_DEFINES

// One shader source implements every SSDM compute stage. Shader variants select
// exactly one MODE_* block below.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Dispatches are rounded up to the 8x8 workgroup size, so every main function
// must reject invocations outside size.
layout(push_constant, std430) uniform Params
{
	ivec2 size;
	int source_mip;
	int coarsest_mip;
} params;

// Projection data has a stable descriptor set across SSDM variants. Material
// displacement itself is stored in normalized screen UV coordinates.
layout(set = 2, binding = 0, std140) uniform ProjectionData
{
	mat4 projection;
	mat4 inv_projection;
} projection_data;

// Binding meaning varies by MODE_*: source is the value being transformed,
// while depth_texture supplies scene depth or a previously solved mapping.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;

// Formats must match the destination textures allocated in SSDM::_dispatch().
// Update the C++ allocation and this declaration together.
#ifdef MODE_WARP_COLOR
layout(rgb10_a2, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#elif defined(MODE_WARP_DEPTH)
layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#elif defined(MODE_SOLVE) || defined(MODE_WARP_VELOCITY)
layout(rg16f, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#else
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D destination;
#endif

// Project a view-space position into normalized screen UV coordinates using
// the renderer's corrected projection.
vec2 project_view(vec3 position)
{
	vec4 clip_position = projection_data.projection * vec4(position, 1.0);
	return clip_position.xy / clip_position.w * 0.5 + 0.5;
}



// Invalid values spread through mip averaging, so reject them before they can
// corrupt a larger portion of the mapping pyramid.
bool finite_vec2(vec2 value)
{
	return !any(isnan(value)) && !any(isinf(value));
}



// Packed SSDM stages can use this variant when all four channels need checking.
bool finite_vec4(vec4 value)
{
	return !any(isnan(value)) && !any(isinf(value));
}



#ifdef MODE_WARP_VELOCITY
// Resample velocity through the completed inverse map so temporal effects use
// the same visible source point as displaced color and depth.
void main()
{
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)))
	{
		return;
	}

	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 mapping_offset = texelFetch(depth_texture, pixel, 0).rg;
	vec2 source_uv = clamp(target_uv + mapping_offset, vec2(0.0), vec2(1.0));

	imageStore(destination, pixel, textureLod(source, source_uv, 0.0));
}
#endif



#ifdef MODE_PROJECT
// Pack material displacement and source depth into the base pyramid level. XY
// is source-to-target UV displacement and Z remains attached source depth.
void main()
{
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)))
	{
		return;
	}

	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec2 displacement = texelFetch(source, pixel, 0).rg;

	// Materials emit the source-to-target offset directly in normalized screen
	// UV coordinates. Zero is the identity mapping for both geometry and the
	// unwritten background; depth follows the source pixel during the gather.
	if (!finite_vec2(displacement))
	{
		imageStore(destination, pixel, vec4(0.0, 0.0, depth, 1.0));
		return;
	}

	imageStore(destination, pixel, vec4(displacement, depth, 1.0));
}
#endif



#ifdef MODE_DOWNSAMPLE
// Average clamped 2x2 neighborhoods to build the coarse displacement pyramid.
// Coarse levels give the inverse solve a stable starting estimate.
void main()
{
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)))
	{
		return;
	}

	ivec2 source_pixel = pixel * 2;
	ivec2 source_max = textureSize(source, params.source_mip) - 1;

	// Clamp edge taps because odd-sized mip levels may not have a complete
	// source neighborhood.
	vec2 displacement_sum = texelFetch(source, source_pixel, params.source_mip).rg;
	displacement_sum += texelFetch(source, min(source_pixel + ivec2(1, 0), source_max), params.source_mip).rg;
	displacement_sum += texelFetch(source, min(source_pixel + ivec2(0, 1), source_max), params.source_mip).rg;
	displacement_sum += texelFetch(source, min(source_pixel + ivec2(1, 1), source_max), params.source_mip).rg;

	imageStore(destination, pixel, vec4(displacement_sum * 0.25, 0.0, 0.0));
}
#endif



#ifdef MODE_SOLVE
// Average four samples at the corners of the current mip texel. The half-texel
// offsets are important: filtered samples include the estimate's local source
// neighborhood, which is the barycenter construction described by the paper.
vec2 sample_barycenter_displacement(vec2 source_uv, vec2 half_texel)
{
	vec2 sample_uv_00 = clamp(source_uv + vec2(-half_texel.x, -half_texel.y), vec2(0.0), vec2(1.0));
	vec2 sample_uv_10 = clamp(source_uv + vec2(half_texel.x, -half_texel.y), vec2(0.0), vec2(1.0));
	vec2 sample_uv_01 = clamp(source_uv + vec2(-half_texel.x, half_texel.y), vec2(0.0), vec2(1.0));
	vec2 sample_uv_11 = clamp(source_uv + vec2(half_texel.x, half_texel.y), vec2(0.0), vec2(1.0));

	vec2 average_displacement = textureLod(source, sample_uv_00, float(params.source_mip)).rg;
	average_displacement += textureLod(source, sample_uv_10, float(params.source_mip)).rg;
	average_displacement += textureLod(source, sample_uv_01, float(params.source_mip)).rg;
	average_displacement += textureLod(source, sample_uv_11, float(params.source_mip)).rg;
	return average_displacement * 0.25;
}



// Follow the 2008 SSDM paper's coarse-to-fine barycenter search. Each mip takes
// four displacement samples around the current source estimate, averages them,
// and carries the corrected estimate into the next finer level of pyramid B.
void main()
{
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)))
	{
		return;
	}

	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);

	// The coarsest level starts from identity. Finer levels add the inverse-map
	// offset found by the preceding, coarser solve.
	vec2 source_uv = target_uv;
	if (params.source_mip != params.coarsest_mip)
	{
		source_uv += textureLod(depth_texture, target_uv, 0.0).rg;
	}

	vec2 half_texel = 0.5 / vec2(params.size);

	// Refinement happens by descending the mip pyramid, not by repeatedly
	// sampling one level. The next finer dispatch reuses this result with a
	// smaller texel footprint and therefore a more precise source neighborhood.
	vec2 average_displacement = sample_barycenter_displacement(source_uv, half_texel);

	// Pyramid A stores source-to-target displacement. Inverting it therefore
	// subtracts the averaged vector from the target to estimate its source.
	vec2 proposed_source_uv = target_uv - average_displacement;
	if (finite_vec2(proposed_source_uv))
	{
		source_uv = clamp(proposed_source_uv, vec2(0.0), vec2(1.0));
	}

	// Store an offset rather than an absolute UV. Zero remains the identity map
	// and consumers can add the result directly to their target UV.
	imageStore(destination, pixel, vec4(source_uv - target_uv, 0.0, 0.0));
}
#endif



#ifdef MODE_WARP_COLOR
// Gather color from the source position selected by the inverse mapping.
void main()
{
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)))
	{
		return;
	}

	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 mapping_offset = texelFetch(depth_texture, pixel, 0).rg;
	vec2 source_uv = clamp(target_uv + mapping_offset, vec2(0.0), vec2(1.0));

	imageStore(destination, pixel, textureLod(source, source_uv, 0.0));
}
#endif



#ifdef MODE_WARP_DEPTH
// Gather depth from the same visible source position used for color. The base
// pyramid keeps source depth in B while XY contains displacement.
void main()
{
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)))
	{
		return;
	}

	vec2 target_uv = (vec2(pixel) + 0.5) / vec2(params.size);
	vec2 mapping_offset = texelFetch(depth_texture, pixel, 0).rg;
	vec2 source_uv = clamp(target_uv + mapping_offset, vec2(0.0), vec2(1.0));
	float target_depth = textureLod(source, source_uv, 0.0).b;

	imageStore(destination, pixel, vec4(target_depth));
}
#endif
