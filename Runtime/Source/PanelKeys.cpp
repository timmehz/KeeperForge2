#include "PanelKeys.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "LevelEditor.h"
#include "Logger.h"
#include "Memory.h"
#include "StringUtils.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace KeeperForge2 {
	namespace {
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// DirectInput key codes by name.
		struct KeyName { const char* name; std::int32_t code; };
		constexpr KeyName KeyNames[] = {
			{ "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 }, { "5", 0x06 }, { "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0A }, { "0", 0x0B },
			{ "q", 0x10 }, { "w", 0x11 }, { "e", 0x12 }, { "r", 0x13 }, { "t", 0x14 }, { "y", 0x15 }, { "u", 0x16 }, { "i", 0x17 }, { "o", 0x18 }, { "p", 0x19 },
			{ "a", 0x1E }, { "s", 0x1F }, { "d", 0x20 }, { "f", 0x21 }, { "g", 0x22 }, { "h", 0x23 }, { "j", 0x24 }, { "k", 0x25 }, { "l", 0x26 },
			{ "z", 0x2C }, { "x", 0x2D }, { "c", 0x2E }, { "v", 0x2F }, { "b", 0x30 }, { "n", 0x31 }, { "m", 0x32 },
			{ "f1", 0x3B }, { "f2", 0x3C }, { "f3", 0x3D }, { "f4", 0x3E }, { "f5", 0x3F }, { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 }, { "f10", 0x44 }, { "f11", 0x57 }, { "f12", 0x58 },
			{ "numpad0", 0x52 }, { "numpad1", 0x4F }, { "numpad2", 0x50 }, { "numpad3", 0x51 }, { "numpad4", 0x4B }, { "numpad5", 0x4C }, { "numpad6", 0x4D }, { "numpad7", 0x47 }, { "numpad8", 0x48 }, { "numpad9", 0x49 },
			{ "grave", 0x29 }, { "`", 0x29 }, { "~", 0x29 }, { "escape", 0x01 }, { "esc", 0x01 },
		};
		constexpr std::int32_t NoKey = 0;

		// The tabs, as the game's own buttons for them are set up. Panels 1 to 3 are in that order from left to right after
		// Creatures, which is panel 4.
		struct Tab { const char* setting; const char* defaultKey; std::int32_t button; std::int32_t panel; std::int32_t key; };
		std::array<Tab, 4> tabs = { {
			{ "panel_key_creatures", "1", 0xF8, 4, NoKey },
			{ "panel_key_rooms", "2", 0xFB, 1, NoKey },
			{ "panel_key_spells", "3", 0xFA, 2, NoKey },
			{ "panel_key_workshop", "4", 0xF9, 3, NoKey },
		} };

		// The Sell button: its click sends the click action with its id and sets these. The handler itself cannot be called
		// from a key, as it reads the id through a pointer that is only known to be set during a click. Pressed again the
		// key lets go of Sell, as the game does on a right click.
		constexpr const char* SellSetting = "sell_key";
		constexpr const char* SellDefaultKey = "grave";
		constexpr std::int32_t SellButton = 255;
		constexpr std::int32_t SellTool = 4;
		constexpr std::int32_t NoTool = 0;
		std::int32_t sellKey = NoKey;
		void ToggleSell(std::uint8_t* playerInterface) {
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceTool) == SellTool) {
				Store<std::int32_t>(playerInterface + Game::Offset::InterfaceTool, NoTool);
				Store<std::int32_t>(playerInterface + Game::Offset::InterfaceToolDragging, 0);
				Store<std::int32_t>(playerInterface + Game::Offset::InterfaceToolArea, 0);
				reinterpret_cast<void(__fastcall*)(std::uint8_t*, void*, std::int32_t, std::int32_t)>(Game::Resolve(Game::Address::CursorSetState))(playerInterface + Game::Offset::InterfaceCursor, nullptr, 0, 0);
				return;
			}
			reinterpret_cast<void(__fastcall*)(std::uint8_t*, void*, std::int32_t, std::int32_t)>(Game::Resolve(Game::Address::InterfaceClickAction))(playerInterface, nullptr, 0, SellButton);
			Store<std::int32_t>(playerInterface + Game::Offset::InterfaceTool, SellTool);
			Store<std::int32_t>(playerInterface + Game::Offset::InterfaceToolDragging, 0);
		}

		// The options menu. The key event opens it on the game's own Escape action, but it was reported not to, by a cause not
		// found yet. So if the menu did not change during the key event the key toggles it here, with the Options button's own
		// click. As that only runs after the event, it can never toggle the menu twice.
		constexpr const char* OptionsSetting = "options_key";
		constexpr const char* OptionsDefaultKey = "escape";
		constexpr int MaxOptionsLogs = 8;
		std::int32_t optionsKey = NoKey;
		int optionsLogs = 0;

		constexpr std::uint32_t CtrlOrAlt = 2 | 4;
		constexpr std::uint32_t PathCameraMode = 18;
		// Spectating after a defeat in multiplayer, as the key event itself checks.
		constexpr std::uint32_t PlayerSpectatingFlag = 0x80000;
		constexpr std::uintptr_t PlayerFlags = 0x76;

		const std::uint8_t* Camera(const std::uint8_t* playerInterface) {
			const auto* owner = *reinterpret_cast<const std::uint8_t* const*>(playerInterface + Game::Offset::InterfaceOwner);
			auto* bridge = *reinterpret_cast<std::uint8_t* const*>(owner + Game::Offset::OwnerBridge);
			const auto getCamera = (*reinterpret_cast<const std::uintptr_t* const*>(bridge))[Game::Offset::BridgeGetCamera / sizeof(std::uintptr_t)];
			return reinterpret_cast<const std::uint8_t*(__fastcall*)(std::uint8_t*)>(getCamera)(bridge);
		}
		// Only the view from above has the side panel. In a creature keys 1 to 4 are its abilities.
		bool PanelInUse(const std::uint8_t* playerInterface) {
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceKeysHeld) != 0) return false;
			const auto* things = reinterpret_cast<const std::uint8_t* const*>(Game::Resolve(Game::Address::ThingTable));
			const auto* player = things[Load<std::uint16_t>(playerInterface + Game::Offset::InterfacePlayerTag)];
			if (player == nullptr || Load<std::uint16_t>(player + Game::Offset::PlayerPossessed) != 0 || (Load<std::uint32_t>(player + PlayerFlags) & PlayerSpectatingFlag) != 0) return false;
			const auto* camera = Camera(playerInterface);
			return camera != nullptr && Load<std::uint32_t>(camera + Game::Offset::CameraMode) != PathCameraMode;
		}
		// Not while typing, nor when the key means something else to the game: Escape skips a camera path or move.
		bool OptionsMayToggle(const std::uint8_t* playerInterface) {
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceKeysHeld) != 0 || Load<std::int32_t>(playerInterface + Game::Offset::InterfaceTyping) != 0) return false;
			const auto* camera = Camera(playerInterface);
			return camera != nullptr && Load<std::uint32_t>(camera + Game::Offset::CameraMode) != PathCameraMode
				&& Load<std::int32_t>(camera + Game::Offset::CameraMoveEnd) == 0 && Load<std::int32_t>(camera + Game::Offset::CameraTurnEnd) == 0;
		}
		void LogOptionsKey(const std::uint8_t* playerInterface, bool mayToggle, std::int32_t before, std::int32_t after) {
			if (optionsLogs >= MaxOptionsLogs) return;
			++optionsLogs;
			Logger::Debug(std::format("Panel keys: options key, the menu went {} to {} in the game's key event; may toggle {}; held {}, typing {}, focus {} {}, other {}, shown {}.", before, after, mayToggle,
				Load<std::int32_t>(playerInterface + Game::Offset::InterfaceKeysHeld), Load<std::int32_t>(playerInterface + Game::Offset::InterfaceTyping),
				Load<std::int32_t>(playerInterface + Game::Offset::InterfaceFocus), Load<std::int32_t>(playerInterface + Game::Offset::InterfaceFocus + 4),
				Load<std::int32_t>(playerInterface + Game::Offset::InterfaceKeysOther), Load<std::int32_t>(playerInterface + Game::Offset::InterfacePanelShown)));
		}

		using TabClickFn = void(__cdecl*)(std::int32_t button, std::int32_t panel, std::uint8_t* playerInterface);
		using KeyEventFn = std::int32_t(__cdecl*)(std::int32_t key, std::int32_t pressed, std::uint32_t modifiers, std::uint8_t* playerInterface);
		KeyEventFn originalKeyEvent = nullptr;
		std::int32_t __cdecl KeyEventDetour(std::int32_t key, std::int32_t pressed, std::uint32_t modifiers, std::uint8_t* playerInterface) {
			if (LevelEditor::OnKey(key, pressed, modifiers, playerInterface)) return 1;
			const bool plainPress = pressed != 0 && (modifiers & CtrlOrAlt) == 0;
			if (plainPress) {
				if (sellKey != NoKey && sellKey == key && PanelInUse(playerInterface)) ToggleSell(playerInterface);
				for (const auto& tab : tabs) {
					if (tab.key == NoKey || tab.key != key) continue;
					// The click itself looks at Shift, so Shift with the key pins the panel as Shift with a click does.
					if (PanelInUse(playerInterface)) reinterpret_cast<TabClickFn>(Game::Resolve(Game::Address::PanelTabClick))(tab.button, tab.panel, playerInterface);
					break;
				}
			}
			const bool optionsPress = plainPress && optionsKey != NoKey && optionsKey == key;
			const bool mayToggle = optionsPress && OptionsMayToggle(playerInterface);
			const auto menuBefore = Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen);
			const auto result = originalKeyEvent(key, pressed, modifiers, playerInterface);
			if (!optionsPress) return result;
			const auto menuAfter = Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen);
			LogOptionsKey(playerInterface, mayToggle, menuBefore, menuAfter);
			if (mayToggle && menuAfter == menuBefore) {
				reinterpret_cast<TabClickFn>(Game::Resolve(Game::Address::InterfaceToggleOptions))(0, 0, playerInterface);
				// The key event's own call also brings the panels back.
				Store<std::int32_t>(playerInterface + Game::Offset::InterfacePanelShown, 1);
			}
			return result;
		}

		std::int32_t KeyCode(const std::string& setting, const std::string& value) {
			const auto name = StringUtils::ToLower(value);
			if (name == "off") return NoKey;
			for (const auto& key : KeyNames) {
				if (name == key.name) return key.code;
			}
			Logger::Warning("Panel keys: " + setting + " has an unknown key, " + value + ", so it is off.");
			return NoKey;
		}
	}

	void PanelKeys::Install() {
		bool any = false;
		std::string keys;
		for (auto& tab : tabs) {
			const auto value = Config::GetString(tab.setting, tab.defaultKey);
			tab.key = KeyCode(tab.setting, value);
			any = any || tab.key != NoKey;
			keys += std::string(keys.empty() ? "" : ", ") + tab.setting + " " + value;
		}
		const auto sellValue = Config::GetString(SellSetting, SellDefaultKey);
		sellKey = KeyCode(SellSetting, sellValue);
		any = any || sellKey != NoKey;
		keys += std::string(", ") + SellSetting + " " + sellValue;
		const auto optionsValue = Config::GetString(OptionsSetting, OptionsDefaultKey);
		optionsKey = KeyCode(OptionsSetting, optionsValue);
		any = any || optionsKey != NoKey;
		keys += std::string(", ") + OptionsSetting + " " + optionsValue;
		if (!any) return;
		const auto target = Game::Resolve(Game::Address::InterfaceKeyEvent);
		// "push -1; push <handler>; mov eax, fs:[0]"
		if (!Memory::Matches(target, { 0x6A, 0xFF, 0x68, 0xF0, 0x8C, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 })) {
			Logger::Warning("Panel keys: unexpected game code at the key event, the keys are off.");
			return;
		}
		if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&KeyEventDetour), reinterpret_cast<void**>(&originalKeyEvent)) || !HookManager::EnableHook(target)) {
			HookManager::RemoveHook(target);
			Logger::Warning("Panel keys: could not hook the key event, the keys are off.");
			return;
		}
		Logger::Debug("Panel keys: " + keys + ".");
	}
}
