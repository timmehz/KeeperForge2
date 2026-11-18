#version 450
// FXAA, the same as FxaaHlsl in Runtime/Source/Renderer/ShaderSources.h (see there for how it works).
// input0 is the frame read linearly, input1 the frame read by point. metrics = 1/width, 1/height, width, height;
// params = contrast always ignored below this, contrast ignored below this share of the brightest neighbor, how much
// the softening may blur, how many steps the walk along an edge takes.
layout(push_constant) uniform PostConstants {
	vec4 metrics;
	vec4 params;
} post;
layout(set = 0, binding = 0) uniform sampler2D input0;
layout(set = 0, binding = 1) uniform sampler2D input1;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

float Luma(vec3 c) {
	return dot(c, vec3(0.299, 0.587, 0.114));
}
float LumaAt(vec2 at) {
	return Luma(textureLod(input0, at, 0.0).rgb);
}
#define LUMA_NEAR(dx, dy) Luma(textureLodOffset(input1, uv, 0.0, ivec2(dx, dy)).rgb)

void main() {
	vec2 texel = post.metrics.xy;
	vec4 center = textureLod(input1, uv, 0.0);
	float m = Luma(center.rgb);
	float n = LUMA_NEAR(0, -1);
	float s = LUMA_NEAR(0, 1);
	float w = LUMA_NEAR(-1, 0);
	float e = LUMA_NEAR(1, 0);
	float highest = max(m, max(max(n, s), max(w, e)));
	float lowest = min(m, min(min(n, s), min(w, e)));
	float contrast = highest - lowest;
	if (contrast < max(post.params.x, highest * post.params.y)) {
		color = center;
		return;
	}
	float nw = LUMA_NEAR(-1, -1);
	float ne = LUMA_NEAR(1, -1);
	float sw = LUMA_NEAR(-1, 1);
	float se = LUMA_NEAR(1, 1);
	float neighborhood = (2.0 * (n + s + w + e) + nw + ne + sw + se) / 12.0;
	float blur = smoothstep(0.0, 1.0, clamp(abs(neighborhood - m) / contrast, 0.0, 1.0));
	blur = blur * blur * post.params.z;
	float acrossRows = abs(nw - 2.0 * w + sw) + 2.0 * abs(n - 2.0 * m + s) + abs(ne - 2.0 * e + se);
	float acrossColumns = abs(nw - 2.0 * n + ne) + 2.0 * abs(w - 2.0 * m + e) + abs(sw - 2.0 * s + se);
	bool horizontal = acrossRows >= acrossColumns;
	float before = horizontal ? n : w;
	float after = horizontal ? s : e;
	float stepAcross = horizontal ? texel.y : texel.x;
	float edgeLuma;
	float gradient;
	if (abs(before - m) >= abs(after - m)) {
		stepAcross = -stepAcross;
		edgeLuma = 0.5 * (before + m);
		gradient = abs(before - m);
	}
	else {
		edgeLuma = 0.5 * (after + m);
		gradient = abs(after - m);
	}
	float threshold = gradient * 0.25;
	vec2 start = uv;
	if (horizontal) start.y += stepAcross * 0.5;
	else start.x += stepAcross * 0.5;
	vec2 along = horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
	vec2 back = start - along;
	vec2 forward = start + along;
	float endBack = LumaAt(back) - edgeLuma;
	float endForward = LumaAt(forward) - edgeLuma;
	bool doneBack = abs(endBack) >= threshold;
	bool doneForward = abs(endForward) >= threshold;
	int steps = int(post.params.w);
	for (int i = 1; i < steps && !(doneBack && doneForward); ++i) {
		float stride = i < 3 ? 1.0 : (i < 6 ? 2.0 : 4.0);
		if (!doneBack) {
			back -= along * stride;
			endBack = LumaAt(back) - edgeLuma;
			doneBack = abs(endBack) >= threshold;
		}
		if (!doneForward) {
			forward += along * stride;
			endForward = LumaAt(forward) - edgeLuma;
			doneForward = abs(endForward) >= threshold;
		}
	}
	float distanceBack = horizontal ? uv.x - back.x : uv.y - back.y;
	float distanceForward = horizontal ? forward.x - uv.x : forward.y - uv.y;
	bool backNearer = distanceBack < distanceForward;
	float shift = 0.5 - min(distanceBack, distanceForward) / (distanceBack + distanceForward);
	float endNear = backNearer ? endBack : endForward;
	if ((endNear < 0.0) == (m < edgeLuma)) shift = 0.0;
	shift = max(shift, blur);
	vec2 at = uv;
	if (horizontal) at.y += shift * stepAcross;
	else at.x += shift * stepAcross;
	color = vec4(textureLod(input0, at, 0.0).rgb, center.a);
}
