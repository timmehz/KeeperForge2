#pragma once

// The ray tracer's compute shaders in HLSL for DirectX 11 and 12 (compiled at run time as cs_5_0, like ShaderSources.h):
// the same two passes as Runtime/Shaders/ray_ao.comp and ray_blur.comp (see docs/game/ray-tracing.md), kept in step with
// them. Here the three results are written straight into their textures (RGBA), as neither DirectX copies a buffer into
// a texture the way Vulkan does. Names HLSL keeps for itself (point, triangle, lit, noise) are avoided.
namespace KeeperForge2::Render::Shaders {
	inline constexpr const char* RayHlsl = R"(
// RayTraceSettings in Backend.h (80 bytes).
cbuffer Settings : register(b0) {
	uint2 size;
	float2 frameSize;
	float2 center;
	float focal;
	float radius;
	uint rays;
	uint frameNumber;
	uint nodeCount;
	uint triangleCount;
	uint lightCount;
	float occlusion;
	float shadows;
	float ambient;
	float reflections;
	float reflectReach;
	float bounce;
	float bounceReach;
};

// Two float4 per node: min and leftOrFirst (int bits), max and count (int bits). Three per triangle: a corner (w the
// material: 1 water or lava) and two edges. Two per light: position and reach, color and strength.
StructuredBuffer<float4> Nodes : register(t0);
StructuredBuffer<float4> Triangles : register(t1);
StructuredBuffer<float4> Lights : register(t2);
Texture2D<float4> Frame : register(t3);
SamplerState Linear : register(s0);
// The trace's result before smoothing (how much light is kept, depth), the reflections (color, how much shows), the
// bounced light before smoothing, the smoothed result (gray), the smoothed bounced light.
RWStructuredBuffer<float2> Raw : register(u0);
RWTexture2D<float4> Reflections : register(u1);
RWStructuredBuffer<float4> BounceRaw : register(u2);
RWTexture2D<float4> Result : register(u3);
RWTexture2D<float4> Bounce : register(u4);

static const float Infinity = 1e30;

float HitTriangle(float3 origin, float3 direction, uint index) {
	float3 corner = Triangles[index * 3].xyz;
	float3 edge1 = Triangles[index * 3 + 1].xyz;
	float3 edge2 = Triangles[index * 3 + 2].xyz;
	float3 p = cross(direction, edge2);
	float determinant = dot(edge1, p);
	if (abs(determinant) < 1e-12) return Infinity;
	float reciprocal = 1.0 / determinant;
	float3 toOrigin = origin - corner;
	float u = dot(toOrigin, p) * reciprocal;
	if (u < 0.0 || u > 1.0) return Infinity;
	float3 q = cross(toOrigin, edge1);
	float v = dot(direction, q) * reciprocal;
	if (v < 0.0 || u + v > 1.0) return Infinity;
	float t = dot(edge2, q) * reciprocal;
	return t > 0.0 ? t : Infinity;
}

float HitBox(float3 origin, float3 inverseDirection, float3 low, float3 high, float limit) {
	float3 t0 = (low - origin) * inverseDirection;
	float3 t1 = (high - origin) * inverseDirection;
	float3 entries = min(t0, t1);
	float3 exits = max(t0, t1);
	float entry = max(max(entries.x, entries.y), max(entries.z, 0.0));
	float leave = min(min(exits.x, exits.y), min(exits.z, limit));
	return entry <= leave ? entry : Infinity;
}

// The nearest hit within limit (any hit when anyHit), its distance or Infinity; with solidOnly water and lava are
// passed through.
float Trace(float3 origin, float3 direction, float limit, bool anyHit, bool solidOnly, out uint hitIndex) {
	hitIndex = 0xFFFFFFFF;
	if (nodeCount == 0) return Infinity;
	float3 safe = direction + (float3)(direction == 0.0) * 1e-20;
	float3 inverseDirection = 1.0 / safe;
	float nearest = limit;
	uint stack[32];
	uint depth = 0;
	stack[depth++] = 0;
	[loop] while (depth > 0) {
		uint node = stack[--depth];
		float4 low = Nodes[node * 2];
		float4 high = Nodes[node * 2 + 1];
		if (HitBox(origin, inverseDirection, low.xyz, high.xyz, nearest) == Infinity) continue;
		uint leftOrFirst = (uint)asint(low.w);
		uint count = (uint)asint(high.w);
		if (count > 0) {
			[loop] for (uint i = 0; i < count; ++i) {
				if (solidOnly && Triangles[(leftOrFirst + i) * 3].w > 0.5) continue;
				float t = HitTriangle(origin, direction, leftOrFirst + i);
				if (t < nearest) {
					nearest = t;
					hitIndex = leftOrFirst + i;
					if (anyHit) return t;
				}
			}
			continue;
		}
		float leftNear = HitBox(origin, inverseDirection, Nodes[leftOrFirst * 2].xyz, Nodes[leftOrFirst * 2 + 1].xyz, nearest);
		float rightNear = HitBox(origin, inverseDirection, Nodes[(leftOrFirst + 1) * 2].xyz, Nodes[(leftOrFirst + 1) * 2 + 1].xyz, nearest);
		if (depth + 2 > 32) continue;
		if (leftNear <= rightNear) {
			if (rightNear != Infinity) stack[depth++] = leftOrFirst + 1;
			if (leftNear != Infinity) stack[depth++] = leftOrFirst;
		}
		else {
			if (leftNear != Infinity) stack[depth++] = leftOrFirst;
			if (rightNear != Infinity) stack[depth++] = leftOrFirst + 1;
		}
	}
	return hitIndex == 0xFFFFFFFF ? Infinity : nearest;
}

float2 Random(uint2 pixel, uint ray) {
	float pattern = frac(52.9829189 * frac(0.06711056 * (float)pixel.x + 0.00583715 * (float)pixel.y));
	float a = frac(pattern + (float)ray * 0.618034);
	float b = frac((float)ray / (float)max(rays, 1) + pattern * 0.5 + 0.5 / (float)max(rays, 1));
	return float2(a, b);
}

// A cosine-weighted direction over the half sphere around normal.
float3 HemisphereRay(float3 side, float3 up, float3 normal, float2 r) {
	float angle = 6.2831853 * r.x;
	float along = sqrt(r.y);
	return normalize(side * (cos(angle) * along) + up * (sin(angle) * along) + normal * sqrt(max(0.0, 1.0 - r.y)));
}

bool OnScreen(float3 there, out float2 onScreen) {
	onScreen = center + focal * there.xy / max(there.z, 1e-6);
	return there.z > 1e-3 && all(onScreen >= -0.5) && all(onScreen < frameSize - 0.5);
}

[numthreads(8, 8, 1)]
void TraceMain(uint3 id : SV_DispatchThreadID) {
	uint2 pixel = id.xy;
	if (pixel.x >= size.x || pixel.y >= size.y) return;
	// Checkerboard, as ray_ao.comp: every other pixel a trace (bit 0 of frameNumber), all of them with bit 1.
	if ((frameNumber & 2) == 0 && ((pixel.x + pixel.y + frameNumber) & 1) != 0) return;
	uint index = pixel.y * size.x + pixel.x;
	float2 screen = ((float2)pixel + 0.5) * frameSize / (float2)size - 0.5;
	float3 direction = normalize(float3((screen - center) / focal, 1.0));
	Reflections[pixel] = float4(0.0, 0.0, 0.0, 0.0);
	BounceRaw[index] = float4(0.0, 0.0, 0.0, 0.0);
	uint seenIndex;
	float hitDistance = Trace(float3(0.0, 0.0, 0.0), direction, Infinity, false, false, seenIndex);
	if (hitDistance == Infinity) {
		Raw[index] = float2(1.0, 0.0);
		return;
	}
	float3 hitPoint = direction * hitDistance;
	float3 normal = normalize(cross(Triangles[seenIndex * 3 + 1].xyz, Triangles[seenIndex * 3 + 2].xyz));
	if (dot(normal, direction) > 0.0) normal = -normal;
	float3 side = normalize(abs(normal.x) > 0.5 ? cross(normal, float3(0.0, 1.0, 0.0)) : cross(normal, float3(1.0, 0.0, 0.0)));
	float3 up = cross(normal, side);
	float3 start = hitPoint + normal * (radius * 0.02);
	bool mirror = Triangles[seenIndex * 3].w > 0.5;

	// Ambient occlusion.
	float closed = 0.0;
	uint aoRays = occlusion > 0.0 && !mirror ? rays : 0;
	[loop] for (uint ray = 0; ray < aoRays; ++ray) {
		uint unused;
		float hit = Trace(start, HemisphereRay(side, up, normal, Random(pixel, ray)), radius, true, true, unused);
		if (hit != Infinity) closed += 1.0 - hit / radius;
	}
	float open = 1.0 - closed / (float)max(aoRays, 1);

	// Bounce light.
	uint bounceRays = bounce > 0.0 && !mirror ? max(rays / 2, 2) : 0;
	float3 bounced = float3(0.0, 0.0, 0.0);
	[loop] for (uint b = 0; b < bounceRays; ++b) {
		float3 out3 = HemisphereRay(side, up, normal, Random(pixel, b + 17));
		uint unused;
		float hit = Trace(start, out3, bounceReach, false, true, unused);
		if (hit == Infinity) continue;
		float2 onScreen;
		if (!OnScreen(start + out3 * hit, onScreen)) continue;
		bounced += Frame.SampleLevel(Linear, (onScreen + 0.5) / frameSize, 0).rgb * (1.0 - hit / bounceReach);
	}
	if (bounceRays > 0) BounceRaw[index] = float4(bounced / (float)bounceRays, 0.0);

	// Shadows: every light counts as arriving; only the strongest few here get a ray (as ray_ao.comp).
	float arrived = ambient;
	float total = ambient;
	uint shadowLights = shadows > 0.0 && !mirror ? lightCount : 0;
	float strongest[4] = { 0.0, 0.0, 0.0, 0.0 };
	uint strongestLight[4] = { 0, 0, 0, 0 };
	[loop] for (uint l = 0; l < shadowLights; ++l) {
		float4 placeAndReach = Lights[l * 2];
		float4 colorAndStrength = Lights[l * 2 + 1];
		float3 toLight = placeAndReach.xyz - hitPoint;
		float away = length(toLight);
		if (away >= placeAndReach.w || away < 1e-4) continue;
		float facing = dot(normal, toLight / away);
		if (facing <= 0.0) continue;
		float falloff = 1.0 - away / placeAndReach.w;
		float share = dot(colorAndStrength.rgb, float3(0.3333, 0.3333, 0.3333)) * colorAndStrength.a * falloff * falloff * facing;
		if (share <= 0.0) continue;
		total += share;
		arrived += share;
		if (share < 0.03 || share <= strongest[3]) continue;
		uint slot = 3;
		[loop] while (slot > 0 && strongest[slot - 1] < share) {
			strongest[slot] = strongest[slot - 1];
			strongestLight[slot] = strongestLight[slot - 1];
			--slot;
		}
		strongest[slot] = share;
		strongestLight[slot] = l;
	}
	[loop] for (uint s = 0; s < 4; ++s) {
		if (strongest[s] <= 0.0) break;
		float3 toLight = Lights[strongestLight[s] * 2].xyz - hitPoint;
		float away = length(toLight);
		float limit = away - min(0.25, away * 0.5);
		uint unused;
		if (limit > 0.0 && Trace(start, toLight / away, limit, true, true, unused) != Infinity) arrived -= strongest[s];
	}
	float lightShare = total > 0.0 ? arrived / total : 1.0;
	float keep = lerp(1.0, open, saturate(occlusion)) * lerp(1.0, lightShare, saturate(shadows));

	// Reflections on water and lava.
	if (mirror && reflections > 0.0) {
		float3 reflected = reflect(direction, normal);
		float3 from = hitPoint + normal * (radius * 0.02);
		uint unused;
		float travelled = Trace(from, reflected, reflectReach, false, true, unused);
		float2 onScreen;
		if (travelled != Infinity && OnScreen(from + reflected * travelled, onScreen)) {
			float3 color = Frame.SampleLevel(Linear, (onScreen + 0.5) / frameSize, 0).rgb;
			float facing = saturate(dot(-direction, normal));
			float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
			float2 edge = min(onScreen, frameSize - onScreen) / (frameSize * 0.08);
			float fade = saturate(min(edge.x, edge.y)) * (1.0 - travelled / reflectReach);
			Reflections[pixel] = float4(color, saturate(reflections * (0.3 + 0.7 * fresnel) * fade));
		}
	}
	Raw[index] = float2(keep, hitPoint.z);
}

[numthreads(8, 8, 1)]
void BlurMain(uint3 id : SV_DispatchThreadID) {
	int2 pixel = (int2)id.xy;
	int2 extent = (int2)size;
	if (pixel.x >= extent.x || pixel.y >= extent.y) return;
	float2 here = Raw[pixel.y * extent.x + pixel.x];
	float open = 1.0;
	float3 light = float3(0.0, 0.0, 0.0);
	if (here.y > 0.0) {
		float sum = 0.0;
		float3 lightSum = float3(0.0, 0.0, 0.0);
		float weights = 0.0;
		[loop] for (int dy = -3; dy <= 3; ++dy) {
			[loop] for (int dx = -3; dx <= 3; ++dx) {
				int2 other = clamp(pixel + int2(dx, dy), int2(0, 0), extent - 1);
				float2 value = Raw[other.y * extent.x + other.x];
				if (value.y <= 0.0) continue;
				float spread = exp(-(float)(dx * dx + dy * dy) / 8.0);
				float depthWeight = exp(-abs(value.y - here.y) / (here.y * 0.02 + 1e-6));
				float weight = spread * depthWeight;
				sum += value.x * weight;
				lightSum += BounceRaw[other.y * extent.x + other.x].rgb * weight;
				weights += weight;
			}
		}
		open = weights > 0.0 ? sum / weights : here.x;
		light = weights > 0.0 ? lightSum / weights : BounceRaw[pixel.y * extent.x + pixel.x].rgb;
	}
	float gray = saturate(open);
	Result[pixel] = float4(gray, gray, gray, 1.0);
	Bounce[pixel] = float4(saturate(light * bounce), 1.0);
}
)";
}
