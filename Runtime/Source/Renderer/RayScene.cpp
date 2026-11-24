#include "RayScene.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace KeeperForge2::Render {
	namespace {
		// Leaves of at most this many triangles when splitting costs more than it saves; never more than LeafLimit.
		constexpr std::uint32_t LeafSize = 2;
		constexpr std::uint32_t LeafLimit = 16;
		constexpr int Bins = 12;
		// How many triangles' edges are looked at for the typical edge.
		constexpr std::size_t EdgeSamples = 2048;
		constexpr float Huge = (std::numeric_limits<float>::max)();

		float Length(const float a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
	}

	void RayScene::Bounds::Reset() {
		for (int axis = 0; axis < 3; ++axis) {
			min[axis] = Huge;
			max[axis] = -Huge;
		}
	}
	void RayScene::Bounds::Grow(const float point[3]) {
		for (int axis = 0; axis < 3; ++axis) {
			min[axis] = (std::min)(min[axis], point[axis]);
			max[axis] = (std::max)(max[axis], point[axis]);
		}
	}
	void RayScene::Bounds::Grow(const Bounds& other) {
		for (int axis = 0; axis < 3; ++axis) {
			min[axis] = (std::min)(min[axis], other.min[axis]);
			max[axis] = (std::max)(max[axis], other.max[axis]);
		}
	}
	float RayScene::Bounds::Area() const {
		const float x = max[0] - min[0];
		const float y = max[1] - min[1];
		const float z = max[2] - min[2];
		if (x < 0.0f || y < 0.0f || z < 0.0f) return 0.0f;
		return x * y + y * z + z * x;
	}

	void RayScene::Begin(std::uint32_t frameWidth, std::uint32_t frameHeight, float focalLength, float middleX, float middleY) {
		Clear();
		begun = true;
		width = frameWidth;
		height = frameHeight;
		focal = focalLength;
		centerX = middleX;
		centerY = middleY;
	}

	void RayScene::Clear() {
		begun = false;
		triangles.clear();
		ordered.clear();
		nodes.clear();
		typicalEdge = 0.0f;
	}

	void RayScene::Add(const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* indices, std::uint32_t indexCount, float material) {
		if (!begun || focal <= 0.0f) return;
		const float middleX = centerX;
		const float middleY = centerY;
		for (std::uint32_t i = 0; i + 2 < indexCount; i += 3) {
			float corners[3][3];
			bool usable = true;
			for (int corner = 0; corner < 3; ++corner) {
				const std::uint16_t index = indices[i + corner];
				if (index >= vertexCount) {
					usable = false;
					break;
				}
				const Vertex& vertex = vertices[index];
				// rhw is 1/w; w is the depth in the camera's own units. A point at or behind the camera has none.
				if (!(vertex.rhw > 0.0f) || !std::isfinite(vertex.rhw)) {
					usable = false;
					break;
				}
				const float w = 1.0f / vertex.rhw;
				corners[corner][0] = (vertex.x - middleX) * w / focal;
				corners[corner][1] = (vertex.y - middleY) * w / focal;
				corners[corner][2] = w;
			}
			if (!usable) continue;
			RayTriangle triangle{};
			for (int axis = 0; axis < 3; ++axis) {
				triangle.corner[axis] = corners[0][axis];
				triangle.edge1[axis] = corners[1][axis] - corners[0][axis];
				triangle.edge2[axis] = corners[2][axis] - corners[0][axis];
			}
			triangle.corner[3] = material;
			// No area: nothing to hit.
			const float cross[3] = {
				triangle.edge1[1] * triangle.edge2[2] - triangle.edge1[2] * triangle.edge2[1],
				triangle.edge1[2] * triangle.edge2[0] - triangle.edge1[0] * triangle.edge2[2],
				triangle.edge1[0] * triangle.edge2[1] - triangle.edge1[1] * triangle.edge2[0],
			};
			if (!(Length(cross) > 1e-12f)) continue;
			triangles.push_back(triangle);
		}
	}

	void RayScene::Build() {
		nodes.clear();
		ordered.clear();
		const std::uint32_t count = TriangleCount();
		if (count == 0) return;

		// Each triangle's bounds and middle.
		triangleBounds.resize(count);
		centers.resize(static_cast<std::size_t>(count) * 3);
		order.resize(count);
		for (std::uint32_t i = 0; i < count; ++i) {
			const RayTriangle& triangle = triangles[i];
			const float a[3] = { triangle.corner[0], triangle.corner[1], triangle.corner[2] };
			const float b[3] = { a[0] + triangle.edge1[0], a[1] + triangle.edge1[1], a[2] + triangle.edge1[2] };
			const float c[3] = { a[0] + triangle.edge2[0], a[1] + triangle.edge2[1], a[2] + triangle.edge2[2] };
			Bounds& bounds = triangleBounds[i];
			bounds.Reset();
			bounds.Grow(a);
			bounds.Grow(b);
			bounds.Grow(c);
			for (int axis = 0; axis < 3; ++axis) centers[static_cast<std::size_t>(i) * 3 + axis] = (bounds.min[axis] + bounds.max[axis]) * 0.5f;
			order[i] = i;
		}

		// The typical edge, from a spread of triangles.
		{
			std::vector<float> edges;
			const std::size_t step = (std::max<std::size_t>)(1, count / EdgeSamples);
			edges.reserve(count / step + 1);
			for (std::size_t i = 0; i < count; i += step) {
				const RayTriangle& triangle = triangles[i];
				const float third[3] = { triangle.edge2[0] - triangle.edge1[0], triangle.edge2[1] - triangle.edge1[1], triangle.edge2[2] - triangle.edge1[2] };
				edges.push_back((std::max)({ Length(triangle.edge1), Length(triangle.edge2), Length(third) }));
			}
			const auto middle = edges.begin() + static_cast<std::ptrdiff_t>(edges.size() / 2);
			std::nth_element(edges.begin(), middle, edges.end());
			typicalEdge = *middle;
		}

		// The tree, top down: each node splits where the binned surface area heuristic says, children made in pairs.
		nodes.reserve(static_cast<std::size_t>(count) * 2);
		nodes.push_back({});
		struct Work {
			std::uint32_t node;
			std::uint32_t first;
			std::uint32_t count;
		};
		std::vector<Work> stack;
		stack.push_back({ 0, 0, count });
		while (!stack.empty()) {
			const Work work = stack.back();
			stack.pop_back();
			Bounds bounds;
			Bounds middles;
			bounds.Reset();
			middles.Reset();
			for (std::uint32_t i = work.first; i < work.first + work.count; ++i) {
				bounds.Grow(triangleBounds[order[i]]);
				middles.Grow(&centers[static_cast<std::size_t>(order[i]) * 3]);
			}
			const auto makeLeaf = [&]() {
				RayNode& node = nodes[work.node];
				for (int axis = 0; axis < 3; ++axis) {
					node.boundsMin[axis] = bounds.min[axis];
					node.boundsMax[axis] = bounds.max[axis];
				}
				node.leftOrFirst = static_cast<std::int32_t>(work.first);
				node.count = static_cast<std::int32_t>(work.count);
			};
			if (work.count <= LeafSize) {
				makeLeaf();
				continue;
			}

			// The cheapest split over the three axes.
			float bestCost = Huge;
			int bestAxis = -1;
			int bestSplit = 0;
			for (int axis = 0; axis < 3; ++axis) {
				const float low = middles.min[axis];
				const float extent = middles.max[axis] - low;
				if (!(extent > 1e-9f)) continue;
				Bounds binBounds[Bins];
				std::uint32_t binCounts[Bins]{};
				for (Bounds& bin : binBounds) bin.Reset();
				const float scale = static_cast<float>(Bins) / extent;
				for (std::uint32_t i = work.first; i < work.first + work.count; ++i) {
					const int bin = (std::min)(Bins - 1, static_cast<int>((centers[static_cast<std::size_t>(order[i]) * 3 + axis] - low) * scale));
					binBounds[bin].Grow(triangleBounds[order[i]]);
					++binCounts[bin];
				}
				// Left sides from the left, right sides from the right.
				float leftArea[Bins - 1];
				std::uint32_t leftCount[Bins - 1];
				Bounds running;
				running.Reset();
				std::uint32_t total = 0;
				for (int split = 0; split < Bins - 1; ++split) {
					running.Grow(binBounds[split]);
					total += binCounts[split];
					leftArea[split] = running.Area();
					leftCount[split] = total;
				}
				running.Reset();
				total = 0;
				for (int split = Bins - 2; split >= 0; --split) {
					running.Grow(binBounds[split + 1]);
					total += binCounts[split + 1];
					if (leftCount[split] == 0 || total == 0) continue;
					const float cost = static_cast<float>(leftCount[split]) * leftArea[split] + static_cast<float>(total) * running.Area();
					if (cost < bestCost) {
						bestCost = cost;
						bestAxis = axis;
						bestSplit = split;
					}
				}
			}
			const float leafCost = static_cast<float>(work.count) * bounds.Area();
			if (bestAxis < 0 || (bestCost >= leafCost && work.count <= LeafLimit)) {
				makeLeaf();
				continue;
			}

			// The triangles left of the split first.
			const float low = middles.min[bestAxis];
			const float scale = static_cast<float>(Bins) / (middles.max[bestAxis] - low);
			const auto begin = order.begin() + work.first;
			const auto end = begin + work.count;
			const auto middle = std::partition(begin, end, [&](std::uint32_t triangle) {
				const int bin = (std::min)(Bins - 1, static_cast<int>((centers[static_cast<std::size_t>(triangle) * 3 + bestAxis] - low) * scale));
				return bin <= bestSplit;
			});
			const auto leftCount = static_cast<std::uint32_t>(middle - begin);
			if (leftCount == 0 || leftCount == work.count) {
				makeLeaf();
				continue;
			}
			const auto left = static_cast<std::uint32_t>(nodes.size());
			nodes.push_back({});
			nodes.push_back({});
			RayNode& node = nodes[work.node];
			for (int axis = 0; axis < 3; ++axis) {
				node.boundsMin[axis] = bounds.min[axis];
				node.boundsMax[axis] = bounds.max[axis];
			}
			node.leftOrFirst = static_cast<std::int32_t>(left);
			node.count = 0;
			stack.push_back({ left, work.first, leftCount });
			stack.push_back({ left + 1, work.first + leftCount, work.count - leftCount });
		}

		// The triangles in the tree's order, so a leaf's are side by side.
		ordered.resize(count);
		for (std::uint32_t i = 0; i < count; ++i) ordered[i] = triangles[order[i]];
	}

	RaySceneData RayScene::Data() const {
		RaySceneData data;
		data.nodes = nodes.data();
		data.nodeCount = static_cast<std::uint32_t>(nodes.size());
		data.triangles = ordered.data();
		data.triangleCount = static_cast<std::uint32_t>(ordered.size());
		return data;
	}
}
