/**************************************************************************/
/*  ssdm.h                                                               */
/**************************************************************************/
#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/ssdm.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ssdm_depth.glsl.gen.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_SCOPE_SSDM SNAME("rb_ssdm")
#define RB_SSDM_DISPLACEMENT SNAME("displacement")
#define RB_SSDM_VECTORS SNAME("vectors")
#define RB_SSDM_MAPPING SNAME("mapping")
#define RB_SSDM_COLOR SNAME("color")
#define RB_SSDM_DEPTH SNAME("depth")
#define RB_SSDM_VELOCITY SNAME("velocity")

namespace RendererRD {

class CopyEffects;

class SSDM {
	struct PushConstant {
		int32_t size[2];
		int32_t source_mip;
		int32_t refinement_steps;
		int32_t coarsest_mip;
	};
	struct ProjectionData {
		float projection[16];
		float inv_projection[16];
	};

	enum Mode { MODE_PROJECT, MODE_DOWNSAMPLE, MODE_SOLVE, MODE_WARP_COLOR, MODE_WARP_DEPTH, MODE_WARP_VELOCITY, MODE_MAX };
	SsdmShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID projection_buffer;
	SsdmDepthShaderRD depth_shader;
	RID depth_shader_version;
	PipelineCacheRD depth_pipeline;
	int max_mipmaps = 6;
	int refinement_steps = 2;

	static void _store_projection(const Projection &p_projection, float *r_values);
	void _dispatch(Mode p_mode, RID p_source, RID p_depth, RID p_destination, const Size2i &p_size, int p_source_mip, int p_coarsest_mip, const Projection &p_projection);

public:
	SSDM();
	~SSDM();

	RID get_displacement_framebuffer(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	void process(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Projection &p_projection, CopyEffects &p_copy_effects);
	void apply_composite(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	bool is_enabled() const;
};

} // namespace RendererRD
