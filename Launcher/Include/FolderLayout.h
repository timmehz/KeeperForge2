#pragma once
#include <string>
#include <vector>
namespace KeeperForge2 {
	// KeeperForge2 keeps its files in the game folder's KeeperForge2 folder (see Paths). Older versions wrote Logs\,
	// SCRSHOTS\ and config.yml straight into the game folder; those are moved in.
	class FolderLayout {
		public:
			// Creates the KeeperForge2 folders and moves the old files in. Runs before the logger starts, so it returns
			// what it did (and what it could not do) as lines to log afterwards.
			static std::vector<std::string> Prepare();
	};
}
