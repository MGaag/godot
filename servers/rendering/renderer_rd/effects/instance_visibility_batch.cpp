/**************************************************************************/
/*  instance_visibility_batch.cpp                                              */
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

#include "instance_visibility_batch.h"

#include "core/os/os.h"
#include "core/templates/sort_array.h"

using namespace RendererRD;

InstanceVisibilityBatch::InstanceVisibilityBatch() {
	RD *rd = RD::get_singleton();
	uint32_t storage_limit = MIN(rd->limit_get(RD::LIMIT_MAX_STORAGE_BUFFERS_PER_SHADER_STAGE), rd->limit_get(RD::LIMIT_MAX_STORAGE_BUFFERS_PER_UNIFORM_SET));
	width = MIN(16u, storage_limit > 2 ? (storage_limit - 2) / 4 : 0);
	width = MIN(width, uint32_t(rd->limit_get(RD::LIMIT_MAX_SAMPLERS_PER_SHADER_STAGE)));
	width = MIN(width, uint32_t(rd->limit_get(RD::LIMIT_MAX_TEXTURES_PER_SHADER_STAGE)));
	width = MIN(width, uint32_t(rd->limit_get(RD::LIMIT_MAX_SAMPLERS_PER_UNIFORM_SET)));
	width = MIN(width, uint32_t(rd->limit_get(RD::LIMIT_MAX_TEXTURES_PER_UNIFORM_SET)));
	if (width < 3) {
		return;
	}
	String switches = "#define JOB_SWITCH(ACTION) ";
	for (uint32_t i = 0; i < width; i++) {
		switches += vformat("ACTION(%d) ", i);
	}
	switches += "\n";
	Vector<String> modes;
	modes.push_back(vformat("#define BATCH_SIZE %d\n#define MODE_PREPARE\n", width) + switches);
	modes.push_back(vformat("#define BATCH_SIZE %d\n", width) + switches);
	shader.initialize(modes);
	version = shader.version_create();
	prepare_pipeline = rd->compute_pipeline_create(shader.version_get_shader(version, 0));
	cull_pipeline = rd->compute_pipeline_create(shader.version_get_shader(version, 1));
	RD::SamplerState state;
	sampler = rd->sampler_create(state);
	if (OS::get_singleton()->has_environment("TOWN_GPU_CULLING_REPORT")) {
		print_line(vformat("GPU placement batch width: %d", width));
	}
}

void InstanceVisibilityBatch::_release(Chunk &p_chunk) {
	RD *rd = RD::get_singleton();
	for (RID set : { p_chunk.prepare_set, p_chunk.cull_set }) {
		if (rd->uniform_set_is_valid(set)) {
			rd->free_rid(set);
		}
	}
	if (p_chunk.parameters.is_valid()) {
		rd->free_rid(p_chunk.parameters);
		allocated_bytes -= width * sizeof(InstanceVisibility::PushConstants);
	}
	if (p_chunk.control.is_valid()) {
		rd->free_rid(p_chunk.control);
		allocated_bytes -= (3 * width + 4) * sizeof(uint32_t);
	}
	if (OS::get_singleton()->has_environment("TOWN_GPU_CULLING_REPORT")) {
		print_line(vformat("GPU placement batch metadata bytes: %d", allocated_bytes));
	}
	p_chunk = Chunk();
}

InstanceVisibilityBatch::~InstanceVisibilityBatch() {
	for (View &view : views) {
		for (KeyValue<RID, LocalVector<Chunk>> &page : view.pages) {
			for (Chunk &chunk : page.value) {
				_release(chunk);
			}
		}
	}
	RD *rd = RD::get_singleton();
	for (RID resource : { prepare_pipeline, cull_pipeline, sampler }) {
		if (resource.is_valid()) {
			rd->free_rid(resource);
		}
	}
	if (version.is_valid()) {
		shader.version_free(version);
	}
}

