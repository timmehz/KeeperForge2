#pragma once

// Shader sources compiled at run time: HLSL for DirectX 11 and 12 (by d3dcompiler_47.dll, which ships with Windows).
// Vulkan takes SPIR-V, made ahead of time from Runtime/Shaders (see VulkanShaders.h). All of them do the same thing:
// - vertex: the game's pre-transformed vertex (screen x, y, depth z, 1/w) into clip space, keeping 1/w so textures
//   are perspective-correct; the D3DCOLOR (bytes B, G, R, A) is swizzled to RGBA;
// - pixel: up to three Direct3D 6 texture stages (D3DTOP color and alpha operations on D3DTA arguments, and the two
//   bump operations, which move the next stage's texture coordinates), then the alpha test. Stages without a texture read white. There are no mip maps, so textures are read at level 0.
namespace KeeperForge2::Render::Shaders {
	inline constexpr const char* DrawHlsl = R"(
cbuffer DrawConstants : register(b0) {
	int4 colorOp;
	int4 colorArg1;
	int4 colorArg2;
	int4 alphaOp;
	int4 alphaArg1;
	int4 alphaArg2;
	int4 texCoordIndex;
	int4 misc;
	float4 textureFactor;
	float4 view;
	float4 depthParams;
	float4 bumpMatrix;
	float4 bumpLuminance;
};
Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
Texture2D texture2 : register(t2);
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);
SamplerState sampler2 : register(s2);

struct VertexIn {
	float4 position : POSITION;
	float4 color : COLOR;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float2 uv2 : TEXCOORD2;
};
struct PixelIn {
	float4 position : SV_Position;
	float4 color : COLOR;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float2 uv2 : TEXCOORD2;
};

PixelIn VertexMain(VertexIn input) {
	PixelIn output;
	float w = input.position.w > 0.0 ? 1.0 / input.position.w : 1.0;
	float x = (input.position.x + 0.5) * view.x - 1.0;
	float y = (input.position.y + 0.5) * view.y + view.z;
	float z = input.position.z * depthParams.x + depthParams.y;
	output.position = float4(x * w, y * w, z * w, w);
	output.color = input.color.zyxw;
	output.uv0 = input.uv0;
	output.uv1 = input.uv1;
	output.uv2 = input.uv2;
	return output;
}

float4 Argument(int argument, float4 current, float4 diffuse, float4 texel) {
	int selector = argument & 15;
	float4 value = float4(0.0, 0.0, 0.0, 0.0);
	if (selector == 0) value = diffuse;
	else if (selector == 1) value = current;
	else if (selector == 2) value = texel;
	else if (selector == 3) value = textureFactor;
	if ((argument & 16) != 0) value = 1.0 - value;
	if ((argument & 32) != 0) value = value.aaaa;
	return value;
}

float4 Operate(int op, float4 a, float4 b, float4 current, float4 diffuse, float4 texel) {
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
	if (op == 12) return lerp(b, a, diffuse.a);
	if (op == 13) return lerp(b, a, texel.a);
	if (op == 14) return lerp(b, a, textureFactor.a);
	if (op == 15) return a + b * (1.0 - texel.a);
	if (op == 16) return lerp(b, a, current.a);
	if (op == 17) return a;
	if (op == 18) return float4(a.rgb + a.a * b.rgb, a.a);
	if (op == 19) return float4(a.rgb * b.rgb + a.a, a.a);
	if (op == 20) return float4((1.0 - a.a) * b.rgb + a.rgb, a.a);
	if (op == 21) return float4((1.0 - a.rgb) * b.rgb + a.a, a.a);
	if (op == 24) return saturate(dot(a.rgb - 0.5, b.rgb - 0.5) * 4.0).xxxx;
	return current;
}

bool AlphaPasses(int func, float alpha) {
	float a = round(saturate(alpha) * 255.0);
	float r = round(view.w * 255.0);
	if (func == 1) return false;
	if (func == 2) return a < r;
	if (func == 3) return a == r;
	if (func == 4) return a <= r;
	if (func == 5) return a > r;
	if (func == 6) return a != r;
	if (func == 7) return a >= r;
	return true;
}

