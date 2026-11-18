#version 450
// Our own morphological anti-aliasing ("smaa"), the same as SmaaHlsl in Runtime/Source/Renderer/ShaderSources.h (see
// there for how it works). Drawn with post.vert. compile_vulkan_shaders.ps1 builds it once per pass, defining
// SMAA_PASS_EDGE (input0 = frame), SMAA_PASS_WEIGHT (input0 = edges) or SMAA_PASS_BLEND (input0 = frame,
// input1 = weights). metrics = 1/width, 1/height, width, height; params = edge threshold, search pixels, contrast factor.
layout(push_constant) uniform PostConstants {
	vec4 metrics;
	vec4 params;
} post;
layout(set = 0, binding = 0) uniform sampler2D input0;
layout(set = 0, binding = 1) uniform sampler2D input1;
layout(set = 0, binding = 2) uniform sampler2D input2;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 result;

ivec2 Last() {
	return ivec2(post.metrics.zw) - 1;
}

#if defined(SMAA_PASS_EDGE)
float Luma(ivec2 at) {
	return dot(texelFetch(input0, clamp(at, ivec2(0), Last()), 0).rgb, vec3(0.2126, 0.7152, 0.0722));
}
void main() {
	ivec2 at = ivec2(gl_FragCoord.xy);
	float center = Luma(at);
	float leftLuma = Luma(at + ivec2(-1, 0));
	float topLuma = Luma(at + ivec2(0, -1));
	float left = abs(center - leftLuma);
	float top = abs(center - topLuma);
	vec2 edges = step(vec2(post.params.x), vec2(left, top));
	if (edges.x + edges.y == 0.0) {
		result = vec4(0.0);
		return;
	}
	float right = abs(center - Luma(at + ivec2(1, 0)));
	float bottom = abs(center - Luma(at + ivec2(0, 1)));
	float leftOfLeft = abs(leftLuma - Luma(at + ivec2(-2, 0)));
	float aboveTop = abs(topLuma - Luma(at + ivec2(0, -2)));
	float strongest = max(max(max(left, top), max(right, bottom)), max(leftOfLeft, aboveTop));
	result = vec4(edges * step(vec2(strongest), post.params.z * vec2(left, top)), 0.0, 0.0);
}

#elif defined(SMAA_PASS_WEIGHT)
vec2 EdgesAt(ivec2 at) {
	if (any(lessThan(at, ivec2(0))) || any(greaterThan(at, Last()))) return vec2(0.0);
	return texelFetch(input0, at, 0).rg;
}
vec2 Coverage(float offset, float len, float startHeight, float endHeight) {
	float middle = 0.5 * len;
	vec2 cut = vec2(0.0);
	float from = offset;
	float to = min(offset + 1.0, middle);
	if (to > from) {
		float area = startHeight * ((to - from) - (to * to - from * from) / (2.0 * middle));
		cut += vec2(max(area, 0.0), max(-area, 0.0));
	}
	from = max(offset, middle);
	to = offset + 1.0;
	if (to > from) {
		float area = endHeight * (0.5 * (to * to - from * from) - middle * (to - from)) / middle;
		cut += vec2(max(area, 0.0), max(-area, 0.0));
	}
	return cut;
}
void main() {
	ivec2 at = ivec2(gl_FragCoord.xy);
	vec2 own = EdgesAt(at);
	result = vec4(0.0);
	if (own.x + own.y == 0.0) return;
	int steps = int(post.params.y);
	// The edge along the top of this pixel: + is up.
	if (own.y > 0.0) {
		int left = 0;
		float leftHeight = 0.0;
		for (int i = 0; i < steps; ++i) {
			vec2 here = EdgesAt(at + ivec2(-i, 0));
			if (here.y == 0.0) break;
			left = i;
			float above = EdgesAt(at + ivec2(-i, -1)).x;
			if (here.x + above > 0.0) {
				leftHeight = 0.5 * (above - here.x);
				break;
			}
		}
		int right = 0;
		float rightHeight = 0.0;
		for (int j = 0; j < steps; ++j) {
			right = j;
			vec2 next = EdgesAt(at + ivec2(j + 1, 0));
			float above = EdgesAt(at + ivec2(j + 1, -1)).x;
			if (next.x + above > 0.0) {
				rightHeight = 0.5 * (above - next.x);
				break;
			}
			if (next.y == 0.0) break;
		}
		vec2 cut = Coverage(float(left), float(left + right + 1), leftHeight, rightHeight);
		result.xy = cut.yx;
	}
	// The edge along the left of this pixel: + is to the left.
	if (own.x > 0.0) {
		int up = 0;
		float upHeight = 0.0;
		for (int i = 0; i < steps; ++i) {
			vec2 here = EdgesAt(at + ivec2(0, -i));
			if (here.x == 0.0) break;
			up = i;
			float leftSide = EdgesAt(at + ivec2(-1, -i)).y;
			if (here.y + leftSide > 0.0) {
				upHeight = 0.5 * (leftSide - here.y);
				break;
			}
		}
		int down = 0;
		float downHeight = 0.0;
		for (int j = 0; j < steps; ++j) {
			down = j;
			vec2 next = EdgesAt(at + ivec2(0, j + 1));
			float leftSide = EdgesAt(at + ivec2(-1, j + 1)).y;
			if (next.y + leftSide > 0.0) {
				downHeight = 0.5 * (leftSide - next.y);
				break;
			}
			if (next.x == 0.0) break;
		}
		vec2 cut = Coverage(float(up), float(up + down + 1), upHeight, downHeight);
		result.zw = cut.yx;
	}
}

#else
void main() {
	ivec2 at = ivec2(gl_FragCoord.xy);
	ivec2 last = Last();
	vec4 color = texelFetch(input0, at, 0);
	vec4 own = texelFetch(input1, at, 0);
	float fromBelow = at.y < last.y ? texelFetch(input1, at + ivec2(0, 1), 0).y : 0.0;
	float fromRight = at.x < last.x ? texelFetch(input1, at + ivec2(1, 0), 0).w : 0.0;
	float total = own.x + own.z + fromBelow + fromRight;
	if (total <= 0.0) {
		result = color;
		return;
	}
	float scale = total > 1.0 ? 1.0 / total : 1.0;
	vec3 mixed = color.rgb * (1.0 - total * scale);
	if (own.x > 0.0) mixed += own.x * scale * texelFetch(input0, at + ivec2(0, -1), 0).rgb;
	if (fromBelow > 0.0) mixed += fromBelow * scale * texelFetch(input0, at + ivec2(0, 1), 0).rgb;
	if (own.z > 0.0) mixed += own.z * scale * texelFetch(input0, at + ivec2(-1, 0), 0).rgb;
	if (fromRight > 0.0) mixed += fromRight * scale * texelFetch(input0, at + ivec2(1, 0), 0).rgb;
	result = vec4(mixed, color.a);
}
#endif
