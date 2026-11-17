#[compute]
#version 450

// Compacts GPU-authored instance records for one camera or shadow volume.
// The caller retains both the placement sources and reusable output buffers.
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D source_positions;
layout(set = 0, binding = 1, std430) readonly buffer SourceInstances { float words[]; } src;
layout(set = 0, binding = 2, std430) readonly buffer SourceCommand { uint words[]; } src_command;
layout(set = 0, binding = 3, std430) buffer VisibleInstances { uint words[]; } dst;
layout(set = 0, binding = 4, std430) buffer VisibleCommand { uint words[]; } dst_command;
layout(set = 0, binding = 5, std140) uniform Params {
	vec4 planes[16];
	vec4 origin_tile_height;
	float radius;
	uint max_instances;
	uint source_width;
	uint layout_info;
} params;

bool accepts_instance(out uint cell) {
	// Read only the actual accepted source instances, not unused capacity.
	uint instance = gl_GlobalInvocationID.x;
	if (instance >= params.max_instances || instance >= src_command.words[1]) {
		return false;
	}
	uint plane_count = params.layout_info & 255u;
	uint stride = (params.layout_info >> 8u) & 255u;
	uint index_word = (params.layout_info >> 16u) & 255u;
	uint source_bank = (params.layout_info >> 24u) & 1u;
	uint source = (instance + source_bank * params.max_instances) * stride;
	cell = uint(src.words[source + index_word] + 0.5);
	vec4 position = texelFetch(source_positions, ivec2(int(cell % params.source_width), int(cell / params.source_width)), 0);
	vec3 center = vec3(params.origin_tile_height.x + position.r * params.origin_tile_height.z,
			position.b * params.origin_tile_height.w,
			params.origin_tile_height.y + position.g * params.origin_tile_height.z);

	// One millimeter plus one part per million of world position protects the
	// half-space test from floating-point cancellation at a frustum boundary.
	float tolerance = 0.001 + 0.000001 * max(max(abs(center.x), abs(center.y)), abs(center.z));
	for (uint i = 0u; i < plane_count; i++) {
		vec4 plane = params.planes[i];
		if (dot(plane.xyz, center) + plane.w < -(params.radius + tolerance)) {
			return false;
		}
	}

	return true;
}

shared uint accepted_count;
shared uint output_base;
void main() {
	// Reserve once per workgroup to reduce contention on the global draw count.
	if (gl_LocalInvocationIndex == 0u) {
		accepted_count = 0u;
	}
	barrier();
	uint cell = 0u;
	bool accepted = accepts_instance(cell);
	uint local_slot = 0u;
	if (accepted) {
		local_slot = atomicAdd(accepted_count, 1u);
	}
	barrier();
	if (gl_LocalInvocationIndex == 0u && accepted_count > 0u) {
		output_base = atomicAdd(dst.words[params.max_instances], accepted_count);
	}
	barrier();
	if (accepted) {
		dst.words[output_base + local_slot] = cell;
	}
	// Each group arrives after its writes. The final arrival publishes the
	// count and clears the two scratch counters for the next ordered pass.
	memoryBarrierBuffer();
	barrier();
	if (gl_LocalInvocationIndex == 0u) {
		uint completed_groups = atomicAdd(dst.words[params.max_instances + 1u], 1u) + 1u;
		if (completed_groups == src_command.words[5]) {
			dst_command.words[1] = atomicExchange(dst.words[params.max_instances], 0u);
			atomicExchange(dst.words[params.max_instances + 1u], 0u);
		}
	}
}
