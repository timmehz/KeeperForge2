#pragma once
namespace KeeperForge2 {
	// Keys that open the side panel's tabs in a level, as a click on the tab does: 1 Creatures, 2 Rooms, 3 Spells and
	// 4 Workshop by default, a key that turns Sell on and off (the ` key by default) and one that opens and closes the options
	// menu (Escape by default). Set by the "panel_key_" settings, "sell_key" and "options_key" in config.yml. See
	// docs/game/panel-keys.md.
	class PanelKeys {
		public:
			static void Install();
	};
}
