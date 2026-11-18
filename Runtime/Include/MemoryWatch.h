#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Keeps an eye on the game's 32-bit address space and logs it when it changes, so a crash from running out can be
	// told apart from other crashes. When the game was started with the 4 GB flag (memory_4gb in config.yml), it also
	// keeps the memory figures the game reads at 2 GB, as the game cannot handle larger ones.
	class MemoryWatch {
		public:
			struct AddressSpace {
				std::uint32_t limitMb = 0;
				std::uint32_t committedMb = 0;
				std::uint32_t freeMb = 0;
				std::uint32_t largestFreeMb = 0;
			};
			// Uses no heap and no locks of ours, so a crash report can call it.
			static AddressSpace Read();
			static void Install();
	};
}
