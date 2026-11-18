#include "FolderLayout.h"
#include "Paths.h"
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;
namespace KeeperForge2 {
	namespace {
		// Moves one file, unless the new place already holds one of that name. Returns false if it was left behind.
		bool MoveOneFile(const fs::path& from, const fs::path& to) {
			std::error_code error;
			if (fs::exists(to, error)) { return false; }
			fs::create_directories(to.parent_path(), error);
			fs::rename(from, to, error);
			return !error;
		}
		// Moves every file of an old folder into the new one, then removes the old folder if it ended up empty.
		void MoveFolder(const fs::path& from, const fs::path& to, std::vector<std::string>& report) {
			std::error_code error;
			if (!fs::is_directory(from, error)) { return; }
			std::size_t moved = 0;
			std::size_t left = 0;
			for (const auto& entry : fs::directory_iterator(from, error)) {
				if (!entry.is_regular_file(error)) {
					++left;
					continue;
				}
				if (MoveOneFile(entry.path(), to / entry.path().filename())) { ++moved; }
				else { ++left; }
			}
			if (left == 0) { fs::remove(from, error); }
			if (moved > 0) { report.push_back("Moved " + std::to_string(moved) + " file(s) from " + from.string() + " to " + to.string() + "."); }
			if (left > 0) { report.push_back("Left " + std::to_string(left) + " item(s) in " + from.string() + " (already in " + to.string() + ", a folder, or could not be moved)."); }
		}
	}

	std::vector<std::string> FolderLayout::Prepare() {
		std::vector<std::string> report;
		if (!Paths::CreateFolders()) { report.push_back("Could not create the " + Paths::Root().string() + " folders."); }
		MoveFolder("Logs", Paths::Logs(), report);
		MoveFolder("SCRSHOTS", Paths::Screenshots(), report);
		std::error_code error;
		if (fs::is_regular_file("config.yml", error)) {
			if (MoveOneFile("config.yml", Paths::Config())) { report.push_back("Moved config.yml to " + Paths::Config().string() + "."); }
			else { report.push_back("Left the old config.yml in the game folder: " + Paths::Config().string() + " is used."); }
		}
		return report;
	}
}
