#include <Windows.h>
#include "Runtime.h"
#include "GameFixes.h"
#include "HookManager.h"
#include "Renderer.h"
#include <cstdint>

namespace KeeperForge2 {
	DWORD WINAPI RuntimeStartupThread(LPVOID) {
		Initialize();
		return 0;
	}

	namespace {
		// The game's start routine (the old C runtime's: no arguments, and it never returns, it exits the process).
		using GameEntryFn = void(__cdecl*)();
		GameEntryFn originalGameEntry = nullptr;
		void __cdecl GameEntryDetour() {
			// Windows has started every DLL and the game's own code has not run yet: the runtime starts here, on the
			// game's main thread, so it is in place before the game sets anything up.
			Initialize();
			originalGameEntry();
		}
	}

	bool StartEarly() {
		Renderer::BeforeDllsStart();
		GameFixes::BeforeGameStarts();
		if (!HookManager::Initialize()) return false;
		const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe + dos->e_lfanew);
		const std::uintptr_t entry = exe + nt->OptionalHeader.AddressOfEntryPoint;
		if (!HookManager::CreateHook(entry, reinterpret_cast<void*>(&GameEntryDetour), reinterpret_cast<void**>(&originalGameEntry))) return false;
		if (!HookManager::EnableHook(entry)) {
			HookManager::RemoveHook(entry);
			return false;
		}
		return true;
	}
}
