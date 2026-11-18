#pragma once
namespace KeeperForge2 {
	// The game loads a loading screen image made for the exact screen size and copies it to the top-left corner
	// unscaled. Only the original 4:3 sizes exist, so at other sizes it is missing or tiny. This loads the largest
	// original size that fits and draws it scaled to fill the whole screen, trimming the edges of the image that
	// don't fit the screen's shape.
	class WidescreenLoadingScreen {
		public:
			static bool Install();
	};
}