float4 PixelMain(PixelIn input) : SV_Target {
	float2 uvs[3] = { input.uv0, input.uv1, input.uv2 };
	float4 diffuse = input.color;
	float4 current = diffuse;
	// A bump stage moves the next stage's texture coordinates (and, with luminance, scales its texel).
	float2 bumpOffset = float2(0.0, 0.0);
	float bumpLight = 1.0;
	[unroll] for (int stage = 0; stage < 3; ++stage) {
		if (colorOp[stage] == 1) break;
		float2 uv = uvs[clamp(texCoordIndex[stage], 0, 2)] + bumpOffset;
		float4 texel = float4(1.0, 1.0, 1.0, 1.0);
		bool textured = (misc.y & (1 << stage)) != 0;
		if (textured) {
			if (stage == 0) texel = texture0.SampleLevel(sampler0, uv, 0.0);
			else if (stage == 1) texel = texture1.SampleLevel(sampler1, uv, 0.0);
			else texel = texture2.SampleLevel(sampler2, uv, 0.0);
		}
		if (colorOp[stage] == 22 || colorOp[stage] == 23) {
			// The bump texel: du in red, dv in green (stored + 128), luminance in blue. Color and alpha go on unchanged.
			bumpOffset = float2(0.0, 0.0);
			bumpLight = 1.0;
			if (textured) {
				float du = texel.r * (255.0 / 128.0) - 1.0;
				float dv = texel.g * (255.0 / 128.0) - 1.0;
				bumpOffset = float2(du * bumpMatrix.x + dv * bumpMatrix.z, du * bumpMatrix.y + dv * bumpMatrix.w);
				if (colorOp[stage] == 23) bumpLight = saturate(texel.b * bumpLuminance.x + bumpLuminance.y);
			}
			continue;
		}
		texel.rgb *= bumpLight;
		bumpOffset = float2(0.0, 0.0);
		bumpLight = 1.0;
		float4 color = Operate(colorOp[stage], Argument(colorArg1[stage], current, diffuse, texel), Argument(colorArg2[stage], current, diffuse, texel), current, diffuse, texel);
		float alpha = current.a;
		if (alphaOp[stage] != 1) {
			alpha = Operate(alphaOp[stage], Argument(alphaArg1[stage], current, diffuse, texel), Argument(alphaArg2[stage], current, diffuse, texel), current, diffuse, texel).a;
		}
		current = saturate(float4(color.rgb, alpha));
	}
	if (misc.x != 0 && !AlphaPasses(misc.x, current.a)) discard;
	return current;
}
)";

	// Draws a picture into a rectangle of the window: rect is left, top, right, bottom in clip space.
	inline constexpr const char* PresentHlsl = R"(
cbuffer PresentConstants : register(b0) {
	float4 rect;
};
Texture2D source : register(t0);
SamplerState sourceSampler : register(s0);
struct PresentOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};
PresentOut PresentVertex(uint id : SV_VertexID) {
	float2 corner = float2(id & 1, id >> 1);
	PresentOut output;
	output.position = float4(lerp(rect.x, rect.z, corner.x), lerp(rect.y, rect.w, corner.y), 0.0, 1.0);
	output.uv = corner;
	return output;
}
float4 PresentPixel(PresentOut input) : SV_Target {
	return float4(source.SampleLevel(sourceSampler, input.uv, 0.0).rgb, 1.0);
}
// MSAA: the CPU picture of the frame into every sample of the multisampled frame, pixel for pixel (rect is the
// whole target), alpha kept.
float4 CopyPixel(PresentOut input) : SV_Target {
	return source.Load(int3(input.position.xy, 0));
}
)";

	// What every FXAA and SMAA pass shares: one triangle that covers the target (uv 0,0 is the top left), the
	// constants (PostConstants in Backend.h), up to three inputs, and a linear (s0) and a point (s1) sampler, both
	// clamped.
	inline constexpr const char* PostCommonHlsl = R"(
