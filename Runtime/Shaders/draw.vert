#version 450
// Vulkan copy of the draw vertex shader in Runtime/Source/Renderer/ShaderSources.h. After changing it, run
// compile_vulkan_shaders.bat to bring Runtime/Source/Renderer/VulkanShaders.h up to date.
layout(set = 0, binding = 0, std140) uniform DrawConstants {
	ivec4 colorOp;
	ivec4 colorArg1;
	ivec4 colorArg2;
	ivec4 alphaOp;
	ivec4 alphaArg1;
	ivec4 alphaArg2;
	ivec4 texCoordIndex;
	ivec4 misc;
	vec4 textureFactor;
	vec4 view;
	vec4 depthParams;
	vec4 bumpMatrix;
	vec4 bumpLuminance;
};
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv0;
layout(location = 3) in vec2 inUv1;
layout(location = 4) in vec2 inUv2;
layout(location = 0) out vec4 vertexColor;
layout(location = 1) out vec2 vertexUv0;
layout(location = 2) out vec2 vertexUv1;
layout(location = 3) out vec2 vertexUv2;
void main() {
	float w = inPosition.w > 0.0 ? 1.0 / inPosition.w : 1.0;
	float x = (inPosition.x + 0.5) * view.x - 1.0;
	float y = (inPosition.y + 0.5) * view.y + view.z;
	float z = inPosition.z * depthParams.x + depthParams.y;
	gl_Position = vec4(x * w, y * w, z * w, w);
	vertexColor = inColor.zyxw;
	vertexUv0 = inUv0;
	vertexUv1 = inUv1;
	vertexUv2 = inUv2;
}
