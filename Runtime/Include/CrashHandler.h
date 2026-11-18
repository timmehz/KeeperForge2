#pragma once
#include <string>
namespace KeeperForge2 {
	// Catches crashes the game does not handle itself and writes what happened to the log: the exception, where it
	// happened (module and offset, which can be looked up in the Ghidra export), the registers and the code
	// addresses found on the stack. A minidump is saved next to the log. Then it reports the crash and closes.
	class CrashHandler {
		public:
			static void Install();
			// Where another thread of the game is now, in the same form as a crash report (its current address and the
			// code addresses on its stack), for logging a hang. The thread is paused while it is read.
			static std::string DescribeThread(unsigned long threadId);
	};
}