cbuffer PostConstants : register(b0) {
	float4 metrics;
	float4 params;
};
Texture2D input0 : register(t0);
Texture2D input1 : register(t1);
Texture2D input2 : register(t2);
SamplerState linearSampler : register(s0);
SamplerState pointSampler : register(s1);
struct PostOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};
float2 FullScreen(uint id, out float4 position) {
	float2 uv = float2((id << 1) & 2, id & 2);
	position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	return uv;
}
PostOut PostVertex(uint id : SV_VertexID) {
	PostOut output;
	output.uv = FullScreen(id, output.position);
	return output;
}
)";

	// FXAA, our own version of the idea: where the brightness changes sharply, find which way the edge runs, follow it
	// both ways to its ends, and read the picture shifted across the edge by how far this pixel is from the nearer
	// end (pixels near the middle of a long edge barely move, those near an end of a step move up to half a pixel).
	// Single pixels and thin lines are also softened by how much they stand out from all eight neighbors.
	// params: x = contrast always ignored below this, y = contrast ignored below this share of the brightest
	// neighbor, z = how much the softening may blur, w = how many steps the walk along an edge takes.
	inline constexpr const char* FxaaHlsl = R"(
float Luma(float3 color) {
	return dot(color, float3(0.299, 0.587, 0.114));
}
float LumaAt(float2 uv) {
	return Luma(input0.SampleLevel(linearSampler, uv, 0.0).rgb);
}
// A macro: texture offsets must be literal numbers.
#define LumaNear(uv, offset) Luma(input0.SampleLevel(pointSampler, uv, 0.0, offset).rgb)
float4 FxaaPixel(PostOut input) : SV_Target {
	float2 uv = input.uv;
	float2 texel = metrics.xy;
	float4 center = input0.SampleLevel(pointSampler, uv, 0.0);
	float m = Luma(center.rgb);
	float n = LumaNear(uv, int2(0, -1));
	float s = LumaNear(uv, int2(0, 1));
	float w = LumaNear(uv, int2(-1, 0));
	float e = LumaNear(uv, int2(1, 0));
	float highest = max(m, max(max(n, s), max(w, e)));
	float lowest = min(m, min(min(n, s), min(w, e)));
	float contrast = highest - lowest;
	[branch] if (contrast < max(params.x, highest * params.y)) return center;
	float nw = LumaNear(uv, int2(-1, -1));
	float ne = LumaNear(uv, int2(1, -1));
	float sw = LumaNear(uv, int2(-1, 1));
	float se = LumaNear(uv, int2(1, 1));
	float neighborhood = (2.0 * (n + s + w + e) + nw + ne + sw + se) / 12.0;
	float blur = smoothstep(0.0, 1.0, saturate(abs(neighborhood - m) / contrast));
	blur = blur * blur * params.z;
	// Brightness changing from row to row (an edge running left to right) or from column to column.
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
	// Along the edge, half a pixel across it (between the two rows or columns), both ways until it ends.
	float2 start = uv;
	if (horizontal) start.y += stepAcross * 0.5;
	else start.x += stepAcross * 0.5;
	float2 along = horizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
	float2 back = start - along;
	float2 forward = start + along;
	float endBack = LumaAt(back) - edgeLuma;
	float endForward = LumaAt(forward) - edgeLuma;
	bool doneBack = abs(endBack) >= threshold;
	bool doneForward = abs(endForward) >= threshold;
	int steps = (int)params.w;
	[loop] for (int i = 1; i < steps && !(doneBack && doneForward); ++i) {
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
	// Only where the nearer end turns the other way from this pixel: that end is where this side's step is.
	float endNear = backNearer ? endBack : endForward;
	if ((endNear < 0.0) == (m < edgeLuma)) shift = 0.0;
	shift = max(shift, blur);
	float2 at = uv;
	if (horizontal) at.y += shift * stepAcross;
	else at.x += shift * stepAcross;
	return float4(input0.SampleLevel(linearSampler, at, 0.0).rgb, center.a);
}
)";

	// "smaa" in config.yml: our own morphological anti-aliasing, in three passes, all drawn with PostVertex and
	// reading whole pixels (no lookup textures).
	//  1. Edges (input0 = frame): red = this pixel differs from the one on its left, green = from the one above.
	//     A difference counts when it is above params.x and not much weaker than the strongest one around it
	//     (params.z times it must reach that), so the faint side of a strong edge is skipped.
	//  2. Weights (input0 = edges): for each edge, follow it both ways (up to params.y pixels) to its two ends. At
	//     each end, an edge crossing it on one side means the outline steps that way there: the true outline is taken
	//     to run from half a pixel to that side at the end to the middle of the edge. The share of this pixel's
	//     column (or row) that the outline cuts off on either side is how much of the pixel across it is mixed in.
	//     x = this pixel takes from the one above, y = the one above takes from this one, z = this pixel takes from
	//     the one on its left, w = the one on the left takes from this one.
	//  3. Blend (input0 = frame, input1 = weights): each pixel mixes in its four neighbors by those amounts.
	inline constexpr const char* SmaaHlsl = R"(
