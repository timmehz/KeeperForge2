#pragma once
namespace KeeperForge2 {
	// For finding sound bugs (a sound that never stops, such as the Library's flying papers): with log_level debug (or
	// trace), every sound the game starts and every command it sends the sound system go into the log and the console
	// (Debug level), with the sound's category name, the game address that asked for it and the game turn. Nothing is
	// hooked at other log levels.
	// See docs/game/sound.md.
	class SoundLog {
		public:
			static void Install();
	};
}
