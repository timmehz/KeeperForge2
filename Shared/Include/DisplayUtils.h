#pragma once
#include <cstdint>
#include <string>
namespace KeeperForge2 {
	class DisplayUtils {
		public:
			// Parses "WIDTHxHEIGHT" (lowercase x).
			static bool ParseResolution(const std::string& text, std::uint32_t& width, std::uint32_t& height);
			// The current mode of the primary display, in real pixels.
			static bool GetDesktopResolution(std::uint32_t& width, std::uint32_t& height);
	};
}
