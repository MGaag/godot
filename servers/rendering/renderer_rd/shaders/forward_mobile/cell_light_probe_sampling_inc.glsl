uint cell_light_flatten_coordinate(ivec3 coordinate)
{
	return uint(coordinate.x + coordinate.z * int(cell_light_probe_metadata.dimensions.x) + coordinate.y * int(cell_light_probe_metadata.dimensions.x * cell_light_probe_metadata.dimensions.z));
}

float cell_light_cardinal_transmissibility(ivec3 from, ivec3 to)
{
	ivec3 delta = to - from;
	if (all(equal(delta, ivec3(0))))
		return 1.0;

	vec4 edges = cell_light_probe_edges.values[cell_light_flatten_coordinate(from)];
	if (all(equal(delta, ivec3(1, 0, 0))))
		return edges.x;

	if (all(equal(delta, ivec3(-1, 0, 0))))
		return edges.y;

	if (all(equal(delta, ivec3(0, 0, 1))))
		return edges.z;

	if (all(equal(delta, ivec3(0, 0, -1))))
		return edges.w;

	return 0.0;
}

float cell_light_transmissibility(ivec3 receiver, ivec3 candidate)
{
	ivec3 delta = candidate - receiver;
	if (abs(delta.x) + abs(delta.z) <= 1)
		return cell_light_cardinal_transmissibility(receiver, candidate);

	// A diagonal probe may contribute through either of its two adjacent
	// routes. Taking the better route avoids an arbitrary axis-order seam.
	ivec3 via_x = ivec3(candidate.x, receiver.y, receiver.z);
	ivec3 via_z = ivec3(receiver.x, receiver.y, candidate.z);
	float route_x = cell_light_cardinal_transmissibility(receiver, via_x) * cell_light_cardinal_transmissibility(via_x, candidate);
	float route_z = cell_light_cardinal_transmissibility(receiver, via_z) * cell_light_cardinal_transmissibility(via_z, candidate);
	return max(route_x, route_z);
}

vec3 sample_cell_light_probe_grid(vec3 world_position, vec3 bent_direction, float aperture_cosine)
{
	if (cell_light_probe_metadata.enabled == 0)
		return vec3(0.0);

	vec3 grid_position = (world_position - cell_light_probe_metadata.origin) * cell_light_probe_metadata.inverse_cell_size;
	ivec3 maximum_coordinate = ivec3(cell_light_probe_metadata.dimensions) - ivec3(1);
	int deck = clamp(int(round(grid_position.y)), 0, maximum_coordinate.y);
	ivec2 base_xz = ivec2(floor(grid_position.xz));
	vec2 fraction = fract(grid_position.xz);
	ivec3 receiver = clamp(ivec3(round(grid_position.x), deck, round(grid_position.z)), ivec3(0), maximum_coordinate);

	ivec3 coordinates[4] = ivec3[](
		ivec3(base_xz.x, deck, base_xz.y),
		ivec3(base_xz.x + 1, deck, base_xz.y),
		ivec3(base_xz.x, deck, base_xz.y + 1),
		ivec3(base_xz.x + 1, deck, base_xz.y + 1)
	);
	float weights[4] = float[](
		(1.0 - fraction.x) * (1.0 - fraction.y),
		fraction.x * (1.0 - fraction.y),
		(1.0 - fraction.x) * fraction.y,
		fraction.x * fraction.y
	);

	float weight_sum = 0.0;
	int probe_indices[4];
	for (uint sample_index = 0; sample_index < 4; sample_index++)
	{
		coordinates[sample_index] = clamp(coordinates[sample_index], ivec3(0), maximum_coordinate);
		probe_indices[sample_index] = cell_light_probe_indices.values[cell_light_flatten_coordinate(coordinates[sample_index])];
		if (probe_indices[sample_index] < 0 || probe_indices[sample_index] >= int(cell_light_probe_metadata.probe_count))
			weights[sample_index] = 0.0;
		else
			weights[sample_index] *= cell_light_transmissibility(receiver, coordinates[sample_index]);

		weight_sum += weights[sample_index];
	}

	if (weight_sum <= 0.0)
		return vec3(0.0);

	vec3 sh[9];
	for (uint coefficient = 0; coefficient < 9; coefficient++)
	{
		sh[coefficient] = vec3(0.0);
		for (uint sample_index = 0; sample_index < 4; sample_index++)
		{
			if (weights[sample_index] <= 0.0)
				continue;

			uint source_index = uint(probe_indices[sample_index]) * 9 + coefficient;
			sh[coefficient] += cell_light_probe_coefficients.values[source_index].rgb * (weights[sample_index] / weight_sum);
		}
	}

	vec3 direction = normalize(bent_direction);
	float clamped_aperture_cosine = clamp(aperture_cosine, 0.0, 1.0);
	float band_1_window = 0.5 * (1.0 + clamped_aperture_cosine);
	float band_2_window = 0.5 * clamped_aperture_cosine * (1.0 + clamped_aperture_cosine);
	vec3 irradiance = sh[0] * 0.282095;
	irradiance += sh[1] * (0.488603 * direction.y * band_1_window);
	irradiance += sh[2] * (0.488603 * direction.z * band_1_window);
	irradiance += sh[3] * (0.488603 * direction.x * band_1_window);
	irradiance += sh[4] * (1.092548 * direction.x * direction.y * band_2_window);
	irradiance += sh[5] * (1.092548 * direction.y * direction.z * band_2_window);
	irradiance += sh[6] * (0.315392 * (3.0 * direction.z * direction.z - 1.0) * band_2_window);
	irradiance += sh[7] * (1.092548 * direction.x * direction.z * band_2_window);
	irradiance += sh[8] * (0.546274 * (direction.x * direction.x - direction.y * direction.y) * band_2_window);
	return max(irradiance, vec3(0.0));
}
