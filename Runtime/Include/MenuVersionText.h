#pragma once
namespace KeeperForge2 {
	// Redraws the main menu's version text ("V1.7", bottom left) a little in from the screen edge, with a
	// "KeeperForge2-vX.Y.Z (n mods loaded)" line underneath, in the game's own font.
	class MenuVersionText {
		public:
			static void Install();
	};
}
