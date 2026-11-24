#pragma once
#include <cstdint>
#include <filesystem>
namespace KeeperForge2 {
	// Custom campaigns: folders in KeeperForge2\campaigns, each with its levels as 1.kwd, 2.kwd, ... (played in that order,
	// their .kld parts next to them) and a campaign.yml giving its name and texts. The Single Player menu's Custom Campaign button (added by LevelEditor's button list) shows them in the
	// skirmish map list; picking one plays its levels one after the other with campaign rules, the next starting as soon
	// as a level is won, and never going past the campaign's own list. Progress is kept in the campaign's progress.yml.
	// Our own design (Flame's button is unfinished); see docs/game/custom-campaign.md.
	class CustomCampaign {
		public:
			static void Install();
			// The Custom Campaign button in the Single Player menu was pressed (before the skirmish screen opens).
			static void OnMenuButton();
			// A level of a custom campaign is being played (its levels keep their own rules, as in the campaign).
			static bool InSession();
			// The game component starts (CoopCampaign's hook): playing a level, or the menus. Returns true when the level
			// is a custom campaign's.
			static bool OnGameComponentStart(bool playing);
			// For a file the game opens: the campaign's own copy while one of its levels is starting or played (a .kwd or
			// .kld under a "maps" folder, by file name), else empty.
			static std::filesystem::path LevelFileFor(const std::filesystem::path& path);
			// The game's text by id (LevelEditor's hook on it asks here first): our own column headings for the campaign
			// list, else nullptr.
			static const char* TextById(std::int32_t id);
			// After the skirmish screen's setup (0x538AC0) has shown it; SkirmishSwapKeeper's hook on it calls this.
			static void AfterSkirmishSetup(std::uint8_t* frontEnd);
	};
}
