#version 450
// The core's frame, opaque: its pixel formats keep no alpha.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	o_color = vec4(texture(u_texture, v_uv).rgb, 1.0);
}
