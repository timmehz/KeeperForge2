#pragma once
namespace KeeperForge2 {
	// Adds a "Swap Keeper" button to the skirmish lobby: select a keeper row, press it, then click the row to trade
	// places with. The two players swap keeper slots (color and start position), and the game starts with the
	// human player on whichever keeper they moved to. Skirmish only.
	class SkirmishSwapKeeper {
		public:
			static void Install();
			// Whether the skirmish screen's setup (0x538AC0) is hooked here; its hook then calls
			// CustomCampaign::AfterSkirmishSetup, as there can be only one hook on it.
			static bool SetupHooked();
	};
}
