#pragma once
namespace KeeperForge2 {
	// For finding why neutral creatures do not fight back in a Combat Pit: with log_level debug (or trace), every state
	// change of a creature standing in a Combat Pit or marked as a pit fighter, and every melee hit where either side is
	// one, go into the log (Debug level) with the creature's owner, state and pit fields. Nothing is hooked at other
	// log levels. See docs/game/game-fixes.md, "Neutral creatures in the Combat Pit".
	class CombatPitLog {
		public:
			static void Install();
	};
}
