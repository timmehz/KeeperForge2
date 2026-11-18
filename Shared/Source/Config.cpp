#include "Config.h"
#include "ConfigFile.h"
#include "DisplayUtils.h"
#include "Paths.h"
#include "StringUtils.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>
namespace KeeperForge2 {
	namespace {
		using Check = std::function<bool(const std::string&)>;

		// The whole text as a number, or nothing if it is not only digits.
		std::optional<std::uint32_t> WholeNumber(std::string_view text) {
			std::uint32_t number = 0;
			const auto* end = text.data() + text.size();
			const auto [parsed, error] = std::from_chars(text.data(), end, number);
			if (text.empty() || error != std::errc{} || parsed != end) { return std::nullopt; }
			return number;
		}
		// One of the given words (any case).
		Check OneOf(std::initializer_list<std::string_view> words) {
			std::vector<std::string> list(words.begin(), words.end());
			return [list](const std::string& value) { return std::ranges::contains(list, StringUtils::ToLower(value)); };
		}
		const Check onOff = OneOf({ "on", "off" });
		// A whole number from min to max, or one of the given words (any case).
		Check Number(std::uint32_t min, std::uint32_t max, std::initializer_list<std::string_view> words = {}) {
			Check word = OneOf(words);
			return [min, max, word](const std::string& value) {
				if (word(value)) { return true; }
				const auto number = WholeNumber(value);
				return number && *number >= min && *number <= max;
			};
		}
		constexpr std::uint32_t noMaximum = std::numeric_limits<std::uint32_t>::max();
		// auto, off or WIDTHxHEIGHT.
		bool ValidResolution(const std::string& value) {
			const auto lower = StringUtils::ToLower(value);
			std::uint32_t width = 0, height = 0;
			return lower == "auto" || lower == "off" || DisplayUtils::ParseResolution(lower, width, height);
		}
		// game, or six hex digits.
		bool ValidColor(const std::string& value) {
			if (StringUtils::ToLower(value) == "game") { return true; }
			return value.size() == 6 && std::ranges::all_of(value, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
		}
		// One or two room ids (up to three digits each) joined with +. Whether the ids are rooms is checked where they
		// are used.
		bool ValidRoomIds(const std::string& value) {
			std::size_t ids = 0;
			std::size_t digits = 0;
			for (const char c : value + "+") {
				if (c == '+') {
					if (digits == 0 || ++ids > 2) { return false; }
					digits = 0;
				}
				else if (std::isdigit(static_cast<unsigned char>(c))) {
					if (++digits > 3) { return false; }
				}
				else if (c != ' ' && c != '\t') { return false; }
			}
			return true;
		}

		constexpr const char* configHeader = "# KeeperForge2 configuration\n";
		// Every setting config.yml holds, in the order it is written. Missing ones are added with their default, ones
		// no longer listed here are removed, and values the setting's check turns down go back to the default; the
		// other values already set are kept. Settings whose words are read in more than one way (renderer,
		// anti_aliasing, the keys) are left to the code that reads them.
		const std::vector<ConfigSetting> settings = {
			{ "log_level", "info",
				"Log detail: trace, debug, info, warning, error or critical.",
				OneOf({ "trace", "debug", "info", "warning", "error", "critical" }) },
			{ "logs_kept", "10",
				"Most log files kept in KeeperForge2\\Logs; the oldest are deleted (0 = keep all).",
				Number(0, noMaximum) },
			{ "screenshots_cleared", "on",
				"on: delete the old screenshots in KeeperForge2\\SCRSHOTS every time the game starts.", onOff },
			{ "resolution", "auto",
				"auto (desktop size), WIDTHxHEIGHT such as 1920x1080, or off (the game's 4:3 list).", ValidResolution },
			{ "gui_layout", "anchored",
				"Wide-screen menus and panels: anchored (kept in shape, at the edges) or stretched.",
				OneOf({ "anchored", "stretched" }) },
			{ "widescreen_menus", "on",
				"Main menu at the game's resolution (off = 640x480).", onOff },
			{ "menu_height", "480",
				"Height the main menu is drawn at before scaling. Higher is sharper, but text and videos get smaller.",
				Number(480, noMaximum) },
			{ "fullscreen_videos", "stretch",
				"Videos: stretch (fill the screen), bars (keep their shape) or off.",
				OneOf({ "stretch", "bars", "off" }) },
			{ "memory_4gb", "on",
				"Let the game use 4 GB (runs a patched copy, KeeperForge2-DKII-DX.exe). Experimental; off if it crashes.", onOff },
			{ "field_of_view", "100",
				"3D view width in percent (100 = original). Higher shows more but stretches the edges.",
				Number(50, 300) },
			{ "renderer", "vulkan",
				"dx11, dx12 or vulkan. If it cannot start, the others are tried." },
			{ "window_mode", "borderless",
				"borderless (a window filling the screen) or fullscreen (minimized when you switch away).",
				OneOf({ "borderless", "fullscreen" }) },
			{ "anti_aliasing", "smaa x4",
				"off, or fxaa, smaa or msaa with x2, x4 or x8 (such as smaa x4). msaa is sharpest but slowest." },
			{ "bump_mapping", "all",
				"The game's own bump mapping: all, water (water and the Temple's pool, see bump_temple), lava, or off.",
				// on and luminance are the old words; the runtime turns them into all (and bump_luminance on).
				OneOf({ "all", "water", "lava", "off", "on", "luminance" }) },
			{ "bump_temple", "on",
				"Bump mapping of the Temple's pool too, when water is bump mapped: on or off.", onOff },
			{ "bump_luminance", "off",
				"Bump mapping also makes the liquid lighter and darker: on or off.", onOff },
			{ "panel_text_cache", "on",
				"Redraw panel text only when it changes (off = every frame, for finding drawing problems).", onOff },
			{ "gpu_blits", "on",
				"Draw menus and panels on the graphics card: on, clears (clears only) or off (in memory, slower).",
				OneOf({ "on", "clears", "off" }) },
			{ "frame_rate", "60",
				"Frames per second: a number, 0 for no limit, or vsync. Game speed does not change.",
				Number(15, 1000, { "vsync", "0" }) },
			{ "cpu_threads", "4",
				"CPU threads the game may use: a number, all, or game (1). Experimental; game if it crashes.",
				Number(1, noMaximum, { "all", "game" }) },
			{ "run_in_background", "on",
				"Keep running when minimized or unfocused (muted, 1 fps outside multiplayer). off = pause.", onOff },
			{ "wasd_camera", "on",
				"WASD moves the view like the arrow keys (the ally window moves from A to Y).", onOff },
			{ "wheel_zoom", "on",
				"Mouse wheel zooms the view.", onOff },
			{ "middle_click_camera_reset", "on",
				"Middle mouse button puts the view's zoom and rotation back as at the level's start (it stays where it is).", onOff },
			{ "panel_key_creatures", "1",
				"Keys for the side panel tabs: a letter, a number, F1-F12, numpad0-numpad9, or off. Shift keeps the panel open." },
			{ "panel_key_rooms", "2", "" },
			{ "panel_key_spells", "3", "" },
			{ "panel_key_workshop", "4", "" },
			{ "sell_key", "grave",
				"Key that toggles Sell (grave = the ` ~ key), or off." },
			{ "options_key", "escape",
				"Key that opens and closes the options menu, or off." },
			{ "editor_outline_color", "80C0FF",
				"Editor action point outline color: six hex digits, or game.", ValidColor },
			{ "editor_dark_windows", "on",
				"Editor windows (creature properties) in dark colors. off = Windows' own colors.", onOff },
			{ "editor_check_maps", "off",
				"on: check every editor map once at the next start and copy them to KeeperForge2\\maps (then turns off).", onOff },
			{ "steam_multiplayer", "on",
				"Online play through Steam (needs steam_api.dll and Steam running). TCP/IP games go through Steam.\n"
				"This and steam_player_name start on when steam_api.dll is in the game or KeeperForge2 folder, else off.", onOff },
			{ "steam_lobby", "public",
				"public: every hosted game gets its own Steam lobby and shows in everyone's TCP/IP session list,\n"
				"no need to be friends (refreshed every 15 s, F5 there for at once). friends: only friends you invite.",
				OneOf({ "public", "friends" }) },
			{ "steam_lobby_password", "",
				"Lobby password: only players with the same password share a Steam lobby and can see and join its games.\n"
				"Empty = no password. Use quotes if it has a # in it. Everyone in a lobby must type the same one." },
			{ "steam_player_name", "on",
				"Use your Steam name in multiplayer (first 16 characters). off = the name you type.", onOff },
			{ "mercenary-portal-spawns", "mercenary",
				"Mercenary Portal: mercenary (heroes and the game's mercenaries), normal (heroes only) or off.\n"
				"In multiplayer every player uses the host's mercenary-portal settings.",
				OneOf({ "mercenary", "normal", "off" }) },
			{ "mercenary-portal-stone-knights", "on",
				"Stone Knights can come through the portal.", onOff },
			{ "mercenary-portal-royals", "on",
				"Each keeper gets one royal per game.", onOff },
			{ "mercenary-portal-wizard-room-ids", "6",
				"Rooms each hero needs: one or two ids joined with + (the first room's size sets how often it comes).\n"
				"Treasury 1, Lair 2, Portal 3, Hatchery 4, Heart 5, Library 6, Training 7, Wooden Bridge 8, Guard Room 9,\n"
				"Workshop 10, Prison 11, Torture 12, Temple 13, Graveyard 14, Casino 15, Combat Pit 16, Stone Bridge 17.",
				ValidRoomIds },
			{ "mercenary-portal-guard-room-ids", "7", "", ValidRoomIds },
			{ "mercenary-portal-giant-room-ids", "10", "", ValidRoomIds },
			{ "mercenary-portal-thief-room-ids", "15", "", ValidRoomIds },
			{ "mercenary-portal-knight-room-ids", "16", "", ValidRoomIds },
			{ "mercenary-portal-elven-archer-room-ids", "9", "", ValidRoomIds },
			{ "mercenary-portal-monk-room-ids", "14", "", ValidRoomIds },
			{ "mercenary-portal-fairy-room-ids", "13", "", ValidRoomIds },
			{ "mercenary-portal-royal-guard-room-ids", "12 + 7", "", ValidRoomIds },
			{ "mercenary-portal-stone-knight-room-ids", "12 + 16", "", ValidRoomIds },
			{ "mercenary-portal-royal-room-ids", "2 + 4", "", ValidRoomIds },
			{ "flame_packs", "all",
				"Flame resource packs (flame\\resources\\<pack>\\data) used for game files: all, off, or pack names joined by ;\n"
				"(the first named wins). Mods' own data folders (KeeperForge2\\mods\\<mod>\\data) always come first." },
			{ "keeper-dwarf-workers", "off",
				"Keepers get Dwarfs instead of Imps (the host's setting in multiplayer).", onOff },
			{ "create-imp-price-rises", "on",
				"Create Imp (and Create Dwarf) costs more for each worker the keeper has; off keeps it at its base price\n"
				"(the host's setting in multiplayer).", onOff },
		};
		ConfigFile file;
	}
	bool Config::Load() {
		Paths::CreateFolders();
		// The Steam settings are on by default only when there is a steam_api.dll where the runtime looks for one (the
		// game folder, which is the working directory, or the KeeperForge2 folder). Only defaults: a value already in
		// config.yml is kept.
		std::error_code error;
		const bool steamApi = std::filesystem::is_regular_file("steam_api.dll", error)
			|| std::filesystem::is_regular_file(Paths::Root() / "steam_api.dll", error);
		auto chosen = settings;
		for (auto& setting : chosen) {
			if (setting.key == "steam_multiplayer" || setting.key == "steam_player_name") setting.defaultValue = steamApi ? "on" : "off";
		}
		return file.Load(Paths::Config(), chosen, configHeader);
	}
	std::vector<std::string> Config::LoadChanges() {
		std::vector<std::string> lines;
		for (const auto& change : file.Removed()) {
			lines.push_back("config.yml: removed '" + change.key + ": " + change.value + "', it is not a setting.");
		}
		for (const auto& change : file.Reset()) {
			lines.push_back("config.yml: '" + change.key + ": " + change.value + "' is not a value it can take, set back to the default.");
		}
		return lines;
	}
	std::string Config::GetString(const std::string& key, const std::string& defaultValue) { return file.Get(key, defaultValue); }
	bool Config::SetString(const std::string& key, const std::string& value) { return file.Set(key, value); }
}
