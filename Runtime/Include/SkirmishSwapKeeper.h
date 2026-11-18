#pragma once
namespace KeeperForge2 {
	// Adds a "Swap Keeper" button to the skirmish lobby: select a keeper row, press it, then click the row to trade
	// places with. The two players swap keeper slots (color and start position), and the game starts with the
	// human player on whichever keeper they moved to. Skirmish only.
	class SkirmishSwapKeeper {
		public:
			static void Install();
	};
}
