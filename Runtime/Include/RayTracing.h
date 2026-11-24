#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// The game's side of the ray tracer (the renderer's side is Source/Renderer/RayScene.*; docs/game/ray-tracing.md):
	// the engine's camera as it drew the world last, and the lights near what it sees, in the camera's own space (the
	// space the tracer rebuilds the scene in: x right, y down, z into the screen, in tiles).
	struct RayCamera {
		// World position (tiles), the rotation (row major, view to world), the focal length and the projection's middle
		// (frame pixels).
		float position[3];
		float rotation[9];
		float focal;
		float centerX;
		float centerY;
	};
	struct RayLightInfo {
		// In camera space (tiles), the light's reach (tiles), color (0-1) and strength (the game's 128 = 1).
		float position[3];
		float radius;
		float color[3];
		float strength;
	};
	class RayTracing {
		public:
			// The engine's camera; false when it has none that makes sense (no 3D drawn yet).
			static bool Camera(RayCamera& camera);
			// Up to maxLights of the level's lights that can light what the camera sees (in front of it and near the view),
			// nearest first, into lights. 0 outside a level.
			static std::uint32_t Lights(const RayCamera& camera, float frameWidth, float frameHeight, RayLightInfo* lights, std::uint32_t maxLights);
	};
}