int2 SmaaLast() {
	return int2(metrics.zw) - 1;
}
float SmaaLuma(int2 at) {
	return dot(input0.Load(int3(clamp(at, int2(0, 0), SmaaLast()), 0)).rgb, float3(0.2126, 0.7152, 0.0722));
}
float2 SmaaEdgePixel(PostOut input) : SV_Target {
	int2 at = int2(input.position.xy);
	float center = SmaaLuma(at);
	float leftLuma = SmaaLuma(at + int2(-1, 0));
	float topLuma = SmaaLuma(at + int2(0, -1));
	float left = abs(center - leftLuma);
	float top = abs(center - topLuma);
	float2 edges = step(params.x, float2(left, top));
	[branch] if (edges.x + edges.y == 0.0) return float2(0.0, 0.0);
	float right = abs(center - SmaaLuma(at + int2(1, 0)));
	float bottom = abs(center - SmaaLuma(at + int2(0, 1)));
	float leftOfLeft = abs(leftLuma - SmaaLuma(at + int2(-2, 0)));
	float aboveTop = abs(topLuma - SmaaLuma(at + int2(0, -2)));
	float strongest = max(max(max(left, top), max(right, bottom)), max(leftOfLeft, aboveTop));
	return edges * step(strongest, params.z * float2(left, top));
}

