#pragma once
namespace KeeperForge2 {
	// patch.dll keeps the whole game on one CPU thread. This gives it the number set by "cpu_threads" in config.yml, so
	// sound, network, the pointer and the graphics driver stop competing with the game's main thread. The game's own
	// work stays on that thread. See docs/game/cpu-threads.md.
	class CpuThreads {
		public:
			// After the game's DLLs have started: patch.dll sets its limit as it starts.
			static void Install();
	};
}
