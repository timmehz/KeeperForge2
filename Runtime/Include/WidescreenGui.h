#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// The game lays out all its 2D GUI in a 2560x1920 (4:3) space and scales it to the screen with separate x and y
	// factors, which stretches everything sideways on a wide screen. This keeps the 4:3 proportions instead.
	// In-game, the layout is drawn at its own shape starting at the screen's left edge (a plain scale, because the
	// game also converts sizes with it) and each window is moved by its id: the control panel and top-left box stay
	// at the left edge, the keeper icon and alarms move to the right edge, the top info bar is widened, dialogs stay
	// in the 4:3 area. Background pieces drawn against the layout's right edge (stone strip, rods, end caps) follow
	// the screen's right edge. The main menu is drawn as the original 4:3 menu, centered: while it draws, the frame it
	// draws into starts at the centered 4:3 area. The mouse goes through the same mappings, so clicks line up.
	class WidescreenGui {
		public:
			static bool Install();
			// Called after the game sets a screen mode.
			static void OnScreenMode(std::int32_t height);
			// While the menu draws centered: switches drawing to the whole screen (returning the 4:3 area's left margin
			// in pixels, 0 if nothing changed) and back. For pieces that belong at the screen's edges.
			static std::int32_t BeginFullScreenMenuDraw();
			static void EndFullScreenMenuDraw();
	};
}
