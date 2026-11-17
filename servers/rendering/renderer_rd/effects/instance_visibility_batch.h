/**************************************************************************/
/*  instance_visibility_batch.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "instance_visibility.h"
#include "servers/rendering/renderer_rd/shaders/effects/instance_visibility_batch.glsl.gen.h"

namespace RendererRD {
// Groups independent placement streams by their existing GPU page identity.
// Each view retains only its latest descriptors for each page/chunk. Page
// retirement invalidates descriptors and releases their small metadata buffers.
class InstanceVisibilityBatch {
	struct Chunk {
		RID parameters;
		RID control;
		RID prepare_set;
		RID cull_set;
		Vector<RID> resources;
		Vector<uint8_t> parameter_bytes;
	};
	struct View {
		HashMap<RID, LocalVector<Chunk>> pages;
	};
	InstanceVisibilityBatchShaderRD shader;
	RID version;
	RID prepare_pipeline;
	RID cull_pipeline;
	RID sampler;
	uint32_t width = 0;
	uint64_t allocated_bytes = 0;
	LocalVector<View> views;
	void _release(Chunk &p_chunk);
	bool _prepare_chunk(Chunk &r_chunk, const InstanceVisibility::Job *const *p_jobs, uint32_t p_count);

public:
	InstanceVisibilityBatch();
	~InstanceVisibilityBatch();
	bool is_valid() const { return width >= 3 && prepare_pipeline.is_valid() && cull_pipeline.is_valid() && sampler.is_valid(); }
	// One small prefix pass builds GPU dispatch arguments; one culling dispatch
	// processes all streams in each chunk without CPU visibility/count readback.
	bool dispatch(const LocalVector<InstanceVisibility::Job> &p_jobs, uint32_t p_view);
};
} // namespace RendererRD
