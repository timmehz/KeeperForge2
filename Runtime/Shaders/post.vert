#version 450
// One triangle that covers the whole target, for the anti-aliasing passes and the MSAA copy. uv 0,0 is the top
// left: Vulkan's clip space y points down, the same way as the frame's rows.
layout(location = 0) out vec2 uv;

void main() {
	uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
