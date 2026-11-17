#ifndef SKY_CONE_FILTER_RD_H
#define SKY_CONE_FILTER_RD_H

#include "servers/rendering/renderer_rd/shaders/environment/sky_cone_filter.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Normalized cosine-cone sky convolution. SkyRD owns the source lifetime and
// per-sky output; this component owns only the reusable GPU filter pipeline.
class SkyConeFilterRD {
	SkyConeFilterShaderRD shader;
	RID version;
	RID pipeline;
	RID sampler;

public:
	static constexpr uint32_t SIZE = 64;
	static constexpr uint32_t LEVELS = 8;

	// Allocate one fixed-resolution angular hierarchy. Keeping every level at
	// 64 square preserves normal-dependent lighting even for a full hemisphere.
	RID create_texture();
	// Refilter after the sky source changes; cosine importance sampling supplies
	// a normalized average, leaving material AO responsible for visible energy.
	void update(RID p_source, RID p_destination, float p_border_size, bool p_realtime);
	// Release the reusable sampler, pipeline and shader version before RD exits.
	~SkyConeFilterRD();
};

} // namespace RendererRD

#endif
