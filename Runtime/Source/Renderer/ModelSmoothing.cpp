#include "ModelSmoothing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>

// Model smoothing, our own (docs/game/renderer.md). How it stays free of cracks:
// - Corners at the same place on the screen (the same x, y and rhw, bit for bit) are one corner, whatever their texture
//   coordinates, so a seam in a model's texture does not split it.
// - A corner's normal is the average of the faces around it; the corner is smooth when every one of those faces is
//   within 60 degrees of it. A sharp corner (a crease, a horn's tip) never curves.
// - An edge curves only when it has exactly two triangles in this draw, facing the same way, and both its corners are
//   smooth. Its curve then depends only on its two corners and their normals, the same for both triangles, and its
//   points are worked out once (in one direction) and shared, so both sides meet exactly. Every other edge stays
//   straight and is not cut, so the edges a draw shares with another draw (whose normals are its own) never move.
// - A triangle whose three edges curve is cut into a grid; one with only some curved edges is a fan around its middle
//   through every point of its edges; one with none is left as it was.
namespace KeeperForge2::Render {
	namespace {
		struct Vec3 {
			float x = 0.0f, y = 0.0f, z = 0.0f;
		};
		Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }

		// A corner is smooth when every face around it is within 60 degrees of their average.
		constexpr float SmoothCosine = 0.5f;
		// An edge curves when its curve's middle is at least this far (pixels) from the straight edge's.
		constexpr float CurvePixels = 0.35f;
		// Nor when the curve would stand out more than this share of the edge's length (normals that make no sense).
		constexpr float MostBulge = 0.5f;
		constexpr float NearestDepth = 1.0e-4f;
		constexpr std::size_t MostVertices = 65535;

		struct ScreenPoint {
			float x, y, rhw;
		};
		struct Edge {
			std::uint32_t low = 0;
			std::uint32_t high = 0;
			std::uint32_t count = 0;
			// +1 for a triangle that runs low to high, -1 for high to low: 0 with two triangles facing the same way.
			std::int32_t direction = 0;
			bool curved = false;
			// The curve's two inner control points, from low to high.
			Vec3 nearLow;
			Vec3 nearHigh;
			// The points that cut it into segments pieces, from low to high (segments - 1 of them).
			std::vector<ScreenPoint> points;
		};
		std::uint64_t EdgeKey(std::uint32_t a, std::uint32_t b) {
			const std::uint32_t low = std::min<std::uint32_t>(a, b);
			const std::uint32_t high = std::max<std::uint32_t>(a, b);
			return static_cast<std::uint64_t>(low) << 32 | high;
		}

		// A cubic curve's point (Bernstein form).
		Vec3 Cubic(Vec3 p0, Vec3 c1, Vec3 c2, Vec3 p3, float t) {
			const float s = 1.0f - t;
			return p0 * (s * s * s) + c1 * (3.0f * s * s * t) + c2 * (3.0f * s * t * t) + p3 * (t * t * t);
		}
		// A PN triangle edge's inner control point near a: a third of the way to b, moved onto a's tangent plane.
		Vec3 ControlNear(Vec3 a, Vec3 b, Vec3 normalA) {
			const float along = Dot(b - a, normalA);
			return (a * 2.0f + b - normalA * along) * (1.0f / 3.0f);
		}

