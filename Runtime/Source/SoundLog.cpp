#include "SoundLog.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <Windows.h>

#pragma intrinsic(_ReturnAddress)

namespace KeeperForge2 {
	namespace {
		// The sound system (0x75B468, vtable 0x66F7FC): its methods, all thiscall.
		// Category lookup (vtable + 0x1C): (bank name, category name) ret 8, returns the category id.
		constexpr std::uintptr_t SoundCategoryLookup = 0x00567790;
		// Play at a position (+ 0x20): (old handle, category, sound, int position[3]) ret 0x10, returns the new handle.
		constexpr std::uintptr_t SoundPlayAt = 0x005678F0;
		// Play (+ 0x24): (old handle, category, sound) ret 0xC, returns the new handle.
		constexpr std::uintptr_t SoundPlay = 0x00567810;
		// Command (+ 0x38): four values ret 0x10 (what they mean is not known yet; logged as they are).
		constexpr std::uintptr_t SoundCommand = 0x00567B30;

		using LookupFn = std::int32_t(__fastcall*)(void* system, void* unused, const char* bank, const char* name);
		using PlayAtFn = std::int32_t(__fastcall*)(void* system, void* unused, std::int32_t handle, std::int32_t category, std::int32_t sound, const std::int32_t* position);
		using PlayFn = std::int32_t(__fastcall*)(void* system, void* unused, std::int32_t handle, std::int32_t category, std::int32_t sound);
		using CommandFn = std::int32_t(__fastcall*)(void* system, void* unused, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d);
		LookupFn originalLookup = nullptr;
		PlayAtFn originalPlayAt = nullptr;
		PlayFn originalPlay = nullptr;
		CommandFn originalCommand = nullptr;

		std::mutex lock;
		std::unordered_map<std::int32_t, std::string> categoryNames;
		// Each (kind, caller, category, sound) is written out the first few times; after that it is only counted, and the
		// counts are written every half minute so busy sounds (footsteps) do not bury the rest.
		constexpr std::uint32_t LinesPerSound = 3;
		constexpr ULONGLONG SummaryMilliseconds = 30000;
		using SoundKey = std::tuple<char, std::uintptr_t, std::int32_t, std::int32_t>;
		std::map<SoundKey, std::uint32_t> counts;
		std::map<SoundKey, std::uint32_t> unwritten;
		ULONGLONG lastSummary = 0;

		std::string Hex(std::uintptr_t value) {
			char text[16]{};
			std::snprintf(text, sizeof(text), "0x%X", static_cast<unsigned>(value));
			return text;
		}
		// A return address in the game, as an address in our Ghidra export (default image base).
		std::uintptr_t GameAddress(void* returnAddress) {
			const auto address = reinterpret_cast<std::uintptr_t>(returnAddress);
			const auto base = Memory::GetModuleBase();
			if (address < base || address >= base + Memory::GetModuleSize()) return address;
			return address - base + Game::ImageBase;
		}
		std::string Turn() {
			auto* world = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (world == nullptr) return "menu";
			return std::to_string(reinterpret_cast<std::uint32_t(__fastcall*)(void*)>(Game::Resolve(Game::Address::WorldGameTurn))(world));
		}
		std::string CategoryName(std::int32_t category) {
			const auto found = categoryNames.find(category);
			return found != categoryNames.end() ? found->second : "?";
		}
		// True if this one should be written out in full. Called with the lock held.
		bool Count(const SoundKey& key) {
			const auto seen = ++counts[key];
			if (seen <= LinesPerSound) return true;
			++unwritten[key];
			return false;
		}
		void WriteSummary() {
			const auto now = GetTickCount64();
			if (now - lastSummary < SummaryMilliseconds) return;
			lastSummary = now;
			for (const auto& [key, count] : unwritten) {
				const auto& [kind, caller, category, sound] = key;
				Logger::Debug("Sound: " + std::to_string(count) + " more " + (kind == 'c' ? "commands" : "plays") + " of category "
					+ std::to_string(category) + " (" + CategoryName(category) + ") sound " + std::to_string(sound) + " from " + Hex(caller)
					+ " in the last " + std::to_string(SummaryMilliseconds / 1000) + " s (" + std::to_string(counts[key]) + " in all).");
			}
			unwritten.clear();
		}

