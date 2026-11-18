#include "ModCheck.h"
#include "GameAddresses.h"
#include "Logger.h"
#include "Memory.h"
#include "ModData.h"
#include "Paths.h"
#include "SessionOptions.h"
#include "SteamMultiplayer.h"
#include "StringUtils.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef KEEPERFORGE2_VERSION
#define KEEPERFORGE2_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;

namespace KeeperForge2 {
	namespace {
		// FNV-1a, 64 bits, folded to 32 at the end.
		constexpr std::uint64_t HashStart = 0xCBF29CE484222325ull;
		constexpr std::uint64_t HashPrime = 0x100000001B3ull;
		void HashBytes(std::uint64_t& hash, const void* data, std::size_t size) {
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for (std::size_t i = 0; i < size; ++i) {
				hash ^= bytes[i];
				hash *= HashPrime;
			}
		}
		void HashText(std::uint64_t& hash, const std::string& text) {
			HashBytes(hash, text.data(), text.size());
			const std::uint8_t end = 0;
			HashBytes(hash, &end, 1);
		}
		bool HashFile(std::uint64_t& hash, const fs::path& path) {
			std::ifstream file(path, std::ios::binary);
			if (!file) return false;
			std::vector<char> buffer(1 << 16);
			while (file) {
				file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto read = file.gcount();
				if (read > 0) HashBytes(hash, buffer.data(), static_cast<std::size_t>(read));
			}
			return true;
		}
		std::uint32_t Fold(std::uint64_t hash) { return static_cast<std::uint32_t>(hash ^ (hash >> 32)); }
		std::vector<fs::path> SortedEntries(const fs::path& folder, bool folders, bool recursive) {
			std::vector<fs::path> found;
			std::error_code error;
			if (!fs::is_directory(folder, error)) return found;
			const auto take = [&](const fs::directory_entry& entry) {
				if (folders ? entry.is_directory(error) : entry.is_regular_file(error)) found.push_back(entry.path());
			};
			if (recursive) for (const auto& entry : fs::recursive_directory_iterator(folder, error)) take(entry);
			else for (const auto& entry : fs::directory_iterator(folder, error)) take(entry);
			std::ranges::sort(found, [](const fs::path& a, const fs::path& b) { return StringUtils::ToLower(a.string()) < StringUtils::ToLower(b.string()); });
			return found;
		}
		std::string Lower(const fs::path& path) { return StringUtils::ToLower(path.generic_string()); }
		std::string Hex(std::uint32_t value) {
			char text[16]{};
			std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(value));
			return text;
		}

