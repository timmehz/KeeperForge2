#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace KeeperForge2 {
	// Who a player is to KeeperForge2, by Steam account (64-bit id): None -> Friend -> Admin -> Creator. The built-in lists
	// of accounts are in PlayerRanks.cpp; the rank command changes them for everyone in the Steam lobby, and every machine
	// saves the changes it took in KeeperForge2\ranks.dat, sealed with Windows' data protection (the newest change of a player's rank wins). A rank only means
	// something for an account Steam vouches for (a member of the Steam lobby, see SteamMultiplayer): a game name alone
	// proves nothing. See docs/api/console-commands.md.
	// Moderation: a player may act only on players of a lower rank, never on themselves. Admins act on None and
	// Friends; the Creator on None, Friends and Admins. Only the Creator's own account can change the Creator's rank,
	// and it keeps the Creator's rights whatever rank it shows, so it can always change it back.
	class PlayerRanks {
		public:
			enum class Rank : std::uint8_t { None = 0, Friend = 1, Admin = 2, Creator = 3 };
			// Registers for the rank messages over Steam. Before SteamMultiplayer::Install.
			static void Install();
			// The rank a player has now (shown in colors): a rank set with the rank command, else the built-in lists.
			// An account in more than one built-in list gets the highest rank.
			static Rank RankOf(std::uint64_t steamId);
			// Whether this is the Creator's own account (whatever rank it shows now).
			static bool IsCreator(std::uint64_t steamId);
			// What a player may do: their rank, except that the Creator's account always has the Creator's rights.
			static Rank AuthorityOf(std::uint64_t steamId);
			// Whether a player of rank `actor` may use moderation commands on a player of rank `target`.
			static bool CanModerate(Rank actor, Rank target);
			static bool CanModerate(std::uint64_t actorSteamId, std::uint64_t targetSteamId);
			// Whether the rank may use moderation commands at all (Admin and Creator).
			static bool IsModerator(Rank rank);
			// Whether `actor` may give `target` the rank; when not, why (if asked).
			static bool CanSetRank(std::uint64_t actorSteamId, std::uint64_t targetSteamId, Rank rank, std::string* why = nullptr);
			// Whether `actor` may kick `target` from the Steam lobby; when not, why (if asked). Admins: None, Friends and
			// themselves. The Creator: everyone below, and themselves. Nobody else kicks the Creator.
			static bool CanKick(std::uint64_t actorSteamId, std::uint64_t targetSteamId, std::string* why = nullptr);
			static const char* Name(Rank rank);
			// "none", "friend", "admin", "creator" (any case; also "friends", "admins").
			static std::optional<Rank> ParseRank(std::string_view text);
			// The console's commands; each returns the text to print.
			static std::string RankCommand(std::string_view steamId, std::string_view rankName);
			static std::string RanksCommand();
	};
}
