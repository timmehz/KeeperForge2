#include "Autosave.h"
#include "Config.h"
#include "CoopCampaign.h"
#include "FrameRate.h"
#include "GameAddresses.h"
#include "LevelEditor.h"
#include "Logger.h"
#include "Memory.h"
#include "StringUtils.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

// Ported from Flame: autosave (the timer, the file name, keeping the last few, refreshing the save list). Counting game
// turns instead of the clock is ours.
namespace KeeperForge2 {
	namespace {
		std::uint32_t intervalMinutes = 0;
		std::size_t keepCount = 3;

		// The world the turns were counted in, the turn of the last save (or of the start of counting), and the turn seen
		// last frame (a jump means a saved game was loaded).
		const std::uint8_t* countedWorld = nullptr;
		std::uint32_t lastSaveTurn = 0;
		std::uint32_t lastSeenTurn = 0;
		bool counting = false;

		// The pattern the save list keeps a pointer to.
		constexpr char SavePattern[] = "*.sav";
		constexpr char AutosavePrefix[] = "autosave-";

		std::uint32_t ParseNumber(const std::string& text, std::uint32_t fallback) {
			std::uint32_t value = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			return error == std::errc{} && end == text.data() + text.size() ? value : fallback;
		}

		std::uint8_t* World() { return Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer)); }
		bool GameRunning() {
			const auto playerInterface = Game::Resolve(Game::Address::PlayerInterface);
			return Memory::Read<void*>(playerInterface + Game::Offset::InterfaceSession) != nullptr;
		}
		bool SinglePlayer() {
			const auto mode = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode));
			return mode != Game::PlayModeMultiplayer && !CoopCampaign::InSession();
		}
		std::uint32_t GameTurn(std::uint8_t* world) {
			return reinterpret_cast<std::uint32_t(__fastcall*)(void*)>(Game::Resolve(Game::Address::WorldGameTurn))(world);
		}
		std::uint32_t TurnsPerSecond(const std::uint8_t* world) {
			const auto* session = *reinterpret_cast<std::uint8_t* const*>(world + Game::Offset::WorldSession);
			const std::uint32_t turns = session != nullptr ? *reinterpret_cast<const std::uint32_t*>(session + Game::Offset::SessionTurnsPerSecond) : 0;
			// The game's default when the session has none.
			return turns != 0 ? turns : 20;
		}

		std::string FileName() {
			// The name only: the date and time on this machine.
			const std::time_t now = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &now);
			char text[64]{};
			std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
			return std::string(AutosavePrefix) + text + ".SAV";
		}

		// Keeps the newest keepCount autosaves (by the time written) and removes the rest.
		void RemoveOldAutosaves(const fs::path& folder) {
			std::error_code error;
			std::vector<std::pair<fs::file_time_type, fs::path>> autosaves;
			for (const auto& entry : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, error)) {
				if (!entry.is_regular_file(error)) continue;
				const auto name = StringUtils::ToLower(entry.path().filename().string());
				if (!name.starts_with(AutosavePrefix) || !name.ends_with(".sav")) continue;
				autosaves.emplace_back(entry.last_write_time(error), entry.path());
			}
			if (autosaves.size() <= keepCount) return;
			std::ranges::sort(autosaves, [](const auto& a, const auto& b) { return a.first > b.first; });
			for (std::size_t i = keepCount; i < autosaves.size(); ++i) {
				if (fs::remove(autosaves[i].second, error)) Logger::Debug("Autosave: removed the old autosave " + autosaves[i].second.filename().string() + ".");
			}
		}

		// The in-game save and load windows keep a list of the saved games; it is filled again so the new one shows.
		void RefreshSaveList(const char* folder) {
			auto* list = reinterpret_cast<void*>(Game::Resolve(Game::Address::InGameSaveList));
			reinterpret_cast<void(__fastcall*)(void*, void*)>(Game::Resolve(Game::Address::FileListReset))(list, nullptr);
			reinterpret_cast<std::uint8_t(__fastcall*)(void*, void*, const char*, const char*, std::uint32_t)>(
				Game::Resolve(Game::Address::FileListCollect))(list, nullptr, folder, SavePattern, 1);
		}

		void SaveNow(std::uint8_t* world) {
			const auto* folderText = reinterpret_cast<const char*>(Game::Resolve(Game::Address::SavesFolder));
			const fs::path folder(folderText);
			std::error_code error;
			// As the game's own quicksave: the folder is made when it is missing.
			fs::create_directories(folder, error);
			const std::string path = (folder / FileName()).string();
			// The world's save to a file, thiscall (world, path) ret 4, 1 when written.
			const auto save = reinterpret_cast<std::int32_t(__fastcall*)(void*, void*, const char*)>(Game::Resolve(Game::Address::WorldSaveFile));
			if (save(world, nullptr, path.c_str()) == 0) {
				Logger::Warning("Autosave: the game could not write " + path + ".");
				return;
			}
			Logger::Info("Autosave: saved " + fs::path(path).filename().string() + ".");
			RemoveOldAutosaves(folder);
			RefreshSaveList(folderText);
		}

		void AfterPresent() {
			std::uint8_t* world = World();
			if (world == nullptr || !GameRunning() || !SinglePlayer() || LevelEditor::InSession()) {
				counting = false;
				return;
			}
			const std::uint32_t turn = GameTurn(world);
			const std::uint32_t perSecond = TurnsPerSecond(world);
			// A new level, another world, or a jump in turns (a saved game loaded): count from here.
			if (!counting || world != countedWorld || turn < lastSeenTurn || turn - lastSeenTurn > perSecond * 5) {
				counting = true;
				countedWorld = world;
				lastSaveTurn = turn;
				lastSeenTurn = turn;
				return;
			}
			lastSeenTurn = turn;
			const std::uint32_t interval = intervalMinutes * 60 * perSecond;
			if (turn - lastSaveTurn < interval) return;
			lastSaveTurn = turn;
			SaveNow(world);
		}
	}

	void Autosave::Install() {
		const auto setting = StringUtils::ToLower(Config::GetString("autosave", "5"));
		keepCount = (std::max<std::size_t>)(1, ParseNumber(Config::GetString("autosave_keep", "3"), 3));
		if (setting == "off") {
			Logger::Info("Autosave: off.");
			return;
		}
		intervalMinutes = ParseNumber(setting, 5);
		if (intervalMinutes == 0) intervalMinutes = 5;
		if (!FrameRate::IsPresentHooked()) {
			Logger::Warning("Autosave: the frame hook is not in place, so the game is not autosaved.");
			return;
		}
		FrameRate::AddAfterPresent(&AfterPresent);
		Logger::Info("Autosave: every " + std::to_string(intervalMinutes) + " min of play in single player, keeping the last "
			+ std::to_string(keepCount) + ".");
	}
}
