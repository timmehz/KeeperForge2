#pragma once
namespace KeeperForge2 {
	// Saves the game every few minutes of play, as the game's own Save would, to autosave-<date>-<time>.SAV in its saves
	// folder, keeping the newest few. Single player only (campaign, skirmish, My Pet Dungeon): never in multiplayer or a
	// co-op campaign, nor in the level editor. Play time is counted in game turns, so menus and pauses do not count.
	// Controlled by "autosave" (minutes, or off) and "autosave_keep" in config.yml. Ported from Flame's autosave; see
	// docs/game/autosave.md.
	class Autosave {
		public:
			static void Install();
	};
}
