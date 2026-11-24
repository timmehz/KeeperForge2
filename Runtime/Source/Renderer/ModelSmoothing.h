#pragma once
#include "Backend.h"
#include <cstdint>
#include <vector>

// Model smoothing (config.yml model_smoothing, docs/game/renderer.md): the game's triangles, which arrive already on the
// screen, are turned back into the camera's space, each made a curved patch from its corners and their smoothed normals
// (PN triangles), cut into smaller triangles and put back on the screen. Round shapes get rounder and their outlines
// smoother; flat surfaces stay flat. Our own code.
namespace KeeperForge2::Render {
	// The projection the triangles were made with: screen = center + focal * view.xy / view.z, rhw = 1 / view.z.
	struct SmoothingProjection {
		float focal = 0.0f;
		float centerX = 0.0f;
		float centerY = 0.0f;
	};

	// segments: how many pieces a curved edge is cut into (2 to 4). triangles: every triangle of one draw, three indices
	// each, before back faces are culled (so the patches see both sides of an outline). Writes the new vertices and
	// triangles (the same winding as the ones they come from) and returns true, or returns false when nothing in the draw
	// is curved enough to change (or there would be more than 65535 vertices), leaving the output undefined.
	bool SmoothTriangles(const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* triangles, std::uint32_t indexCount,
		const SmoothingProjection& projection, std::uint32_t segments, std::vector<Vertex>& outVertices, std::vector<std::uint16_t>& outIndices);
}
