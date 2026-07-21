#[vertex]

#version 450

layout(location = 0) out vec2 uv;

void main() {
	vec2 vertices[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	vec2 position = vertices[gl_VertexIndex];
	uv = position * 0.5 + 0.5;
	gl_Position = vec4(position, 0.0, 1.0);
}

#[fragment]

#version 450

layout(location = 0) in vec2 uv;
layout(set = 0, binding = 0) uniform sampler2D displaced_depth;
layout(set = 0, binding = 1) uniform sampler2D source_color;
layout(set = 0, binding = 2) uniform sampler2D source_mapping;
layout(location = 0) out vec4 frag_color;

void main() {
	vec2 source_uv = clamp(uv + texture(source_mapping, uv).rg, vec2(0.0), vec2(1.0));
	frag_color = texture(source_color, source_uv);
	gl_FragDepth = texture(displaced_depth, uv).r;
}
