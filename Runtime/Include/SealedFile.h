#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
namespace KeeperForge2::SealedFile {
	// Text files sealed with Windows' data protection (CryptProtectData, DPAPI): only this Windows account on this PC can
	// open them, and a file changed by hand no longer opens. `purpose` is mixed into the seal, so one file cannot be
	// passed off as another. Written through a temporary file, then renamed over the old one. Returns false (and logs) on
	// failure.
	bool Write(const std::filesystem::path& path, std::string_view purpose, const std::string& text);
	// The text; nullopt when there is no file (`missing` true) or it does not open (`missing` false: changed, damaged,
	// or made by another Windows account or PC).
	std::optional<std::string> Read(const std::filesystem::path& path, std::string_view purpose, bool& missing);
}
