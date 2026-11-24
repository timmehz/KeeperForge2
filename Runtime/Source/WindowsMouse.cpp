#include "WindowsMouse.h"
#include "CameraControls.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "Renderer.h"
#include "StringUtils.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <Windows.h>

// Ported from Flame: patches/replace_mouse_dinput_to_user32.cpp (and its switches in dk2/MyDxMouse.cpp: no DirectInput
// mouse is made, and window activation leaves the missing mouse out). See docs/game/mouse.md.
namespace KeeperForge2 {
	namespace {
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }

		// The game's mouse keys (it puts them after DirectInput's keyboard codes) and its button flags.
		constexpr std::array<std::uint32_t, 4> ButtonKeys = { 0xF0, 0xF1, 0xF2, 0xF3 };
		constexpr std::size_t LeftButton = 0;
		constexpr std::size_t RightButton = 1;
		constexpr std::size_t MiddleButton = 2;
		constexpr std::size_t OtherButton = 3;
		constexpr std::uint32_t ButtonPressedFlag = 0x08;
		constexpr std::uint8_t ButtonPressedData = 0x80;
		constexpr std::uint32_t AxisX = 0;
		constexpr std::uint32_t AxisWheel = 8;
		// Flame's: in a creature the movement counts as if the window were 640x480, times the sensitivity / 10, and the
		// pointer goes back to the middle once it is 20 pixels from it.
		constexpr float LookWidth = 640.0f;
		constexpr float LookHeight = 480.0f;
		constexpr float SensitivityScale = 10.0f;
		constexpr LONG LookRecentre = 20;
		// Every thing by tag (16384 bytes of pointers) [reference: Flame's sceneObjects].
		constexpr std::uint16_t ThingTableSize = 4096;

		std::atomic<bool> on = false;

