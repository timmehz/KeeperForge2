#include "ConsoleCommands.h"
#include "Bans.h"
#include "Logger.h"
#include "ModerationCommands.h"
#include "Notices.h"
#include "PerformanceStats.h"
#include "PlayerRanks.h"
#include "SteamMultiplayer.h"
#include "StringUtils.h"
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		using Part = PerformanceStats::Part;
		constexpr const char* HelpText =
			"Console commands (type one here, then press Enter):\n"
			"  cpu     the game's processor use\n"
			"  ram     the game's memory use\n"
			"  gpu     the game's graphics card use: load and video memory (VRAM)\n"
			"  fps     frames per second and frame time in ms (also: ms, frametime)\n"
			"  stats   all four\n"
			"  reset   start the figures again: reset alone for all, or reset cpu, reset ram, reset gpu, reset fps\n"
			"  online  how many KeeperForge2 players are online over Steam\n"
			"  ping    your latency in ms to Steam's servers (and to the players in your Steam lobby)\n"
			"  ranks   the ranks of everyone in the Steam lobby\n"
			"  help    this list\n";
		// Shown by help only to Admins and the Creator (by this machine's own ranks).
		constexpr const char* ModerationHelpText =
			"Moderation (Admins and the Creator, through the Steam lobby):\n"
			"  rank <SteamID64> <none|friend|admin|creator>   set a player's rank for everyone in the Steam lobby\n"
			"  notify <system|alert> <message>   a message to every KeeperForge2 player on Steam (alert: in red)\n"
			"  kick <SteamID64>   kick a player from the Steam lobby (yourself too)\n"
			"  ban <SteamID64>    ban a player from KeeperForge2 lobbies for good\n"
			"  tempban <time> <SteamID64>   ban for a time, such as tempban 3D 3h 20m <id> (s m h D M Y)\n"
			"  unban <SteamID64>  lift a ban\n"
			"  bans    who is banned\n"
			"  steamid64 <userid>   the SteamID64 of a Steam profile link name (steamcommunity.com/id/<userid>)\n";

		// The online command's answer, from the Steam thread.
		void PrintOnline(const SteamMultiplayer::OnlineCount& count) {
			const int elsewhere = count.total - count.inGame - count.inLobby;
			Logger::Print("Online: " + std::string(count.atLeast ? "at least " : "") + std::to_string(count.total) + " KeeperForge2 player"
				+ (count.total == 1 ? "" : "s") + " over Steam (you included): " + std::to_string(count.inGame) + " in a multiplayer game, "
				+ std::to_string(count.inLobby) + " in a multiplayer lobby, " + std::to_string(elsewhere) + " elsewhere in the game.\n");
		}

		// The ping command's answer, from the Steam thread.
		void PrintPing(const SteamMultiplayer::PingReport& report) {
			std::string text;
			if (report.servers.empty()) {
				text = "Ping: Steam's servers could not be measured"
					+ std::string(report.relayReady ? "" : " (Steam's relay network is not ready: is Steam online?)") + ".\n";
			}
			else {
				text = "Ping to Steam's nearest servers" + std::string(report.timedOut ? " (last measured; the new measurement took too long)" : "") + ":\n";
				for (const auto& server : report.servers) {
					text += "  " + server.code + "  " + std::to_string(server.ping) + " ms";
					if (!server.via.empty()) text += " (through " + server.via + ")";
					if (server.direct >= 0 && server.direct != server.ping) text += ", straight there " + std::to_string(server.direct) + " ms";
					text += "\n";
				}
			}
			if (!report.members.empty()) {
				text += "Ping to the players in your Steam lobby:\n";
				for (const auto& player : report.members) {
					text += "  " + player.name + " (" + std::to_string(player.steamId) + ")  "
						+ (player.ping >= 0 ? std::to_string(player.ping) + " ms" : std::string("no connection yet")) + "\n";
				}
			}
			Logger::Print(text);
		}

		std::optional<Part> PartNamed(std::string_view word) {
			if (word == "cpu") return Part::Cpu;
			if (word == "ram" || word == "memory") return Part::Ram;
			if (word == "gpu" || word == "vram") return Part::Gpu;
			if (word == "fps" || word == "ms" || word == "frametime") return Part::Fps;
			return std::nullopt;
		}
		constexpr Part AllParts[] = { Part::Cpu, Part::Ram, Part::Gpu, Part::Fps };

		std::vector<std::string> Words(std::string_view line) {
			std::vector<std::string> words;
			std::string word;
			for (const char c : line) {
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
					if (!word.empty()) words.push_back(std::move(word));
					word.clear();
				}
				else word += c;
			}
			if (!word.empty()) words.push_back(std::move(word));
			return words;
		}

		// The line as typed after its first `count` words (case and inner spaces kept).
		std::string_view AfterWords(std::string_view line, std::size_t count) {
			for (std::size_t i = 0; i < count; ++i) {
				const auto start = line.find_first_not_of(" \t");
				if (start == std::string_view::npos) return {};
				const auto end = line.find_first_of(" \t", start);
				if (end == std::string_view::npos) return {};
				line.remove_prefix(end);
			}
			const auto start = line.find_first_not_of(" \t");
			return start == std::string_view::npos ? std::string_view() : line.substr(start);
		}

		void Run(std::string_view line) {
			const auto words = Words(StringUtils::ToLower(line));
			if (words.empty()) return;
			const std::string& first = words[0];
			const std::string second = words.size() > 1 ? words[1] : std::string();
			if (words.size() <= 2 && (first == "help" || first == "?")) {
				const bool moderator = PlayerRanks::IsModerator(PlayerRanks::AuthorityOf(SteamMultiplayer::MySteamId()));
				Logger::Print(moderator ? std::string(HelpText) + ModerationHelpText : std::string(HelpText));
				return;
			}
			if (first == "ban" || first == "unban") {
				if (words.size() != 2) { Logger::Print("Use: " + first + " <SteamID64>\n"); return; }
				Logger::Print(first == "ban" ? Bans::BanCommand(words[1]) : Bans::UnbanCommand(words[1]));
				return;
			}
			if (first == "tempban") {
				// The time keeps its case: m is minutes, M months.
				Logger::Print(Bans::TempBanCommand(AfterWords(line, 1)));
				return;
			}
			if (first == "bans" && words.size() == 1) { Logger::Print(Bans::BansCommand()); return; }
			if (first == "steamid64") {
				// As typed: Steam names keep their case and spaces.
				Logger::Print(ModerationCommands::SteamIdCommand(AfterWords(line, 1)));
				return;
			}
			if (first == "kick") {
				if (words.size() != 2) { Logger::Print("Use: kick <SteamID64>\n"); return; }
				if (const auto text = ModerationCommands::KickCommand(words[1]); !text.empty()) Logger::Print(text);
				return;
			}
			if (first == "notify") {
				if (words.size() < 3) { Logger::Print("Use: notify <system|alert> <message>\n"); return; }
				Logger::Print(Notices::NotifyCommand(words[1], AfterWords(line, 2)));
				return;
			}
			if (first == "rank" || first == "ranks") {
				if (words.size() == 1) { Logger::Print(PlayerRanks::RanksCommand()); return; }
				if (first == "rank" && words.size() == 3) { Logger::Print(PlayerRanks::RankCommand(words[1], words[2])); return; }
				Logger::Print("Use: rank <SteamID64> <none|friend|admin|creator>, or ranks for the list.\n");
				return;
			}
			if (words.size() == 1 && first == "online") {
				if (SteamMultiplayer::RequestOnlineCount(&PrintOnline)) Logger::Print("Counting the players online...\n");
				else Logger::Print("online: Steam multiplayer is not running (or this account is banned), so players cannot be counted.\n");
				return;
			}
			if (words.size() == 1 && first == "ping") {
				if (SteamMultiplayer::RequestPing(&PrintPing)) Logger::Print("Measuring the ping to Steam's servers...\n");
				else Logger::Print("ping: Steam multiplayer is not running, so Steam's servers cannot be measured.\n");
				return;
			}
			if (words.size() == 1 && (first == "stats" || first == "all")) {
				std::string text;
				for (const Part part : AllParts) text += PerformanceStats::Report(part);
				Logger::Print(text);
				return;
			}
			// "reset", "reset fps" and "fps reset".
			const bool resetFirst = first == "reset" && words.size() <= 2;
			const bool resetSecond = words.size() == 2 && second == "reset" && PartNamed(first);
			if (resetFirst || resetSecond) {
				const std::string named = resetFirst ? second : first;
				if (named.empty() || named == "all" || named == "stats") {
					for (const Part part : AllParts) PerformanceStats::Reset(part);
					Logger::Print("All figures start again from now.\n");
					return;
				}
				if (const auto part = PartNamed(named)) {
					PerformanceStats::Reset(*part);
					Logger::Print("The " + named + " figures start again from now.\n");
					return;
				}
			}
			else if (words.size() == 1) {
				if (const auto part = PartNamed(first)) { Logger::Print(PerformanceStats::Report(*part)); return; }
			}
			Logger::Print("Unknown command '" + std::string(line) + "'. Type help for the list.\n");
		}

		std::string ToUtf8(const wchar_t* text, int length) {
			const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
			std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
			if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), size, nullptr, nullptr);
			return result;
		}
		std::string Trimmed(std::string text) {
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) return {};
			const auto last = text.find_last_not_of(" \t\r\n");
			return text.substr(first, last - first + 1);
		}

		// Blocks in ReadConsoleW until a line is entered; ends when the console goes (the launcher closed). The game
		// closing ends the thread with it.
		void ReadLoop(HANDLE input) {
			std::wstring pending;
			wchar_t buffer[256];
			for (;;) {
				DWORD read = 0;
				if (!ReadConsoleW(input, buffer, static_cast<DWORD>(std::size(buffer)), &read, nullptr)) {
					Logger::Debug("Console commands: the console can no longer be read (error " + std::to_string(GetLastError()) + "), stopping.");
					break;
				}
				if (read == 0) continue;
				pending.append(buffer, read);
				// A line longer than the buffer arrives in parts; act once its end is in.
				std::size_t end = 0;
				while ((end = pending.find(L'\n')) != std::wstring::npos) {
					const std::string line = Trimmed(ToUtf8(pending.data(), static_cast<int>(end)));
					pending.erase(0, end + 1);
					if (line.empty()) continue;
					try { Run(line); }
					catch (const std::exception& error) { Logger::Warning(std::string("Console commands: '") + line + "' failed: " + error.what()); }
				}
				if (pending.size() > 4096) pending.clear();
			}
			CloseHandle(input);
		}
	}

	void ConsoleCommands::Install() {
		// The console the logger writes to (the launcher's, attached by Logger::Initialize).
		const HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		DWORD mode = 0;
		if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
			if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
			Logger::Debug("Console commands: no console to read from (the game was not started by the launcher), so none.");
			return;
		}
		// Whole lines, shown as they are typed.
		SetConsoleMode(input, mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
		// Keys pressed in the window before now are not commands.
		FlushConsoleInputBuffer(input);
		std::thread(ReadLoop, input).detach();
		Logger::Info("Console commands: type help in this window for the list (cpu, ram, gpu, fps).");
	}
}
