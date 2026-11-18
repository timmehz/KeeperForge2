#pragma once
namespace KeeperForge2 {
	// Reads commands typed into the console window (the launcher's, which the runtime shares) on a thread of its own
	// and answers there: cpu, ram, gpu, fps, stats, reset, help. See docs/api/console-commands.md.
	class ConsoleCommands {
		public:
			// After the logger has its console.
			static void Install();
	};
}