		std::int32_t __fastcall LookupDetour(void* system, void* unused, const char* bank, const char* name) {
			const auto category = originalLookup(system, unused, bank, name);
			std::lock_guard guard(lock);
			const std::string text = std::string(bank != nullptr ? bank : "") + "/" + (name != nullptr ? name : "");
			const auto found = categoryNames.find(category);
			if (found == categoryNames.end() || found->second != text) {
				categoryNames[category] = text;
				Logger::Debug("Sound: category " + std::to_string(category) + " is " + text + ".");
			}
			return category;
		}
		std::int32_t __fastcall PlayAtDetour(void* system, void* unused, std::int32_t handle, std::int32_t category, std::int32_t sound, const std::int32_t* position) {
			const auto caller = GameAddress(_ReturnAddress());
			const auto result = originalPlayAt(system, unused, handle, category, sound, position);
			std::lock_guard guard(lock);
			if (Count({ 'p', caller, category, sound })) {
				std::string where = "none";
				if (position != nullptr) where = "tile " + std::to_string(position[0] >> 12) + "," + std::to_string(position[1] >> 12);
				Logger::Debug("Sound: play category " + std::to_string(category) + " (" + CategoryName(category) + ") sound " + std::to_string(sound)
					+ " at " + where + ", handle " + Hex(static_cast<std::uint32_t>(handle)) + " -> " + Hex(static_cast<std::uint32_t>(result))
					+ ", from " + Hex(caller) + ", turn " + Turn() + ".");
			}
			WriteSummary();
			return result;
		}
		std::int32_t __fastcall PlayDetour(void* system, void* unused, std::int32_t handle, std::int32_t category, std::int32_t sound) {
			const auto caller = GameAddress(_ReturnAddress());
			const auto result = originalPlay(system, unused, handle, category, sound);
			std::lock_guard guard(lock);
			if (Count({ 'p', caller, category, sound })) {
				Logger::Debug("Sound: play category " + std::to_string(category) + " (" + CategoryName(category) + ") sound " + std::to_string(sound)
					+ " (no position), handle " + Hex(static_cast<std::uint32_t>(handle)) + " -> " + Hex(static_cast<std::uint32_t>(result))
					+ ", from " + Hex(caller) + ", turn " + Turn() + ".");
			}
			WriteSummary();
			return result;
		}
		std::int32_t __fastcall CommandDetour(void* system, void* unused, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
			const auto caller = GameAddress(_ReturnAddress());
			{
				std::lock_guard guard(lock);
				if (Count({ 'c', caller, 0, 0 })) {
					Logger::Debug("Sound: command " + Hex(a) + " " + Hex(b) + " " + Hex(c) + " " + Hex(d) + " from " + Hex(caller) + ", turn " + Turn() + ".");
				}
				WriteSummary();
			}
			return originalCommand(system, unused, a, b, c, d);
		}

		bool Hook(std::uintptr_t address, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			const auto target = Game::Resolve(address);
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
	}

	void SoundLog::Install() {
		if (Logger::GetMinimumLevel() > LogLevel::Debug) return;
		// mov edx, ecx; push esi; push edi; mov eax, [edx+4]
		const bool lookup = Hook(SoundCategoryLookup, { 0x8B, 0xD1, 0x56, 0x57, 0x8B, 0x42, 0x04 }, reinterpret_cast<void*>(&LookupDetour), reinterpret_cast<void**>(&originalLookup));
		// mov eax, [ecx+4]; sub esp, 0x24 / 0x18 / 0x10
		const bool playAt = Hook(SoundPlayAt, { 0x8B, 0x41, 0x04, 0x83, 0xEC, 0x24 }, reinterpret_cast<void*>(&PlayAtDetour), reinterpret_cast<void**>(&originalPlayAt));
		const bool play = Hook(SoundPlay, { 0x8B, 0x41, 0x04, 0x83, 0xEC, 0x18 }, reinterpret_cast<void*>(&PlayDetour), reinterpret_cast<void**>(&originalPlay));
		const bool command = Hook(SoundCommand, { 0x8B, 0x41, 0x04, 0x83, 0xEC, 0x10 }, reinterpret_cast<void*>(&CommandDetour), reinterpret_cast<void**>(&originalCommand));
		Logger::Debug(std::string("Sound log: on (category names ") + (lookup ? "yes" : "no") + ", plays " + (playAt && play ? "yes" : "partly") + ", commands "
			+ (command ? "yes" : "no") + "). Each sound is written out " + std::to_string(LinesPerSound) + " times, then counted.");
	}
}
