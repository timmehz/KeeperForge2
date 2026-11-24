#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// The game's mouse from Windows' messages instead of DirectInput, so the pointer moves as Windows' own does (its
	// pointer speed and acceleration, no lag, the same speed at any resolution). The game's DirectInput mouse is not
	// made; the game window's messages put the game's mouse where Windows' pointer is and hand the buttons to the game
	// as DirectInput's would. In a creature, and while the mouse turns the view, the movement is relative instead (the
	// pointer is kept in the window's middle). Only with our renderers (dx11, dx12, vulkan), which watch the game's
	// window. Controlled by "windows_mouse" in config.yml. See docs/game/mouse.md. Ported from Flame:
	// replace_mouse_dinput_to_user32.
	class WindowsMouse {
		public:
			static void Install();
			// True once the game runs without its DirectInput mouse (the messages below are its mouse).
			static bool IsOn();
			// From our renderer's window procedure, for every message of the game's window before the game sees it.
			static void OnWindowMessage(void* window, std::uint32_t message, std::uintptr_t wParam, std::intptr_t lParam);
	};
}
