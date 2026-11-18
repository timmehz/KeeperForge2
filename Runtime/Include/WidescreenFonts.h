#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// The game picks in-game font sizes from a short list of screen widths (400, 512, 800, 1024) and uses the
	// 640x480 sizes for everything else, so text gets tiny at high resolutions. This picks them by screen height
	// instead, from the font sizes the game ships with, including the few fonts loaded by fixed size at startup.
	class WidescreenFonts {
		public:
			static bool Install();
			// Called after the game sets a screen mode.
			static void OnScreenMode(std::int32_t height);
	};
}
