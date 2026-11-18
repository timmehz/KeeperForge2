#include "ModerationCommands.h"
#include "Logger.h"
#include "Notices.h"
#include "PlayerRanks.h"
#include "SteamMultiplayer.h"
#include "StringUtils.h"
#include <charconv>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>
#include <Windows.h>
#include <winhttp.h>

namespace KeeperForge2 {
	namespace {
		// "kf2kick 1 <target Steam id>". Who sent it is what Steam says, never part of the message.
		constexpr std::string_view KickTag = "kf2kick";
		constexpr int KickVersion = 1;

		// A person's own Steam account: public universe (1), individual account (1).
		bool IsPersonalSteamId(std::uint64_t id) { return (id >> 56) == 1 && ((id >> 52) & 0xF) == 1 && (id & 0xFFFFFFFFull) != 0; }
		std::optional<std::uint64_t> ParseSteamId(std::string_view text) {
			std::uint64_t id = 0;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !IsPersonalSteamId(id)) return std::nullopt;
			return id;
		}
		// The player's Steam name and id.
		std::string Who(std::uint64_t id) {
			if (id == SteamMultiplayer::MySteamId()) return "you";
			const auto name = SteamMultiplayer::NameOf(id);
			return name.empty() ? std::to_string(id) : name + " (" + std::to_string(id) + ")";
		}

		// The kick itself, on this machine: leave the lobby when it is us, else shut the player out.
		void Kick(std::uint64_t sender, std::uint64_t target) {
			const auto me = SteamMultiplayer::MySteamId();
			if (target == me) {
				if (sender == me) {
					Logger::Print("kick: you left the Steam lobby.\n");
				}
				else {
					const std::string text = "You were kicked from the lobby by " + Who(sender) + ".";
					Logger::Print("kick: " + text + "\n");
					Notices::ShowLocal(true, text);
				}
				SteamMultiplayer::LeaveLobbyKicked();
				return;
			}
			const std::string text = Who(target) + " was kicked from the lobby by " + Who(sender) + ".";
			SteamMultiplayer::ShutOut(target);
			Logger::Print("kick: " + text + "\n");
			Notices::ShowLocal(false, text);
		}

