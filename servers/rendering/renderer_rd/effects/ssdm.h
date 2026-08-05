/**************************************************************************/
/*  ssdm.h                                                               */
/**************************************************************************/
#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/ssdm.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ssdm_depth.glsl.gen.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

// Screen-space displacement mapping (SSDM) bends the already rendered Mobile
// frame from per-material displacement vectors. The effect builds an inverse
// screen mapping from coarse to fine, warps color/depth/velocity together, and
// then composites the displaced result. RenderForwardMobile owns orchestration;
// the matching compute stages live in shaders/effects/ssdm.glsl.

// These render-buffer keys form the private hand-off between SSDM stages. Keep
// them centralized so allocation, dispatch, and composition use identical names.
#define RB_SCOPE_SSDM SNAME("rb_ssdm")
#define RB_SSDM_DISPLACEMENT SNAME("displacement")
#define RB_SSDM_VECTORS SNAME("vectors")
#define RB_SSDM_MAPPING SNAME("mapping")
#define RB_SSDM_COLOR SNAME("color")
#define RB_SSDM_DEPTH SNAME("depth")
#define RB_SSDM_VELOCITY SNAME("velocity")

namespace RendererRD {

class CopyEffects;

// Owns SSDM pipelines and the transient textures attached to one render buffer.
// It contains no material policy: materials emit vectors, and this class only
// transforms the completed frame according to those vectors.
class SSDM {
	// Every compute variant uses the same compact dispatch contract so pipelines
	// can share descriptor layouts and differ only by MODE_* specialization.
	struct PushConstant {
		int32_t size[2];
		int32_t source_mip;
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
	// Four levels give the coarse solver a 16-pixel reach while keeping the
	// refinement chain small enough for the Mobile renderer's intended budget.
	int max_mipmaps = 4;

	// Converts Godot's projection type to the column-major float layout consumed
	// by the shared shader uniform buffer.
	static void _store_projection(const Projection &p_projection, float *r_values);
	// Runs one compute stage over one mip and binds the resources required by the
	// selected mode. process() owns the ordering between these atomic stages.
	void _dispatch(Mode p_mode, RID p_source, RID p_depth, RID p_destination, const Size2i &p_size, int p_source_mip, int p_coarsest_mip, const Projection &p_projection);

public:
	SSDM();
	~SSDM();

	// Returns the framebuffer used by the final depth-aware composite pass.
	RID get_displacement_framebuffer(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	// Builds the inverse mapping pyramid and warps all frame attributes that must
	// remain spatially aligned after displacement.
	void process(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Projection &p_projection, CopyEffects &p_copy_effects);
	// Writes displaced color and depth back into the active scene framebuffer.
	void apply_composite(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	// Reports the project setting without allocating any render resources.
	bool is_enabled() const;
};

} // namespace RendererRD
