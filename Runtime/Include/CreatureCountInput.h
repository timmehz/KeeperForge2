#pragma once
namespace KeeperForge2 {
	// Lobby game options, Creatures tab: the number next to each creature's slider can be clicked and typed into
	// (digits only, 0 to 32; more is 32). The slider moves to the typed number. Enter or a click elsewhere ends it.
	// See docs/game/session-options.md.
	class CreatureCountInput {
		public:
			static void Install();
	};
}
