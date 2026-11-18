#pragma once
namespace KeeperForge2 {
	// In-game panels (the level objective, the info bar, tooltips...) put their text into a "view": a screen-sized
	// set of 128x32 tiles that is cleared, drawn and uploaded tile by tile. Panels do this every frame even when
	// nothing changed, and the level objective uploads the whole view once per line, which at 4K cost up to 190 ms a
	// frame. This leaves out runs of calls that repeat what the view already shows, holding back the clear that starts
	// them, and uploads a changed view once, right before it is drawn. Views are still redrawn every 5 seconds.
	class GuiTextCache {
		public:
			static void Install();
	};
}
