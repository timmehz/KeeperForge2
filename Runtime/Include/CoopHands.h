#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Two machines playing one keeper in a co-op campaign: each has its own hand within the keeper's one list, only the
	// machine that cast Possess enters the creature, and each sees what the other digs.
	class CoopHands {
		public:
			// False if the game's code is not as expected, nothing changed then.
			static bool Install();
			// A session is starting on this machine.
			static void Reset();
			static void BeforeWorldTick(std::uint8_t* world, const std::uint8_t* actions);
			static void AfterWorldTick(std::uint8_t* world);
	};
}
