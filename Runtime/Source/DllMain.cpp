#include <Windows.h>
#include "Runtime.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
	switch (reason) {
		case DLL_PROCESS_ATTACH: {
			DisableThreadLibraryCalls(hModule);
			// reserved is set when Windows loads the DLL with the game (the launcher put it in the game's import list),
			// and null for LoadLibrary (the launcher's older way, injecting into the running game).
			if (reserved && KeeperForge2::StartEarly()) break;
			HANDLE thread = CreateThread(nullptr, 0, KeeperForge2::RuntimeStartupThread, nullptr, 0, nullptr);
			if (thread) CloseHandle(thread);
			break;
		}
		case DLL_PROCESS_DETACH: {
			KeeperForge2::Shutdown();
			break;
		}
	}
	return TRUE;
}
