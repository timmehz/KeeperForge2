#pragma once
namespace KeeperForge2 {
	// What a claimed Mercenary Portal brings in:
	// - heroes, each once the player has the rooms config.yml sets for it (Stone Knights can be turned off);
	// - one royal per keeper per game (Lord of the Land, King Reginald or one of the three Princes, picked at random
	//   by the game's own weighted pick), once the player has their rooms (can be turned off);
	// - with "mercenary-portal-spawns: mercenary" also the game's own mercenaries, as in Frosty's levels.
	// With "mercenary-portal-spawns: off" none of this happens and the lobby does not list the heroes.
	// The map's creature list for the player still applies. Heroes have no mercenary form in the game's data, so they
	// always come as the normal hero. All of these are session values (the host's in multiplayer).
	class MercenaryPortal {
		public:
			static void Install();
	};
}
