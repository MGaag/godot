#[compute]

#version 450

#VERSION_DEFINES

#define SH_COEFFICIENT_COUNT 9

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Params
{
	uint probe_count;
	uint layer_count;
	uint pad0;
	uint pad1;
}
params;

layout(set = 0, binding = 0, std430) restrict readonly buffer LayerBuffer
{
	vec4 coefficients[];
}
layers;

layout(set = 0, binding = 1, std430) restrict readonly buffer WeightBuffer
{
	float values[];
}
weights;

layout(set = 1, binding = 0, std430) restrict writeonly buffer ResolvedBuffer
{
	vec4 coefficients[];
}
resolved;

void main()
{
	uint destination_index = gl_GlobalInvocationID.x;
	uint coefficient_count = params.probe_count * SH_COEFFICIENT_COUNT;
	if (destination_index >= coefficient_count)
		return;

	uint probe = destination_index / SH_COEFFICIENT_COUNT;
	uint coefficient = destination_index % SH_COEFFICIENT_COUNT;
	vec3 irradiance = vec3(0.0);

	for (uint layer = 0; layer < params.layer_count; layer++)
	{
		uint source_index = (layer * params.probe_count + probe) * SH_COEFFICIENT_COUNT + coefficient;
		irradiance += layers.coefficients[source_index].rgb * weights.values[layer];
	}

	resolved.coefficients[destination_index] = vec4(irradiance, 0.0);
}