		std::uint8_t* InputField(std::uintptr_t offset) {
			return Load<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::InputSystem) + offset));
		}

		// ---- Input events. Made as DirectInput's mouse makes them (0x5DDA90) and handed to the control keys object as it
		// does. The game queues them and marks each handled once taken, so each is kept until then (Flame: the
		// *ActionsInProgress lists, freed in the game loop; here before each new one).
		std::mutex eventsMutex;
		std::vector<std::unique_ptr<std::uint8_t[]>> events;
		std::uint8_t* NewEvent(std::size_t size, std::uintptr_t vtable) {
			std::lock_guard lock(eventsMutex);
			std::erase_if(events, [](const auto& event) { return Load<std::int32_t>(event.get() + Game::Offset::InputEventPending) == 0; });
			auto block = std::make_unique<std::uint8_t[]>(size);
			auto* event = block.get();
			Store<std::uintptr_t>(event, Game::Resolve(vtable));
			// DirectInput's time stamps are milliseconds of the system's clock too. Input only, nothing of the simulation.
			Store<std::uint32_t>(event + Game::Offset::InputEventTime, GetTickCount());
			Store<std::int32_t>(event + Game::Offset::InputEventPending, 1);
			events.push_back(std::move(block));
			return event;
		}
		// Not under the lock: the game's handlers may send the window a message, which comes back here.
		void Dispatch(std::uint8_t* event, std::uintptr_t listenerCall) {
			auto* controlKeys = InputField(Game::Offset::InputControlKeys);
			if (controlKeys == nullptr) {
				Store<std::int32_t>(event + Game::Offset::InputEventPending, 0);
				return;
			}
			using ListenerFn = std::int32_t(__fastcall*)(std::uint8_t* listener, void* unused, std::uint8_t* event);
			const auto call = Load<const std::uintptr_t*>(controlKeys)[listenerCall / sizeof(std::uintptr_t)];
			reinterpret_cast<ListenerFn>(call)(controlKeys, nullptr, event);
		}
		// Ported from Flame: move_mouse. Flame leaves it unused; here a movement of nothing follows each new position, so
		// the game does what it does after DirectInput's movement (keeps the mouse in bounds, redraws the pointer, tells
		// its listeners). The wheel goes this way too, as DirectInput's did.
		void SendMove(std::uint32_t axis, std::int32_t amount) {
			auto* event = NewEvent(Game::Offset::MouseMoveEventSize, Game::Address::MouseMoveEventVtable);
			Store<std::uint32_t>(event + Game::Offset::MouseMoveAxis, axis);
			Store<std::int32_t>(event + Game::Offset::MouseMoveAmount, amount);
			Dispatch(event, Game::Offset::ListenerMouseMove);
		}
		// Ported from Flame: click_mouse. The control keys object puts its own Shift, Ctrl and Alt into the flags
		// (0x5DD010), so they are not sent here (Flame's are overwritten there).
		std::array<bool, 4> held{};
		void SendButton(std::size_t button, bool pressed) {
			if (!pressed && !held[button]) return;
			held[button] = pressed;
			auto* event = NewEvent(Game::Offset::MouseButtonEventSize, Game::Address::MouseButtonEventVtable);
			Store<std::uint32_t>(event + Game::Offset::MouseButtonKey, ButtonKeys[button]);
			Store<std::uint8_t>(event + Game::Offset::MouseButtonData, pressed ? ButtonPressedData : 0);
			Store<std::uint32_t>(event + Game::Offset::MouseButtonFlags, pressed ? ButtonPressedFlag : 0);
			Dispatch(event, Game::Offset::ListenerMouseButton);
		}

		// ---- The pointer
		bool clipped = false;
		// DirectInput's mouse kept the pointer to the game; Windows' would leave the window for another monitor, where the
		// game's mouse stops short of the edge (no scrolling) and a click switches to another program. Held to the window
		// while it is in front, let go otherwise.
		void LetPointerGo() {
			if (!clipped) return;
			ClipCursor(nullptr);
			clipped = false;
		}
		void KeepPointerInWindow(HWND window) {
			if (GetForegroundWindow() != window || IsIconic(window)) return LetPointerGo();
			RECT client{};
			if (!GetClientRect(window, &client)) return;
			MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
			// Windows lets go of it on its own (another window in front, a screen change), so it is checked each time.
			RECT now{};
			if (clipped && GetClipCursor(&now) && EqualRect(&now, &client)) return;
			ClipCursor(&client);
			clipped = true;
		}
		bool OurProgramInFront() {
			const HWND front = GetForegroundWindow();
			DWORD process = 0;
			if (front != nullptr) GetWindowThreadProcessId(front, &process);
			return process == GetCurrentProcessId();
		}

		// Ported from Flame: handle_mouse_move's test. The view is moved by the mouse's movement rather than its position
		// in a creature, and while the mouse turns or zooms the view, unless a menu is open. The interface's own player is
		// the one this computer controls (as PanelKeys reads it).
		bool MouseLookWanted() {
			if (Memory::Read<std::uintptr_t>(Game::Resolve(Game::Address::WorldPointer)) == 0) return false;
			const auto* playerInterface = reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::PlayerInterface));
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceInMenu) != 0) return false;
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceMouseLook) != 0) return true;
			const auto tag = Load<std::uint16_t>(playerInterface + Game::Offset::InterfacePlayerTag);
			if (tag == 0 || tag >= ThingTableSize) return false;
			const auto* things = reinterpret_cast<const std::uint8_t* const*>(Game::Resolve(Game::Address::ThingTable));
			const auto* player = things[tag];
			return player != nullptr && Load<std::uint16_t>(player + Game::Offset::PlayerPossessed) != 0;
		}
		POINT savedPos{};
		POINT lastPos{};
		bool wasRelative = false;
		POINT Middle(const RECT& client) { return { (client.left + client.right) / 2, (client.top + client.bottom) / 2 }; }
		void PutPointer(HWND window, POINT clientPoint) {
			ClientToScreen(window, &clientPoint);
			SetCursorPos(clientPoint.x, clientPoint.y);
		}
		// Ported from Flame: handle_fpv_mouse_move. The movement is added to what the game has not read yet (Flame sets
		// it, which drops the movements of all but the last message between two reads; the game clears it as it reads,
		// 0x5DD6A0). Flame's safe area there is worked out but never used, so it is left out.
		void LookMove(HWND window, POINT pos, const RECT& client) {
			auto* mouse = InputField(Game::Offset::InputMouse);
			const LONG width = client.right - client.left;
			const LONG height = client.bottom - client.top;
			if (mouse == nullptr || width <= 0 || height <= 0) return;
			const float sensitivity = static_cast<float>(Memory::Read<std::int32_t>(Game::Resolve(Game::Address::MouseSensitivity))) / SensitivityScale;
			if (pos.x != lastPos.x) {
				const float delta = static_cast<float>(pos.x - lastPos.x) / static_cast<float>(width) * LookWidth;
				Store<float>(mouse + Game::Offset::MouseMovedX, Load<float>(mouse + Game::Offset::MouseMovedX) + delta * sensitivity);
				lastPos.x = pos.x;
			}
			if (pos.y != lastPos.y) {
				const float delta = static_cast<float>(pos.y - lastPos.y) / static_cast<float>(height) * LookHeight;
				Store<float>(mouse + Game::Offset::MouseMovedY, Load<float>(mouse + Game::Offset::MouseMovedY) + delta * sensitivity);
				lastPos.y = pos.y;
			}
			const POINT middle = Middle(client);
			const LONG dx = pos.x - middle.x;
			const LONG dy = pos.y - middle.y;
			if (dx * dx + dy * dy > LookRecentre * LookRecentre) {
				PutPointer(window, middle);
				lastPos = middle;
			}
		}
		// The game's mouse where Windows' pointer is, in the game's screen pixels (our renderer may show the screen scaled
		// or with bars, which Flame's plain scaling by the window's size does not follow). The movement is kept up as
		// DirectInput's would (0x5DD4A0 adds to it), for what reads the movement rather than the position.
		void PutGameMouse(HWND window, POINT pos) {
			auto* mouse = InputField(Game::Offset::InputMouse);
			if (mouse == nullptr) return;
			const auto width = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenWidth));
			const auto height = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenHeight));
			if (width <= 0 || height <= 0) return;
			std::int32_t x = pos.x;
			std::int32_t y = pos.y;
			Renderer::WindowPointToScreen(window, width, height, x, y);
			const auto newX = static_cast<float>(x);
			const auto newY = static_cast<float>(y);
			Store<float>(mouse + Game::Offset::MouseMovedX, Load<float>(mouse + Game::Offset::MouseMovedX) + newX - Load<float>(mouse + Game::Offset::MouseFloatX));
			Store<float>(mouse + Game::Offset::MouseMovedY, Load<float>(mouse + Game::Offset::MouseMovedY) + newY - Load<float>(mouse + Game::Offset::MouseFloatY));
			Store<float>(mouse + Game::Offset::MouseFloatX, newX);
			Store<float>(mouse + Game::Offset::MouseFloatY, newY);
			using ClampFn = void(__fastcall*)(std::uint8_t* mouse);
			GameFunction<ClampFn>(Game::Address::MouseClamp)(mouse);
		}
		// Ported from Flame: handle_mouse_move.
		void HandleMove(HWND window, POINT pos) {
			RECT client{};
			if (!GetClientRect(window, &client)) return;
			const bool relative = MouseLookWanted();
			if (relative) {
				if (!wasRelative) {
					savedPos = pos;
					pos = Middle(client);
					PutPointer(window, pos);
					lastPos = pos;
				}
				// The pointer is kept in the window's middle.
				LookMove(window, pos, client);
			}
			else {
				if (wasRelative) {
					PutPointer(window, savedPos);
					pos = savedPos;
				}
				PutGameMouse(window, pos);
			}
			wasRelative = relative;
			SendMove(AxisX, 0);
		}
		// Focus gone: buttons held would stay held for the game, and the pointer is free again.
		void LetGo() {
			for (std::size_t button = 0; button < held.size(); ++button) SendButton(button, false);
			LetPointerGo();
		}

		bool Hook(std::uintptr_t target, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
		// Ported from Flame: MyDxInputManagerCb::initMouse with replace_mouse_dinput_to_user32 on (no DirectInput mouse).
		using InitMouseFn = std::int32_t*(__fastcall*)(std::uint8_t* input, void* unused, std::int32_t* result);
		InitMouseFn originalInitMouse = nullptr;
		std::int32_t* __fastcall InitMouseDetour(std::uint8_t* input, void* unused, std::int32_t* result) {
			// One already there: the game's own answer (an error code).
			if (Load<std::uint8_t*>(input + Game::Offset::InputMouseDevice) != nullptr) return originalInitMouse(input, unused, result);
			if (!on.exchange(true)) Logger::Info("Mouse: the game takes its mouse from Windows (windows_mouse); DirectInput's mouse is not used.");
			*result = 0;
			return result;
		}
		// Ported from Flame: MyDxInputManagerCb::onWindowActivated, which leaves the mouse out when it is not made. The
		// game's own (0x5BC210) would write into the missing mouse; without one, its other two steps are done here.
		using WindowActivatedFn = std::int32_t*(__fastcall*)(std::uint8_t* input, void* unused, std::int32_t* result, std::int32_t active);
		WindowActivatedFn originalWindowActivated = nullptr;
		std::int32_t* __fastcall WindowActivatedDetour(std::uint8_t* input, void* unused, std::int32_t* result, std::int32_t active) {
			if (Load<std::uint8_t*>(input + Game::Offset::InputMouseDevice) != nullptr) return originalWindowActivated(input, unused, result, active);
			using SetActiveFn = void(__fastcall*)(std::uint8_t* devicePart, void* unused, std::int32_t active);
			using ApplyFn = std::int32_t*(__fastcall*)(std::uint8_t* input, void* unused, std::int32_t* result);
			if (auto* keyboard = Load<std::uint8_t*>(input + Game::Offset::InputKeyboardDevice); keyboard != nullptr) {
				GameFunction<SetActiveFn>(Game::Address::InputDeviceSetActive)(keyboard + Game::Offset::InputDevicePart, nullptr, active);
			}
			GameFunction<ApplyFn>(Game::Address::InputApplyCooperation)(input, nullptr, result);
			return result;
		}
		bool VtableHolds(std::uintptr_t vtable, std::uintptr_t at, std::uintptr_t function) {
			return Memory::Read<std::uintptr_t>(Game::Resolve(vtable) + at) == Game::Resolve(function);
		}
	}

	bool WindowsMouse::IsOn() { return on; }

	void WindowsMouse::OnWindowMessage(void* windowHandle, std::uint32_t message, std::uintptr_t wParam, std::intptr_t) {
		if (!on) return;
		const auto window = static_cast<HWND>(windowHandle);
		switch (message) {
			case WM_MOUSEMOVE: {
				// Flame's appIsActive, from WM_ACTIVATEAPP; asked here instead, as the window may have been made active
				// before our renderer watched it.
				if (!OurProgramInFront()) break;
				KeepPointerInWindow(window);
				// Where the pointer is now rather than when the message was made (as Flame).
				POINT pos{};
				if (!GetCursorPos(&pos) || !ScreenToClient(window, &pos)) break;
				HandleMove(window, pos);
				break;
			}
			case WM_ACTIVATEAPP:
				if (wParam == 0) LetGo();
				break;
			case WM_ACTIVATE:
				// Also when one of our own windows comes in front (the editor's), which is not another program.
				if (LOWORD(wParam) == WA_INACTIVE) LetGo();
				break;
			// The game's window class has no CS_DBLCLKS (style 3, 0x5B4FE8), so Windows sends no double clicks; were it to,
			// a double click is the second press (the game finds double clicks itself). Flame sends them as a flag
			// without the pressed bit, which the game would read as a release.
			case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SendButton(LeftButton, true); break;
			case WM_LBUTTONUP: SendButton(LeftButton, false); break;
			case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: SendButton(RightButton, true); break;
			case WM_RBUTTONUP: SendButton(RightButton, false); break;
			case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
				SendButton(MiddleButton, true);
				CameraControls::OnMiddleButton();
				break;
			case WM_MBUTTONUP: SendButton(MiddleButton, false); break;
			// Flame: both side buttons are the game's fourth mouse key.
			case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: SendButton(OtherButton, true); break;
			case WM_XBUTTONUP: SendButton(OtherButton, false); break;
			case WM_MOUSEWHEEL: {
				const auto delta = static_cast<std::int32_t>(static_cast<std::int16_t>(HIWORD(wParam)));
				SendMove(AxisWheel, delta);
				CameraControls::OnMouseWheel(delta);
				break;
			}
			default: break;
		}
	}

	void WindowsMouse::Install() {
		// Flame has it on (enabled = true).
		if (StringUtils::ToLower(Config::GetString("windows_mouse", "on")) == "off") {
			Logger::Info("Mouse: windows_mouse is off, the game's DirectInput mouse is used.");
			return;
		}
		if (!Renderer::IsChosen()) {
			Logger::Info("Mouse: windows_mouse needs dx11, dx12 or vulkan (they watch the game's window); the game's DirectInput mouse is used.");
			return;
		}
		if (InputField(Game::Offset::InputMouseDevice) != nullptr) {
			Logger::Warning("Mouse: the game had already made its mouse before KeeperForge2 started; windows_mouse is off.");
			return;
		}
		// The mouse's clamp: push esi; mov esi, ecx; fild dword ptr [esi + 0x34] (its bounds) [verified at 0x5DD630].
		const bool partsFound = VtableHolds(Game::Address::MouseMoveEventVtable, 0, Game::Address::MouseMoveEventSend)
			&& VtableHolds(Game::Address::MouseButtonEventVtable, 0, Game::Address::MouseButtonEventSend)
			&& VtableHolds(Game::Address::ControlKeysVtable, Game::Offset::ListenerMouseMove, Game::Address::ControlKeysMouseMove)
			&& VtableHolds(Game::Address::ControlKeysVtable, Game::Offset::ListenerMouseButton, Game::Address::ControlKeysMouseButton)
			&& Memory::Matches(Game::Resolve(Game::Address::MouseClamp), { 0x56, 0x8B, 0xF1, 0xDB, 0x46, 0x34 })
			// mov eax, [esp + 4]; mov [ecx + 0x10], eax; ret 4
			&& Memory::Matches(Game::Resolve(Game::Address::InputDeviceSetActive), { 0x8B, 0x44, 0x24, 0x04, 0x89, 0x41, 0x10, 0xC2, 0x04, 0x00 })
			// push ecx; push esi; mov esi, ecx; mov eax, [esi + 0x58]
			&& Memory::Matches(Game::Resolve(Game::Address::InputApplyCooperation), { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x58 });
		if (!partsFound) {
			Logger::Warning("Mouse: unexpected game code at the mouse's events, the game's DirectInput mouse is used.");
			return;
		}
		// push esi; mov esi, ecx; push edi; mov edi, [esp + 0x10]; mov ecx, [esi + 0x58]; push edi; add ecx, 0x10
		if (!Hook(Game::Resolve(Game::Address::InputWindowActivated), { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0x4E, 0x58, 0x57, 0x83, 0xC1, 0x10 },
			reinterpret_cast<void*>(&WindowActivatedDetour), reinterpret_cast<void**>(&originalWindowActivated))) {
			Logger::Warning("Mouse: unexpected game code at the window's activation, the game's DirectInput mouse is used.");
			return;
		}
		// push ecx; push esi; push edi; mov edi, ecx; mov eax, [edi + 0x5C]; lea esi, [edi + 0x5C]
		if (!Hook(Game::Resolve(Game::Address::InputInitMouse), { 0x51, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x5C, 0x8D, 0x77, 0x5C },
			reinterpret_cast<void*>(&InitMouseDetour), reinterpret_cast<void**>(&originalInitMouse))) {
			// The activation hook only steps in without a mouse, so it can stay.
			Logger::Warning("Mouse: unexpected game code at the mouse's set-up, the game's DirectInput mouse is used.");
			return;
		}
		Logger::Debug("Mouse: windows_mouse is ready; the game's mouse will come from Windows' messages.");
	}
}
