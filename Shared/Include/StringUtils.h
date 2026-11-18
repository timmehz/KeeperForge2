#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace KeeperForge2 {
	class StringUtils {
		public:
			static std::string ToHex(uintptr_t value);
			static std::string ToLower(std::string_view text);
	};
}
