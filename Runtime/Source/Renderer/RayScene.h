#pragma once
#include "Backend.h"
#include <cstdint>
#include <vector>

// The scene the ray tracer works on (config.yml ray_tracing, docs/game/ray-tracing.md). The game's triangles reach the
// renderer already projected (x and y in pixels, rhw = 1/w); each one drawn solid (depth tested and written, not
// blended) is put back into camera space with the engine's projection (x = (sx - middle x) * w / focal, the same for y,
// z = w; the engine projects screen = middle + focal * view.xy / view.z, 0x575F10) and
// kept for the frame. Build then makes a bounding volume tree over them (binned surface area heuristic) for the tracer.
namespace KeeperForge2::Render {
	class RayScene {
		public:
			// Starts a new frame's scene for a frame of this size, its projection's focal length and middle in pixels.
			void Begin(std::uint32_t width, std::uint32_t height, float focal, float centerX, float centerY);
			bool Begun() const { return begun; }
			// A triangle list as the backends draw it (indices into vertices), of one material (RayMaterialSolid or Mirror).
			void Add(const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* indices, std::uint32_t indexCount, float material);
			std::uint32_t TriangleCount() const { return static_cast<std::uint32_t>(triangles.size()); }
			// Makes the tree. The scene's data stays valid until the next Begin.
			void Build();
			RaySceneData Data() const;
			// The median length of the triangles' longest edges: the size of the game's tiles and walls in scene units,
			// which sets how far ambient occlusion looks.
			float TypicalEdge() const { return typicalEdge; }
			float Focal() const { return focal; }
			float CenterX() const { return centerX; }
			float CenterY() const { return centerY; }
			std::uint32_t Width() const { return width; }
			std::uint32_t Height() const { return height; }
			// Throws the frame's scene away (nothing traced).
			void Clear();

		private:
			struct Bounds {
				float min[3];
				float max[3];
				void Reset();
				void Grow(const float point[3]);
				void Grow(const Bounds& other);
				float Area() const;
			};
			bool begun = false;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			float focal = 0.0f;
			float centerX = 0.0f;
			float centerY = 0.0f;
			float typicalEdge = 0.0f;
			std::vector<RayTriangle> triangles;
			std::vector<RayTriangle> ordered;
			std::vector<RayNode> nodes;
			std::vector<Bounds> triangleBounds;
			std::vector<float> centers;
			std::vector<std::uint32_t> order;
	};
}
