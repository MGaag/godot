// Samples the renderer-owned GT7 SDR lookup before display encoding.
// The shaper and texel-center mapping match GT7LUT's one-time bake exactly.
vec3 tonemap_gt7(vec3 color) {
	// Match the approved reference's middle gray without changing exposure metering.
	color = max(color, vec3(0.0)) * 1.9990369056129782;
	vec3 coordinate = log2(vec3(1.0) + color / 0.01) / log2(1.0 + 8192.0 / 0.01);
	coordinate = (clamp(coordinate, vec3(0.0), vec3(1.0)) * 64.0 + 0.5) / 65.0;
	return textureLod(source_gt7_lut, coordinate, 0.0).rgb;
}
