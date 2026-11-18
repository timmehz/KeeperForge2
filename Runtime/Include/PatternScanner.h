#pragma once
#include <cstdint>
#include <string>
namespace KeeperForge2 {
	class PatternScanner {
	public:
		// The first place in the game's exe matching a pattern such as "8B 44 ?? 04" (hex bytes, ? or ?? for any byte).
		// 0 if there is none or the pattern cannot be read.
		static uintptr_t Find(const std::string& pattern);
		// The same, but 0 also when the pattern matches more than one place.
		static uintptr_t FindUnique(const std::string& pattern);
	};
}
