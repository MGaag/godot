/**************************************************************************/
/*  instance_visibility.h                                              */
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
#include "core/math/plane.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/effects/instance_visibility.glsl.gen.h"

namespace RendererRD {
class InstanceVisibilityBatch;
// Produces GPU-owned indirect instance streams for one conservative view.
class InstanceVisibility {
	InstanceVisibilityShaderRD shader;
	RID version;
	RID pipeline;
	RID sampler;
	uint64_t allocated_id_bytes = 0;
	InstanceVisibilityBatch *batcher = nullptr;

public:
	// Placement and draw commands remain caller-owned; BindingCache owns compact IDs.
	// Positions encode tile-relative X/Z and height.
	struct Sources {
		RID positions;
		RID instances;
		RID command;
		RID visible_instances;
		RID visible_command;
		Vector4 origin_tile_height;
		float radius = 0.0f;
		float center_height = 0.0f;
		uint32_t max_instances = 0;
		uint32_t source_width = 0;
		uint32_t stride_words = 0;
		uint32_t cell_index_word = 0;
		bool second_source_bank = false;
	};
	// Creates the render-thread compute pipeline and nearest position sampler.
	InstanceVisibility();
	// Releases the compute pipeline after all renderer use has ended.
	~InstanceVisibility();
	// Geometry-owned ID storage and non-owning descriptor cache are rebuilt
	// when source identity changes; release storage with the geometry instance.
	struct BindingCache {
		RID positions;
		RID instances;
		RID command;
		RID visible_instances;
		RID visible_command;
		RID set;
		RID draw_set;
		RID compact_ids;
		RID parameters;
		uint32_t capacity = 0;
	};
	// Releases storage with its geometry owner.
	void release(BindingCache &r_cache);
	struct PushConstants {
		float planes[64] = {};
		float origin_tile_height[4] = {};
		float radius = 0.0f;
		uint32_t max_instances = 0;
		uint32_t source_width = 0;
		uint32_t layout_info = 0;
	};
	struct Job {
		Sources sources;
		RID set;
		RID dispatch_command;
		PushConstants push;
	};
	// Prepares one ID stream using normalized outward-facing world planes.
	bool prepare(const Sources &p_sources, const Vector<Plane> &p_planes, BindingCache &r_cache, Job &r_job);
	// Dispatches independent prepared streams together before their draw pass.
	bool dispatch(const LocalVector<Job> &p_jobs, uint32_t p_view = 0);
};
} //namespace RendererRD
