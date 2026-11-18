#pragma once
namespace KeeperForge2 {
	// Keeps the game running while it is minimized or another program has focus (the game pauses itself when it is
	// told it lost focus). While the player cannot see it, the game's sound is muted and, in campaign and skirmish, it
	// runs at 1 frame per second. The game moves at most one game turn per frame, so in multiplayer it keeps its
	// frame rate (and the game's speed for everyone); nothing is put on screen while it is unseen either way.
	// Only with our renderers (dx11, dx12, vulkan), which own the game's window. Controlled by "run_in_background"
	// in config.yml.
	class BackgroundRunning {
		public:
			static void Initialize();
			// For our renderer's window: whether the game should not be told it lost focus.
			static bool KeepsRunning();
			// From the renderer, whenever focus or minimizing changes: can the player see the game?
			static void SetSeen(bool seen);
			// Whether the player can see the game (always true with the original renderer or run_in_background off).
			static bool IsSeen();
			// Called every frame before it is shown: while unseen in campaign or skirmish, waits until a second has
			// passed since the last frame. True when it waited.
			static bool WaitWhileUnseen();
	};
}
