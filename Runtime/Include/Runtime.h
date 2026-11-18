#pragma once
#include <Windows.h>
namespace KeeperForge2 {
	bool Initialize();
	void Shutdown();
	bool IsInitialized();
	DWORD WINAPI RuntimeStartupThread(LPVOID parameter);
	// Loaded among the game's own DLLs (the launcher puts KeeperForge2.dll first in the game's import list), from
	// DllMain: gets ready while no DLL has started, and runs Initialize on the game's main thread before the game's
	// own code. False if it could not, and the runtime then starts on a thread of its own as before.
	bool StartEarly();
}
extern "C" {
	_declspec(dllexport) BOOL KeeperForge2_Initialize();
	_declspec(dllexport) void KeeperForge2_Shutdown();
}
