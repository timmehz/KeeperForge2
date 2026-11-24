#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// More to do in a possessed creature (docs/game/possession.md). A melee hit (left click) at:
	// - a Training Dummy or Training Target: the creature gains one second of the Training Room's experience (up to the
	//   Training Room's highest level, as the room itself).
	// - its own bed: possession ends and the creature goes to bed tired, so it sleeps its full length.
	// - a chicken: it eats the chicken even when not hungry (the game only let hungry creatures eat).
	// - its keeper's Prison Door: the door opens (as it does for an Imp). It stays open while followers are still on their
	//   way in, then closes by itself. If the possessed creature is inside when it closes, possession ends and it and its
	//   followers inside become prisoners of that Prison.
	// Walking into its keeper's Combat Pit: followers who come in become pit fighters and every fighter dies rather than
	// being knocked out (to the death). A follower left with no one to fight goes for the possessed creature. When the
	// possessed creature walks out, or possession ends, the survivors stop fighting and follow it out.
	// Simulation code: it runs in the game's own action handling and after each world tick, on every machine alike.
	class PossessionActions {
		public:
			static void Install();
			// Called by CoopCampaign's world tick hook after the game's tick (play and level tests, not editing).
			static void AfterWorldTick(std::uint8_t* world);
	};
}
