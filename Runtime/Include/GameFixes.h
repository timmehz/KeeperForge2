#pragma once
namespace KeeperForge2 {
	// Fixes for bugs in the game itself (other than creature behavior, see CreatureFixes). Found with the help of
	// the Flame and Ember projects, verified in our export of DKII-DX.exe and written from scratch. See
	// docs/game/game-fixes.md for each one.
	class GameFixes {
		public:
			// Before the game's own start-up code runs (some fixes change it). Logs nothing.
			static void BeforeGameStarts();
			static void Install();
			// The game's window became the active one again (WM_ACTIVATEAPP): keys held while away are let go.
			static void OnWindowActivated();
			// The game's window was asked to close (WM_CLOSE, e.g. Alt+F4). During a game this sends the game's own "exit
			// to Windows" action and returns true (the message is handled); otherwise false, and the game closes as before.
			static bool OnWindowClose();
	};
}
