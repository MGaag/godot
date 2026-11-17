// SkyRD's normalized cosine-weighted aperture hierarchy. Each array layer
// retains directional resolution; runtime interpolates angular footprints.
#[compute]
#version 450
#VERSION_DEFINES

#include "../oct_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D sky_source;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2DArray cone_destination;
layout(push_constant, std430) uniform Params {
	uint size;
	uint levels;
	uint samples;
	float border;
}
params;

void main() {
	uvec3 id = gl_GlobalInvocationID;
	if (id.x >= params.size || id.y >= params.size || id.z >= params.levels) {
		return;
	}
	float usable = 1.0 - 2.0 * params.border;
	// One output texel of octahedral padding, independent of the sky source.
	float output_usable = 1.0 - 2.0 / float(params.size);
	vec3 n = oct_to_vec3_with_border((vec2(id.xy) + 0.5) / float(params.size), output_usable);
	vec3 tangent = normalize(cross(abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), n));
	vec3 bitangent = cross(n, tangent);
	// Linear filter footprints double at each angular level. Their solid
	// angles quadruple, reaching exactly a hemisphere at the final level.
	float omega_fraction = exp2(2.0 * (float(id.z) - float(params.levels - 1u)));
	float cone_cosine = 1.0 - omega_fraction;
	float ao = 1.0 - cone_cosine * cone_cosine;
	vec3 sum = vec3(0.0);
	for (uint i = 0u; i < params.samples; i++) {
		// A truncated cosine-weighted distribution has r^2 = u*AO.
		// Importance sampling includes the cosine weight and normalizes the
		// integral, so no extra N dot L factor or inverse trig is needed.
		float u = (float(i) + 0.5) / float(params.samples);
		float phi = 6.28318530718 * float(bitfieldReverse(i)) * 2.3283064365386963e-10;
		float r = sqrt(u * ao);
		float z = sqrt(max(0.0, 1.0 - r * r));
		vec3 direction = tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + n * z;
		vec2 uv = vec3_to_oct_with_border(direction, vec2(params.border, usable));
		// The source is already the sky's first antialiased spatial level.
		// Do not replace the aperture convolution with a broad source mip.
		sum += textureLod(sky_source, uv, 0.0).rgb;
	}
	imageStore(cone_destination, ivec3(id), vec4(sum / float(params.samples), 1.0));
}
