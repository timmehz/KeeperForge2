#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace KeeperForge2 {
	// Bans from KeeperForge2's Steam lobbies, by Steam account: for good (ban) or until a time on Steam's server clock
	// (tempban), lifted with unban. Who may ban whom is PlayerRanks::CanModerate: only players of a lower rank, never
	// themselves (Admins: None and Friends; the Creator: Admins too; nobody: the Creator).
	// Every machine keeps the bans it took (KeeperForge2\bans.dat, sealed like ranks.dat), passes them on in every lobby it
	// is in (the newest change of a player's ban wins, as with ranks), and leaves banned players out of its lobby
	// (SteamMultiplayer's member filter): no packets either way, their messages ignored. A banned player's own machine
	// leaves a lobby when told it is banned there. See docs/api/console-commands.md.
	class Bans {
		public:
			// Loads bans.dat and registers for the messages over Steam. Before SteamMultiplayer::Install.
			static void Install();
			// Whether the player is banned now. While Steam's clock cannot be read, a tempban still counts.
			static bool IsBanned(std::uint64_t steamId);
			// "3D 3h 20m" and the like: s seconds, m minutes, h hours, D days, M months (30 days), Y years (365 days).
			static std::optional<std::int64_t> ParseDuration(std::string_view text);
			// The console's commands; each returns the text to print.
			static std::string BanCommand(std::string_view steamId);
			// The line after "tempban", as typed: "<time parts...> <SteamID64>".
			static std::string TempBanCommand(std::string_view arguments);
			static std::string UnbanCommand(std::string_view steamId);
			static std::string BansCommand();
	};
}
