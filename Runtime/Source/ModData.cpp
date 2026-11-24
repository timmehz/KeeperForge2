#include "ModData.h"
#include "CreatureRegistry.h"
#include "CustomCampaign.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "Paths.h"
#include "StringUtils.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace KeeperForge2 {
	namespace {
		// Each kind of data: its folder under data\ (spelled as the game spells it, with a last backslash where the game
		// has one) and its file manager in the resources (bytes from the start; 0x55C020 and 0x55C700).
		struct DataKind {
			const char* folder;
			std::uintptr_t manager;
		};
		constexpr std::array<DataKind, 9> Kinds = { {
			{ "Meshes", 0xB30 },
			{ "EngineTextures", 0xD98 },
			{ "sprite\\", 0x1268 },
			{ "frontend", 0x1604 },
			{ "paths", 0x14D0 },
			{ "editor", 0x1000 },
			{ "text\\", 0x139C },
			{ "Texture", 0xECC },
			{ "palette", 0x1134 },
		} };

		// A folder that may hold game data: its data\ folder and a name for the log.
		struct Source {
			fs::path data;
			std::string name;
		};

		using OpenByPathFn = std::int32_t*(__cdecl*)(std::int32_t* status, void** file, const char* path, std::uint32_t access);
		OpenByPathFn originalOpenByPath = nullptr;
		constexpr std::uint32_t WriteAccess = 0x40000000;
		using AddLooseFoldersFn = void(__fastcall*)(std::uint8_t* resources);
		using AddFolderFn = std::int32_t*(__cdecl*)(std::int32_t* status, void* manager, const char* format, ...);
		AddLooseFoldersFn originalAddLooseFolders = nullptr;
		AddFolderFn addFolder = nullptr;
		bool logged = false;

		std::string Trim(const std::string& text) {
			const auto first = text.find_first_not_of(" \t");
			if (first == std::string::npos) return {};
			const auto last = text.find_last_not_of(" \t");
			return text.substr(first, last - first + 1);
		}

		std::vector<fs::path> SortedFolders(const fs::path& parent) {
			std::vector<fs::path> folders;
			std::error_code error;
			if (!fs::is_directory(parent, error)) return folders;
			for (const auto& entry : fs::directory_iterator(parent, error)) {
				if (entry.is_directory(error)) folders.push_back(entry.path());
			}
			std::ranges::sort(folders, [](const fs::path& a, const fs::path& b) { return StringUtils::ToLower(a.filename().string()) < StringUtils::ToLower(b.filename().string()); });
			return folders;
		}
		fs::path DataFolder(const fs::path& folder) {
			std::error_code error;
			const auto data = folder / "data";
			return fs::is_directory(data, error) ? data : fs::path{};
		}

		// Lowest priority first (each one added goes in front of the ones before it).
		std::vector<Source> Sources() {
			std::vector<Source> sources;
			// flame_packs: all (every pack folder; the first by name wins, as in a Flame list), off, or names joined by
			// ';' (the first named wins, as in Flame's own list).
			const auto setting = Config::GetString("flame_packs", "all");
			const auto lower = StringUtils::ToLower(setting);
			const fs::path packs = fs::path("flame") / "resources";
			std::vector<fs::path> chosen;
			if (lower == "all") chosen = SortedFolders(packs);
			else if (lower != "off") {
				std::string name;
				for (const char c : setting + ";") {
					if (c != ';') {
						name += c;
						continue;
					}
					const auto trimmed = Trim(name);
					name.clear();
					if (trimmed.empty()) continue;
					std::error_code error;
					if (fs::is_directory(packs / trimmed, error)) chosen.push_back(packs / trimmed);
					else Logger::Warning("Mod data: the Flame pack \"" + trimmed + "\" in flame_packs is not in flame\\resources.");
				}
			}
			for (auto it = chosen.rbegin(); it != chosen.rend(); ++it) {
				const auto data = DataFolder(*it);
				if (!data.empty()) sources.push_back({ data, "Flame pack " + it->filename().string() });
			}
			for (const auto& folder : SortedFolders(Paths::Mods())) {
				const auto data = DataFolder(folder);
				if (!data.empty()) sources.push_back({ data, "mod " + folder.filename().string() });
			}
			return sources;
		}

		void AddSources(std::uint8_t* resources) {
			std::size_t added = 0;
			std::vector<std::string> names;
			for (const auto& source : Sources()) {
				std::size_t kinds = 0;
				for (const auto& kind : Kinds) {
					std::error_code error;
					auto folder = fs::absolute(source.data, error);
					if (error) continue;
					const std::string spelled = kind.folder;
					const bool slash = !spelled.empty() && spelled.back() == '\\';
					folder /= slash ? spelled.substr(0, spelled.size() - 1) : spelled;
					if (!fs::is_directory(folder, error)) continue;
					auto text = folder.string();
					if (slash) text += '\\';
					std::int32_t status = 0;
					addFolder(&status, resources + kind.manager, "%s", text.c_str());
					if (status < 0) {
						Logger::Warning("Mod data: the game could not open " + text + " (" + source.name + ").");
						continue;
					}
					++kinds;
					if (!logged) Logger::Debug("Mod data: " + text + " is searched before the game's own files (" + source.name + ").");
				}
				if (kinds > 0) {
					++added;
					names.push_back(source.name);
				}
			}
			if (logged) return;
			logged = true;
			if (added == 0) {
				Logger::Debug("Mod data: no mod or Flame pack has a data folder.");
				return;
			}
			// Highest priority first for the reader.
			std::string list;
			for (auto it = names.rbegin(); it != names.rend(); ++it) list += (list.empty() ? "" : ", ") + *it;
			Logger::Info("Mod data: game files from " + list + " (first wins).");
		}

		// The sources' data folders, highest priority first, made the first time a file is opened by path.
		std::vector<fs::path> openOrder;
		bool openOrderMade = false;
		std::string gameFolderLower;
		bool redirectLogged = false;
		// A path under the game's Data folder (relative, or under the game folder): the part after "Data\". Empty if not.
		std::string UnderData(const std::string& path) {
			std::string rest = path;
			const auto lower = StringUtils::ToLower(rest);
			if (!gameFolderLower.empty() && lower.starts_with(gameFolderLower)) rest = rest.substr(gameFolderLower.size());
			while (rest.starts_with(".\\") || rest.starts_with("./")) rest = rest.substr(2);
			const auto restLower = StringUtils::ToLower(rest);
			if (restLower.starts_with("data\\") || restLower.starts_with("data/")) return rest.substr(5);
			return {};
		}
		void EnsureOpenOrder() {
			if (openOrderMade) return;
			openOrderMade = true;
			std::error_code error;
			auto folder = fs::current_path(error).string();
			if (!folder.empty() && folder.back() != '\\') folder += '\\';
			gameFolderLower = StringUtils::ToLower(folder);
			const auto sources = Sources();
			for (auto it = sources.rbegin(); it != sources.rend(); ++it) openOrder.push_back(fs::absolute(it->data, error));
		}
		// The first mod or pack holding this file under its data folder, or empty.
		fs::path FromSources(const std::string& underData) {
			EnsureOpenOrder();
			for (const auto& data : openOrder) {
				std::error_code error;
				const auto candidate = data / underData;
				if (fs::is_regular_file(candidate, error)) return candidate;
			}
			return {};
		}
		std::int32_t* __cdecl OpenByPathDetour(std::int32_t* status, void** file, const char* path, std::uint32_t access) {
			if (path == nullptr || (access & WriteAccess) != 0) return originalOpenByPath(status, file, path, access);
			EnsureOpenOrder();
			const auto rest = UnderData(path);
			// Creature types added by mods: the shared list with them added.
			if (!rest.empty()) {
				if (const auto merged = CreatureRegistry::ListFor(rest); !merged.empty()) {
					const auto text = merged.string();
					return originalOpenByPath(status, file, text.c_str(), access);
				}
			}
			fs::path source = path;
			if (const auto candidate = rest.empty() ? fs::path{} : FromSources(rest); !candidate.empty()) {
				source = candidate;
				if (!redirectLogged) {
					redirectLogged = true;
					Logger::Debug("Mod data: " + std::string(path) + " is read from " + candidate.string() + " (first time a file is taken from a mod or pack).");
				}
			}
			// A custom campaign's level being played: its files from the campaign's folder.
			if (const auto campaignFile = CustomCampaign::LevelFileFor(source); !campaignFile.empty()) source = campaignFile;
			// A level's own creature list (wherever the level is, KeeperForge2\maps included): with the mods' types added.
			if (const auto levelList = CreatureRegistry::LevelListFor(source); !levelList.empty()) source = levelList;
			if (source == fs::path(path)) return originalOpenByPath(status, file, path, access);
			const auto text = source.string();
			return originalOpenByPath(status, file, text.c_str(), access);
		}

		// After the game has added its own folders (this is the last of them), ours go in front.
		void __fastcall AddLooseFoldersDetour(std::uint8_t* resources) {
			originalAddLooseFolders(resources);
			if (resources != nullptr) AddSources(resources);
		}
	}

	std::vector<fs::path> ModData::DataFolders() {
		EnsureOpenOrder();
		return openOrder;
	}

	fs::path ModData::Find(const std::string& underData) {
		const auto found = FromSources(underData);
		if (!found.empty()) return found;
		std::error_code error;
		const auto game = fs::absolute(fs::path("Data") / underData, error);
		return fs::is_regular_file(game, error) ? game : fs::path{};
	}

	void ModData::Install() {
		const auto open = Game::Resolve(Game::Address::FileOpenByPath);
		// push -1; push <handler>; mov eax, fs:[0]
		if (!Memory::Matches(open, { 0x6A, 0xFF, 0x68 })
			|| !HookManager::CreateHook(open, reinterpret_cast<void*>(&OpenByPathDetour), reinterpret_cast<void**>(&originalOpenByPath))
			|| !HookManager::EnableHook(open)) {
			HookManager::RemoveHook(open);
			Logger::Warning("Mod data: unexpected game code, level files and creature lists are not taken from mods or Flame packs.");
		}
		const auto target = Game::Resolve(Game::Address::ResourcesAddLooseFolders);
		addFolder = reinterpret_cast<AddFolderFn>(Game::Resolve(Game::Address::ResourcesAddFolder));
		// push ecx; push esi; mov esi, ecx; push edi; lea ecx, [esp + 8]
		if (!Memory::Matches(target, { 0x51, 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0x4C, 0x24, 0x08 })
			|| !HookManager::CreateHook(target, reinterpret_cast<void*>(&AddLooseFoldersDetour), reinterpret_cast<void**>(&originalAddLooseFolders))
			|| !HookManager::EnableHook(target)) {
			HookManager::RemoveHook(target);
			Logger::Warning("Mod data: unexpected game code, mods and Flame packs cannot add game files.");
		}
	}
}
