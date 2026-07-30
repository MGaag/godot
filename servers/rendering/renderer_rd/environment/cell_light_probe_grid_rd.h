/**************************************************************************/
/*  cell_light_probe_grid_rd.h                                           */
/**************************************************************************/

#ifndef CELL_LIGHT_PROBE_GRID_RD_H
#define CELL_LIGHT_PROBE_GRID_RD_H

#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/environment/cell_light_probe.h"
#include "servers/rendering/renderer_rd/environment/cell_light_probe_convolver.h"
#include "servers/rendering/renderer_rd/shaders/environment/cell_light_probe_resolve.glsl.gen.h"

namespace RendererRD {

// Owns the GPU-resident radiance field for one logical probe lattice. Capture
// layers and the two resolved generations are separate so incomplete work can
// never become visible to spatial materials.
class CellLightProbeGridRD {
	struct GridMetadata {
		float origin[3];
		uint32_t enabled;
		float inverse_cell_size[3];
		uint32_t pad0;
		uint32_t dimensions[3];
		uint32_t probe_count;
	};

	struct PushConstant {
		uint32_t probe_count;
		uint32_t layer_count;
		uint32_t pad[2];
	};

	CellLightProbeResolveShaderRD resolve_shader;
	RID resolve_shader_version;
	RID resolve_pipeline;

	RID layer_buffer;
	RID base_buffer;
	RID layer_weight_buffer;
	RID resolved_buffers[2];
	RID metadata_buffer;
	RID cell_index_buffer;
	RID edge_transmissibility_buffer;
	uint32_t active_resolved_buffer = 0;
	uint32_t probe_count = 0;
	uint32_t layer_count = 0;
	uint32_t lattice_cell_count = 0;
	uint64_t capture_generation = 0;
	uint64_t resolve_generation = 0;
	Vector<float> current_layer_weights;

	void _free_buffers();

public:
	void configure(uint32_t p_probe_count, uint32_t p_layer_count, const Vector3i &p_dimensions, const Vector3 &p_origin, const Vector3 &p_cell_size, bool p_enabled = true);
	bool is_configured() const;
	void set_cell_indices(const PackedInt32Array &p_cell_indices);
	void set_edge_transmissibility(const PackedFloat32Array &p_edge_transmissibility);

	// Capture output is written to its private layer. Calling this method does
	// not alter the resolved generation currently used by materials.
	void convolve_capture(CellLightProbeConvolver *p_convolver, RID p_cubemap, uint32_t p_cubemap_size, uint32_t p_probe, CellLightLayerID p_layer);

	// Layer zero is always the base layer. Remaining weights normally contain
	// zero for disabled circuits and circuit energy for enabled circuits.
	void resolve(const Vector<float> &p_layer_weights);

	RID get_active_buffer() const;
	RID get_metadata_buffer() const;
	RID get_cell_index_buffer() const;
	RID get_edge_transmissibility_buffer() const;
	uint32_t get_probe_count() const;
	uint32_t get_layer_count() const;
	uint64_t get_capture_generation() const;
	uint64_t get_resolve_generation() const;
	PackedColorArray debug_get_resolved_sh() const;

	CellLightProbeGridRD();
	~CellLightProbeGridRD();
};

} // namespace RendererRD

#endif // CELL_LIGHT_PROBE_GRID_RD_H
