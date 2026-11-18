#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
namespace KeeperForge2 {
	// One setting a config.yml should hold: its key, the value written when it is missing, and the comment written
	// above it (lines separated by '\n', written as "# line"). valid, if set, is given the value (quotes removed) and
	// says whether it is one the setting can take; a value it turns down is replaced with the default.
	struct ConfigSetting {
		std::string key;
		std::string defaultValue;
		std::string comment;
		std::function<bool(const std::string&)> valid;
	};

	// A setting Load took out of the file, or put back to its default, with the value the file had.
	struct ConfigChange {
		std::string key;
		std::string value;
	};

	// The header at the top of a mod's config.yml:
	//   ### HEADER ###
	//   # author: <authors, separated by ','>
	//   # description: <description, may go on over more "# " lines>
	//   ######
	struct ConfigHeader {
		bool present = false;
		std::vector<std::string> authors;
		std::string description;
	};

	// A config.yml of simple "key: value" lines ('#' starts a comment, values may be quoted).
	class ConfigFile {
		public:
			static constexpr const char* HeaderStart = "### HEADER ###";
			static constexpr const char* HeaderEnd = "######";

			// Reads the file and brings it up to date: settings missing from it are added with their default, keys
			// not listed (and any other lines, such as comments added by hand) are removed, values the setting's valid
			// check turns down are put back to the default, and the other values already there are kept. The file is
			// only rewritten when that changes something. With no settings listed, the values are left as they are.
			// A "### HEADER ###" block at the top is kept as it is; a file without one gets defaultHeader (text written
			// as it is, may be empty). A missing file is created. Returns false if the file could not be read or
			// written.
			bool Load(const std::filesystem::path& path, const std::vector<ConfigSetting>& settings, const std::string& defaultHeader);
			// What the last Load took out of the file (keys not listed) and put back to the default (values turned
			// down), in file order and settings order.
			const std::vector<ConfigChange>& Removed() const { return removed; }
			const std::vector<ConfigChange>& Reset() const { return reset; }
			std::string Get(const std::string& key, const std::string& defaultValue) const;
			bool Has(const std::string& key) const;
			// Changes one value and writes it to the file loaded last, keeping every other line (comments included)
			// as it is. A key the file does not have yet is added at the end. Returns false if the file could not be
			// written.
			bool Set(const std::string& key, const std::string& value);
			const std::unordered_map<std::string, std::string>& Values() const { return values; }
			const ConfigHeader& Header() const { return header; }

			// A header block with the given authors and description (which may hold several lines).
			static std::string MakeHeader(const std::string& authors, const std::string& description);
		private:
			std::filesystem::path path;
			std::unordered_map<std::string, std::string> values;
			ConfigHeader header;
			std::vector<ConfigChange> removed;
			std::vector<ConfigChange> reset;
	};
}
