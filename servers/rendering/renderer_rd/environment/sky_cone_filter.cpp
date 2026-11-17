#include "sky_cone_filter.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

// Create the angular levels independently of specular GGX filtering and its
// spatial mips. One array is 256 KiB and no per-level CPU pass is needed.
RID SkyConeFilterRD::create_texture() {
	RD::TextureFormat format;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	format.width = SIZE;
	format.height = SIZE;
	format.array_layers = LEVELS;
	format.mipmaps = 1;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID texture = RD::get_singleton()->texture_create(format, RD::TextureView());
	RD::get_singleton()->set_resource_name(texture, "Cosine-weighted sky visibility cones");
	return texture;
}

// Build all apertures in one dispatch. Initialization is lazy so unrelated
// projects with cone filtering disabled allocate no pipeline or sampler.
void SkyConeFilterRD::update(RID p_source, RID p_destination, float p_border_size, bool p_realtime) {
	RD *rd = RD::get_singleton();
	if (version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		shader.initialize(modes);
		version = shader.version_create();
		pipeline = rd->compute_pipeline_create(shader.version_get_shader(version, 0));
		RD::SamplerState state;
		state.min_filter = RD::SAMPLER_FILTER_LINEAR;
		state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
		state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		sampler = rd->sampler_create(state);
	}
	struct PushConstant {
		uint32_t size;
		uint32_t levels;
		uint32_t samples;
		float border;
	} push_constant = { SIZE, LEVELS, p_realtime ? 32u : 64u, p_border_size };
	RID shader_rid = shader.version_get_shader(version, 0);
	RD::Uniform source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_source }));
	RD::Uniform destination(RD::UNIFORM_TYPE_IMAGE, 1, p_destination);
	RID uniforms = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, source, destination);
	RENDER_TIMESTAMP("Sky cosine cone begin");
	rd->draw_command_begin_label("Cosine-weighted sky cones");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, uniforms, 0);
	rd->compute_list_set_push_constant(list, &push_constant, sizeof(push_constant));
	rd->compute_list_dispatch_threads(list, SIZE, SIZE, LEVELS);
	rd->compute_list_end();
	rd->draw_command_end_label();
	RENDER_TIMESTAMP("Sky cosine cone end");
}

// Shader versions own their pipelines; the sampler has an independent lifetime.
SkyConeFilterRD::~SkyConeFilterRD() {
	if (version.is_valid()) {
		shader.version_free(version);
	}
	if (sampler.is_valid()) {
		RD::get_singleton()->free_rid(sampler);
	}
}
