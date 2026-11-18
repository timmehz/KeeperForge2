#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
namespace KeeperForge2 {
	// Checks the game folder's files before the game starts: each must be there under its name, and (for the game's
	// own files) have the size and SHA-256 checksum of a known original copy.
	class GameFiles {
		public:
			// Logs every problem found, then stops the launcher (Logger::Error) if a file is missing or not original.
			static void Verify(const std::filesystem::path& gameDirectory);
			// The file's SHA-256 checksum as 64 lowercase hex digits, or nothing if it could not be read.
			static std::optional<std::string> Sha256(const std::filesystem::path& file);
	};
}