		void OnMessage(std::uint64_t sender, const std::string& message) {
			if (!message.starts_with(KickTag) || message.size() == KickTag.size() || message[KickTag.size()] != ' ') return;
			// This machine's own kicks were done when typed.
			if (sender == SteamMultiplayer::MySteamId()) return;
			std::vector<std::string_view> words;
			std::string_view rest(message);
			while (!rest.empty()) {
				const auto space = rest.find(' ');
				if (const auto word = rest.substr(0, space); !word.empty()) words.push_back(word);
				if (space == std::string_view::npos) break;
				rest.remove_prefix(space + 1);
			}
			int version = 0;
			if (words.size() != 3) return;
			std::from_chars(words[1].data(), words[1].data() + words[1].size(), version);
			const auto target = ParseSteamId(words[2]);
			if (version != KickVersion || !target) return;
			// Every machine checks for itself, with its own ranks, that the sender may do it.
			std::string why;
			if (!PlayerRanks::CanKick(sender, *target, &why)) {
				Logger::Info("kick: a kick from " + std::to_string(sender) + " was refused (" + why + ").");
				return;
			}
			Kick(sender, *target);
		}
	}

	void ModerationCommands::Install() { SteamMultiplayer::AddLobbyMessageHandler(&OnMessage); }

	std::string ModerationCommands::KickCommand(std::string_view steamIdText) {
		const auto target = ParseSteamId(steamIdText);
		if (!target) return "kick: '" + std::string(steamIdText) + "' is not a Steam account id (the 17-digit SteamID64, such as 76561198000000000).\n";
		const auto me = SteamMultiplayer::MySteamId();
		if (me == 0) return "kick: Steam multiplayer is not running.\n";
		std::string why;
		if (!PlayerRanks::CanKick(me, *target, &why)) return "kick: not allowed: " + why + ".\n";
		if (*target != me && !SteamMultiplayer::IsInLobby(*target)) return "kick: " + std::to_string(*target) + " is not in your Steam lobby.\n";
		// To the lobby first (so the kicked player hears it before we stop talking to them), then done here.
		if (!SteamMultiplayer::SendLobbyMessage(std::string(KickTag) + " " + std::to_string(KickVersion) + " " + std::to_string(*target))) {
			return "kick: there is no Steam lobby to send it to yet; try again in a moment.\n";
		}
		Kick(me, *target);
		return {};
	}

	namespace {
		// A Steam custom URL name: letters, digits, _ and - (Steam allows 3 to 32).
		bool IsProfileName(std::string_view text) {
			if (text.size() < 2 || text.size() > 32) return false;
			for (const char c : text) {
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
			}
			return true;
		}
		// "https://steamcommunity.com/id/<name>/" or "steamcommunity.com/id/<name>" to the name.
		std::string_view ProfileNameOf(std::string_view text) {
			if (const auto at = text.find("/id/"); at != std::string_view::npos) {
				text.remove_prefix(at + 4);
				text = text.substr(0, text.find('/'));
			}
			return text;
		}
		// The text between <tag> and </tag> (CDATA wrapping taken off), or "".
		std::string Between(const std::string& xml, const std::string& tag) {
			const auto start = xml.find("<" + tag + ">");
			if (start == std::string::npos) return {};
			const auto from = start + tag.size() + 2;
			const auto end = xml.find("</" + tag + ">", from);
			if (end == std::string::npos) return {};
			std::string value = xml.substr(from, end - from);
			if (value.starts_with("<![CDATA[") && value.ends_with("]]>")) value = value.substr(9, value.size() - 12);
			return value;
		}
		// GET https://steamcommunity.com<path>: the body, or nullopt (with why).
		std::optional<std::string> FetchSteamCommunity(const std::wstring& path, std::string& why) {
			std::optional<std::string> body;
			HINTERNET session = WinHttpOpen(L"KeeperForge2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			HINTERNET connection = session ? WinHttpConnect(session, L"steamcommunity.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
			HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
			if (request) WinHttpSetTimeouts(request, 10000, 10000, 10000, 15000);
			DWORD status = 0, statusSize = sizeof(status);
			if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
				why = "Steam's profile pages could not be reached (error " + std::to_string(GetLastError()) + ")";
			}
			else if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status != 200) {
				why = "Steam answered with status " + std::to_string(status);
			}
			else {
				std::string text;
				for (;;) {
					DWORD available = 0;
					if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
					const std::size_t at = text.size();
					text.resize(at + available);
					DWORD read = 0;
					if (!WinHttpReadData(request, text.data() + at, available, &read)) break;
					text.resize(at + read);
					if (text.size() > 512 * 1024) break;
				}
				body = std::move(text);
			}
			if (request) WinHttpCloseHandle(request);
			if (connection) WinHttpCloseHandle(connection);
			if (session) WinHttpCloseHandle(session);
			return body;
		}
		// On its own thread: the custom URL's account, from its public profile page (XML).
		void LookUpProfileName(std::string name) {
			std::string why;
			const std::wstring path = L"/id/" + std::wstring(name.begin(), name.end()) + L"/?xml=1";
			const auto xml = FetchSteamCommunity(path, why);
			if (!xml) {
				Logger::Print("steamid64: " + name + ": " + why + ".\n");
				return;
			}
			const auto id = Between(*xml, "steamID64");
			if (id.empty() || !ParseSteamId(id)) {
				const auto error = Between(*xml, "error");
				Logger::Print("steamid64: no Steam profile has the custom URL '" + name + "'" + (error.empty() ? "" : " (" + error + ")") + ".\n");
				return;
			}
			const auto shown = Between(*xml, "steamID");
			Logger::Print("steamid64: steamcommunity.com/id/" + name + " is " + id + (shown.empty() ? "" : " (" + shown + ")") + ".\n");
		}
	}

	std::string ModerationCommands::SteamIdCommand(std::string_view text) {
		if (!PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(SteamMultiplayer::MySteamId()))) {
			return "steamid64: not allowed: only Admins and the Creator can look up Steam accounts.\n";
		}
		if (text.empty()) return "Use: steamid64 <userid> (the name in a Steam profile link, steamcommunity.com/id/<userid>)\n";
		std::string answer;
		// Members of our lobby whose Steam name is the text (any case): names are not unique, so all of them.
		const auto wanted = StringUtils::ToLower(text);
		std::vector<std::pair<std::uint64_t, std::string>> members;
		if (const auto me = SteamMultiplayer::MySteamId(); me != 0) members.emplace_back(me, SteamMultiplayer::MyName());
		for (const auto& member : SteamMultiplayer::OtherMembers()) members.emplace_back(member.steamId, member.name);
		for (const auto& [id, name] : members) {
			if (StringUtils::ToLower(name) == wanted) answer += "steamid64: in your Steam lobby, " + name + " is " + std::to_string(id) + ".\n";
		}
		const auto profileName = ProfileNameOf(text);
		if (IsProfileName(profileName)) {
			std::thread(LookUpProfileName, std::string(profileName)).detach();
			answer += "steamid64: looking up steamcommunity.com/id/" + std::string(profileName) + "...\n";
		}
		else if (answer.empty()) {
			answer = "steamid64: no one in your Steam lobby is named '" + std::string(text) + "', and it is not a profile link name"
				" (letters, digits, _ and - only).\n";
		}
		return answer;
	}
}
