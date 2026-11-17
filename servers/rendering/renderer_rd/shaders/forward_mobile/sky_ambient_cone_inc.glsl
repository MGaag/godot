// Mobile diffuse sky sampling. Material visibility cones select normalized
// cosine-convolved angular levels; other materials retain the standard IBL path.
// SkyRD owns convolution, storage, exposure and invalidation. Materials own AO.

hvec3 mobile_unoccluded_sky(vec3 direction, vec3 direction_ddx, vec3 direction_ddy) {
	vec2 uv = vec3_to_oct_with_border(direction, vec2(scene_data.radiance_border_size, 1.0 - 2.0 * scene_data.radiance_border_size));
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	float lod = vec3_to_oct_lod(direction_ddx, direction_ddy, scene_data.radiance_pixel_size);
	return hvec3(textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(uv, MAX_ROUGHNESS_LOD), lod).rgb);
#else
	return hvec3(textureLod(sampler2D(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, MAX_ROUGHNESS_LOD).rgb);
#endif
}

hvec3 mobile_ambient_sky(vec3 direction, vec3 direction_ddx, vec3 direction_ddy, float aperture_cosine, float coverage) {
#ifdef AMBIENT_SKY_CONE_USED
	ivec3 dimensions = textureSize(sampler2DArray(sky_cosine_cones, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), 0);
	float weight = dimensions.z > 1 ? clamp(coverage, 0.0, 1.0) : 0.0;
	if (weight > 0.0) {
		// Each angular level doubles linear footprint and quadruples solid angle.
		// Omega/(2*pi) = 1-cos(theta), so no inverse trig is needed. Direction
		// derivatives impose a minimum angular footprint to limit aliasing.
		float omega_fraction = 1.0 - clamp(aperture_cosine, 0.0, 1.0);
		float screen_fraction = 0.5 * max(dot(direction_ddx, direction_ddx), dot(direction_ddy, direction_ddy));
		float last = float(dimensions.z - 1);
		float lod = clamp(0.5 * log2(max(max(omega_fraction, screen_fraction), 1e-10)) + last, 0.0, last);
		float level = floor(lod);
		float border = 1.0 / float(dimensions.x);
		vec2 uv = vec3_to_oct_with_border(direction, vec2(border, 1.0 - 2.0 * border));
		hvec3 a = hvec3(textureLod(sampler2DArray(sky_cosine_cones, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(uv, level), 0.0).rgb);
		hvec3 b = hvec3(textureLod(sampler2DArray(sky_cosine_cones, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(uv, min(level + 1.0, last)), 0.0).rgb);
		hvec3 cone = mix(a, b, half(fract(lod)));
		// Only mixed material boundaries sample both lighting models. Grass
		// retains its original lookup; full rock samples just the cone hierarchy.
		if (weight >= 1.0) {
			return cone;
		}
		return mix(mobile_unoccluded_sky(direction, direction_ddx, direction_ddy), cone, half(weight));
	}
#endif
	return mobile_unoccluded_sky(direction, direction_ddx, direction_ddy);
}
