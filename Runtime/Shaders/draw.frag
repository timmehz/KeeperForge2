#version 450
// Vulkan copy of the draw pixel shader in Runtime/Source/Renderer/ShaderSources.h. After changing it, run
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
layout(set = 0, binding = 1) uniform sampler2D texture0;
layout(set = 0, binding = 2) uniform sampler2D texture1;
layout(set = 0, binding = 3) uniform sampler2D texture2;
layout(location = 0) in vec4 vertexColor;
layout(location = 1) in vec2 vertexUv0;
layout(location = 2) in vec2 vertexUv1;
layout(location = 3) in vec2 vertexUv2;
layout(location = 0) out vec4 fragmentColor;

vec4 Argument(int argument, vec4 current, vec4 diffuse, vec4 texel) {
	int selector = argument & 15;
	vec4 value = vec4(0.0);
	if (selector == 0) value = diffuse;
	else if (selector == 1) value = current;
	else if (selector == 2) value = texel;
	else if (selector == 3) value = textureFactor;
	if ((argument & 16) != 0) value = vec4(1.0) - value;
	if ((argument & 32) != 0) value = vec4(value.a);
	return value;
}

vec4 Operate(int op, vec4 a, vec4 b, vec4 current, vec4 diffuse, vec4 texel) {
	if (op == 2) return a;
	if (op == 3) return b;
	if (op == 4) return a * b;
	if (op == 5) return a * b * 2.0;
	if (op == 6) return a * b * 4.0;
	if (op == 7) return a + b;
	if (op == 8) return a + b - 0.5;
	if (op == 9) return (a + b - 0.5) * 2.0;
	if (op == 10) return a - b;
	if (op == 11) return a + b - a * b;
	if (op == 12) return mix(b, a, diffuse.a);
	if (op == 13) return mix(b, a, texel.a);
	if (op == 14) return mix(b, a, textureFactor.a);
	if (op == 15) return a + b * (1.0 - texel.a);
	if (op == 16) return mix(b, a, current.a);
	if (op == 17) return a;
	if (op == 18) return vec4(a.rgb + a.a * b.rgb, a.a);
	if (op == 19) return vec4(a.rgb * b.rgb + a.a, a.a);
	if (op == 20) return vec4((1.0 - a.a) * b.rgb + a.rgb, a.a);
	if (op == 21) return vec4((1.0 - a.rgb) * b.rgb + a.a, a.a);
	if (op == 24) return vec4(clamp(dot(a.rgb - 0.5, b.rgb - 0.5) * 4.0, 0.0, 1.0));
	return current;
}

bool AlphaPasses(int func, float alpha) {
	float a = floor(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
	float r = floor(view.w * 255.0 + 0.5);
	if (func == 1) return false;
	if (func == 2) return a < r;
	if (func == 3) return a == r;
	if (func == 4) return a <= r;
	if (func == 5) return a > r;
	if (func == 6) return a != r;
	if (func == 7) return a >= r;
	return true;
}

void main() {
	vec2 uvs[3] = vec2[3](vertexUv0, vertexUv1, vertexUv2);
	vec4 diffuse = vertexColor;
	vec4 current = diffuse;
	// A bump stage moves the next stage's texture coordinates (and, with luminance, scales its texel).
	vec2 bumpOffset = vec2(0.0);
	float bumpLight = 1.0;
	for (int stage = 0; stage < 3; ++stage) {
		if (colorOp[stage] == 1) break;
		vec2 uv = uvs[clamp(texCoordIndex[stage], 0, 2)] + bumpOffset;
		vec4 texel = vec4(1.0);
		bool textured = (misc.y & (1 << stage)) != 0;
		if (textured) {
			if (stage == 0) texel = textureLod(texture0, uv, 0.0);
			else if (stage == 1) texel = textureLod(texture1, uv, 0.0);
			else texel = textureLod(texture2, uv, 0.0);
		}
		if (colorOp[stage] == 22 || colorOp[stage] == 23) {
			// The bump texel: du in red, dv in green (stored + 128), luminance in blue. Color and alpha go on unchanged.
			bumpOffset = vec2(0.0);
			bumpLight = 1.0;
			if (textured) {
				float du = texel.r * (255.0 / 128.0) - 1.0;
				float dv = texel.g * (255.0 / 128.0) - 1.0;
				bumpOffset = vec2(du * bumpMatrix.x + dv * bumpMatrix.z, du * bumpMatrix.y + dv * bumpMatrix.w);
				if (colorOp[stage] == 23) bumpLight = clamp(texel.b * bumpLuminance.x + bumpLuminance.y, 0.0, 1.0);
			}
			continue;
		}
		texel.rgb *= bumpLight;
		bumpOffset = vec2(0.0);
		bumpLight = 1.0;
		vec4 color = Operate(colorOp[stage], Argument(colorArg1[stage], current, diffuse, texel), Argument(colorArg2[stage], current, diffuse, texel), current, diffuse, texel);
		float alpha = current.a;
		if (alphaOp[stage] != 1) {
			alpha = Operate(alphaOp[stage], Argument(alphaArg1[stage], current, diffuse, texel), Argument(alphaArg2[stage], current, diffuse, texel), current, diffuse, texel).a;
		}
		current = clamp(vec4(color.rgb, alpha), 0.0, 1.0);
	}
	if (misc.x != 0 && !AlphaPasses(misc.x, current.a)) discard;
	fragmentColor = current;
}