		// What this machine runs: KeeperForge2's version (from the build), a hash of its DLL (another build of the same
		// version may play differently), and a fingerprint of the mods (their DLLs, and the editor data of mods and Flame
		// packs: creature lists, levels; art, text and sounds cannot put the game out of step and are left out).
		struct Identity {
			std::array<std::uint8_t, 3> version{};
			std::uint32_t build = 0;
			std::uint32_t mods = 0;
		};
		Identity local;
		bool made = false;
		std::array<std::uint8_t, 3> ParseVersion(const char* text) {
			std::array<std::uint8_t, 3> parts{};
			unsigned a = 0, b = 0, c = 0;
			if (std::sscanf(text, "%u.%u.%u", &a, &b, &c) >= 1) parts = { static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c) };
			return parts;
		}
		std::string VersionText(const std::array<std::uint8_t, 3>& version) {
			return std::to_string(version[0]) + "." + std::to_string(version[1]) + "." + std::to_string(version[2]);
		}
		void Make() {
			made = true;
			local.version = ParseVersion(KEEPERFORGE2_VERSION);
			std::uint64_t build = HashStart;
			HMODULE self = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Make), &self)) {
				wchar_t path[MAX_PATH]{};
				if (GetModuleFileNameW(self, path, MAX_PATH) != 0) HashFile(build, path);
			}
			local.build = Fold(build);
			std::uint64_t mods = HashStart;
			std::size_t dlls = 0;
			std::size_t dataFolders = 0;
			std::error_code error;
			const auto modsFolder = fs::absolute(Paths::Mods(), error);
			for (const auto& folder : SortedEntries(modsFolder, true, false)) {
				for (const auto& file : SortedEntries(folder, false, false)) {
					if (StringUtils::ToLower(file.extension().string()) != ".dll") continue;
					HashText(mods, Lower(fs::relative(file, modsFolder, error)));
					HashFile(mods, file);
					++dlls;
				}
			}
			for (const auto& data : ModData::DataFolders()) {
				const auto editor = data / "editor";
				const auto files = SortedEntries(editor, false, true);
				if (files.empty()) continue;
				++dataFolders;
				HashText(mods, Lower(data.parent_path().filename()));
				for (const auto& file : files) {
					HashText(mods, Lower(fs::relative(file, editor, error)));
					HashFile(mods, file);
				}
			}
			local.mods = Fold(mods);
			Logger::Info("Mods check: KeeperForge2 v" + VersionText(local.version) + " (build " + Hex(local.build) + "), mods " + Hex(local.mods) + " ("
				+ std::to_string(dlls) + " mod DLLs, " + std::to_string(dataFolders) + " data folders with editor data).");
		}
		const Identity& Local() {
			if (!made) Make();
			return local;
		}

		// Where it travels: the free bytes (2-4) of the lobby's creature list rows 1 to 4, rows of the game's own creatures
		// (the first row and the heroes' rows are taken by the session options and the Mercenary Portal); bit 0x40 of the
		// first row's options byte says it is there.
		//   row 1: mods bytes 0-2; row 2: mods byte 3, version major, minor; row 3: version patch, build bytes 0-1;
		//   row 4: build bytes 2-3.
		constexpr std::uint8_t PresentFlag = 0x40;
		constexpr std::uint32_t RowsNeeded = 5;
		std::uint8_t* Rows(const std::uint8_t* frontEnd, std::uint32_t& count) {
			count = Memory::Read<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frontEnd) + Game::Offset::LobbyCreatureCount);
			return *reinterpret_cast<std::uint8_t* const*>(frontEnd + Game::Offset::LobbyCreatureList);
		}
		std::uint8_t* Free(std::uint8_t* rows, std::size_t row, std::size_t byte) { return rows + row * Game::CreatureListEntrySize + 2 + byte; }
		void WriteToList(std::uint8_t* frontEnd) {
			std::uint32_t count = 0;
			auto* rows = Rows(frontEnd, count);
			if (rows == nullptr || count < RowsNeeded) return;
			const auto& self = Local();
			const std::array<std::uint8_t, 12> bytes = {
				static_cast<std::uint8_t>(self.mods), static_cast<std::uint8_t>(self.mods >> 8), static_cast<std::uint8_t>(self.mods >> 16),
				static_cast<std::uint8_t>(self.mods >> 24), self.version[0], self.version[1],
				self.version[2], static_cast<std::uint8_t>(self.build), static_cast<std::uint8_t>(self.build >> 8),
				static_cast<std::uint8_t>(self.build >> 16), static_cast<std::uint8_t>(self.build >> 24), 0 };
			for (std::size_t i = 0; i < bytes.size(); ++i) *Free(rows, 1 + i / 3, i % 3) = bytes[i];
			rows[Game::CreatureListFreeByte] |= PresentFlag;
		}

		// What the host sent, as last read.
		bool hostKnown = false;
		bool hostSent = false;
		Identity host;
		std::uint32_t lastLogged = 0;
		bool loggedMissing = false;
		void ReadFromList(const std::uint8_t* frontEnd) {
			std::uint32_t count = 0;
			auto* rows = Rows(frontEnd, count);
			if (rows == nullptr || count < RowsNeeded) return;
			hostKnown = true;
			hostSent = (rows[Game::CreatureListFreeByte] & PresentFlag) != 0;
			const auto& self = Local();
			if (!hostSent) {
				if (!loggedMissing) {
					loggedMissing = true;
					Logger::Warning("Mods check: the host did not send its KeeperForge2 version and mods (an older KeeperForge2?); every player needs the same.");
				}
				return;
			}
			std::array<std::uint8_t, 12> bytes{};
			for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = *Free(rows, 1 + i / 3, i % 3);
			host.mods = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | static_cast<std::uint32_t>(bytes[3]) << 24;
			host.version = { bytes[4], bytes[5], bytes[6] };
			host.build = bytes[7] | bytes[8] << 8 | bytes[9] << 16 | static_cast<std::uint32_t>(bytes[10]) << 24;
			const auto key = host.mods ^ host.build;
			if (key == lastLogged) return;
			lastLogged = key;
			if (host.version != self.version) Logger::Warning("Mods check: the host uses KeeperForge2 v" + VersionText(host.version) + ", this machine v" + VersionText(self.version) + ".");
			else if (host.build != self.build) Logger::Warning("Mods check: the host uses another build of KeeperForge2 v" + VersionText(host.version) + " (" + Hex(host.build) + ", this machine " + Hex(self.build) + ").");
			if (host.mods != self.mods) Logger::Warning("Mods check: the host's mods (" + Hex(host.mods) + ") are not the same as this machine's (" + Hex(self.mods) + ").");
			if (host.version == self.version && host.build == self.build && host.mods == self.mods) Logger::Info("Mods check: the host's KeeperForge2 and mods match this machine's.");
		}

		// The notice in the game's chat: 0x409EC0, thiscall (interface, sender, text) ret 8, copies 0x102 bytes of text. A
		// sender of 0x8181 is one of the game's special senders, drawn in its own color, with no name.
		constexpr std::uint16_t NoticeSender = 0x8181;
		constexpr std::size_t ChatLineCharacters = 0x102 / sizeof(wchar_t);
		using ChatLineFn = void(__fastcall*)(void* playerInterface, void* unused, std::uint32_t sender, const wchar_t* text);
		void Say(void* playerInterface, const std::string& text) {
			std::array<wchar_t, ChatLineCharacters> line{};
			const auto length = (std::min)(text.size(), line.size() - 1);
			for (std::size_t i = 0; i < length; ++i) line[i] = static_cast<wchar_t>(static_cast<unsigned char>(text[i]));
			reinterpret_cast<ChatLineFn>(Game::Resolve(Game::Address::ChatAddLocalLine))(playerInterface, nullptr, NoticeSender, line.data());
		}
	}

	std::uint32_t ModCheck::LocalFingerprint() { return Local().mods; }

	namespace {
		// What this machine runs, for Steam lobby member data: "v=1.2.3;b=XXXXXXXX;m=XXXXXXXX".
		std::string IdentityText() {
			const auto& self = Local();
			return "v=" + VersionText(self.version) + ";b=" + Hex(self.build) + ";m=" + Hex(self.mods);
		}
		bool ParseIdentity(const std::string& text, Identity& out) {
			unsigned a = 0, b = 0, c = 0, build = 0, mods = 0;
			if (std::sscanf(text.c_str(), "v=%u.%u.%u;b=%x;m=%x", &a, &b, &c, &build, &mods) != 5) return false;
			out.version = { static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c) };
			out.build = build;
			out.mods = mods;
			return true;
		}
		// Steam games: every other member of the Steam lobby put what it runs on itself, so the host and every player can
		// compare with all of them. Empty when there is nothing to compare (no Steam lobby).
		std::vector<std::string> SteamLines(bool& compared) {
			compared = false;
			std::vector<std::string> problems;
			const auto members = SteamMultiplayer::OtherMembers();
			if (members.empty()) return problems;
			compared = true;
			const auto& self = Local();
			for (const auto& member : members) {
				Identity other;
				if (!ParseIdentity(member.identity, other)) {
					problems.push_back(member.name + " did not send a KeeperForge2 version (an older KeeperForge2?).");
					continue;
				}
				if (other.version != self.version) problems.push_back(member.name + " uses KeeperForge2 v" + VersionText(other.version) + ", you have v" + VersionText(self.version) + ".");
				else if (other.build != self.build) problems.push_back(member.name + " uses another build of KeeperForge2 v" + VersionText(other.version) + ".");
				if (other.mods != self.mods) problems.push_back(member.name + "'s mods or Flame packs are not the same as yours.");
			}
			for (const auto& problem : problems) Logger::Warning("Mods check (Steam lobby): " + problem);
			if (problems.empty()) Logger::Info("Mods check: every member of the Steam lobby runs the same KeeperForge2 and mods.");
			// The game's chat keeps 3 lines: at most 2 about players, then the ending.
			std::vector<std::string> lines;
			if (problems.size() <= 2) for (const auto& problem : problems) lines.push_back("KeeperForge2: " + problem);
			else lines.push_back("KeeperForge2: " + std::to_string(problems.size()) + " differences with players in your Steam lobby (see the log).");
			if (!lines.empty()) lines.push_back("KeeperForge2: any of them in this game will put it out of step.");
			return lines;
		}
	}

	void ModCheck::OnInterfaceReady(void* playerInterface) {
		if (playerInterface == nullptr) return;
		if (Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode)) != Game::PlayModeMultiplayer) return;
		bool compared = false;
		const auto steamLines = SteamLines(compared);
		if (compared) {
			for (const auto& line : steamLines) Say(playerInterface, line);
			return;
		}
		if (!hostKnown) return;
		const auto& self = Local();
		std::vector<std::string> lines;
		if (!hostSent) lines.push_back("KeeperForge2: the host did not send its KeeperForge2 version. Every player needs the same KeeperForge2 and mods.");
		else {
			if (host.version != self.version) {
				lines.push_back("KeeperForge2: the host uses KeeperForge2 v" + VersionText(host.version) + ", you have v" + VersionText(self.version) + ".");
			}
			else if (host.build != self.build) lines.push_back("KeeperForge2: the host uses another build of KeeperForge2 v" + VersionText(host.version) + ".");
			if (host.mods != self.mods) lines.push_back("KeeperForge2: your mods or Flame packs are not the same as the host's.");
			if (!lines.empty()) lines.push_back("KeeperForge2: this game will go out of step.");
		}
		for (const auto& line : lines) Say(playerInterface, line);
	}

	void ModCheck::Install() {
		// Made now, on the game's thread, so the Steam thread only reads it.
		Local();
		SteamMultiplayer::SetIdentityProvider(&IdentityText);
		SessionOptions::OnLobbyWrite(&WriteToList);
		SessionOptions::OnLobbyRead(&ReadFromList);
	}
}
