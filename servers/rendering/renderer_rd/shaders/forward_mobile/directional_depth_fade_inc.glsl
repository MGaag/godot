// Fade each directional shadow comparison by the light-space blocker gap.
// The material supplies the world-space distance at half shadow strength.
#ifdef DIRECTIONAL_SHADOW_FADE_USED
float directional_depth_fade_tap(vec2 uv, float receiver_depth, vec4 atlas_rect, float depth_range, float half_distance) {
	vec2 texel = scene_data.directional_shadow_pixel_size;
	uv = clamp(uv, atlas_rect.xy + texel * 0.5, atlas_rect.zw - texel * 0.5);
	vec2 weights = fract(uv / texel - 0.5);
	vec4 depths = textureGather(sampler2D(directional_shadow_atlas, SAMPLER_NEAREST_CLAMP), uv);
	vec4 gap = max(depths - receiver_depth, vec4(0.0)) * depth_range;
	vec4 visibility = exp2(-gap / half_distance);
	return mix(mix(visibility.w, visibility.z, weights.x), mix(visibility.x, visibility.y, weights.x), weights.y);
}

half sample_directional_depth_fade(vec4 coord, vec2 filter_size, vec4 atlas_rect, float depth_range, float half_distance, float taa_frame_count) {
	if (half_distance <= 0.0) {
		return sample_directional_pcf_shadow(directional_shadow_atlas, filter_size, coord, taa_frame_count);
	}
	if (sc_directional_soft_shadow_samples() == 0) {
		return half(directional_depth_fade_tap(coord.xy, coord.z, atlas_rect, depth_range, half_distance));
	}
	float angle = quick_hash(gl_FragCoord.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI;
	mat2 rotation = mat2(vec2(cos(angle), -sin(angle)), vec2(sin(angle), cos(angle)));
	float visibility = 0.0;
	SPEC_CONSTANT_LOOP_ANNOTATION
	for (uint sample_index = 0; sample_index < sc_directional_soft_shadow_samples(); sample_index++) {
		vec2 uv = coord.xy + filter_size * (rotation * scene_data_block.data.directional_soft_shadow_kernel[sample_index].xy);
		visibility += directional_depth_fade_tap(uv, coord.z, atlas_rect, depth_range, half_distance);
	}
	return half(visibility / float(sc_directional_soft_shadow_samples()));
}
#endif
