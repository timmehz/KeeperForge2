#include "PatternScanner.h"
#include "Memory.h"
#include <algorithm>
#include <charconv>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		// One byte of a pattern; empty matches any byte.
		using PatternByte = std::optional<std::uint8_t>;
		// Empty if the text is not a valid pattern.
		std::vector<PatternByte> ParsePattern(const std::string& pattern) {
			std::vector<PatternByte> bytes;
			std::istringstream stream(pattern);
			std::string token;
			while (stream >> token) {
				if (token == "?" || token == "??") {
					bytes.emplace_back();
					continue;
				}
				std::uint8_t value = 0;
				const char* end = token.data() + token.size();
				const auto parsed = std::from_chars(token.data(), end, value, 16);
				if (parsed.ec != std::errc{} || parsed.ptr != end) { return {}; }
				bytes.emplace_back(value);
			}
			return bytes;
		}
	}
	uintptr_t PatternScanner::Find(const std::string& pattern) {
		const auto bytes = ParsePattern(pattern);
		if (bytes.empty()) return 0;
		const std::span memory(reinterpret_cast<const std::uint8_t*>(Memory::GetModuleBase()), Memory::GetModuleSize());
		const auto found = std::ranges::search(memory, bytes, [](std::uint8_t byte, const PatternByte& wanted) { return !wanted || byte == *wanted; });
		return found.empty() ? 0 : reinterpret_cast<uintptr_t>(found.data());
	}
	uintptr_t PatternScanner::FindUnique(const std::string& pattern) {
		const auto bytes = ParsePattern(pattern);
		if (bytes.empty()) return 0;
		const std::span memory(reinterpret_cast<const std::uint8_t*>(Memory::GetModuleBase()), Memory::GetModuleSize());
		const auto matches = [](std::uint8_t byte, const PatternByte& wanted) { return !wanted || byte == *wanted; };
		const auto found = std::ranges::search(memory, bytes, matches);
		if (found.empty()) return 0;
		const auto after = memory.subspan(static_cast<std::size_t>(found.data() - memory.data()) + 1);
		if (!std::ranges::search(after, bytes, matches).empty()) return 0;
		return reinterpret_cast<uintptr_t>(found.data());
	}
}
