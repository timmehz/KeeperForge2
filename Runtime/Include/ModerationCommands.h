#pragma once
#include <string>
#include <string_view>
namespace KeeperForge2 {
	// Moderation commands typed in the console that act on players in our Steam lobby (rank and notify have their own
	// homes: PlayerRanks, Notices). Each goes to the lobby as a KeeperForge2 lobby message; every machine checks for itself,
	// with its own ranks, that the sender may do it (PlayerRanks), so a changed copy of KeeperForge2 fools only itself.
	// See docs/api/console-commands.md.
	class ModerationCommands {
		public:
			// Registers for the messages over Steam. Before SteamMultiplayer::Install.
			static void Install();
			// kick <SteamID64>: the player leaves our Steam lobby, and every other machine shuts them out until they have
			// left it. Returns the text to print.
			static std::string KickCommand(std::string_view steamId);
			// steamid64 <userid>: the SteamID64 of a Steam custom profile URL name (steamcommunity.com/id/<userid>; unique,
			// looked up on Steam's public profile page on a thread of its own, answer printed when it comes), and of
			// every member of our Steam lobby whose Steam name is that text (names are not unique). Admins and the
			// Creator only. `text` is the line after the command, as typed.
			static std::string SteamIdCommand(std::string_view text);
	};
}
