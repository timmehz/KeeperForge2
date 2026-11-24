#pragma once
#include <filesystem>
namespace KeeperForge2::Paths {
	// Everything KeeperForge2 writes lives in one folder inside the game folder (the working directory):
	//   KeeperForge2\config.yml, KeeperForge2\Logs\, KeeperForge2\SCRSHOTS\ and KeeperForge2\mods\.
	std::filesystem::path Root();
	std::filesystem::path Config();
	std::filesystem::path Logs();
	std::filesystem::path Screenshots();
	std::filesystem::path Mods();
	// Levels saved from the editor. Made when the first one is saved.
	std::filesystem::path Maps();
	// Custom campaigns, one folder each (docs/game/custom-campaign.md). Mods install theirs here too.
	std::filesystem::path Campaigns();
	// Creates the folder and its Logs and mods folders. Returns false if one could not be made.
	bool CreateFolders();
}
