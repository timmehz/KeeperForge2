#include "PlayerRanks.h"
#include "Logger.h"
#include "Paths.h"
#include "SealedFile.h"
#include "StringUtils.h"
#include "SteamMultiplayer.h"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace KeeperForge2 {
	namespace {
		using Rank = PlayerRanks::Rank;
		// KeeperForge2's creator.
		constexpr std::uint64_t CreatorSteamId = 76561198215298520ull;
		// Admins: may use moderation commands on None and Friends. Add one per line, such as
		// 76561198000000000ull,
		const std::vector<std::uint64_t> AdminSteamIds = {};
		// The creator's friends. Add one per line, as above.
		const std::vector<std::uint64_t> FriendSteamIds = { 76561199201879978ull };

		// Ranks set with the rank command, with when the change was made (seconds since 1970, UTC, by the clock of the
		// machine that made it): the newest change of a player's rank wins, wherever it comes from, so a machine that
		// missed changes cannot bring an old rank back. A change back to the built-in rank is kept too, for its time.
		// Saved in KeeperForge2\ranks.dat. Read on the game's threads (colors), changed on the Steam thread and the
		// console's.
		struct SetRank {
			Rank rank = Rank::None;
			std::int64_t time = 0;
		};
		std::mutex ranksMutex;
		std::unordered_map<std::uint64_t, SetRank> setRanks;
		std::mutex saveMutex;
		// A change dated further ahead than this (a machine's clock far off, or made up) is refused.
		constexpr std::int64_t MaxFutureSeconds = 300;

		// The rank message between machines: "kf2rank 2 <target Steam id> <rank number> <time>". Who sent it is what
		// Steam says, never part of the message.
		constexpr std::string_view MessageTag = "kf2rank";
		constexpr int MessageVersion = 2;

		// Steam's server time when Steam is reachable (the same for everyone), else this PC's clock.
		std::int64_t Now() {
			if (const auto steam = SteamMultiplayer::ServerTime()) return *steam;
			return static_cast<std::int64_t>(std::time(nullptr));
		}
		Rank BuiltInRank(std::uint64_t steamId) {
			if (steamId == 0) return Rank::None;
			if (steamId == CreatorSteamId) return Rank::Creator;
			if (std::ranges::find(AdminSteamIds, steamId) != AdminSteamIds.end()) return Rank::Admin;
			if (std::ranges::find(FriendSteamIds, steamId) != FriendSteamIds.end()) return Rank::Friend;
			return Rank::None;
		}
		// A person's own Steam account: public universe (1), individual account (1).
		bool IsPersonalSteamId(std::uint64_t id) { return (id >> 56) == 1 && ((id >> 52) & 0xF) == 1 && (id & 0xFFFFFFFFull) != 0; }
		template <typename T> bool ParseNumber(std::string_view text, T& value) {
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
		}
		std::optional<std::uint64_t> ParseSteamId(std::string_view text) {
			std::uint64_t id = 0;
			if (!ParseNumber(text, id) || !IsPersonalSteamId(id)) return std::nullopt;
			return id;
		}
		std::vector<std::string_view> Words(std::string_view text) {
			std::vector<std::string_view> words;
			while (!text.empty()) {
				const auto space = text.find_first_of(" \t\r\n");
				const auto word = text.substr(0, space);
				if (!word.empty()) words.push_back(word);
				if (space == std::string_view::npos) break;
				text.remove_prefix(space + 1);
			}
			return words;
		}
		std::string RankMessage(std::uint64_t target, const SetRank& set) {
			return std::string(MessageTag) + " " + std::to_string(MessageVersion) + " " + std::to_string(target) + " "
				+ std::to_string(static_cast<int>(set.rank)) + " " + std::to_string(set.time);
		}
		std::string Who(std::uint64_t id) {
			if (id == SteamMultiplayer::MySteamId()) return std::to_string(id) + " (you)";
			return std::to_string(id);
		}

		// ---- KeeperForge2\ranks.dat: the text "<SteamID64> <rank> <time>" per line, sealed with Windows' data protection
		// (CryptProtectData, DPAPI): only this Windows account on this PC can open it, and a file changed by hand no
		// longer opens, so it is thrown away (the ranks come back from the Steam lobby). This only keeps the file from
		// being read or edited by hand: a player who changes their own copy of KeeperForge2 can still fool their own
		// machine, which is why every machine checks each rank change for itself and trusts no other machine's file.
		std::filesystem::path RanksFile() { return Paths::Root() / "ranks.dat"; }
		void Save() {
			std::vector<std::pair<std::uint64_t, SetRank>> all;
			{
				std::lock_guard lock(ranksMutex);
				all.assign(setRanks.begin(), setRanks.end());
			}
			std::ranges::sort(all, [](const auto& a, const auto& b) { return a.first < b.first; });
			std::string text;
			for (const auto& [id, set] : all) {
				text += std::to_string(id) + ' ' + StringUtils::ToLower(PlayerRanks::Name(set.rank)) + ' ' + std::to_string(set.time) + '\n';
			}
			std::lock_guard lock(saveMutex);
			SealedFile::Write(RanksFile(), "ranks", text);
		}
		void Load() {
			bool missing = false;
			const auto text = SealedFile::Read(RanksFile(), "ranks", missing);
			if (!text) {
				// Changed by hand, damaged, or from another Windows account or PC.
				if (!missing) Logger::Warning("Ranks: ranks.dat could not be opened (changed, damaged, or made by another Windows account), so it is not used;"
					" the ranks come back from the Steam lobby's Admins and Creator.");
				return;
			}
			std::size_t loaded = 0, skipped = 0;
			std::lock_guard lock(ranksMutex);
			std::string_view rest(*text);
			while (!rest.empty()) {
				const auto end = rest.find('\n');
				const auto words = Words(rest.substr(0, end));
				rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
				if (words.empty()) continue;
				const auto id = words.size() == 3 ? ParseSteamId(words[0]) : std::nullopt;
				const auto rank = words.size() == 3 ? PlayerRanks::ParseRank(words[1]) : std::nullopt;
				std::int64_t time = 0;
				if (!id || !rank || !ParseNumber(words[2], time) || time <= 0) {
					++skipped;
					continue;
				}
				setRanks[*id] = { *rank, time };
				++loaded;
			}
			if (loaded > 0) Logger::Info("Ranks: " + std::to_string(loaded) + " saved rank(s) loaded from ranks.dat.");
			if (skipped > 0) Logger::Warning("Ranks: " + std::to_string(skipped) + " saved rank(s) could not be read and were skipped.");
		}

		enum class Applied { Changed, Same, Older };
		// Takes the change when it is newer than what is known for the player (same second: the higher rank). Saved
		// when it is taken.
		Applied Apply(std::uint64_t target, SetRank set) {
			Applied result = Applied::Changed;
			{
				std::lock_guard lock(ranksMutex);
				const auto found = setRanks.find(target);
				if (found != setRanks.end()) {
					const auto& known = found->second;
					if (known.time == set.time && known.rank == set.rank) return Applied::Same;
					if (set.time < known.time || (set.time == known.time && set.rank < known.rank)) return Applied::Older;
					if (known.rank == set.rank) result = Applied::Same;
				}
				else if (set.rank == BuiltInRank(target)) result = Applied::Same;
				setRanks[target] = set;
			}
			Save();
			return result;
		}

		// A rank message from a member of our Steam lobby (this machine's own come back too).
		void OnLobbyMessage(std::uint64_t sender, const std::string& text) {
			if (!text.starts_with(MessageTag) || text.size() == MessageTag.size() || text[MessageTag.size()] != ' ') return;
			const auto words = Words(text);
			int version = 0, number = -1;
			std::uint64_t target = 0;
			SetRank set;
			if (words.size() != 5 || !ParseNumber(words[1], version) || version != MessageVersion) return;
			if (!ParseNumber(words[2], target) || !ParseNumber(words[3], number) || !ParseNumber(words[4], set.time)) return;
			if (!IsPersonalSteamId(target) || number < 0 || number > static_cast<int>(Rank::Creator) || set.time <= 0) return;
			set.rank = static_cast<Rank>(number);
			if (set.time > Now() + MaxFutureSeconds) {
				Logger::Info("Ranks: a rank change from " + Who(sender) + " was refused (dated in the future; is a clock wrong?).");
				return;
			}
			// Every machine checks for itself, with its own ranks, that the sender may do it.
			std::string why;
			if (!PlayerRanks::CanSetRank(sender, target, set.rank, &why)) {
				Logger::Info("Ranks: a rank change from " + Who(sender) + " was refused (" + why + ").");
				return;
			}
			if (Apply(target, set) == Applied::Changed) {
				Logger::Print("Ranks: " + Who(sender) + " made " + Who(target) + " " + PlayerRanks::Name(set.rank) + ".\n");
			}
		}
		// A few seconds after a player joined our Steam lobby (or this machine did): every moderator sends the ranks it
		// knows and may set itself, with their times, so everyone ends up with the newest of each. (Messages go to the
		// whole lobby; older or known ranks change nothing.)
		void OnMemberJoined(std::uint64_t) {
			const auto me = SteamMultiplayer::MySteamId();
			if (!PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(me))) return;
			std::vector<std::pair<std::uint64_t, SetRank>> known;
			{
				std::lock_guard lock(ranksMutex);
				known.assign(setRanks.begin(), setRanks.end());
			}
			// Admins first, so an admin is known before what they did.
			std::ranges::sort(known, [](const auto& a, const auto& b) { return a.second.rank > b.second.rank; });
			for (const auto& [target, set] : known) {
				if (PlayerRanks::CanSetRank(me, target, set.rank)) SteamMultiplayer::SendLobbyMessage(RankMessage(target, set));
			}
		}
	}

	void PlayerRanks::Install() {
		Load();
		SteamMultiplayer::AddLobbyMessageHandler(&OnLobbyMessage);
		SteamMultiplayer::AddMemberJoinedHandler(&OnMemberJoined);
	}

	PlayerRanks::Rank PlayerRanks::RankOf(std::uint64_t steamId) {
		if (steamId == 0) return Rank::None;
		{
			std::lock_guard lock(ranksMutex);
			if (const auto found = setRanks.find(steamId); found != setRanks.end()) return found->second.rank;
		}
		return BuiltInRank(steamId);
	}
	bool PlayerRanks::IsCreator(std::uint64_t steamId) { return steamId != 0 && steamId == CreatorSteamId; }
	// The Creator's account keeps every right whatever rank it was given (it can always use every command, and set any
	// rank on itself), so it can never lock itself out.
	PlayerRanks::Rank PlayerRanks::AuthorityOf(std::uint64_t steamId) {
		return steamId != 0 && steamId == CreatorSteamId ? Rank::Creator : RankOf(steamId);
	}
	bool PlayerRanks::IsModerator(Rank rank) { return rank >= Rank::Admin; }
	// Only downwards: a moderator never acts on their own rank or above (so Admins not on Admins, nobody on the Creator).
	bool PlayerRanks::CanModerate(Rank actor, Rank target) { return IsModerator(actor) && target < actor; }
	bool PlayerRanks::CanModerate(std::uint64_t actorSteamId, std::uint64_t targetSteamId) {
		// Nobody moderates themselves, and only the Creator's own account touches the Creator.
		if (actorSteamId == 0 || actorSteamId == targetSteamId || targetSteamId == CreatorSteamId) return false;
		return CanModerate(AuthorityOf(actorSteamId), RankOf(targetSteamId));
	}
	bool PlayerRanks::CanSetRank(std::uint64_t actor, std::uint64_t target, Rank rank, std::string* why) {
		const auto refuse = [why](const char* reason) {
			if (why) *why = reason;
			return false;
		};
		if (actor == 0) return refuse("not in a Steam lobby");
		// The Creator's account alone sets its own rank, to anything (it keeps the Creator's rights to change it back).
		if (target == CreatorSteamId) return actor == CreatorSteamId ? true : refuse("only the Creator can change the Creator's rank");
		const Rank authority = AuthorityOf(actor);
		if (!IsModerator(authority)) return refuse("only Admins and the Creator can set ranks");
		if (rank == Rank::Creator) return refuse("there is only one Creator");
		if (actor == target) return refuse("Admins cannot change their own rank");
		if (RankOf(target) >= authority) return refuse(authority == Rank::Admin ? "Admins cannot change other Admins' ranks" : "that player's rank is not below yours");
		if (rank >= authority) return refuse(authority == Rank::Admin ? "Admins can only make players None or Friend" : "that rank is not below yours");
		return true;
	}
	bool PlayerRanks::CanKick(std::uint64_t actor, std::uint64_t target, std::string* why) {
		const auto refuse = [why](const char* reason) {
			if (why) *why = reason;
			return false;
		};
		if (actor == 0) return refuse("not in a Steam lobby");
		const Rank authority = AuthorityOf(actor);
		if (!IsModerator(authority)) return refuse("only Admins and the Creator can kick");
		// Admins and the Creator may kick themselves.
		if (actor == target) return true;
		if (target == CreatorSteamId) return refuse("only the Creator can kick the Creator");
		if (RankOf(target) >= authority) return refuse(authority == Rank::Admin ? "Admins cannot kick other Admins" : "that player's rank is not below yours");
		return true;
	}
	const char* PlayerRanks::Name(Rank rank) {
		switch (rank) {
			case Rank::Friend: return "Friend";
			case Rank::Admin: return "Admin";
			case Rank::Creator: return "Creator";
			default: return "None";
		}
	}
	std::optional<PlayerRanks::Rank> PlayerRanks::ParseRank(std::string_view text) {
		const auto word = StringUtils::ToLower(text);
		if (word == "none") return Rank::None;
		if (word == "friend" || word == "friends") return Rank::Friend;
		if (word == "admin" || word == "admins") return Rank::Admin;
		if (word == "creator") return Rank::Creator;
		return std::nullopt;
	}

	std::string PlayerRanks::RankCommand(std::string_view steamIdText, std::string_view rankName) {
		const auto target = ParseSteamId(steamIdText);
		if (!target) return "rank: '" + std::string(steamIdText) + "' is not a Steam account id (the 17-digit SteamID64, such as 76561198000000000).\n";
		const auto rank = ParseRank(rankName);
		if (!rank) return "rank: '" + std::string(rankName) + "' is not a rank: none, friend, admin or creator.\n";
		const auto me = SteamMultiplayer::MySteamId();
		if (me == 0) return "rank: Steam multiplayer is not running, so ranks cannot be set.\n";
		std::string why;
		if (!CanSetRank(me, *target, *rank, &why)) return "rank: not allowed: " + why + ".\n";
		// Never older than what is known, even with this machine's clock behind.
		SetRank set{ *rank, Now() };
		{
			std::lock_guard lock(ranksMutex);
			if (const auto found = setRanks.find(*target); found != setRanks.end() && found->second.time >= set.time) set.time = found->second.time + 1;
		}
		if (!SteamMultiplayer::SendLobbyMessage(RankMessage(*target, set))) return "rank: there is no Steam lobby to send it to yet; try again in a moment.\n";
		// Every machine in the lobby, this one too, takes it when Steam delivers it; taken (and saved) here at once as well.
		Apply(*target, set);
		Logger::Info("Ranks: made " + Who(*target) + " " + Name(*rank) + " (sent to the Steam lobby, saved).");
		return "rank: " + Who(*target) + " is now " + Name(*rank) + " for everyone in the Steam lobby (saved).\n";
	}

	std::string PlayerRanks::RanksCommand() {
		const auto me = SteamMultiplayer::MySteamId();
		std::string text;
		const auto line = [](std::uint64_t id, const std::string& name, bool you) {
			return "  " + std::to_string(id) + "  " + std::string(Name(RankOf(id))) + "  " + name + (you ? " (you)" : "") + "\n";
		};
		if (me != 0) {
			text += "Ranks in the Steam lobby:\n";
			text += line(me, SteamMultiplayer::MyName(), true);
			for (const auto& member : SteamMultiplayer::OtherMembers()) text += line(member.steamId, member.name, false);
		}
		else text += "Steam multiplayer is not running.\n";
		std::vector<std::pair<std::uint64_t, SetRank>> changed;
		{
			std::lock_guard lock(ranksMutex);
			changed.assign(setRanks.begin(), setRanks.end());
		}
		if (!changed.empty()) {
			std::ranges::sort(changed, [](const auto& a, const auto& b) { return a.first < b.first; });
			text += "Set with the rank command (saved in ranks.dat):\n";
			for (const auto& [id, set] : changed) text += "  " + std::to_string(id) + "  " + Name(set.rank) + "\n";
		}
		return text;
	}
}
