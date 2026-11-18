#pragma once
namespace KeeperForge2 {
	// KeeperForge2's creator's name is drawn in changing rainbow colors in multiplayer (the lobby's player list, the in-game
	// player list and the creator's chat lines), so players know who made it; admins are drawn in dark red (#8B0000)
	// and the creator's friends in gold (the Steam account lists are in PlayerRanks.cpp). All are known by their Steam
	// account, which Steam vouches for in the Steam lobby (SteamMultiplayer): only a player whose account is in the lobby
	// and whose game name is that account's Steam name gets the color; copying the name does nothing. When one of them
	// joins or leaves the multiplayer lobby, a "SYSTEM:" line is added to the lobby chat on each machine (locally,
	// nothing is sent). The game is not changed.
	class CreatorName {
		public:
			static void Install();
	};
}
