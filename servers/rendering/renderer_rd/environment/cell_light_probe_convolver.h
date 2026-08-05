/**************************************************************************/
/*  cell_light_probe_convolver.h                                         */
/**************************************************************************/

#ifndef CELL_LIGHT_PROBE_CONVOLVER_H
#define CELL_LIGHT_PROBE_CONVOLVER_H

#include "servers/rendering/renderer_rd/shaders/environment/cell_light_probe_convolve.glsl.gen.h"

namespace RendererRD {

// GPU reduction stage for cell-probe lighting. RendererSceneRenderRD supplies a
// completed transient cubemap; CellLightProbeGridRD supplies persistent layer
// storage. The shader implementation is cell_light_probe_convolve.glsl.

// Reduces a transient lit cubemap to nine RGB spherical-harmonic coefficients
// without copying image data back to the CPU. One small partial buffer is
// shared by captures because the scheduler serializes convolution jobs.
class CellLightProbeConvolver {
	enum Mode {
		MODE_PROJECT_FACES,
		MODE_REDUCE_FACES,
		MODE_MAX,
	};

	struct PushConstant {
		uint32_t source_size;
		uint32_t destination_offset;
		uint32_t subtraction_offset;
		// Stored as uint rather than bool to match std430 push-constant layout.
		uint32_t subtract_existing;
	};

	CellLightProbeConvolveShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID partial_buffer;

public:
	// The destination is a storage buffer of vec4 values. Nine consecutive
	// entries beginning at p_destination_offset receive one probe's SH9 result.
	// When p_subtract_existing is true, the referenced SH record is subtracted
	// after projection. Toggleable circuits use this to store only their signed
	// contribution relative to the permanent base capture.
	void process(RID p_source_cubemap, uint32_t p_source_size, RID p_destination_buffer, uint32_t p_destination_offset, RID p_subtraction_buffer, bool p_subtract_existing = false, uint32_t p_subtraction_offset = 0);

	CellLightProbeConvolver();
	~CellLightProbeConvolver();
};

} // namespace RendererRD

#endif // CELL_LIGHT_PROBE_CONVOLVER_H
