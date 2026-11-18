#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// The 3D camera's focal length is a fixed 320 pixels and its distance is divided by the screen width, so wider
	// screens put the camera closer with a much wider angle (a fish-eye look). This scales the focal length with
	// the screen height and puts the distance back to the original 640 wide value, which gives the original
	// 640x480 framing plus extra on the sides of a wide screen.
	class WidescreenCamera {
		public:
			// fieldOfViewPercent: 100 matches the original 640x480 view; higher shows more.
			static void Install(std::uint32_t fieldOfViewPercent);
			// Called after the game sets a screen mode.
			static void OnScreenMode(std::int32_t height);
	};
}
