#include "StringUtils.h"
#include <cctype>
#include <format>
#include <ranges>
namespace KeeperForge2 {
	std::string StringUtils::ToHex(uintptr_t value) { return std::format("0x{:X}", value); }
	std::string StringUtils::ToLower(std::string_view text) {
		return text | std::views::transform([](unsigned char c) { return static_cast<char>(std::tolower(c)); }) | std::ranges::to<std::string>();
	}
}
