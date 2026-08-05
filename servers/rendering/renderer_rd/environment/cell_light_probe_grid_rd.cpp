/**************************************************************************/
/*  cell_light_probe_grid_rd.cpp                                         */
/**************************************************************************/

#include "cell_light_probe_grid_rd.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

// Owns the GPU buffers sampled by Forward Mobile. Captures update private layer
// records; resolve() composes into an inactive grid buffer and swaps only after
// dispatch, preserving one coherent lighting generation on screen.

namespace RendererRD {

CellLightProbeGridRD::CellLightProbeGridRD() {
	Vector<String> modes;
	modes.push_back("\n");
	resolve_shader.initialize(modes);
	resolve_shader_version = resolve_shader.version_create();
	resolve_pipeline = RD::get_singleton()->compute_pipeline_create(resolve_shader.version_get_shader(resolve_shader_version, 0));
}

CellLightProbeGridRD::~CellLightProbeGridRD() {
	_free_buffers();

	if (resolve_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(resolve_pipeline);
	}
	resolve_shader.version_free(resolve_shader_version);
}

void CellLightProbeGridRD::_free_buffers() {
	if (layer_buffer.is_valid()) {
		RD::get_singleton()->free_rid(layer_buffer);
		layer_buffer = RID();
	}
	if (base_buffer.is_valid()) {
		RD::get_singleton()->free_rid(base_buffer);
		base_buffer = RID();
	}
	if (layer_weight_buffer.is_valid()) {
		RD::get_singleton()->free_rid(layer_weight_buffer);
		layer_weight_buffer = RID();
	}
	for (RID &resolved_buffer : resolved_buffers) {
		if (resolved_buffer.is_valid()) {
			RD::get_singleton()->free_rid(resolved_buffer);
			resolved_buffer = RID();
		}
	}
	if (metadata_buffer.is_valid()) {
		RD::get_singleton()->free_rid(metadata_buffer);
		metadata_buffer = RID();
	}
	if (cell_index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(cell_index_buffer);
		cell_index_buffer = RID();
	}
	if (edge_transmissibility_buffer.is_valid()) {
		RD::get_singleton()->free_rid(edge_transmissibility_buffer);
		edge_transmissibility_buffer = RID();
	}

	probe_count = 0;
	layer_count = 0;
	lattice_cell_count = 0;
	active_resolved_buffer = 0;
	current_layer_weights.clear();
}

void CellLightProbeGridRD::configure(uint32_t p_probe_count, uint32_t p_layer_count, const Vector3i &p_dimensions, const Vector3 &p_origin, const Vector3 &p_cell_size, bool p_enabled) {
	ERR_FAIL_COND(p_probe_count == 0);
	ERR_FAIL_COND(p_layer_count == 0);
	ERR_FAIL_COND(p_dimensions.x <= 0 || p_dimensions.y <= 0 || p_dimensions.z <= 0);
	ERR_FAIL_COND(p_cell_size.x <= 0.0f || p_cell_size.y <= 0.0f || p_cell_size.z <= 0.0f);

	_free_buffers();
	probe_count = p_probe_count;
	layer_count = p_layer_count;

	const uint32_t coefficient_stride = CellLightSH9::COEFFICIENT_COUNT * sizeof(float) * 4;
	layer_buffer = RD::get_singleton()->storage_buffer_create(probe_count * layer_count * coefficient_stride);
	base_buffer = RD::get_singleton()->storage_buffer_create(probe_count * coefficient_stride);
	layer_weight_buffer = RD::get_singleton()->storage_buffer_create(layer_count * sizeof(float));
	resolved_buffers[0] = RD::get_singleton()->storage_buffer_create(probe_count * coefficient_stride);
	resolved_buffers[1] = RD::get_singleton()->storage_buffer_create(probe_count * coefficient_stride);
	RD::get_singleton()->buffer_clear(layer_buffer, 0, probe_count * layer_count * coefficient_stride);
	RD::get_singleton()->buffer_clear(base_buffer, 0, probe_count * coefficient_stride);
	RD::get_singleton()->buffer_clear(resolved_buffers[0], 0, probe_count * coefficient_stride);
	RD::get_singleton()->buffer_clear(resolved_buffers[1], 0, probe_count * coefficient_stride);

	GridMetadata metadata = {};
	metadata.origin[0] = p_origin.x;
	metadata.origin[1] = p_origin.y;
	metadata.origin[2] = p_origin.z;
	metadata.enabled = p_enabled;
	metadata.inverse_cell_size[0] = 1.0f / p_cell_size.x;
	metadata.inverse_cell_size[1] = 1.0f / p_cell_size.y;
	metadata.inverse_cell_size[2] = 1.0f / p_cell_size.z;
	metadata.dimensions[0] = p_dimensions.x;
	metadata.dimensions[1] = p_dimensions.y;
	metadata.dimensions[2] = p_dimensions.z;
	metadata.probe_count = probe_count;
	Vector<uint8_t> metadata_bytes;
	metadata_bytes.resize(sizeof(GridMetadata));
	memcpy(metadata_bytes.ptrw(), &metadata, sizeof(GridMetadata));
	metadata_buffer = RD::get_singleton()->storage_buffer_create(sizeof(GridMetadata), metadata_bytes);

	lattice_cell_count = p_dimensions.x * p_dimensions.y * p_dimensions.z;
	PackedInt32Array initial_indices;
	initial_indices.resize(lattice_cell_count);
	for (uint32_t i = 0; i < lattice_cell_count; i++) {
		initial_indices.write[i] = i < probe_count ? i : -1;
	}
	cell_index_buffer = RD::get_singleton()->storage_buffer_create(lattice_cell_count * sizeof(int32_t), initial_indices.to_byte_array());

	PackedFloat32Array initial_edges;
	initial_edges.resize(lattice_cell_count * 4);
	initial_edges.fill(1.0f);
	edge_transmissibility_buffer = RD::get_singleton()->storage_buffer_create(lattice_cell_count * sizeof(float) * 4, initial_edges.to_byte_array());

	Vector<float> initial_weights;
	initial_weights.resize(layer_count);
	initial_weights.write[0] = 1.0f;
	resolve(initial_weights);
}

void CellLightProbeGridRD::set_cell_indices(const PackedInt32Array &p_cell_indices) {
	ERR_FAIL_COND(!is_configured());
	ERR_FAIL_COND((uint32_t)p_cell_indices.size() != lattice_cell_count);
	RD::get_singleton()->buffer_update(cell_index_buffer, 0, p_cell_indices.size() * sizeof(int32_t), p_cell_indices.ptr());
}

void CellLightProbeGridRD::set_edge_transmissibility(const PackedFloat32Array &p_edge_transmissibility) {
	ERR_FAIL_COND(!is_configured());
	ERR_FAIL_COND((uint32_t)p_edge_transmissibility.size() != lattice_cell_count * 4);
	RD::get_singleton()->buffer_update(edge_transmissibility_buffer, 0, p_edge_transmissibility.size() * sizeof(float), p_edge_transmissibility.ptr());
}

bool CellLightProbeGridRD::is_configured() const {
	return layer_buffer.is_valid();
}

void CellLightProbeGridRD::convolve_capture(CellLightProbeConvolver *p_convolver, RID p_cubemap, uint32_t p_cubemap_size, uint32_t p_probe, CellLightLayerID p_layer) {
	ERR_FAIL_NULL(p_convolver);
	ERR_FAIL_COND(!is_configured());
	ERR_FAIL_UNSIGNED_INDEX(p_probe, probe_count);
	ERR_FAIL_UNSIGNED_INDEX(p_layer, layer_count);

	const uint32_t destination_offset = (p_layer * probe_count + p_probe) * CellLightSH9::COEFFICIENT_COUNT;
	const uint32_t base_offset = p_probe * CellLightSH9::COEFFICIENT_COUNT;
	const bool is_contribution_layer = p_layer != CELL_LIGHT_BASE_LAYER;
	if (is_contribution_layer) {
		p_convolver->process(p_cubemap, p_cubemap_size, layer_buffer, destination_offset, base_buffer, true, base_offset);
	} else {
		// A separate base buffer avoids binding one RD resource for both read and
		// write when later captures subtract permanent lighting on the GPU.
		p_convolver->process(p_cubemap, p_cubemap_size, base_buffer, base_offset, layer_buffer);
		const uint32_t record_size = CellLightSH9::COEFFICIENT_COUNT * sizeof(float) * 4;
		RD::get_singleton()->buffer_copy(base_buffer, layer_buffer, base_offset * sizeof(float) * 4, destination_offset * sizeof(float) * 4, record_size);
	}
	capture_generation++;
}

void CellLightProbeGridRD::resolve(const Vector<float> &p_layer_weights) {
	ERR_FAIL_COND(!is_configured());
	ERR_FAIL_COND((uint32_t)p_layer_weights.size() != layer_count);

	Vector<float> effective_weights = p_layer_weights;
	// The permanent environment is structural and cannot be accidentally
	// disabled by a missing or malformed game-side circuit state.
	effective_weights.write[0] = 1.0f;
	current_layer_weights = effective_weights;
	RD::get_singleton()->buffer_update(layer_weight_buffer, 0, layer_count * sizeof(float), effective_weights.ptr());

	PushConstant push_constant = {};
	push_constant.probe_count = probe_count;
	push_constant.layer_count = layer_count;

	const uint32_t inactive_buffer = 1 - active_resolved_buffer;
	RID shader_rid = resolve_shader.version_get_shader(resolve_shader_version, 0);
	RD::Uniform layer_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, layer_buffer);
	RD::Uniform weight_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, layer_weight_buffer);
	RD::Uniform resolved_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, resolved_buffers[inactive_buffer]);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, layer_uniform, weight_uniform), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, resolved_uniform), 1);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, probe_count * CellLightSH9::COEFFICIENT_COUNT, 1, 1);
	RD::get_singleton()->compute_list_end();

	active_resolved_buffer = inactive_buffer;
	resolve_generation++;
}

