#include "CameraControls.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "SessionOptions.h"
#include "StringUtils.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// The game's key actions.
		constexpr std::int32_t ActionZoomIn = 0x0E;
		constexpr std::int32_t ActionZoomOut = 0x0F;
		constexpr std::int32_t ActionUp = 0x10;
		constexpr std::int32_t ActionDown = 0x11;
		constexpr std::int32_t ActionLeft = 0x12;
		constexpr std::int32_t ActionRight = 0x13;
		constexpr std::int32_t ActionAllyWindow = 0x24;
		constexpr std::size_t ActionEntrySize = 8;
		// DirectInput key codes.
		constexpr std::int32_t KeyW = 0x11;
		constexpr std::int32_t KeyY = 0x15;
		constexpr std::int32_t KeyA = 0x1E;
		constexpr std::int32_t KeyS = 0x1F;
		constexpr std::int32_t KeyD = 0x20;
		constexpr std::int32_t KeyUp = 0xC8;
		constexpr std::int32_t KeyLeft = 0xCB;
		constexpr std::int32_t KeyRight = 0xCD;
		constexpr std::int32_t KeyDown = 0xD0;
		// Ctrl + S and Alt + A are the game's own, so WASD does not move the view with Ctrl or Alt held.
		constexpr std::uint32_t CtrlOrAlt = 2 | 4;

		bool wasdCamera = false;
		bool wheelZoom = false;
		bool cameraReset = false;

		std::int32_t ActionKey(std::int32_t action) {
			return Load<std::int32_t>(reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::ActionKeyTable)) + action * ActionEntrySize);
		}
		using KeyHeldFn = std::int32_t(__cdecl*)(std::int32_t key);
		bool KeyHeld(std::int32_t key) { return reinterpret_cast<KeyHeldFn>(Game::Resolve(Game::Address::IsKeyHeld))(key) != 0; }

		// The extra key of a movement action, while the action still has its default arrow key. 0: none.
		std::int32_t ExtraKey(std::int32_t action) {
			switch (action) {
				case ActionUp: return ActionKey(action) == KeyUp ? KeyW : 0;
				case ActionDown: return ActionKey(action) == KeyDown ? KeyS : 0;
				case ActionLeft: return ActionKey(action) == KeyLeft ? KeyA : 0;
				case ActionRight: return ActionKey(action) == KeyRight ? KeyD : 0;
				default: return 0;
			}
		}

		// ---- The wheel: each notch counts as the game's zoom key being seen held this many times. The mouse data can
		// come on another thread than the polling. Polled only in a level, so turns left over from a menu are dropped.
		constexpr std::int32_t ZoomPollsPerNotch = 6;
		constexpr std::int32_t MaxZoomPolls = ZoomPollsPerNotch * 4;
		constexpr std::int32_t WheelNotch = 120;
		constexpr ULONGLONG WheelForgetMs = 500;
		std::atomic<std::int32_t> zoomPolls = 0;
		std::atomic<ULONGLONG> lastWheel = 0;

		// Takes one poll of the wanted direction (positive zooms in).
		bool TakeZoomPoll(std::int32_t direction) {
			if (GetTickCount64() - lastWheel.load() > WheelForgetMs) {
				zoomPolls = 0;
				return false;
			}
			const std::int32_t polls = zoomPolls.load();
			if (polls * direction <= 0) return false;
			zoomPolls -= direction;
			return true;
		}

		// ---- Middle click: the view's zoom and angles go back to what they were when the level started (taken at the
		// first poll of the view's keys in a level, after any opening camera path), through the game's own smooth move
		// (as when changing views), with the view staying over the same spot. Only this machine's view changes: nothing
		// goes into the game's simulation or over the network.
		constexpr ULONGLONG ResetForgetMs = 500;
		constexpr std::int32_t ResetMoveMs = 500;
		std::atomic<ULONGLONG> resetClick = 0;
		// Set at each level start; the view is taken at the next poll in a keeper view.
		std::atomic<bool> startViewWanted = false;
		struct StartView {
			bool taken = false;
			std::int32_t zoom = 0;
			std::uint16_t turn = 0;
			std::uint16_t tilt = 0;
			std::uint16_t lean = 0;
		};
		StartView startView;

		std::uint8_t* World() { return *reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::WorldPointer)); }
		// This machine's camera (the bridge's), or nullptr outside a level.
		std::uint8_t* LocalCamera() {
			const auto* world = World();
			if (world == nullptr) return nullptr;
			const auto* session = Load<const std::uint8_t*>(world + Game::Offset::WorldSession);
			if (session == nullptr) return nullptr;
			auto* bridge = Load<std::uint8_t*>(session + Game::Offset::SessionBridge);
			if (bridge == nullptr) return nullptr;
			const auto getCamera = Load<const std::uintptr_t*>(bridge)[Game::Offset::BridgeGetCamera / sizeof(std::uintptr_t)];
			return reinterpret_cast<std::uint8_t*(__fastcall*)(std::uint8_t*)>(getCamera)(bridge);
		}
		// The game's keeper views (not in a creature, not on a camera path).
		bool KeeperView(std::uint32_t mode) {
			switch (mode) {
				case 3: case 4: case 5: case 9: case 13: case 14: case 15: return true;
				default: return false;
			}
		}
		void OnLevelStart() {
			startViewWanted = true;
			resetClick = 0;
		}
		// Called each time the game polls the view's keys (it does only while the view can be moved by the player).
		void ServiceReset() {
			const bool wantStart = startViewWanted.load();
			const ULONGLONG click = resetClick.load();
			if (!wantStart && click == 0) return;
			auto* camera = LocalCamera();
			if (camera == nullptr) return;
			const auto mode = Load<std::uint32_t>(camera + Game::Offset::CameraMode);
			if (!KeeperView(mode) || Load<std::uint32_t>(camera + Game::Offset::CameraTweenEnd) != 0) return;
			if (startViewWanted.exchange(false)) {
				startView.taken = true;
				startView.zoom = Load<std::int32_t>(camera + Game::Offset::CameraZoom);
				startView.turn = Load<std::uint16_t>(camera + Game::Offset::CameraTurn);
				startView.tilt = Load<std::uint16_t>(camera + Game::Offset::CameraTilt);
				startView.lean = Load<std::uint16_t>(camera + Game::Offset::CameraLean);
				Logger::Debug("Camera controls: the level's starting view is zoom " + std::to_string(startView.zoom) + ", turn " + std::to_string(startView.turn)
					+ ", tilt " + std::to_string(startView.tilt) + ".");
			}
			if (click == 0) return;
			resetClick = 0;
			if (!startView.taken || GetTickCount64() - click > ResetForgetMs) return;
			// Already there: nothing to move.
			if (Load<std::int32_t>(camera + Game::Offset::CameraZoom) == startView.zoom && Load<std::uint16_t>(camera + Game::Offset::CameraTurn) == startView.turn
				&& Load<std::uint16_t>(camera + Game::Offset::CameraTilt) == startView.tilt) return;
			// The move's targets: the same spot, values and mode, but the starting zoom and angles.
			std::memcpy(camera + Game::Offset::CameraTweenPosition, camera + Game::Offset::CameraPosition, 3 * sizeof(std::int32_t));
			Store<std::int32_t>(camera + Game::Offset::CameraTweenZoom, startView.zoom);
			Store<std::uint16_t>(camera + Game::Offset::CameraTweenTurn, startView.turn);
			Store<std::uint16_t>(camera + Game::Offset::CameraTweenTilt, startView.tilt);
			Store<std::uint16_t>(camera + Game::Offset::CameraTweenLean, startView.lean);
			Store<std::int32_t>(camera + Game::Offset::CameraTweenViewA, Load<std::int32_t>(camera + Game::Offset::CameraViewA));
			Store<std::int32_t>(camera + Game::Offset::CameraTweenViewB, Load<std::int32_t>(camera + Game::Offset::CameraViewB));
			Store<std::uint32_t>(camera + Game::Offset::CameraTweenMode, mode);
			using StartMoveFn = void(__fastcall*)(std::uint8_t* camera, void* unused, std::int32_t milliseconds);
			reinterpret_cast<StartMoveFn>(Game::Resolve(Game::Address::CameraFinishTween))(camera, nullptr, ResetMoveMs);
		}

		using KeyActionHeldFn = std::uint32_t(__fastcall*)(void* self, void* unused, std::int32_t action, std::uint32_t modifiers, std::int32_t ignoreModifiers);
		KeyActionHeldFn originalActionHeld = nullptr;
		std::uint32_t __fastcall ActionHeldDetour(void* self, void* unused, std::int32_t action, std::uint32_t modifiers, std::int32_t ignoreModifiers) {
			// Zoom out is polled once per pass of the view's keys (0x4066A0), only while the player can move the view.
			if (cameraReset && action == ActionZoomOut) ServiceReset();
			const std::uint32_t held = originalActionHeld(self, unused, action, modifiers, ignoreModifiers);
			if (held != 0) return held;
			if (wheelZoom && action == ActionZoomIn) return TakeZoomPoll(1) ? 1 : 0;
			if (wheelZoom && action == ActionZoomOut) return TakeZoomPoll(-1) ? 1 : 0;
			if (wasdCamera && (modifiers & CtrlOrAlt) == 0) {
				const std::int32_t extra = ExtraKey(action);
				if (extra != 0 && KeyHeld(extra)) return 1;
			}
			return 0;
		}

		// A now moves the view, so the ally window it opened is on Y (free by default).
		using KeyMatchesFn = std::uint32_t(__fastcall*)(void* self, void* unused, std::int32_t key, std::int32_t action, std::uint32_t modifiers, std::int32_t ignoreModifiers);
		KeyMatchesFn originalKeyMatches = nullptr;
		std::uint32_t __fastcall KeyMatchesDetour(void* self, void* unused, std::int32_t key, std::int32_t action, std::uint32_t modifiers, std::int32_t ignoreModifiers) {
			if (action == ActionAllyWindow && ActionKey(ActionAllyWindow) == KeyA) {
				if (key == KeyA) return 0;
				if (key == KeyY) key = KeyA;
			}
			return originalKeyMatches(self, unused, key, action, modifiers, ignoreModifiers);
		}

		// Entries of { offset, data, time, sequence }; the game only passes the wheel on, and nothing uses it.
		constexpr std::size_t MouseDataEntrySize = 0x10;
		constexpr std::uint32_t MouseOffsetWheel = 8;
		// The middle button; its data has 0x80 set when pressed. (The game passes it on as key 0xF2, bound to nothing
		// by default.)
		constexpr std::uint32_t MouseOffsetMiddle = 0xE;
		constexpr std::uintptr_t MouseData = 0x30;
		using MouseDataFn = void(__fastcall*)(std::uint8_t* mouse, void* unused, std::int32_t count);
		MouseDataFn originalMouseData = nullptr;
		void __fastcall MouseDataDetour(std::uint8_t* mouse, void* unused, std::int32_t count) {
			const auto* data = *reinterpret_cast<const std::uint8_t* const*>(mouse + MouseData);
			for (std::int32_t i = 0; data != nullptr && i < count; ++i) {
				const auto* entry = data + static_cast<std::size_t>(i) * MouseDataEntrySize;
				if (cameraReset && Load<std::uint32_t>(entry) == MouseOffsetMiddle) {
					if ((Load<std::uint32_t>(entry + 4) & 0x80) != 0) resetClick = (std::max)(GetTickCount64(), 1ull);
					continue;
				}
				if (!wheelZoom || Load<std::uint32_t>(entry) != MouseOffsetWheel) continue;
				const auto delta = Load<std::int32_t>(entry + 4);
				if (delta == 0) continue;
				const std::int32_t notches = (std::max)(1, std::abs(delta) / WheelNotch);
				const std::int32_t direction = delta > 0 ? 1 : -1;
				// Turning the other way replaces what was left of the last turn.
				const std::int32_t left = zoomPolls.load();
				const std::int32_t kept = left * direction > 0 ? left : 0;
				zoomPolls = std::clamp(kept + direction * notches * ZoomPollsPerNotch, -MaxZoomPolls, MaxZoomPolls);
				lastWheel = GetTickCount64();
			}
			originalMouseData(mouse, unused, count);
		}

		bool Hook(std::uintptr_t target, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
		bool SettingOn(const char* name) {
			return StringUtils::ToLower(Config::GetString(name, "on")) != "off";
		}
	}

	void CameraControls::Install() {
		const bool wantWasd = SettingOn("wasd_camera");
		const bool wantWheel = SettingOn("wheel_zoom");
		const bool wantReset = SettingOn("middle_click_camera_reset");
		if (!wantWasd && !wantWheel && !wantReset) return;
		// "mov ecx, [esp + 4]; push esi; mov eax, [ecx * 8 + action key table]"
		if (!Hook(Game::Resolve(Game::Address::IsActionKeyHeld), { 0x8B, 0x4C, 0x24, 0x04, 0x56, 0x8B, 0x04, 0xCD, 0x8F, 0x9D, 0x75, 0x00 },
			reinterpret_cast<void*>(&ActionHeldDetour), reinterpret_cast<void**>(&originalActionHeld))) {
			Logger::Warning("Camera controls: unexpected game code at the key actions, WASD, wheel zoom and the middle click reset are off.");
			return;
		}
		if (wantWasd) {
			wasdCamera = true;
			if (!Hook(Game::Resolve(Game::Address::ActionKeyMatches), { 0x8B, 0x4C, 0x24, 0x08, 0x56, 0x8B, 0x04, 0xCD, 0x8F, 0x9D, 0x75, 0x00 },
				reinterpret_cast<void*>(&KeyMatchesDetour), reinterpret_cast<void**>(&originalKeyMatches))) {
				Logger::Warning("Camera controls: unexpected game code at the key presses, A also still opens the ally window.");
			}
		}
		if (wantWheel || wantReset) {
			// "push ecx; mov eax, [esp + 8]; push ebp; mov ebp, ecx; push esi; mov ecx, eax; dec eax"
			const bool mouseHooked = Hook(Game::Resolve(Game::Address::MouseHandleData), { 0x51, 0x8B, 0x44, 0x24, 0x08, 0x55, 0x8B, 0xE9, 0x56, 0x8B, 0xC8, 0x48 },
				reinterpret_cast<void*>(&MouseDataDetour), reinterpret_cast<void**>(&originalMouseData));
			if (!mouseHooked) Logger::Warning("Camera controls: unexpected game code at the mouse data, wheel zoom and the middle click reset are off.");
			// "push esi; push edi; mov edi, [esp + 0xC]; mov esi, ecx; test edi, edi"
			const bool moveFound = Memory::Matches(Game::Resolve(Game::Address::CameraFinishTween), { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1, 0x85, 0xFF });
			if (wantReset && mouseHooked && !moveFound) Logger::Warning("Camera controls: unexpected game code at the camera's move, the middle click reset is off.");
			wheelZoom = wantWheel && mouseHooked;
			cameraReset = wantReset && mouseHooked && moveFound;
			if (cameraReset) SessionOptions::OnLevelStart(&OnLevelStart);
		}
		Logger::Info(std::string("Camera controls: WASD ") + (wasdCamera ? "on" : "off") + ", wheel zoom " + (wheelZoom ? "on" : "off")
			+ ", middle click reset " + (cameraReset ? "on" : "off") + ".");
	}
}
