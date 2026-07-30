#[compute]

#version 450

#VERSION_DEFINES

#define SH_COEFFICIENT_COUNT 9
#define CUBE_FACE_COUNT 6
#define WORKGROUP_WIDTH 8
#define WORKGROUP_SIZE (WORKGROUP_WIDTH * WORKGROUP_WIDTH)

layout(local_size_x = WORKGROUP_WIDTH, local_size_y = WORKGROUP_WIDTH, local_size_z = 1) in;

layout(push_constant, std430) uniform Params
{
	uint source_size;
	uint destination_offset;
	uint subtraction_offset;
	uint subtract_existing;
}
params;

#ifdef PROJECT_FACES

layout(set = 0, binding = 0) uniform samplerCube source_cubemap;

layout(set = 1, binding = 0, std430) restrict writeonly buffer PartialBuffer
{
	vec4 values[];
}
partial;

shared vec4 reductions[SH_COEFFICIENT_COUNT][WORKGROUP_SIZE];

vec3 cube_direction(uint face, vec2 uv)
{
	if (face == 0)
		return normalize(vec3(1.0, -uv.y, -uv.x));

	if (face == 1)
		return normalize(vec3(-1.0, -uv.y, uv.x));

	if (face == 2)
		return normalize(vec3(uv.x, 1.0, uv.y));

	if (face == 3)
		return normalize(vec3(uv.x, -1.0, -uv.y));

	if (face == 4)
		return normalize(vec3(uv.x, -uv.y, 1.0));

	return normalize(vec3(-uv.x, -uv.y, -1.0));
}

void evaluate_sh_basis(vec3 direction, out float basis[SH_COEFFICIENT_COUNT])
{
	float x = direction.x;
	float y = direction.y;
	float z = direction.z;
	basis[0] = 0.282095;
	basis[1] = 0.488603 * y;
	basis[2] = 0.488603 * z;
	basis[3] = 0.488603 * x;
	basis[4] = 1.092548 * x * y;
	basis[5] = 1.092548 * y * z;
	basis[6] = 0.315392 * (3.0 * z * z - 1.0);
	basis[7] = 1.092548 * x * z;
	basis[8] = 0.546274 * (x * x - y * y);
}

void main()
{
	uint local_index = gl_LocalInvocationID.y * WORKGROUP_WIDTH + gl_LocalInvocationID.x;
	uint face = gl_WorkGroupID.z;

	vec4 sums[SH_COEFFICIENT_COUNT];
	for (uint coefficient = 0; coefficient < SH_COEFFICIENT_COUNT; coefficient++)
		sums[coefficient] = vec4(0.0);

	// Each face is one workgroup. Invocations stride over the source so the
	// shader also supports capture sizes other than the initial 16 by 16.
	for (uint y = gl_LocalInvocationID.y; y < params.source_size; y += WORKGROUP_WIDTH)
	{
		for (uint x = gl_LocalInvocationID.x; x < params.source_size; x += WORKGROUP_WIDTH)
		{
			vec2 uv = (vec2(x, y) + 0.5) / float(params.source_size) * 2.0 - 1.0;
			vec3 direction = cube_direction(face, uv);
			vec3 radiance = textureLod(source_cubemap, direction, 0.0).rgb;
			float solid_angle_weight = pow(1.0 + dot(uv, uv), -1.5);

			float basis[SH_COEFFICIENT_COUNT];
			evaluate_sh_basis(direction, basis);
			for (uint coefficient = 0; coefficient < SH_COEFFICIENT_COUNT; coefficient++)
				sums[coefficient] += vec4(radiance * basis[coefficient] * solid_angle_weight, solid_angle_weight);
		}
	}

	for (uint coefficient = 0; coefficient < SH_COEFFICIENT_COUNT; coefficient++)
		reductions[coefficient][local_index] = sums[coefficient];

	groupMemoryBarrier();
	barrier();

	for (uint width = WORKGROUP_SIZE / 2; width > 0; width >>= 1)
	{
		if (local_index < width)
		{
			for (uint coefficient = 0; coefficient < SH_COEFFICIENT_COUNT; coefficient++)
				reductions[coefficient][local_index] += reductions[coefficient][local_index + width];
		}

		groupMemoryBarrier();
		barrier();
	}

	if (local_index == 0)
	{
		for (uint coefficient = 0; coefficient < SH_COEFFICIENT_COUNT; coefficient++)
			partial.values[face * SH_COEFFICIENT_COUNT + coefficient] = reductions[coefficient][0];
	}
}

#endif

#ifdef REDUCE_FACES

layout(set = 0, binding = 0, std430) restrict readonly buffer PartialBuffer
{
	vec4 values[];
}
partial;

layout(set = 1, binding = 0, std430) restrict writeonly buffer DestinationBuffer
{
	vec4 coefficients[];
}
destination;

layout(set = 1, binding = 1, std430) restrict readonly buffer SubtractionBuffer
{
	vec4 coefficients[];
}
subtraction;

void main()
{
	uint coefficient = gl_LocalInvocationID.y * WORKGROUP_WIDTH + gl_LocalInvocationID.x;
	if (coefficient >= SH_COEFFICIENT_COUNT)
		return;

	vec3 projected_radiance = vec3(0.0);
	float total_weight = 0.0;
	for (uint face = 0; face < CUBE_FACE_COUNT; face++)
	{
		vec4 face_value = partial.values[face * SH_COEFFICIENT_COUNT + coefficient];
		projected_radiance += face_value.rgb;

		if (coefficient == 0)
			total_weight += face_value.a;
	}

	// Every coefficient stores the same accumulated weight, so non-zero
	// coefficient invocations read it from coefficient zero.
	if (coefficient != 0)
	{
		for (uint face = 0; face < CUBE_FACE_COUNT; face++)
			total_weight += partial.values[face * SH_COEFFICIENT_COUNT].a;
	}

	float normalization = total_weight > 0.0 ? (4.0 * 3.14159265359) / total_weight : 0.0;
	float diffuse_kernel = coefficient == 0 ? 3.14159265359 : (coefficient < 4 ? 2.09439510239 : 0.78539816339);
	vec3 result = projected_radiance * normalization * diffuse_kernel;

	// Non-base layers retain only the circuit's change from permanent lighting.
	// Signed values are intentional: a switched light can alter shadowed energy.
	if (params.subtract_existing != 0)
		result -= subtraction.coefficients[params.subtraction_offset + coefficient].rgb;

	destination.coefficients[params.destination_offset + coefficient] = vec4(result, 0.0);
}

#endif
