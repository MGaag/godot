/**************************************************************************/
/*  instance_visibility.cpp                                            */
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

#include "instance_visibility.h"

#include "main/frame_cpu_profile.h"
#include "instance_visibility_batch.h"
#include "core/os/os.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
using namespace RendererRD;

// Owns the reusable compute pipeline; placement and scratch buffers stay with
// the scene owner, and the renderer chooses the planes for each draw pass.

// Create the compute program once, lazily when the renderer first needs it.
InstanceVisibility::InstanceVisibility() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	version = shader.version_create();
	RID program = shader.version_get_shader(version, 0);
	pipeline = RD::get_singleton()->compute_pipeline_create(program);
	RD::SamplerState state;
	sampler = RD::get_singleton()->sampler_create(state);
	batcher = memnew(InstanceVisibilityBatch);
	if (!batcher->is_valid()) {
		memdelete(batcher);
		batcher = nullptr;
	}
}

// Release renderer-owned resources after the last draw using the program.
InstanceVisibility::~InstanceVisibility() {
	if (batcher != nullptr) {
		memdelete(batcher);
	}
	if (sampler.is_valid()) {
		RD::get_singleton()->free_rid(sampler);
	}
	if (pipeline.is_valid()) {
		RD::get_singleton()->free_rid(pipeline);
	}
	shader.version_free(version);
}

// Prepare one independent output before the shared compute list begins.
bool InstanceVisibility::prepare(const Sources &p_sources, const Vector<Plane> &p_planes, BindingCache &r_cache, Job &r_job) {
	Sources sources = p_sources;
	ERR_FAIL_COND_V(p_planes.size() > 16 || sources.stride_words > 255 || sources.cell_index_word >= sources.stride_words || sources.source_width == 0, false);
	ERR_FAIL_COND_V(!pipeline.is_valid() || !sampler.is_valid(), false);
	RD *device = RD::get_singleton();
	if (r_cache.capacity != sources.max_instances) {
		if (r_cache.compact_ids.is_valid()) {
			release(r_cache);
		}
		// Vec4-aligned storage also serves the existing transform-buffer binding.
		Vector<uint8_t> initial;
		initial.resize_initialized(((sources.max_instances + 5) / 4) * 16);
		r_cache.compact_ids = device->storage_buffer_create(initial.size(), initial);
		ERR_FAIL_COND_V(!r_cache.compact_ids.is_valid(), false);
		r_cache.capacity = sources.max_instances;
		allocated_id_bytes += ((sources.max_instances + 5) / 4) * 16;
		if (OS::get_singleton()->has_environment("TOWN_GPU_CULLING_REPORT")) {
			print_line(vformat("GPU placement ID allocation bytes: %d", allocated_id_bytes));
		}
	}
	sources.visible_instances = r_cache.compact_ids;
	// Convert Godot outward planes to the shader's inward half-spaces.
	PushConstants &push = r_job.push;
	static_assert(sizeof(PushConstants) == 288);
	for (int i = 0; i < p_planes.size(); i++) {
		const Plane &p = p_planes[i];
		push.planes[i * 4] = -p.normal.x;
		push.planes[i * 4 + 1] = -p.normal.y;
		push.planes[i * 4 + 2] = -p.normal.z;
		push.planes[i * 4 + 3] = p.d - p.normal.y * sources.center_height;
	}
	for (int i = 0; i < 4; i++) {
		push.origin_tile_height[i] = sources.origin_tile_height[i];
	}
	push.radius = sources.radius;
	push.max_instances = sources.max_instances;
	push.source_width = sources.source_width;
	push.layout_info = uint32_t(p_planes.size()) | (sources.stride_words << 8) | (sources.cell_index_word << 16) | (uint32_t(sources.second_source_bank) << 24);
	// Reuse the geometry-owned descriptor only while every referenced resource
	// still matches; the shared cache invalidates sets when resources are freed.
	RD *rd = RD::get_singleton();
	if (batcher == nullptr) {
		if (!r_cache.parameters.is_valid()) {
			r_cache.parameters = rd->uniform_buffer_create(sizeof(PushConstants));
			ERR_FAIL_COND_V(!r_cache.parameters.is_valid(), false);
		}
		ERR_FAIL_COND_V(rd->buffer_update(r_cache.parameters, 0, sizeof(PushConstants), &push) != OK, false);
	}
	const bool matches = r_cache.positions == sources.positions && r_cache.instances == sources.instances && r_cache.command == sources.command && r_cache.visible_instances == sources.visible_instances && r_cache.visible_command == sources.visible_command;
	if (batcher == nullptr && (!matches || !rd->uniform_set_is_valid(r_cache.set))) {
		RD::Uniform positions;
		positions.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		positions.binding = 0;
		positions.append_id(sampler);
		positions.append_id(sources.positions);
		RD::Uniform input_instances;
		input_instances.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		input_instances.binding = 1;
		input_instances.append_id(sources.instances);
		RD::Uniform input_command;
		input_command.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		input_command.binding = 2;
		input_command.append_id(sources.command);
		RD::Uniform output_instances;
		output_instances.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		output_instances.binding = 3;
		output_instances.append_id(sources.visible_instances);
		RD::Uniform output_command;
		output_command.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		output_command.binding = 4;
		output_command.append_id(sources.visible_command);
		RD::Uniform parameters;
		parameters.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		parameters.binding = 5;
		parameters.append_id(r_cache.parameters);
		RID set = UniformSetCacheRD::get_singleton()->get_cache(shader.version_get_shader(version, 0), 0, positions, input_instances, input_command, output_instances, output_command, parameters);
		ERR_FAIL_COND_V(!set.is_valid(), false);
		r_cache.positions = sources.positions;
		r_cache.instances = sources.instances;
		r_cache.command = sources.command;
		r_cache.visible_instances = sources.visible_instances;
		r_cache.visible_command = sources.visible_command;
		r_cache.set = set;
	}
	r_job.sources = sources;
	r_job.set = r_cache.set;
	r_job.dispatch_command = sources.command;
	return true;
}