RID CellLightProbeGridRD::get_active_buffer() const {
	return resolved_buffers[active_resolved_buffer];
}

RID CellLightProbeGridRD::get_metadata_buffer() const {
	return metadata_buffer;
}

RID CellLightProbeGridRD::get_cell_index_buffer() const {
	return cell_index_buffer;
}

RID CellLightProbeGridRD::get_edge_transmissibility_buffer() const {
	return edge_transmissibility_buffer;
}

uint32_t CellLightProbeGridRD::get_probe_count() const {
	return probe_count;
}

uint32_t CellLightProbeGridRD::get_layer_count() const {
	return layer_count;
}

uint64_t CellLightProbeGridRD::get_capture_generation() const {
	return capture_generation;
}

uint64_t CellLightProbeGridRD::get_resolve_generation() const {
	return resolve_generation;
}

PackedColorArray CellLightProbeGridRD::debug_get_resolved_sh() const {
	ERR_FAIL_COND_V(!is_configured(), PackedColorArray());

	// This intentionally stalls for visualization tooling. Runtime materials use
	// the GPU buffer directly and must never call this diagnostic readback path.
	const uint32_t coefficient_count = probe_count * CellLightSH9::COEFFICIENT_COUNT;
	const uint32_t buffer_size = coefficient_count * sizeof(float) * 4;
	Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(resolved_buffers[active_resolved_buffer], 0, buffer_size);
	ERR_FAIL_COND_V(bytes.size() != (int)buffer_size, PackedColorArray());

	const float *values = reinterpret_cast<const float *>(bytes.ptr());
	PackedColorArray result;
	result.resize(coefficient_count);
	for (uint32_t coefficient = 0; coefficient < coefficient_count; coefficient++) {
		const uint32_t offset = coefficient * 4;
		result.write[coefficient] = Color(values[offset], values[offset + 1], values[offset + 2], values[offset + 3]);
	}
	return result;
}

} // namespace RendererRD