float2 SmaaEdgesAt(int2 at) {
	if (any(at < int2(0, 0)) || any(at > SmaaLast())) return float2(0.0, 0.0);
	return input0.Load(int3(at, 0)).rg;
}
// The outline over an edge length pixels long: from startHeight at its start to 0 at its middle, then to endHeight at
// its end (heights in pixels, + to one side, - to the other). Gives how much of the stretch from offset to
// offset + 1 lies on the + side (x) and on the - side (y).
float2 SmaaCoverage(float offset, float length, float startHeight, float endHeight) {
	float middle = 0.5 * length;
	float2 cut = float2(0.0, 0.0);
	float from = offset;
	float to = min(offset + 1.0, middle);
	if (to > from) {
		float area = startHeight * ((to - from) - (to * to - from * from) / (2.0 * middle));
		cut += float2(max(area, 0.0), max(-area, 0.0));
	}
	from = max(offset, middle);
	to = offset + 1.0;
	if (to > from) {
		float area = endHeight * (0.5 * (to * to - from * from) - middle * (to - from)) / middle;
		cut += float2(max(area, 0.0), max(-area, 0.0));
	}
	return cut;
}
float4 SmaaWeightPixel(PostOut input) : SV_Target {
	int2 at = int2(input.position.xy);
	float2 own = SmaaEdgesAt(at);
	[branch] if (own.x + own.y == 0.0) return float4(0.0, 0.0, 0.0, 0.0);
	int steps = int(params.y);
	float4 weights = float4(0.0, 0.0, 0.0, 0.0);
	// The edge along the top of this pixel: + is up.
	[branch] if (own.y > 0.0) {
		int left = 0;
		float leftHeight = 0.0;
		[loop] for (int i = 0; i < steps; ++i) {
			float2 here = SmaaEdgesAt(at + int2(-i, 0));
			if (here.y == 0.0) break;
			left = i;
			float above = SmaaEdgesAt(at + int2(-i, -1)).x;
			if (here.x + above > 0.0) {
				leftHeight = 0.5 * (above - here.x);
				break;
			}
		}
		int right = 0;
		float rightHeight = 0.0;
		[loop] for (int j = 0; j < steps; ++j) {
			right = j;
			float2 next = SmaaEdgesAt(at + int2(j + 1, 0));
			float above = SmaaEdgesAt(at + int2(j + 1, -1)).x;
			if (next.x + above > 0.0) {
				rightHeight = 0.5 * (above - next.x);
				break;
			}
			if (next.y == 0.0) break;
		}
		float2 cut = SmaaCoverage(float(left), float(left + right + 1), leftHeight, rightHeight);
		weights.xy = float2(cut.y, cut.x);
	}
	// The edge along the left of this pixel: + is to the left.
	[branch] if (own.x > 0.0) {
		int up = 0;
		float upHeight = 0.0;
		[loop] for (int i = 0; i < steps; ++i) {
			float2 here = SmaaEdgesAt(at + int2(0, -i));
			if (here.x == 0.0) break;
			up = i;
			float leftSide = SmaaEdgesAt(at + int2(-1, -i)).y;
			if (here.y + leftSide > 0.0) {
				upHeight = 0.5 * (leftSide - here.y);
				break;
			}
		}
		int down = 0;
		float downHeight = 0.0;
		[loop] for (int j = 0; j < steps; ++j) {
			down = j;
			float2 next = SmaaEdgesAt(at + int2(0, j + 1));
			float leftSide = SmaaEdgesAt(at + int2(-1, j + 1)).y;
			if (next.y + leftSide > 0.0) {
				downHeight = 0.5 * (leftSide - next.y);
				break;
			}
			if (next.x == 0.0) break;
		}
		float2 cut = SmaaCoverage(float(up), float(up + down + 1), upHeight, downHeight);
		weights.zw = float2(cut.y, cut.x);
	}
	return weights;
}

float4 SmaaBlendPixel(PostOut input) : SV_Target {
	int2 at = int2(input.position.xy);
	int2 last = SmaaLast();
	float4 color = input0.Load(int3(at, 0));
	float4 own = input1.Load(int3(at, 0));
	float fromBelow = at.y < last.y ? input1.Load(int3(at + int2(0, 1), 0)).y : 0.0;
	float fromRight = at.x < last.x ? input1.Load(int3(at + int2(1, 0), 0)).w : 0.0;
	float total = own.x + own.z + fromBelow + fromRight;
	[branch] if (total <= 0.0) return color;
	float scale = total > 1.0 ? 1.0 / total : 1.0;
	float3 mixed = color.rgb * (1.0 - total * scale);
	if (own.x > 0.0) mixed += own.x * scale * input0.Load(int3(at + int2(0, -1), 0)).rgb;
	if (fromBelow > 0.0) mixed += fromBelow * scale * input0.Load(int3(at + int2(0, 1), 0)).rgb;
	if (own.z > 0.0) mixed += own.z * scale * input0.Load(int3(at + int2(-1, 0), 0)).rgb;
	if (fromRight > 0.0) mixed += fromRight * scale * input0.Load(int3(at + int2(1, 0), 0)).rgb;
	return float4(mixed, color.a);
}
)";
}
