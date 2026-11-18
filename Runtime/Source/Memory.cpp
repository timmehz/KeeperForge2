#include "Memory.h"
#include <cstring>
#include <Psapi.h>

namespace KeeperForge2 {
	HMODULE Memory::moduleHandle = nullptr;
	uintptr_t Memory::GetModuleBase() {
		if (moduleHandle == nullptr) moduleHandle = GetModuleHandle(nullptr);
		return reinterpret_cast<uintptr_t>(moduleHandle);
	}
	size_t Memory::GetModuleSize() {
		if (moduleHandle == nullptr) moduleHandle = GetModuleHandle(nullptr);
		MODULEINFO moduleInfo{};
		if (!GetModuleInformation(GetCurrentProcess(),moduleHandle, &moduleInfo,sizeof(moduleInfo))) return 0;
		return moduleInfo.SizeOfImage;
	}
	bool Memory::Protect(uintptr_t address, std::size_t size, DWORD protection,DWORD& oldProtection) {
		return VirtualProtect(reinterpret_cast<void*>(address), size, protection, &oldProtection) != FALSE;
	}
	void Memory::Patch(uintptr_t address, const std::vector<std::uint8_t>& bytes) {
		DWORD oldProtection{};
		if (!Protect(address, bytes.size(), PAGE_EXECUTE_READWRITE, oldProtection)) return;
		std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
		Protect(address, bytes.size(), oldProtection, oldProtection);
	}
	void Memory::Fill(uintptr_t address, std::uint8_t value, std::size_t count) {
		DWORD oldProtection{};
		if (!Protect(address, count, PAGE_EXECUTE_READWRITE, oldProtection)) return;
		std::memset(reinterpret_cast<void*>(address), value, count);
		Protect(address, count, oldProtection, oldProtection);
	}
	bool Memory::Matches(uintptr_t address, const std::vector<std::uint8_t>& bytes) {
		return std::memcmp(reinterpret_cast<const void*>(address), bytes.data(), bytes.size()) == 0;
	}
	bool Memory::WriteCall(uintptr_t address, const void* destination) {
		std::uint8_t bytes[5]{ 0xE8 };
		const auto relative = static_cast<std::int32_t>(reinterpret_cast<uintptr_t>(destination) - (address + 5));
		std::memcpy(&bytes[1], &relative, sizeof(relative));
		DWORD oldProtection{};
		if (!Protect(address, sizeof(bytes), PAGE_EXECUTE_READWRITE, oldProtection)) return false;
		std::memcpy(reinterpret_cast<void*>(address), bytes, sizeof(bytes));
		Protect(address, sizeof(bytes), oldProtection, oldProtection);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), sizeof(bytes));
		return true;
	}
	bool Memory::WriteJump(uintptr_t address, const void* destination, std::size_t size) {
		if (size < 5) return false;
		std::vector<std::uint8_t> bytes(size, 0x90);
		const auto relative = static_cast<std::int32_t>(reinterpret_cast<uintptr_t>(destination) - (address + 5));
		bytes[0] = 0xE9;
		std::memcpy(&bytes[1], &relative, sizeof(relative));
		DWORD oldProtection{};
		if (!Protect(address, size, PAGE_EXECUTE_READWRITE, oldProtection)) return false;
		std::memcpy(reinterpret_cast<void*>(address), bytes.data(), size);
		Protect(address, size, oldProtection, oldProtection);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
		return true;
	}
}
