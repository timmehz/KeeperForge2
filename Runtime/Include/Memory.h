#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace KeeperForge2 {
	class Memory {
		public:
			static uintptr_t GetModuleBase();
			static size_t GetModuleSize();
			template<typename T> static T Read(uintptr_t address);
			template<typename T> static void Write(uintptr_t address,T value);
			static bool Protect(uintptr_t address,std::size_t size,DWORD protection,DWORD& oldProtection);
			static void Patch(uintptr_t address,const std::vector<std::uint8_t>& bytes);
			static void Fill(uintptr_t address,std::uint8_t value,std::size_t count);
			static bool Matches(uintptr_t address,const std::vector<std::uint8_t>& bytes);
			// Overwrites size bytes (at least 5) with a jmp to destination, padding the rest with NOPs.
			static bool WriteJump(uintptr_t address,const void* destination,std::size_t size);
			// Overwrites the 5-byte "call rel32" at address so it calls destination instead.
			static bool WriteCall(uintptr_t address,const void* destination);
		private:
			static HMODULE moduleHandle;
	};
	template<typename T> T Memory::Read(uintptr_t address){return *reinterpret_cast<const T*>(address);}
	template<typename T> void Memory::Write(uintptr_t address,T value) {
		DWORD oldProtection{};
		if (!Protect(address, sizeof(T), PAGE_EXECUTE_READWRITE, oldProtection)) return;
		*reinterpret_cast<T*>(address) = value;
		Protect(address, sizeof(T), oldProtection, oldProtection);
	}
}
