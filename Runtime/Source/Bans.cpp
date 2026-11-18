#include "Bans.h"
#include "Logger.h"
#include "Notices.h"
#include "Paths.h"
#include "PlayerRanks.h"
#include "SealedFile.h"
#include "SteamMultiplayer.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		// A player's ban: until when (0 for good, Lifted when unbanned), and when that was decided (for the newest change
		// to win, as with ranks). Times are seconds since 1970 UTC on Steam's server clock.
		constexpr std::int64_t Forever = 0;
		constexpr std::int64_t Lifted = -1;
		struct Ban {
			std::int64_t until = Forever;
			std::int64_t time = 0;
			std::uint64_t by = 0;
		};
		std::mutex bansMutex;
		std::unordered_map<std::uint64_t, Ban> bans;
		std::mutex saveMutex;
		// The standing ban list needs making again (a ban changed here; see Tick).
		std::atomic<bool> standingDirty = true;
		constexpr std::int64_t MaxFutureSeconds = 300;
		constexpr std::int64_t MaxDurationSeconds = 100ll * 365 * 24 * 3600;

		// "kf2ban 1 <target> <until> <time>": until 0 for good, -1 lifted. Who sent it is what Steam says.
		constexpr std::string_view MessageTag = "kf2ban";
		constexpr int MessageVersion = 1;
		// A moderator's standing list of the bans it may set: "kf2banlist 1 <id>:<until>:<time> ..." (Tick).
		constexpr std::string_view ListTag = "kf2banlist";
		constexpr std::size_t MaxListBytes = 3700;

		// Steam's server time when Steam is reachable, else this PC's clock (only to date a change; a tempban's end is
		// only judged by Steam's clock).
		std::int64_t Now() {
			if (const auto steam = SteamMultiplayer::ServerTime()) return *steam;
			return static_cast<std::int64_t>(std::time(nullptr));
		}
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
				if (const auto word = text.substr(0, space); !word.empty()) words.push_back(word);
				if (space == std::string_view::npos) break;
				text.remove_prefix(space + 1);
			}
			return words;
		}
		std::string Who(std::uint64_t id) {
			if (id == SteamMultiplayer::MySteamId()) return "you";
			const auto name = SteamMultiplayer::NameOf(id);
			return name.empty() ? std::to_string(id) : name + " (" + std::to_string(id) + ")";
		}
		// "3D 3h 20m" from seconds (largest units first, at most three).
		std::string DurationText(std::int64_t seconds) {
			constexpr std::pair<std::int64_t, const char*> Units[] = {
				{ 365ll * 86400, "Y" }, { 30ll * 86400, "M" }, { 86400, "D" }, { 3600, "h" }, { 60, "m" }, { 1, "s" },
			};
			std::string text;
			int parts = 0;
			for (const auto& [size, unit] : Units) {
				if (seconds < size || parts == 3) continue;
				text += (text.empty() ? "" : " ") + std::to_string(seconds / size) + unit;
				seconds %= size;
				++parts;
			}
			return text.empty() ? "0s" : text;
		}
		bool Active(const Ban& ban, std::optional<std::int64_t> steamNow) {
			if (ban.until == Lifted) return false;
			if (ban.until == Forever) return true;
			// Without Steam's clock a tempban stays: its time cannot be checked.
			return !steamNow || *steamNow < ban.until;
		}
		std::string Describe(const Ban& ban) {
			if (ban.until == Lifted) return "unbanned";
			if (ban.until == Forever) return "banned for good";
			const auto steamNow = SteamMultiplayer::ServerTime();
			if (!steamNow) return "banned (tempban; time left unknown while Steam cannot be reached)";
			if (*steamNow >= ban.until) return "tempban over";
			return "banned for " + DurationText(ban.until - *steamNow) + " more";
		}
		std::string Message(std::uint64_t target, const Ban& ban) {
			return std::string(MessageTag) + " " + std::to_string(MessageVersion) + " " + std::to_string(target) + " " + std::to_string(ban.until) + " "
				+ std::to_string(ban.time);
		}

		// ---- KeeperForge2\bans.dat: "<SteamID64> <until> <time> <by>" per line, sealed (SealedFile).
		std::filesystem::path BansFile() { return Paths::Root() / "bans.dat"; }
		void Save() {
			std::vector<std::pair<std::uint64_t, Ban>> all;
			{
				std::lock_guard lock(bansMutex);
				all.assign(bans.begin(), bans.end());
			}
			std::ranges::sort(all, [](const auto& a, const auto& b) { return a.first < b.first; });
			std::string text;
			for (const auto& [id, ban] : all) {
				text += std::to_string(id) + ' ' + std::to_string(ban.until) + ' ' + std::to_string(ban.time) + ' ' + std::to_string(ban.by) + '\n';
			}
			std::lock_guard lock(saveMutex);
			SealedFile::Write(BansFile(), "bans", text);
		}
		void Load() {
			bool missing = false;
			const auto text = SealedFile::Read(BansFile(), "bans", missing);
			if (!text) {
				if (!missing) Logger::Warning("Bans: bans.dat could not be opened (changed, damaged, or made by another Windows account), so it is not used;"
					" the bans come back from the Steam lobby's Admins and Creator.");
				return;
			}
			std::size_t loaded = 0;
			std::lock_guard lock(bansMutex);
			std::string_view rest(*text);
			while (!rest.empty()) {
				const auto end = rest.find('\n');
				const auto words = Words(rest.substr(0, end));
				rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
				Ban ban;
				const auto id = words.size() == 4 ? ParseSteamId(words[0]) : std::nullopt;
				if (!id || !ParseNumber(words[1], ban.until) || !ParseNumber(words[2], ban.time) || !ParseNumber(words[3], ban.by) || ban.time <= 0) continue;
				bans[*id] = ban;
				++loaded;
			}
			if (loaded > 0) Logger::Info("Bans: " + std::to_string(loaded) + " saved ban(s) loaded from bans.dat.");
		}

		enum class Applied { Changed, Same, Older };
		// Takes the change when it is newer than what is known for the player (same second: the longer ban). Saved when
		// taken.
		Applied Apply(std::uint64_t target, const Ban& ban) {
			{
				std::lock_guard lock(bansMutex);
				const auto found = bans.find(target);
				if (found != bans.end()) {
					const auto& known = found->second;
					if (known.time == ban.time && known.until == ban.until) return Applied::Same;
					const auto length = [](std::int64_t until) { return until == Forever ? INT64_MAX : until; };
					if (ban.time < known.time || (ban.time == known.time && length(ban.until) < length(known.until))) return Applied::Older;
				}
				else if (ban.until == Lifted) {
					// Lifting a ban nobody here knew of: kept for its time, nothing to show.
					bans[target] = ban;
					return Applied::Same;
				}
				bans[target] = ban;
			}
			standingDirty = true;
			Save();
			return Applied::Changed;
		}

		// What a ban means here, for a ban about someone else: their machine is left out of our lobby (the member
		// filter). This machine's own ban is acted on by Tick.
		void Enforce(std::uint64_t target, const Ban& ban, std::uint64_t by, bool changed) {
			if (!changed) return;
			const bool active = Active(ban, SteamMultiplayer::ServerTime());
			if (target == SteamMultiplayer::MySteamId()) {
				Logger::Print("Bans: you are " + Describe(ban) + " (by " + Who(by) + ").\n");
				return;
			}
			const std::string text = Who(target) + " is " + Describe(ban) + " (by " + Who(by) + ").";
			Logger::Print("Bans: " + text + "\n");
			// On screen only when it is about someone in this lobby; the rest is in the console.
			if (SteamMultiplayer::IsInLobby(target) || !active) Notices::ShowLocal(false, text);
		}

		// This account's own ban: Steam multiplayer is off while it lasts (SteamMultiplayer::SetSelfBlocked).
		std::string OwnBanText() {
			const auto me = SteamMultiplayer::MySteamId();
			Ban ban;
			{
				std::lock_guard lock(bansMutex);
				if (const auto found = bans.find(me); found != bans.end()) ban = found->second;
			}
			return "You are " + Describe(ban) + " from KeeperForge2 lobbies (by " + Who(ban.by) + "). Steam multiplayer is off until the ban ends.";
		}
		ULONGLONG lastBlockedNotice = 0;
		void OnBlockedAttempt() {
			const ULONGLONG now = GetTickCount64();
			if (now - lastBlockedNotice < 10000) return;
			lastBlockedNotice = now;
			const auto text = OwnBanText();
			Logger::Print("Bans: " + text + "\n");
			Notices::ShowLocal(true, text);
		}

		// Every Admin and the Creator keep a standing list of the bans they may set (lifted ones too) on the notice board
		// while online, so a banned player's machine, which has no lobby, still hears of bans and unbans.
		void RefreshStandingPost(std::uint64_t me) {
			if (!PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(me))) {
				SteamMultiplayer::SetStandingPost({});
				return;
			}
			std::vector<std::pair<std::uint64_t, Ban>> known;
			{
				std::lock_guard lock(bansMutex);
				known.assign(bans.begin(), bans.end());
			}
			// Newest first, as many as fit in one message.
			std::ranges::sort(known, [](const auto& a, const auto& b) { return a.second.time > b.second.time; });
			std::string text = std::string(ListTag) + " " + std::to_string(MessageVersion);
			bool any = false;
			for (const auto& [target, ban] : known) {
				if (!PlayerRanks::CanModerate(me, target)) continue;
				const std::string entry = " " + std::to_string(target) + ":" + std::to_string(ban.until) + ":" + std::to_string(ban.time);
				if (text.size() + entry.size() > MaxListBytes) break;
				text += entry;
				any = true;
			}
			SteamMultiplayer::SetStandingPost(any ? text : std::string());
		}
		bool wasBanned = false;
		ULONGLONG lastStanding = 0;
		void Tick() {
			const auto me = SteamMultiplayer::MySteamId();
			if (me == 0) return;
			const bool banned = Bans::IsBanned(me);
			SteamMultiplayer::SetSelfBlocked(banned);
			if (banned != wasBanned) {
				wasBanned = banned;
				if (banned) {
					lastBlockedNotice = GetTickCount64();
					const auto text = OwnBanText();
					Logger::Print("Bans: " + text + "\n");
					Notices::ShowLocal(true, text);
				}
				else {
					Logger::Print("Bans: your ban is over; Steam multiplayer is on again.\n");
					Notices::ShowLocal(false, "Your KeeperForge2 ban is over; you can play online again.");
				}
			}
			const ULONGLONG now = GetTickCount64();
			if (standingDirty.exchange(false) || now - lastStanding >= 5 * 60 * 1000) {
				lastStanding = now;
				RefreshStandingPost(me);
			}
		}

		// One ban change from `sender` (a lobby message, or an entry of a standing list).
		void Take(std::uint64_t sender, std::uint64_t target, Ban ban) {
			ban.by = sender;
			if (ban.time <= 0 || ban.until < Lifted) return;
			if (ban.time > Now() + MaxFutureSeconds) {
				Logger::Info("Bans: a ban from " + std::to_string(sender) + " was refused (dated in the future; is a clock wrong?).");
				return;
			}
			// Every machine checks for itself, with its own ranks, that the sender may do it.
			if (!PlayerRanks::CanModerate(sender, target)) {
				Logger::Info("Bans: a ban change from " + std::to_string(sender) + " was refused (their rank is not above that player's).");
				return;
			}
			const auto applied = Apply(target, ban);
			if (applied == Applied::Older) return;
			Enforce(target, ban, sender, applied == Applied::Changed);
		}

		// A moderator's standing list: "kf2banlist 1 <id>:<until>:<time> ...".
		void OnList(std::uint64_t sender, const std::string& text) {
			const auto words = Words(text);
			int version = 0;
			if (words.size() < 2 || !ParseNumber(words[1], version) || version != MessageVersion) return;
			for (std::size_t i = 2; i < words.size(); ++i) {
				const auto entry = words[i];
				const auto first = entry.find(':');
				const auto second = first == std::string_view::npos ? std::string_view::npos : entry.find(':', first + 1);
				if (second == std::string_view::npos) continue;
				const auto target = ParseSteamId(entry.substr(0, first));
				Ban ban;
				if (!target || !ParseNumber(entry.substr(first + 1, second - first - 1), ban.until) || !ParseNumber(entry.substr(second + 1), ban.time)) continue;
				Take(sender, *target, ban);
			}
		}
		void OnMessage(std::uint64_t sender, const std::string& text) {
			if (text.starts_with(ListTag) && text.size() > ListTag.size() && text[ListTag.size()] == ' ') {
				OnList(sender, text);
				return;
			}
			if (!text.starts_with(MessageTag) || text.size() == MessageTag.size() || text[MessageTag.size()] != ' ') return;
			const auto words = Words(text);
			int version = 0;
			Ban ban;
			if (words.size() != 5 || !ParseNumber(words[1], version) || version != MessageVersion) return;
			const auto target = ParseSteamId(words[2]);
			if (!target || !ParseNumber(words[3], ban.until) || !ParseNumber(words[4], ban.time) || ban.time <= 0 || ban.until < Lifted) return;
			Take(sender, *target, ban);
		}
		// A few seconds after a player joined our lobby (or this machine did): every moderator passes on the bans it may
		// set itself, so newcomers know them and a banned newcomer's machine hears it is banned.
		void OnMemberJoined(std::uint64_t) {
			const auto me = SteamMultiplayer::MySteamId();
			if (!PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(me))) return;
			std::vector<std::pair<std::uint64_t, Ban>> known;
			{
				std::lock_guard lock(bansMutex);
				known.assign(bans.begin(), bans.end());
			}
			for (const auto& [target, ban] : known) {
				if (PlayerRanks::CanModerate(me, target)) SteamMultiplayer::SendLobbyMessage(Message(target, ban));
			}
		}

		// Sends a ban change to this lobby at once and to every KeeperForge2 player through the notice board, and takes it
		// here.
		std::string Issue(std::string_view command, std::string_view steamIdText, std::int64_t until) {
			const auto target = ParseSteamId(steamIdText);
			if (!target) return std::string(command) + ": '" + std::string(steamIdText) + "' is not a Steam account id (the 17-digit SteamID64, such as 76561198000000000).\n";
			const auto me = SteamMultiplayer::MySteamId();
			if (me == 0) return std::string(command) + ": Steam multiplayer is not running.\n";
			if (!PlayerRanks::CanModerate(me, *target)) {
				return std::string(command) + ": not allowed: you can only " + (until == Lifted ? "unban" : "ban")
					+ " players of a lower rank than yours (Admins: None and Friends; the Creator: Admins too), never yourself or the Creator.\n";
			}
			Ban ban{ until, Now(), me };
			{
				std::lock_guard lock(bansMutex);
				if (const auto found = bans.find(*target); found != bans.end() && found->second.time >= ban.time) ban.time = found->second.time + 1;
			}
			const auto message = Message(*target, ban);
			const bool lobby = SteamMultiplayer::SendLobbyMessage(message);
			const bool everyone = SteamMultiplayer::PostToEveryone(message);
			Apply(*target, ban);
			const std::string text = Who(*target) + " is " + Describe(ban) + ".";
			Logger::Info("Bans: " + text);
			if (SteamMultiplayer::IsInLobby(*target)) Notices::ShowLocal(false, Who(*target) + " is " + Describe(ban) + " (by you).");
			return std::string(command) + ": " + text + (lobby || everyone ? " Sent to this lobby and every KeeperForge2 player on Steam." : " Saved here; no Steam lobby to send it to yet.") + "\n";
		}
	}

	void Bans::Install() {
		Load();
		SteamMultiplayer::AddLobbyMessageHandler(&OnMessage);
		SteamMultiplayer::AddMemberJoinedHandler(&OnMemberJoined);
		SteamMultiplayer::SetMemberFilter([](std::uint64_t id) { return !Bans::IsBanned(id); });
		SteamMultiplayer::AddTickHandler(&Tick);
		SteamMultiplayer::SetBlockedAttemptHandler(&OnBlockedAttempt);
	}

	bool Bans::IsBanned(std::uint64_t steamId) {
		// The Creator's account is never banned, whatever a bans.dat or a message says.
		if (PlayerRanks::IsCreator(steamId)) return false;
		std::unique_lock lock(bansMutex);
		if (bans.empty()) return false;
		const auto found = bans.find(steamId);
		if (found == bans.end()) return false;
		const Ban ban = found->second;
		lock.unlock();
		return Active(ban, SteamMultiplayer::ServerTime());
	}

	std::optional<std::int64_t> Bans::ParseDuration(std::string_view text) {
		std::int64_t total = 0;
		const auto parts = Words(text);
		if (parts.empty()) return std::nullopt;
		for (const auto part : parts) {
			if (part.size() < 2) return std::nullopt;
			std::int64_t count = 0;
			if (!ParseNumber(part.substr(0, part.size() - 1), count) || count <= 0) return std::nullopt;
			std::int64_t unit = 0;
			switch (part.back()) {
				case 's': unit = 1; break;
				case 'm': unit = 60; break;
				case 'h': unit = 3600; break;
				case 'D': unit = 86400; break;
				case 'M': unit = 30ll * 86400; break;
				case 'Y': unit = 365ll * 86400; break;
				default: return std::nullopt;
			}
			if (count > MaxDurationSeconds / unit) return std::nullopt;
			total += count * unit;
			if (total > MaxDurationSeconds) return std::nullopt;
		}
		return total;
	}

	std::string Bans::BanCommand(std::string_view steamId) { return Issue("ban", steamId, Forever); }

	std::string Bans::TempBanCommand(std::string_view arguments) {
		const auto words = Words(arguments);
		if (words.size() < 2) return "Use: tempban <time> <SteamID64>, the time in parts such as 3D 3h 20m (s, m, h, D, M, Y).\n";
		const auto idText = words.back();
		const auto timeText = arguments.substr(0, static_cast<std::size_t>(idText.data() - arguments.data()));
		const auto seconds = ParseDuration(timeText);
		if (!seconds) {
			return "tempban: '" + std::string(timeText) + "' is not a time. Use parts such as 3D 3h 20m: s seconds, m minutes, h hours, D days, M months, Y years"
				" (at most 100 years).\n";
		}
		// A tempban ends at a moment on Steam's clock, the same for everyone; without it there is nothing to count from.
		const auto steamNow = SteamMultiplayer::ServerTime();
		if (!steamNow) return "tempban: Steam's clock cannot be read right now (is Steam online?); try again in a moment.\n";
		return Issue("tempban", idText, *steamNow + *seconds);
	}

	std::string Bans::UnbanCommand(std::string_view steamId) { return Issue("unban", steamId, Lifted); }

	std::string Bans::BansCommand() {
		// Who is banned is for Admins and the Creator only (by this machine's own ranks).
		if (!PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(SteamMultiplayer::MySteamId()))) {
			return "bans: not allowed: only Admins and the Creator can see who is banned.\n";
		}
		std::vector<std::pair<std::uint64_t, Ban>> all;
		{
			std::lock_guard lock(bansMutex);
			all.assign(bans.begin(), bans.end());
		}
		const auto steamNow = SteamMultiplayer::ServerTime();
		std::erase_if(all, [&](const auto& entry) { return !Active(entry.second, steamNow); });
		if (all.empty()) return "No one is banned.\n";
		std::ranges::sort(all, [](const auto& a, const auto& b) { return a.first < b.first; });
		std::string text = "Banned:\n";
		for (const auto& [id, ban] : all) text += "  " + Who(id) + "  " + Describe(ban) + "  (by " + Who(ban.by) + ")\n";
		return text;
	}
}
