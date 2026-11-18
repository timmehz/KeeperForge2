#pragma once
#include <string>
namespace KeeperForge2 {
	// Measures what the game uses, once a second on a thread of its own: processor, memory, graphics card (load and
	// video memory) and frames per second. Each keeps its minimum, maximum, current and average since the runtime
	// started or since it was last reset. Read by the console's commands (ConsoleCommands). Nothing here touches the
	// simulation.
	class PerformanceStats {
		public:
			enum class Part { Cpu, Ram, Gpu, Fps };
			static void Install();
			// The figures as lines of text for the console, ending with a line break.
			static std::string Report(Part part);
			static void Reset(Part part);
	};
}
