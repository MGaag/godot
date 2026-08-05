/**************************************************************************/
/*  ssdm.cpp                                                             */
/**************************************************************************/
#include "ssdm.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

// Implements the ordered SSDM frame transform declared in ssdm.h. Resource
// creation stays in process(), atomic compute work stays in _dispatch(), and the
// final raster composite stays in apply_composite() so Mobile pass orchestration
// can treat the effect like other RendererRD post-processes.

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
	modes.push_back("\n#define MODE_WARP_COLOR\n");
	modes.push_back("\n#define MODE_WARP_DEPTH\n");
	modes.push_back("\n#define MODE_WARP_VELOCITY\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	projection_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(ProjectionData));
	depth_shader.initialize({ "" });
	depth_shader_version = depth_shader.version_create();
	RD::PipelineDepthStencilState depth_state;
	depth_state.enable_depth_test = true;
	depth_state.depth_compare_operator = RD::COMPARE_OP_ALWAYS;
	depth_state.enable_depth_write = true;
	depth_pipeline.setup(depth_shader.version_get_shader(depth_shader_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), depth_state, RD::PipelineColorBlendState::create_disabled(), 0);

	GLOBAL_DEF("rendering/renderer/ssdm/enabled", false);
	// The original SSDM paper recommends a short three-to-four-level pyramid.
	// Going substantially coarser can average silhouette displacement into the
	// zero-displacement background and lose the detail needed by the inverse map.
	GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/renderer/ssdm/max_mipmaps", PROPERTY_HINT_RANGE, "2,8,1"), 4);
	ProjectSettings::get_singleton()->set_as_basic("rendering/renderer/ssdm/enabled", true);
	ProjectSettings::get_singleton()->set_as_basic("rendering/renderer/ssdm/max_mipmaps", true);
}

SSDM::~SSDM() {
	depth_pipeline.clear();
	for (int i = 0; i < MODE_MAX; i++) {
		if (RD::get_singleton()->compute_pipeline_is_valid(pipelines[i])) {
			RD::get_singleton()->free_rid(pipelines[i]);
		}
	}
	if (projection_buffer.is_valid()) {
		RD::get_singleton()->free_rid(projection_buffer);
	}
	depth_shader.version_free(depth_shader_version);
	shader.version_free(shader_version);
}

bool SSDM::is_enabled() const {
	return GLOBAL_GET("rendering/renderer/ssdm/enabled");
}

RID SSDM::get_displacement_framebuffer(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), RID());
	const Size2i size = p_render_buffers->get_internal_size();
	max_mipmaps = GLOBAL_GET("rendering/renderer/ssdm/max_mipmaps");
	const uint32_t mipmaps = MIN(uint32_t(max_mipmaps), uint32_t(Image::get_image_required_mipmaps(size.x, size.y, Image::FORMAT_RGBAF) + 1));
	if (p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT) && p_render_buffers->get_texture_format(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT).format != RD::DATA_FORMAT_R16G16_SFLOAT) {
		p_render_buffers->clear_context(RB_SCOPE_SSDM);
	}
	if (p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_VECTORS) && p_render_buffers->get_texture_format(RB_SCOPE_SSDM, RB_SSDM_VECTORS).mipmaps != mipmaps) {
		// All SSDM resources share this context. Recreate them together so the A
		// and B pyramids always have matching layouts after a setting change.
		p_render_buffers->clear_context(RB_SCOPE_SSDM);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT)) {
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT, RD::DATA_FORMAT_R16G16_SFLOAT,
				RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT,
				RD::TEXTURE_SAMPLES_1, size, p_render_buffers->get_view_count());
	}
	return FramebufferCacheRD::get_singleton()->get_cache_multiview(p_render_buffers->get_view_count(), p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT), p_render_buffers->get_depth_texture());
}

void SSDM::_dispatch(Mode p_mode, RID p_source, RID p_depth, RID p_destination, const Size2i &p_size, int p_source_mip, int p_coarsest_mip, const Projection &p_projection) {
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
	pc.coarsest_mip = p_coarsest_mip;
	RD::Uniform projection_data(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, projection_buffer);
	RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(list, pipelines[p_mode]);
	RD::get_singleton()->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 0, source, depth), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 1, destination), 1);
	RD::get_singleton()->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 2, projection_data), 2);
	RD::get_singleton()->compute_list_set_push_constant(list, &pc, sizeof(pc));
	RD::get_singleton()->compute_list_dispatch_threads(list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();
}

