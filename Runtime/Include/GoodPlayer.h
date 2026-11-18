#pragma once
namespace KeeperForge2 {
	// Makes the Good player (heroes, see "Swap to Good Player" in SkirmishSwapKeeper) playable in a game: its hand
	// keeps what it picks up, Dwarfs take the place of Imps as its workers, and Create Imp becomes Create Dwarf.
	// Keepers are not affected.
	class GoodPlayer {
		public:
			static void Install();
	};
}
