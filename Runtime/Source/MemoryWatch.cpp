#include "MemoryWatch.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		constexpr DWORD CheckEveryMs = 60000;
		// A change smaller than this is not worth a line in the log.
		constexpr std::uint32_t LogChangeMb = 16;
		// A 3840x2160 surface alone takes about 32 MB.
		constexpr std::uint32_t LowBlockMb = 64;
		constexpr SIZE_T ReportedLimit = 0x7FFFFFFF;

		using GlobalMemoryStatusFn = void(WINAPI*)(LPMEMORYSTATUS);
		GlobalMemoryStatusFn originalGlobalMemoryStatus = nullptr;
		// With the 4 GB flag Windows gives the real figures, or -1 above 4 GB. The game reads total memory as a signed
		// number and adds free memory and free page file together, so both go wrong. Without the flag Windows stops at
		// 2 GB, which is what the game sees here too.
		void WINAPI GlobalMemoryStatusDetour(LPMEMORYSTATUS status) {
			originalGlobalMemoryStatus(status);
			for (SIZE_T* value : { &status->dwTotalPhys, &status->dwAvailPhys, &status->dwTotalPageFile, &status->dwAvailPageFile, &status->dwTotalVirtual, &status->dwAvailVirtual }) {
				*value = (std::min)(*value, ReportedLimit);
			}
		}
		// The lobby's Ram column only shows this number, so it can tell the truth: what the game can use, up to 4 GB.
		constexpr std::uint32_t LargestLobbyMemoryMb = 4096;
		using LobbyPlayerMemoryFn = std::uint32_t(__cdecl*)();
		LobbyPlayerMemoryFn originalLobbyPlayerMemory = nullptr;
		std::uint32_t __cdecl LobbyPlayerMemoryDetour() {
			MEMORYSTATUSEX status{};
			status.dwLength = sizeof(status);
			if (!GlobalMemoryStatusEx(&status)) { return originalLobbyPlayerMemory(); }
			return static_cast<std::uint32_t>((std::min)((status.ullTotalPhys >> 20) + 1, static_cast<DWORDLONG>(LargestLobbyMemoryMb)));
		}
		std::string Describe(const MemoryWatch::AddressSpace& space) {
			return std::to_string(space.committedMb) + " MB committed, " + std::to_string(space.freeMb) + " MB free, largest free block "
				+ std::to_string(space.largestFreeMb) + " MB, of " + std::to_string(space.limitMb) + " MB.";
		}
		DWORD WINAPI WatchThread(void*) {
			std::uint32_t loggedFree = 0;
			bool low = false;
			for (bool first = true;; first = false) {
				const MemoryWatch::AddressSpace space = MemoryWatch::Read();
				const std::uint32_t change = space.freeMb > loggedFree ? space.freeMb - loggedFree : loggedFree - space.freeMb;
				if (first || change >= LogChangeMb) {
					loggedFree = space.freeMb;
					Logger::Debug("Memory: " + Describe(space));
				}
				if (space.largestFreeMb < LowBlockMb && !low) { Logger::Warning("Memory: address space is running out. " + Describe(space)); }
				low = space.largestFreeMb < LowBlockMb;
				Sleep(CheckEveryMs);
			}
		}
		void KeepReportedMemoryAt2Gb() {
			const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
			const FARPROC function = kernel ? GetProcAddress(kernel, "GlobalMemoryStatus") : nullptr;
			const auto target = reinterpret_cast<std::uintptr_t>(function);
			if (!target
				|| !HookManager::CreateHook(target, reinterpret_cast<void*>(&GlobalMemoryStatusDetour), reinterpret_cast<void**>(&originalGlobalMemoryStatus))
				|| !HookManager::EnableHook(target)) {
				Logger::Warning("Memory: could not keep the memory figures at 2 GB; the game may choose its low memory settings.");
			}
			// "sub esp, 0x24; lea ecx, [esp]; call"
			const auto lobbyMemory = Game::Resolve(Game::Address::LobbyPlayerMemory);
			if (!Memory::Matches(lobbyMemory, { 0x83, 0xEC, 0x24, 0x8D, 0x4C, 0x24, 0x00, 0xE8 })
				|| !HookManager::CreateHook(lobbyMemory, reinterpret_cast<void*>(&LobbyPlayerMemoryDetour), reinterpret_cast<void**>(&originalLobbyPlayerMemory))
				|| !HookManager::EnableHook(lobbyMemory)) {
				Logger::Warning("Memory: could not change the lobby's Ram column, it stays at 2048.");
			}
		}
	}

	MemoryWatch::AddressSpace MemoryWatch::Read() {
		SYSTEM_INFO system{};
		GetSystemInfo(&system);
		auto at = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
		const auto end = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
		std::uint64_t committed = 0, free = 0, largestFree = 0;
		MEMORY_BASIC_INFORMATION region{};
		while (at < end && VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region))) {
			if (region.State == MEM_FREE) {
				free += region.RegionSize;
				largestFree = (std::max)(largestFree, static_cast<std::uint64_t>(region.RegionSize));
			}
			else if (region.State == MEM_COMMIT) { committed += region.RegionSize; }
			const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
			// The last region can end at the very top, where the address wraps to 0.
			if (next <= at) { break; }
			at = next;
		}
		const auto megabytes = [](std::uint64_t bytes) { return static_cast<std::uint32_t>(bytes >> 20); };
		return { megabytes(static_cast<std::uint64_t>(end) + 1), megabytes(committed), megabytes(free), megabytes(largestFree) };
	}

	void MemoryWatch::Install() {
		const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe + dos->e_lfanew);
		const bool largeAddressAware = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
		Logger::Info(std::string("Memory: the game runs ") + (largeAddressAware ? "with" : "without") + " the 4 GB flag.");
		if (largeAddressAware) { KeepReportedMemoryAt2Gb(); }
		if (const HANDLE thread = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr)) { CloseHandle(thread); }
	}
}