void SSDM::process(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Projection &p_projection, CopyEffects &p_copy_effects) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	const Size2i size = p_render_buffers->get_internal_size();
	max_mipmaps = GLOBAL_GET("rendering/renderer/ssdm/max_mipmaps");
	ProjectionData projection_data;
	_store_projection(p_projection, projection_data.projection);
	_store_projection(p_projection.inverse(), projection_data.inv_projection);
	RD::get_singleton()->buffer_update(projection_buffer, 0, sizeof(projection_data), &projection_data);
	const uint32_t mipmaps = MIN(uint32_t(max_mipmaps), uint32_t(Image::get_image_required_mipmaps(size.x, size.y, Image::FORMAT_RGBAF) + 1));
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_VECTORS)) {
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_VECTORS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1, mipmaps);
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_MAPPING, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1, mipmaps);
	}
	RID displacement = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_DISPLACEMENT);
	RID depth = p_render_buffers->get_depth_texture();
	RID vectors = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_VECTORS);
	_dispatch(MODE_PROJECT, displacement, depth, p_render_buffers->get_texture_slice(RB_SCOPE_SSDM, RB_SSDM_VECTORS, 0, 0), size, 0, mipmaps - 1, p_projection);
	for (uint32_t mip = 1; mip < mipmaps; mip++) {
		Size2i mip_size(MAX(1, size.x >> mip), MAX(1, size.y >> mip));
		RID source_mip = p_render_buffers->get_texture_slice(RB_SCOPE_SSDM, RB_SSDM_VECTORS, 0, mip - 1);
		RID destination_mip = p_render_buffers->get_texture_slice(RB_SCOPE_SSDM, RB_SSDM_VECTORS, 0, mip);
		_dispatch(MODE_DOWNSAMPLE, source_mip, depth, destination_mip, mip_size, 0, mipmaps - 1, p_projection);
	}
	RID mapping = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_MAPPING);
	for (int32_t mip = int32_t(mipmaps) - 1; mip >= 0; mip--) {
		Size2i mip_size(MAX(1, size.x >> mip), MAX(1, size.y >> mip));
		RID previous_mapping = mip == int32_t(mipmaps) - 1 ? depth : p_render_buffers->get_texture_slice(RB_SCOPE_SSDM, RB_SSDM_MAPPING, 0, mip + 1);
		RID destination_mapping = p_render_buffers->get_texture_slice(RB_SCOPE_SSDM, RB_SSDM_MAPPING, 0, mip);
		_dispatch(MODE_SOLVE, vectors, previous_mapping, destination_mapping, mip_size, mip, mipmaps - 1, p_projection);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_COLOR)) {
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_COLOR, RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT, RD::TEXTURE_SAMPLES_1, size);
	}
	RID color_copy = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_COLOR);
	RID color_copy_fb = FramebufferCacheRD::get_singleton()->get_cache(color_copy);
	p_copy_effects.copy_to_fb_rect(p_render_buffers->get_internal_texture(), color_copy_fb, Rect2i(0, 0, size.x, size.y));
	if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_DEPTH)) {
		p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size);
	}
	_dispatch(MODE_WARP_DEPTH, vectors, mapping, p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_DEPTH), size, 0, mipmaps - 1, p_projection);
	if (p_render_buffers->has_velocity_buffer(false)) {
		if (!p_render_buffers->has_texture(RB_SCOPE_SSDM, RB_SSDM_VELOCITY)) {
			p_render_buffers->create_texture(RB_SCOPE_SSDM, RB_SSDM_VELOCITY, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size);
		}
		RID velocity = p_render_buffers->get_velocity_buffer(false);
		RID warped_velocity = p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_VELOCITY);
		_dispatch(MODE_WARP_VELOCITY, velocity, mapping, warped_velocity, size, 0, mipmaps - 1, p_projection);
		RD::get_singleton()->texture_copy(warped_velocity, velocity, Vector3(), Vector3(), Vector3(size.x, size.y, 1), 0, 0, 0, 0);
	}
}

void SSDM::apply_composite(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	RID target_depth = p_render_buffers->get_depth_texture();
	Vector<RID> textures;
	textures.push_back(p_render_buffers->get_internal_texture());
	textures.push_back(target_depth);
	RD::FramebufferPass pass;
	pass.color_attachments.push_back(0);
	pass.depth_attachment = 1;
	Vector<RD::FramebufferPass> passes;
	passes.push_back(pass);
	RID framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multipass(textures, passes, p_render_buffers->get_view_count());
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = depth_shader.version_get_shader(depth_shader_version, 0);
	RD::Uniform depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_DEPTH) }));
	RD::Uniform color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear_sampler, p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_COLOR) }));
	RD::Uniform mapping(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_render_buffers->get_texture(RB_SCOPE_SSDM, RB_SSDM_MAPPING) }));
	RD::DrawListID list = RD::get_singleton()->draw_list_begin(framebuffer, RD::DRAW_DEFAULT_ALL);
	RD::get_singleton()->draw_list_bind_render_pipeline(list, depth_pipeline.get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(framebuffer)));
	RD::get_singleton()->draw_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, depth, color, mapping), 0);
	RD::get_singleton()->draw_list_draw(list, false, 1, 3);
	RD::get_singleton()->draw_list_end();
}
