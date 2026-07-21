/**************************************************************************/
/*  ssdm.cpp                                                             */
/**************************************************************************/
#include "ssdm.h"

#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

void SSDM::_store_projection(const Projection &p_projection, float *r_values) {
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			r_values[column * 4 + row] = p_projection.columns[column][row];
		}
	}
}

SSDM::SSDM() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_PROJECT\n");
	modes.push_back("\n#define MODE_DOWNSAMPLE\n");
	modes.push_back("\n#define MODE_SOLVE\n");
	modes.push_back("\n#define MODE_WARP\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

SSDM::~SSDM() {
	shader.version_free(shader_version);
}

RID SSDM::get_displacement_framebuffer(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), RID());
	const Size2i size = p_render_buffers->get_internal_size();
	if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT)) {
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT, RD::DATA_FORMAT_R16_SFLOAT,
				RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT,
				RD::TEXTURE_SAMPLES_1, size, p_render_buffers->get_view_count());
	}
	return FramebufferCacheRD::get_singleton()->get_cache_multiview(p_render_buffers->get_view_count(), p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT), p_render_buffers->get_depth_texture());
}

void SSDM::_dispatch(Mode p_mode, RID p_source, RID p_depth, RID p_destination, const Size2i &p_size, int p_source_mip, const Projection &p_projection) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = shader.version_get_shader(shader_version, p_mode);
	RD::Uniform source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_source }));
	RD::Uniform depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, p_depth }));
	RD::Uniform destination(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ p_destination }));
	PushConstant pc = {};
	pc.size[0] = p_size.x;
	pc.size[1] = p_size.y;
	pc.source_mip = p_source_mip;
	_store_projection(p_projection, pc.projection);
	_store_projection(p_projection.inverse(), pc.inv_projection);
	RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(list, pipelines[p_mode]);
	RD::get_singleton()->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 0, source, depth), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 1, destination), 1);
	RD::get_singleton()->compute_list_set_push_constant(list, &pc, sizeof(pc));
	RD::get_singleton()->compute_list_dispatch_threads(list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();
}

void SSDM::process(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Projection &p_projection) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	const Size2i size = p_render_buffers->get_internal_size();
	const uint32_t mipmaps = MIN(6u, uint32_t(Image::get_image_required_mipmaps(size.x, size.y, Image::FORMAT_RGF) + 1));
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_VECTORS)) {
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_VECTORS, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1, mipmaps);
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_MAPPING, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size);
	}
	RID displacement = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT);
	RID depth = p_render_buffers->get_depth_texture();
	RID vectors = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_VECTORS);
	_dispatch(MODE_PROJECT, displacement, depth, p_render_buffers->get_texture_slice(RB_SCOPE_SSDM, RB_SSDM_VECTORS, 0, 0), size, 0, p_projection);
	for (uint32_t mip = 1; mip < mipmaps; mip++) {
		Size2i mip_size(MAX(1, size.x >> mip), MAX(1, size.y >> mip));
		_dispatch(MODE_DOWNSAMPLE, vectors, depth, p_render_buffers->get_texture_slice(RB_SCOPE_SSDM, RB_SSDM_VECTORS, 0, mip), mip_size, mip - 1, p_projection);
	}
	RID mapping = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_MAPPING);
	_dispatch(MODE_SOLVE, vectors, depth, mapping, size, mipmaps - 1, p_projection);
	if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_COLOR)) {
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_COLOR, RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32, usage, RD::TEXTURE_SAMPLES_1, size);
	}
	RID warped_color = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_COLOR);
	_dispatch(MODE_WARP, p_render_buffers->get_internal_texture(), mapping, warped_color, size, 0, p_projection);
	RD::get_singleton()->texture_copy(warped_color, p_render_buffers->get_internal_texture(), Vector3(), Vector3(), Vector3(size.x, size.y, 1), 0, 0, 0, 0);
}
