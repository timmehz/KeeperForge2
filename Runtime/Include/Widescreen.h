#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Lets the game run at any resolution (including widescreen) instead of only its built-in 4:3 list.
	// Controlled by "resolution" in config.yml: auto (desktop resolution), WIDTHxHEIGHT, or off.
	// Problems are logged as warnings and leave the game at its normal resolution; they never close the game.
	class Widescreen {
		public:
			static void Initialize();
			static bool IsEnabled();
			static std::uint32_t GetWidth();
			static std::uint32_t GetHeight();
	};
}