// Independent buffers need no intermediate barriers; the draw graph orders
// this compute list against its reset commands and the following render pass.
bool InstanceVisibility::dispatch(const LocalVector<Job> &p_jobs, uint32_t p_view) {
	FrameCPUProfile::Scope cpu_phase("GPU culling command recording");
	if (p_jobs.is_empty()) {
		return true;
	}
	if (batcher != nullptr) {
		return batcher->dispatch(p_jobs, p_view);
	}
	RD *rd = RD::get_singleton();
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	for (const Job &job : p_jobs) {
		rd->compute_list_bind_uniform_set(list, job.set, 0);
		// Placement publication writes the exact workgroup count on the GPU.
		rd->compute_list_dispatch_indirect(list, job.dispatch_command, 5 * sizeof(uint32_t));
	}
	rd->compute_list_end();
	return true;
}

// Geometry lifetime owns the lazy ID storage; descriptor caches invalidate on free.
void InstanceVisibility::release(BindingCache &r_cache) {
	if (r_cache.parameters.is_valid()) {
		RD::get_singleton()->free_rid(r_cache.parameters);
		r_cache.parameters = RID();
	}
	if (r_cache.compact_ids.is_valid()) {
		allocated_id_bytes -= ((r_cache.capacity + 5) / 4) * 16;
		RD::get_singleton()->free_rid(r_cache.compact_ids);
		r_cache.compact_ids = RID();
		r_cache.capacity = 0;
		if (OS::get_singleton()->has_environment("TOWN_GPU_CULLING_REPORT")) {
			print_line(vformat("GPU placement ID allocation bytes: %d", allocated_id_bytes));
		}
	}
}