bool InstanceVisibilityBatch::_prepare_chunk(Chunk &r_chunk, const InstanceVisibility::Job *const *p_jobs, uint32_t p_count) {
	RD *rd = RD::get_singleton();
	if (!r_chunk.parameters.is_valid()) {
		RID parameters = rd->storage_buffer_create(width * sizeof(InstanceVisibility::PushConstants));
		RID control = rd->storage_buffer_create((3 * width + 4) * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		if (!parameters.is_valid() || !control.is_valid()) {
			if (parameters.is_valid()) {
				rd->free_rid(parameters);
			}
			if (control.is_valid()) {
				rd->free_rid(control);
			}
			return false;
		}
		r_chunk.parameters = parameters;
		r_chunk.control = control;
		allocated_bytes += width * sizeof(InstanceVisibility::PushConstants) + (3 * width + 4) * sizeof(uint32_t);
		if (OS::get_singleton()->has_environment("TOWN_GPU_CULLING_REPORT")) {
			print_line(vformat("GPU placement batch metadata bytes: %d", allocated_bytes));
		}
	}
	Vector<uint8_t> parameters;
	parameters.resize_initialized(width * sizeof(InstanceVisibility::PushConstants));
	for (uint32_t i = 0; i < p_count; i++) {
		memcpy(parameters.ptrw() + i * sizeof(InstanceVisibility::PushConstants), &p_jobs[i]->push, sizeof(InstanceVisibility::PushConstants));
	}
	if (parameters != r_chunk.parameter_bytes) {
		ERR_FAIL_COND_V(rd->buffer_update(r_chunk.parameters, 0, parameters.size(), parameters.ptr()) != OK, false);
		r_chunk.parameter_bytes = parameters;
	}
	Vector<RID> resources;
	resources.resize(width * 5);
	for (uint32_t i = 0; i < width; i++) {
		const InstanceVisibility::Sources &source = p_jobs[MIN(i, p_count - 1)]->sources;
		resources.write[i * 5] = source.positions;
		resources.write[i * 5 + 1] = source.instances;
		resources.write[i * 5 + 2] = source.command;
		resources.write[i * 5 + 3] = source.visible_instances;
		resources.write[i * 5 + 4] = source.visible_command;
	}
	if (resources != r_chunk.resources || !rd->uniform_set_is_valid(r_chunk.prepare_set) || !rd->uniform_set_is_valid(r_chunk.cull_set)) {
		for (RID set : { r_chunk.prepare_set, r_chunk.cull_set }) {
			if (rd->uniform_set_is_valid(set)) {
				rd->free_rid(set);
			}
		}
		Vector<RD::Uniform> uniforms;
		uniforms.resize(4 * width + 3);
		for (uint32_t binding = 0; binding < 4 * width + 3; binding++) {
			uniforms.write[binding].binding = binding;
			uniforms.write[binding].uniform_type = binding == 0 ? RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE : RD::UNIFORM_TYPE_STORAGE_BUFFER;
		}
		for (uint32_t i = 0; i < width; i++) {
			uniforms.write[0].append_id(sampler);
			uniforms.write[0].append_id(resources[i * 5]);
			for (uint32_t kind = 0; kind < 4; kind++) {
				uniforms.write[1 + kind * width + i].append_id(resources[i * 5 + kind + 1]);
			}
		}
		uniforms.write[1 + 4 * width].append_id(r_chunk.parameters);
		uniforms.write[2 + 4 * width].append_id(r_chunk.control);
		Vector<RD::Uniform> prepare_uniforms;
		for (uint32_t i = 0; i < width; i++) {
			prepare_uniforms.push_back(uniforms[1 + width + i]);
			prepare_uniforms.push_back(uniforms[1 + 3 * width + i]);
		}
		prepare_uniforms.push_back(uniforms[1 + 4 * width]);
		prepare_uniforms.push_back(uniforms[2 + 4 * width]);
		r_chunk.prepare_set = rd->uniform_set_create(prepare_uniforms, shader.version_get_shader(version, 0), 0);
		r_chunk.cull_set = rd->uniform_set_create(uniforms, shader.version_get_shader(version, 1), 0);
		ERR_FAIL_COND_V(!r_chunk.prepare_set.is_valid() || !r_chunk.cull_set.is_valid(), false);
		r_chunk.resources = resources;
	}
	return true;
}

namespace {
struct VisibilityJobOrder {
	bool operator()(const InstanceVisibility::Job *a, const InstanceVisibility::Job *b) const {
		if (a->sources.positions != b->sources.positions) {
			return a->sources.positions < b->sources.positions;
		}
		return a->sources.instances < b->sources.instances;
	}
};
} // namespace

bool InstanceVisibilityBatch::dispatch(const LocalVector<InstanceVisibility::Job> &p_jobs, uint32_t p_view) {
	if (p_jobs.is_empty()) {
		return true;
	}
	RD *rd = RD::get_singleton();
	if (views.size() <= p_view) {
		views.resize(p_view + 1);
	}
	View &view = views[p_view];
	// Resource identity, rather than camera order, owns cache locality. A camera
	// change rebuilds only affected page chunks; retired pages release storage.
	LocalVector<RID> retired;
	for (KeyValue<RID, LocalVector<Chunk>> &page : view.pages) {
		if (!rd->texture_is_valid(page.key)) {
			for (Chunk &chunk : page.value) {
				_release(chunk);
			}
			retired.push_back(page.key);
		}
	}
	for (RID page : retired) {
		view.pages.erase(page);
	}
	LocalVector<const InstanceVisibility::Job *> ordered;
	ordered.reserve(p_jobs.size());
	for (const InstanceVisibility::Job &job : p_jobs) {
		ordered.push_back(&job);
	}
	SortArray<const InstanceVisibility::Job *, VisibilityJobOrder>().sort(ordered.ptr(), ordered.size());
	struct ActiveChunk {
		Chunk *chunk;
		uint32_t count;
	};
	LocalVector<ActiveChunk> active;
	uint32_t first = 0;
	while (first < ordered.size()) {
		const RID page = ordered[first]->sources.positions;
		uint32_t end = first + 1;
		while (end < ordered.size() && ordered[end]->sources.positions == page) {
			end++;
		}
		LocalVector<Chunk> &chunks = view.pages[page];
		const uint32_t needed = (end - first + width - 1) / width;
		if (chunks.size() < needed) {
			chunks.resize(needed);
		}
		for (uint32_t i = 0; i < needed; i++) {
			uint32_t count = MIN(width, end - first);
			ERR_FAIL_COND_V(!_prepare_chunk(chunks[i], ordered.ptr() + first, count), false);
			active.push_back({ &chunks[i], count });
			first += count;
		}
	}
	// All prefix passes precede culling. The graph orders storage writes against
	// both shader reads and indirect-command reads of the generated arguments.
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, prepare_pipeline);
	for (const ActiveChunk &chunk : active) {
		rd->compute_list_bind_uniform_set(list, chunk.chunk->prepare_set, 0);
		rd->compute_list_set_push_constant(list, &chunk.count, sizeof(chunk.count));
		rd->compute_list_dispatch(list, 1, 1, 1);
	}
	rd->compute_list_end();
	list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, cull_pipeline);
	for (const ActiveChunk &chunk : active) {
		rd->compute_list_bind_uniform_set(list, chunk.chunk->cull_set, 0);
		rd->compute_list_set_push_constant(list, &chunk.count, sizeof(chunk.count));
		rd->compute_list_dispatch_indirect(list, chunk.chunk->control, 0);
	}
	rd->compute_list_end();
	return true;
}
