#include "Notices.h"
#include "Logger.h"
#include "PlayerRanks.h"
#include "StringUtils.h"
#include "SteamMultiplayer.h"
#include <charconv>
#include <cwchar>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		// The message: "kf2notify 1 <id> <system|alert> <text>", the id being "<sender's Steam id>-<time>-<count>"
		// (seconds since 1970 UTC). Who sent it is what Steam says, never part of the message.
		constexpr std::string_view MessageTag = "kf2notify";
		constexpr int MessageVersion = 1;
		// The game's chat lines are short; longer messages are refused.
		constexpr std::size_t MaxMessageCharacters = 100;
		// Older than this (or dated this far ahead) a notice is not shown.
		constexpr std::int64_t MaxAgeSeconds = 10 * 60;
		constexpr std::int64_t MaxFutureSeconds = 5 * 60;
		// Waiting to be shown: dropped when nothing showed them for this long (no lobby or level on screen).
		constexpr ULONGLONG WaitMilliseconds = 5 * 60 * 1000;
		constexpr std::size_t MaxShownKept = 20;

		struct Waiting {
			Notices::Notice notice;
			ULONGLONG until = 0;
		};
		std::mutex noticesMutex;
		std::deque<Waiting> waiting;
		// Shown notices' lines ("<label>: <text>"), for their color while they stay in the chat.
		std::deque<std::pair<std::wstring, bool>> shown;
		// Notice ids already taken (they come through the lobby and the notice board both).
		std::unordered_map<std::string, ULONGLONG> seenIds;
		std::uint32_t counter = 0;

		// Steam's server time when Steam is reachable (the same for everyone), else this PC's clock.
		std::int64_t SteamNow() {
			if (const auto steam = SteamMultiplayer::ServerTime()) return *steam;
			return static_cast<std::int64_t>(std::time(nullptr));
		}
		std::wstring Wide(std::string_view utf8) {
			if (utf8.empty()) return {};
			const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
			std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
			if (length > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
			return wide;
		}
		std::size_t Characters(std::string_view utf8) { return Wide(utf8).size(); }
		std::optional<bool> ParseType(std::string_view text) {
			const auto word = StringUtils::ToLower(text);
			if (word == "system") return false;
			if (word == "alert") return true;
			return std::nullopt;
		}
		// The message's text: printable only (no line breaks or control characters).
		bool Printable(std::string_view text) {
			for (const char c : text) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
			return true;
		}

		// Takes a notice (from this machine or another) once; false when it was already taken.
		bool Take(const std::string& id, bool alert, std::string_view text, std::uint64_t sender) {
			const ULONGLONG now = GetTickCount64();
			{
				std::lock_guard lock(noticesMutex);
				if (seenIds.contains(id)) return false;
				seenIds[id] = now;
				if (seenIds.size() > 256) std::erase_if(seenIds, [now](const auto& seen) { return now - seen.second > 30 * 60 * 1000; });
				Notices::Notice notice{ alert, alert ? L"ALERT" : L"SYSTEM", Wide(text) };
				waiting.push_back({ std::move(notice), now + WaitMilliseconds });
			}
			Logger::Print(std::string(alert ? "ALERT" : "SYSTEM") + " (from " + std::to_string(sender) + "): " + std::string(text) + "\n");
			return true;
		}

		void OnMessage(std::uint64_t sender, const std::string& message) {
			if (!message.starts_with(MessageTag) || message.size() == MessageTag.size() || message[MessageTag.size()] != ' ') return;
			// "kf2notify", version, id, type, then the text (with its spaces).
			std::string_view rest(message);
			std::string_view words[4];
			for (auto& word : words) {
				const auto space = rest.find(' ');
				if (space == std::string_view::npos) return;
				word = rest.substr(0, space);
				rest.remove_prefix(space + 1);
			}
			int version = 0;
			std::from_chars(words[1].data(), words[1].data() + words[1].size(), version);
			const auto alert = ParseType(words[3]);
			const std::string id(words[2]);
			if (version != MessageVersion || !alert || rest.empty() || !Printable(rest) || Characters(rest) > MaxMessageCharacters) return;
			// The id starts with the sender's own account and the time it was sent.
			const std::string prefix = std::to_string(sender) + "-";
			if (!id.starts_with(prefix)) return;
			std::int64_t sent = 0;
			const auto timeText = std::string_view(id).substr(prefix.size());
			std::from_chars(timeText.data(), timeText.data() + timeText.size(), sent);
			const auto now = SteamNow();
			if (sent < now - MaxAgeSeconds || sent > now + MaxFutureSeconds) return;
			// Every machine checks for itself that the sender may send notices.
			if (!PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(sender))) {
				Logger::Info("Notices: a notice from " + std::to_string(sender) + " was refused (only Admins and the Creator can send them).");
				return;
			}
			Take(id, *alert, rest, sender);
		}
	}

	void Notices::Install() { SteamMultiplayer::AddLobbyMessageHandler(&OnMessage); }

	std::string Notices::NotifyCommand(std::string_view type, std::string_view message) {
		const auto alert = ParseType(type);
		if (!alert) return "notify: '" + std::string(type) + "' is not a type: system or alert.\n";
		if (message.empty()) return "notify: the message is missing. Use: notify <system|alert> <message>\n";
		if (!Printable(message)) return "notify: the message can only hold printable characters.\n";
		if (Characters(message) > MaxMessageCharacters) return "notify: the message is too long (at most " + std::to_string(MaxMessageCharacters) + " characters).\n";
		const auto me = SteamMultiplayer::MySteamId();
		if (me == 0) return "notify: Steam multiplayer is not running, so notices cannot be sent.\n";
		if (!PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(me))) return "notify: not allowed: only Admins and the Creator can send notices.\n";
		std::uint32_t count = 0;
		{
			std::lock_guard lock(noticesMutex);
			count = ++counter;
		}
		const std::string id = std::to_string(me) + "-" + std::to_string(SteamNow()) + "-" + std::to_string(count);
		const std::string text = std::string(MessageTag) + " " + std::to_string(MessageVersion) + " " + id + " " + (*alert ? "alert" : "system") + " " + std::string(message);
		// This machine, its own Steam lobby at once, and every other lobby through the notice board.
		Take(id, *alert, message, me);
		const bool lobby = SteamMultiplayer::SendLobbyMessage(text);
		const bool everyone = SteamMultiplayer::PostToEveryone(text);
		if (!lobby && !everyone) return "notify: could not be sent (no Steam lobby yet); try again in a moment.\n";
		return std::string("notify: sent") + (everyone ? "; every KeeperForge2 player on Steam gets it within about 30 seconds" : " to this Steam lobby only") + ".\n";
	}

	void Notices::ShowLocal(bool alert, std::string_view message) {
		std::uint32_t count = 0;
		{
			std::lock_guard lock(noticesMutex);
			count = ++counter;
		}
		Take("local-" + std::to_string(count), alert, message, SteamMultiplayer::MySteamId());
	}

	std::optional<Notices::Notice> Notices::Next() {
		std::lock_guard lock(noticesMutex);
		const ULONGLONG now = GetTickCount64();
		while (!waiting.empty() && now > waiting.front().until) waiting.pop_front();
		if (waiting.empty()) return std::nullopt;
		return waiting.front().notice;
	}
	void Notices::Pop() {
		std::lock_guard lock(noticesMutex);
		if (waiting.empty()) return;
		const auto& notice = waiting.front().notice;
		shown.emplace_back(notice.label + L": " + notice.text, notice.alert);
		if (shown.size() > MaxShownKept) shown.pop_front();
		waiting.pop_front();
	}
	std::optional<bool> Notices::ShownLine(const wchar_t* line) {
		if (line == nullptr) return std::nullopt;
		std::lock_guard lock(noticesMutex);
		for (const auto& [text, alert] : shown) {
			if (std::wcsncmp(line, text.c_str(), text.size()) != 0) continue;
			// The line ends there (0, or the lobby chat's 0xFFFF line end).
			const wchar_t after = line[text.size()];
			if (after == 0 || after == static_cast<wchar_t>(0xFFFF)) return alert;
		}
		return std::nullopt;
	}
}