		struct Work {
			std::vector<std::uint32_t> order;
			std::vector<std::uint32_t> weld;
			std::vector<Vec3> position;
			std::vector<Vec3> normal;
			std::vector<std::uint8_t> valid;
			std::vector<std::uint8_t> smooth;
			std::unordered_map<std::uint64_t, Edge> edges;
		};
		Work work;
	}

	bool SmoothTriangles(const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* triangles, std::uint32_t indexCount,
		const SmoothingProjection& projection, std::uint32_t segments, std::vector<Vertex>& outVertices, std::vector<std::uint16_t>& outIndices) {
		if (vertices == nullptr || triangles == nullptr || vertexCount == 0 || indexCount < 3 || !(projection.focal > 1.0f)) return false;
		segments = std::clamp<std::uint32_t>(segments, 2, 4);
		const std::uint32_t triangleCount = indexCount / 3;
		const float focal = projection.focal;
		const float centerX = projection.centerX;
		const float centerY = projection.centerY;

		// One corner per place on the screen.
		auto& order = work.order;
		auto& weld = work.weld;
		order.resize(vertexCount);
		std::iota(order.begin(), order.end(), 0u);
		const auto bits = [vertices](std::uint32_t i) {
			std::uint32_t x, y, w;
			std::memcpy(&x, &vertices[i].x, 4);
			std::memcpy(&y, &vertices[i].y, 4);
			std::memcpy(&w, &vertices[i].rhw, 4);
			return std::array<std::uint32_t, 3>{ x, y, w };
		};
		std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return bits(a) < bits(b); });
		weld.assign(vertexCount, 0);
		std::uint32_t corners = 0;
		for (std::uint32_t i = 0; i < vertexCount; ++i) {
			if (i > 0 && bits(order[i]) != bits(order[i - 1])) ++corners;
			weld[order[i]] = corners;
		}
		++corners;

		// Each corner back in the camera's space: z = 1 / rhw, x and y undone from the projection.
		auto& position = work.position;
		auto& valid = work.valid;
		position.assign(corners, {});
		valid.assign(corners, 0);
		for (std::uint32_t i = 0; i < vertexCount; ++i) {
			const Vertex& v = vertices[i];
			const std::uint32_t c = weld[i];
			if (valid[c] || !(v.rhw > 0.0f) || !std::isfinite(v.rhw) || !std::isfinite(v.x) || !std::isfinite(v.y)) continue;
			const float w = 1.0f / v.rhw;
			position[c] = { (v.x - centerX) * w / focal, (v.y - centerY) * w / focal, w };
			valid[c] = 1;
		}

		// Normals: the faces around each corner, weighted by their size; then whether the corner is smooth.
		auto& normal = work.normal;
		auto& smooth = work.smooth;
		normal.assign(corners, {});
		smooth.assign(corners, 1);
		const auto triangleOk = [&](std::uint32_t t) {
			const std::uint16_t* index = triangles + t * 3;
			if (index[0] >= vertexCount || index[1] >= vertexCount || index[2] >= vertexCount) return false;
			const std::uint32_t a = weld[index[0]], b = weld[index[1]], c = weld[index[2]];
			return valid[a] && valid[b] && valid[c] && a != b && b != c && a != c;
		};
		for (std::uint32_t t = 0; t < triangleCount; ++t) {
			if (!triangleOk(t)) continue;
			const std::uint16_t* index = triangles + t * 3;
			const std::uint32_t a = weld[index[0]], b = weld[index[1]], c = weld[index[2]];
			const Vec3 face = Cross(position[b] - position[a], position[c] - position[a]);
			for (const std::uint32_t corner : { a, b, c }) normal[corner] = normal[corner] + face;
		}
		for (Vec3& n : normal) {
			const float length = Length(n);
			n = length > 0.0f ? n * (1.0f / length) : Vec3{};
		}
		for (std::uint32_t t = 0; t < triangleCount; ++t) {
			if (!triangleOk(t)) continue;
			const std::uint16_t* index = triangles + t * 3;
			const std::uint32_t a = weld[index[0]], b = weld[index[1]], c = weld[index[2]];
			Vec3 face = Cross(position[b] - position[a], position[c] - position[a]);
			const float length = Length(face);
			if (!(length > 0.0f)) continue;
			face = face * (1.0f / length);
			for (const std::uint32_t corner : { a, b, c }) {
				if (Dot(face, normal[corner]) < SmoothCosine) smooth[corner] = 0;
			}
		}

		// The edges: how many triangles each has, which way they run, and whether it curves.
		auto& edges = work.edges;
		edges.clear();
		edges.reserve(static_cast<std::size_t>(triangleCount) * 2);
		for (std::uint32_t t = 0; t < triangleCount; ++t) {
			if (!triangleOk(t)) continue;
			const std::uint16_t* index = triangles + t * 3;
			const std::uint32_t c[3] = { weld[index[0]], weld[index[1]], weld[index[2]] };
			for (int side = 0; side < 3; ++side) {
				const std::uint32_t from = c[side];
				const std::uint32_t to = c[(side + 1) % 3];
				Edge& edge = edges[EdgeKey(from, to)];
				edge.low = std::min<std::uint32_t>(from, to);
				edge.high = std::max<std::uint32_t>(from, to);
				++edge.count;
				edge.direction += from < to ? 1 : -1;
			}
		}
		bool anyCurved = false;
		for (auto& [key, edge] : edges) {
			if (edge.count != 2 || edge.direction != 0 || !smooth[edge.low] || !smooth[edge.high]) continue;
			const Vec3 low = position[edge.low];
			const Vec3 high = position[edge.high];
			if (Length(normal[edge.low]) < 0.5f || Length(normal[edge.high]) < 0.5f) continue;
			const Vec3 nearLow = ControlNear(low, high, normal[edge.low]);
			const Vec3 nearHigh = ControlNear(high, low, normal[edge.high]);
			const Vec3 middle = Cubic(low, nearLow, nearHigh, high, 0.5f);
			const Vec3 straight = (low + high) * 0.5f;
			const float bulge = Length(middle - straight);
			if (!(middle.z > NearestDepth) || bulge > MostBulge * Length(high - low)) continue;
			if (bulge * focal / middle.z < CurvePixels) continue;
			edge.curved = true;
			edge.nearLow = nearLow;
			edge.nearHigh = nearHigh;
			edge.points.clear();
			for (std::uint32_t k = 1; k < segments; ++k) {
				const Vec3 p = Cubic(low, nearLow, nearHigh, high, static_cast<float>(k) / static_cast<float>(segments));
				if (!(p.z > NearestDepth)) {
					edge.curved = false;
					break;
				}
				edge.points.push_back({ centerX + focal * p.x / p.z, centerY + focal * p.y / p.z, 1.0f / p.z });
			}
			if (edge.curved) anyCurved = true;
		}
		if (!anyCurved) return false;

		outVertices.clear();
		outIndices.clear();
		outVertices.reserve(static_cast<std::size_t>(triangleCount) * 6);
		outIndices.reserve(static_cast<std::size_t>(triangleCount) * 3 * 4);
		bool changed = false;

		for (std::uint32_t t = 0; t < triangleCount; ++t) {
			const std::uint16_t* index = triangles + t * 3;
			if (index[0] >= vertexCount || index[1] >= vertexCount || index[2] >= vertexCount) continue;
			const Vertex* corner[3] = { &vertices[index[0]], &vertices[index[1]], &vertices[index[2]] };
			const std::uint32_t base = static_cast<std::uint32_t>(outVertices.size());
			const auto copyAsIs = [&] {
				for (const Vertex* v : corner) outVertices.push_back(*v);
				for (std::uint32_t i = 0; i < 3; ++i) outIndices.push_back(static_cast<std::uint16_t>(base + i));
			};
			if (!triangleOk(t)) {
				copyAsIs();
				continue;
			}
			const std::uint32_t c[3] = { weld[index[0]], weld[index[1]], weld[index[2]] };
			const Edge* side[3] = { &edges[EdgeKey(c[0], c[1])], &edges[EdgeKey(c[1], c[2])], &edges[EdgeKey(c[2], c[0])] };
			const bool curved[3] = { side[0]->curved, side[1]->curved, side[2]->curved };
			if (!curved[0] && !curved[1] && !curved[2]) {
				copyAsIs();
				continue;
			}
			changed = true;
			const Vec3 p[3] = { position[c[0]], position[c[1]], position[c[2]] };
			// Each edge's two inner control points, near its first corner and near its second (as this triangle runs).
			Vec3 nearFirst[3], nearSecond[3];
			for (int s = 0; s < 3; ++s) {
				const Vec3 a = p[s];
				const Vec3 b = p[(s + 1) % 3];
				if (!curved[s]) {
					nearFirst[s] = (a * 2.0f + b) * (1.0f / 3.0f);
					nearSecond[s] = (b * 2.0f + a) * (1.0f / 3.0f);
				}
				else if (c[s] == side[s]->low) {
					nearFirst[s] = side[s]->nearLow;
					nearSecond[s] = side[s]->nearHigh;
				}
				else {
					nearFirst[s] = side[s]->nearHigh;
					nearSecond[s] = side[s]->nearLow;
				}
			}
			// The patch's middle control point (PN triangles: the edges' average, pushed out by half its distance from the
			// corners' average).
			const Vec3 edgeAverage = (nearFirst[0] + nearSecond[0] + nearFirst[1] + nearSecond[1] + nearFirst[2] + nearSecond[2]) * (1.0f / 6.0f);
			const Vec3 cornerAverage = (p[0] + p[1] + p[2]) * (1.0f / 3.0f);
			const Vec3 middle = edgeAverage + (edgeAverage - cornerAverage) * 0.5f;
			// The patch at weights (w0, w1, w2) for corners 0, 1, 2.
			const auto patch = [&](float w0, float w1, float w2) {
				return p[0] * (w0 * w0 * w0) + p[1] * (w1 * w1 * w1) + p[2] * (w2 * w2 * w2)
					+ nearFirst[0] * (3.0f * w0 * w0 * w1) + nearSecond[0] * (3.0f * w0 * w1 * w1)
					+ nearFirst[1] * (3.0f * w1 * w1 * w2) + nearSecond[1] * (3.0f * w1 * w2 * w2)
					+ nearFirst[2] * (3.0f * w2 * w2 * w0) + nearSecond[2] * (3.0f * w2 * w0 * w0)
					+ middle * (6.0f * w0 * w1 * w2);
			};
			// Depth as the game's projection gives it, z = a + b * rhw, from this triangle's corners.
			float depthA = (corner[0]->z + corner[1]->z + corner[2]->z) / 3.0f;
			float depthB = 0.0f;
			{
				int first = 0, second = 1;
				float widest = std::fabs(corner[0]->rhw - corner[1]->rhw);
				if (std::fabs(corner[1]->rhw - corner[2]->rhw) > widest) { first = 1; second = 2; widest = std::fabs(corner[1]->rhw - corner[2]->rhw); }
				if (std::fabs(corner[2]->rhw - corner[0]->rhw) > widest) { first = 2; second = 0; widest = std::fabs(corner[2]->rhw - corner[0]->rhw); }
				if (widest > 1.0e-7f * std::max<float>(corner[first]->rhw, corner[second]->rhw)) {
					depthB = (corner[first]->z - corner[second]->z) / (corner[first]->rhw - corner[second]->rhw);
					depthA = corner[first]->z - depthB * corner[first]->rhw;
				}
			}
			// A new vertex: the corners' colors and texture coordinates mixed by the weights, placed at a screen point.
			const auto make = [&](float w0, float w1, float w2, ScreenPoint at) {
				Vertex v{};
				v.x = at.x;
				v.y = at.y;
				v.rhw = at.rhw;
				v.z = std::clamp(depthA + depthB * at.rhw, 0.0f, 1.0f);
				std::uint32_t diffuse = 0;
				for (std::uint32_t shift = 0; shift < 32; shift += 8) {
					const float mixed = w0 * static_cast<float>((corner[0]->diffuse >> shift) & 0xFF) + w1 * static_cast<float>((corner[1]->diffuse >> shift) & 0xFF)
						+ w2 * static_cast<float>((corner[2]->diffuse >> shift) & 0xFF);
					diffuse |= static_cast<std::uint32_t>(std::clamp(mixed + 0.5f, 0.0f, 255.0f)) << shift;
				}
				v.diffuse = diffuse;
				for (int set = 0; set < 3; ++set) {
					for (int axis = 0; axis < 2; ++axis) {
						v.uv[set][axis] = w0 * corner[0]->uv[set][axis] + w1 * corner[1]->uv[set][axis] + w2 * corner[2]->uv[set][axis];
					}
				}
				outVertices.push_back(v);
			};
			const auto onScreen = [&](Vec3 q, float w0, float w1, float w2) {
				if (!(q.z > NearestDepth)) {
					// Behind the camera's near limit: the flat triangle's point instead.
					const float rhw = w0 * corner[0]->rhw + w1 * corner[1]->rhw + w2 * corner[2]->rhw;
					return ScreenPoint{ w0 * corner[0]->x + w1 * corner[1]->x + w2 * corner[2]->x, w0 * corner[0]->y + w1 * corner[1]->y + w2 * corner[2]->y, rhw };
				}
				return ScreenPoint{ centerX + focal * q.x / q.z, centerY + focal * q.y / q.z, 1.0f / q.z };
			};
			// Point k (1 to segments - 1) of side s, counted from the side's first corner, from the shared list.
			const auto sidePoint = [&](int s, std::uint32_t k) {
				const Edge& edge = *side[s];
				return c[s] == edge.low ? edge.points[k - 1] : edge.points[segments - k - 1];
			};

			if (curved[0] && curved[1] && curved[2]) {
				// A grid: point (i, j) has weight i / n for corner 1 and j / n for corner 2, rows j = 0 to n.
				const std::uint32_t n = segments;
				const float step = 1.0f / static_cast<float>(n);
				for (std::uint32_t j = 0; j <= n; ++j) {
					for (std::uint32_t i = 0; i + j <= n; ++i) {
						const float w1 = static_cast<float>(i) * step;
						const float w2 = static_cast<float>(j) * step;
						const float w0 = 1.0f - w1 - w2;
						if (i == 0 && j == 0) outVertices.push_back(*corner[0]);
						else if (i == n && j == 0) outVertices.push_back(*corner[1]);
						else if (i == 0 && j == n) outVertices.push_back(*corner[2]);
						else if (j == 0) make(w0, w1, w2, sidePoint(0, i));
						else if (i + j == n) make(w0, w1, w2, sidePoint(1, j));
						else if (i == 0) make(w0, w1, w2, sidePoint(2, n - j));
						else make(w0, w1, w2, onScreen(patch(w0, w1, w2), w0, w1, w2));
					}
				}
				const auto at = [&](std::uint32_t i, std::uint32_t j) {
					return static_cast<std::uint16_t>(base + j * (n + 1) - j * (j - 1) / 2 + i);
				};
				for (std::uint32_t j = 0; j < n; ++j) {
					for (std::uint32_t i = 0; i + j < n; ++i) {
						outIndices.push_back(at(i, j));
						outIndices.push_back(at(i + 1, j));
						outIndices.push_back(at(i, j + 1));
						if (i + j + 1 < n) {
							outIndices.push_back(at(i + 1, j));
							outIndices.push_back(at(i + 1, j + 1));
							outIndices.push_back(at(i, j + 1));
						}
					}
				}
			}
			else {
				// A fan from the patch's middle through every point of the three sides, in the triangle's own order.
				const float third = 1.0f / 3.0f;
				make(third, third, third, onScreen(patch(third, third, third), third, third, third));
				for (int s = 0; s < 3; ++s) {
					outVertices.push_back(*corner[s]);
					if (!curved[s]) continue;
					const int next = (s + 1) % 3;
					for (std::uint32_t k = 1; k < segments; ++k) {
						const float t = static_cast<float>(k) / static_cast<float>(segments);
						float w[3]{};
						w[s] = 1.0f - t;
						w[next] = t;
						make(w[0], w[1], w[2], sidePoint(s, k));
					}
				}
				const std::uint32_t loop = static_cast<std::uint32_t>(outVertices.size()) - base - 1;
				for (std::uint32_t k = 0; k < loop; ++k) {
					outIndices.push_back(static_cast<std::uint16_t>(base));
					outIndices.push_back(static_cast<std::uint16_t>(base + 1 + k));
					outIndices.push_back(static_cast<std::uint16_t>(base + 1 + (k + 1) % loop));
				}
			}
			if (outVertices.size() > MostVertices) return false;
		}
		return changed && outVertices.size() <= MostVertices;
	}
}
