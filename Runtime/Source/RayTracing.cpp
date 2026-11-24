#include "RayTracing.h"
#include "GameAddresses.h"
#include "Memory.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace KeeperForge2 {
	namespace {
		constexpr float FixedPerTile = 4096.0f;
		// A list longer than this is taken to be broken.
		constexpr std::uint32_t MostLights = 8192;

		template<typename T> T Load(const std::uint8_t* address) {
			T value{};
			std::memcpy(&value, address, sizeof(T));
			return value;
		}
		bool Finite(float value) { return std::isfinite(value); }

		// The local player's bridge, checked by its vtable; nullptr outside a level.
		const std::uint8_t* Bridge() {
			const auto* world = Memory::Read<const std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (world == nullptr) return nullptr;
			const auto* session = Load<const std::uint8_t*>(world + Game::Offset::WorldSession);
			if (session == nullptr) return nullptr;
			const auto* bridge = Load<const std::uint8_t*>(session + Game::Offset::SessionBridge);
			if (bridge == nullptr || Load<std::uintptr_t>(bridge) != Game::Resolve(Game::Address::BridgeVtable)) return nullptr;
			return bridge;
		}
	}

	bool RayTracing::Camera(RayCamera& camera) {
		std::memcpy(camera.position, reinterpret_cast<const void*>(Game::Resolve(Game::Address::EngineCameraPosition)), sizeof(camera.position));
		std::memcpy(camera.rotation, reinterpret_cast<const void*>(Game::Resolve(Game::Address::EngineCameraRotation)), sizeof(camera.rotation));
		camera.focal = Memory::Read<float>(Game::Resolve(Game::Address::EngineCameraFocal));
		camera.centerX = Memory::Read<float>(Game::Resolve(Game::Address::EngineCameraCenterX));
		camera.centerY = Memory::Read<float>(Game::Resolve(Game::Address::EngineCameraCenterY));
		if (!(camera.focal > 1.0f) || !Finite(camera.focal) || !Finite(camera.centerX) || !Finite(camera.centerY)) return false;
		for (float value : camera.position) if (!Finite(value)) return false;
		float length = 0.0f;
		for (float value : camera.rotation) {
			if (!Finite(value)) return false;
			length += value * value;
		}
		// An orthonormal rotation's nine numbers squared add up to 3.
		return length > 2.5f && length < 3.5f;
	}

	std::uint32_t RayTracing::Lights(const RayCamera& camera, float frameWidth, float frameHeight, RayLightInfo* lights, std::uint32_t maxLights) {
		const auto* bridge = Bridge();
		if (bridge == nullptr || maxLights == 0) return 0;
		std::vector<RayLightInfo> found;
		const auto* light = Load<const std::uint8_t*>(bridge + Game::Offset::BridgeLights);
		for (std::uint32_t walked = 0; light != nullptr && walked < MostLights; ++walked, light = Load<const std::uint8_t*>(light + Game::Offset::LightNext)) {
			const float radius = static_cast<float>(Load<std::int32_t>(light + Game::Offset::LightRadius)) / FixedPerTile;
			const auto strength = Load<std::uint8_t>(light + Game::Offset::LightStrength);
			if (!(radius > 0.0f) || strength == 0) continue;
			// World to camera: the rotation's transpose (it turns view into world) applied to the offset from the camera.
			float offset[3];
			for (int axis = 0; axis < 3; ++axis) {
				offset[axis] = static_cast<float>(Load<std::int32_t>(light + Game::Offset::BridgeThingPosition + axis * 4)) / FixedPerTile - camera.position[axis];
			}
			RayLightInfo info{};
			for (int axis = 0; axis < 3; ++axis) {
				info.position[axis] = camera.rotation[axis] * offset[0] + camera.rotation[3 + axis] * offset[1] + camera.rotation[6 + axis] * offset[2];
			}
			// Only lights that can reach what the camera sees: in front of it, and near the picture's edges at most.
			const float z = info.position[2];
			if (z < -radius) continue;
			if (z > 0.05f) {
				const float x = camera.centerX + camera.focal * info.position[0] / z;
				const float y = camera.centerY + camera.focal * info.position[1] / z;
				const float margin = camera.focal * radius / z;
				if (x < -margin || x > frameWidth + margin || y < -margin || y > frameHeight + margin) continue;
			}
			info.radius = radius;
			for (int channel = 0; channel < 3; ++channel) {
				const auto value = Load<std::int16_t>(light + Game::Offset::LightColor + channel * 2);
				info.color[channel] = std::clamp(static_cast<float>(value) / 255.0f, 0.0f, 1.0f);
			}
			info.strength = static_cast<float>(strength) / 128.0f;
			found.push_back(info);
		}
		// The nearest ones when there are too many.
		if (found.size() > maxLights) {
			std::partial_sort(found.begin(), found.begin() + maxLights, found.end(), [](const RayLightInfo& a, const RayLightInfo& b) {
				return a.position[2] < b.position[2];
			});
			found.resize(maxLights);
		}
		std::copy(found.begin(), found.end(), lights);
		return static_cast<std::uint32_t>(found.size());
	}
}
