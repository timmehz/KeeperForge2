#pragma once
#include <string>
#include <vector>
namespace KeeperForge2 {
	class Config {
		public:
			// Reads KeeperForge2\config.yml (in the working directory, the game folder), creating it with defaults if
			// it does not exist and bringing its list of settings up to date (see ConfigFile::Load): unknown keys are
			// removed and values a setting cannot take go back to its default.
			static bool Load();
			// One line for each key the last Load removed and each value it set back to the default (for the log).
			static std::vector<std::string> LoadChanges();
			static std::string GetString(const std::string& key, const std::string& defaultValue);
			// Changes a value and saves it to config.yml straight away (the rest of the file is kept as it is).
			static bool SetString(const std::string& key, const std::string& value);
	};
}
