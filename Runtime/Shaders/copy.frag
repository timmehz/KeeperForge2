#version 450
// MSAA: the CPU picture of the frame into every sample of the multisampled frame, pixel for pixel, alpha kept.
layout(set = 0, binding = 0) uniform sampler2D input0;
layout(location = 0) out vec4 color;

void main() {
	color = texelFetch(input0, ivec2(gl_FragCoord.xy), 0);
}
