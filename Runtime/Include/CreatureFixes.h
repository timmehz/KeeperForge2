#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Fixes for creature behavior bugs in the game itself.
	// - Making a home: whenever a player changes the map (digging, claiming, building), the game resets every walking
	//   creature of that player, which also canceled a creature walking to the Lair to make its home. In a busy
	//   dungeon it never got there: it stopped, turned and started over (seen with a Lord of the Land). Those walks
	//   now keep going; every other walking creature is reset as before.
	// - Untagging: a worker already digging (or mining gold from) a tile kept going after the tile was untagged, as the
	//   game only checks the task between its actions and a dig is one long action. The action is now stopped as soon
	//   as the task is gone.
	// - Portal pool: late campaign levels let a keeper have the Temple or Combat Pit but give it no Dark Angels or Black
	//   Knights in its creature pool, and a portal only brings types in the pool. Those get a pool entry once the room
	//   can be had (campaign play only; skirmish and multiplayer pools are the lobby's).
	// Simulation code: every player in a multiplayer game runs the same KeeperForge2, so all see the same.
	class CreatureFixes {
		public:
			static void Install();
			// Called by MercenaryPortal, which hooks the portal's creature choice (0x4DBF90) and has a level start callback
			// before CoopCampaign's: at the start of every portal's choice, and at each level start.
			static void BeforePortalChoice(std::uint8_t* room);
			static void OnLevelStart();
	};
}
