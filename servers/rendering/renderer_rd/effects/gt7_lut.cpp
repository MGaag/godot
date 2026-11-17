/**************************************************************************/
/*  gt7_lut.cpp                                                           */
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

#include "gt7_lut.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

// Bake the expensive color appearance transform once, outside the frame's draw lists.
GT7LUT::GT7LUT() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	version = shader.version_create();
	RID shader_rid = shader.version_get_shader(version, 0);
	ERR_FAIL_COND(shader_rid.is_null());
	pipeline = RD::get_singleton()->compute_pipeline_create(shader_rid);

	// The log-shaped 65-cubed lattice preserves dark precision and HDR headroom.
	RD::TextureFormat format;
	format.texture_type = RD::TEXTURE_TYPE_3D;
	format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	format.width = 65;
	format.height = 65;
	format.depth = 65;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	texture = RD::get_singleton()->texture_create(format, RD::TextureView());
	ERR_FAIL_COND(texture.is_null());

	RD::Uniform output;
	output.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	output.binding = 0;
	output.append_id(texture);
	RID uniforms = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, output);
	RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(list, uniforms, 0);
	RD::get_singleton()->compute_list_dispatch(list, 17, 17, 17);
	RD::get_singleton()->compute_list_end();
}

// RenderingDevice invalidates dependent descriptors when their texture is freed.
GT7LUT::~GT7LUT() {
	if (texture.is_valid()) {
		RD::get_singleton()->free_rid(texture);
	}
	if (pipeline.is_valid()) {
		RD::get_singleton()->free_rid(pipeline);
	}
	shader.version_free(version);
}
