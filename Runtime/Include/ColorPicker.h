#pragma once
#include <cstdint>
#include <functional>
#include <string>
namespace KeeperForge2 {
	// A color picker in a window of Windows' own, over the game's window (docs/game/keeper-colors.md): a wheel of hue and
	// saturation with a brightness bar beside it, the color before and now, red, green and blue as numbers, and the hex code.
	// They are linked: changing any one changes the others. It runs on the game's thread, driven by the game's own message
	// loop, like EditorDialogs, and follows config.yml's editor_dark_windows. Colors are 0xRRGGBB.
	class ColorPicker {
		public:
			// Opens it on this color. changed is called as the color changes (every drag, every valid number typed); closed
			// once at the end with the color kept (OK) or the one it opened on (Cancel, Esc, the close box). Default sets the
			// color back to defaultColor. One at a time: false if one is open (it comes to the front) or it could not be made.
			static bool Open(const std::wstring& title, std::uint32_t color, std::uint32_t defaultColor, std::function<void(std::uint32_t color)> changed,
				std::function<void(std::uint32_t color, bool kept)> closed);
			static bool IsOpen();
			// Closes it as Cancel does.
			static void Close();
			// "#RRGGBB", and back from "#RRGGBB" or "RRGGBB" (case does not matter); false if it is not one.
			static std::wstring Hex(std::uint32_t color);
			static bool ParseHex(const std::wstring& text, std::uint32_t& color);
	};
}
