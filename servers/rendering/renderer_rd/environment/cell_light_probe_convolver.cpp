/**************************************************************************/
/*  cell_light_probe_convolver.cpp                                       */
/**************************************************************************/

#include "cell_light_probe_convolver.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

// Converts one completed capture cubemap into persistent diffuse SH9 entirely
// on the GPU. Projection creates workgroup partials; reduction writes the grid's
// base record or a signed circuit contribution relative to that base.

namespace RendererRD {

CellLightProbeConvolver::CellLightProbeConvolver() {
	Vector<String> modes;
	modes.push_back("\n#define PROJECT_FACES\n");
	modes.push_back("\n#define REDUCE_FACES\n");
	shader.initialize(modes);
	shader_version = shader.version_create();

	for (uint32_t i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}

	// Six faces times nine coefficients, stored as RGB plus accumulated solid
	// angle. This scratch buffer is only 864 bytes and is reused for every job.
	partial_buffer = RD::get_singleton()->storage_buffer_create(6 * 9 * sizeof(float) * 4);
}

CellLightProbeConvolver::~CellLightProbeConvolver() {
	if (partial_buffer.is_valid()) {
		RD::get_singleton()->free_rid(partial_buffer);
	}

	for (uint32_t i = 0; i < MODE_MAX; i++) {
		if (pipelines[i].is_valid()) {
			RD::get_singleton()->free_rid(pipelines[i]);
		}
	}

	shader.version_free(shader_version);
}

void CellLightProbeConvolver::process(RID p_source_cubemap, uint32_t p_source_size, RID p_destination_buffer, uint32_t p_destination_offset, RID p_subtraction_buffer, bool p_subtract_existing, uint32_t p_subtraction_offset) {
	ERR_FAIL_COND(p_source_cubemap.is_null());
	ERR_FAIL_COND(p_destination_buffer.is_null());
	ERR_FAIL_COND(p_subtraction_buffer.is_null());
	ERR_FAIL_COND(p_source_size == 0);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	PushConstant push_constant = {};
	push_constant.source_size = p_source_size;
	push_constant.destination_offset = p_destination_offset;
	push_constant.subtraction_offset = p_subtraction_offset;
	push_constant.subtract_existing = p_subtract_existing;

	RID source_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID project_shader = shader.version_get_shader(shader_version, MODE_PROJECT_FACES);
	RID reduce_shader = shader.version_get_shader(shader_version, MODE_REDUCE_FACES);

	RD::Uniform source_uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ source_sampler, p_source_cubemap }));
	RD::Uniform partial_write_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, partial_buffer);
	RD::Uniform partial_read_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, partial_buffer);
	RD::Uniform destination_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_destination_buffer);
	RD::Uniform subtraction_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_subtraction_buffer);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_PROJECT_FACES]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(project_shader, 0, source_uniform), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(project_shader, 1, partial_write_uniform), 1);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, 8, 8, 6);

	// The reduction consumes all six completed face projections.
	RD::get_singleton()->compute_list_add_barrier(compute_list);
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_REDUCE_FACES]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(reduce_shader, 0, partial_read_uniform), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(reduce_shader, 1, destination_uniform, subtraction_uniform), 1);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, 8, 8, 1);
	RD::get_singleton()->compute_list_end();
}

} // namespace RendererRD
