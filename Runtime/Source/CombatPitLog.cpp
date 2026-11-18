#include "CombatPitLog.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace KeeperForge2 {
	namespace {
		// A creature's state block (creature + 0x24E) sets a state: thiscall (block, state, value) ret 8; the block's + 8
		// is its creature. A creature's melee hit: thiscall (creature), its target's tag at creature + 0x399.
		constexpr std::uintptr_t StateBlockSetState = 0x00476D30;
		constexpr std::uintptr_t CreatureMeleeHit = 0x00498E30;
		constexpr std::uintptr_t StateBlockCreature = 0x8;
		constexpr std::uintptr_t CreatureMeleeTarget = 0x399;
		// Creature fields: owner's tag, the "pit fighter" bit of + 0x3E6, and the tag of the player whose room holds it.
		constexpr std::uintptr_t ThingTag = 0x4;
		constexpr std::uintptr_t ThingOwnerTag = 0x28;
		constexpr std::uintptr_t CreatureFlags = 0x3E6;
		constexpr std::uintptr_t ThingPosition = 0x1A;
		constexpr std::uint32_t PitFighterBit = 0x8000;
		constexpr std::uintptr_t CreatureHeldBy = 0x3CB;
		// A map tile's room tag (low 12 bits of + 2) and a room's type (+ 0x18).
		constexpr std::uintptr_t TileRoom = 0x2;
		constexpr std::uintptr_t RoomTypeOffset = 0x18;
		// A pair's hits written out in full; after that one in this many.
		constexpr std::uint32_t HitsInFull = 5;
		constexpr std::uint32_t HitsEvery = 20;

		using SetStateFn = std::int32_t(__fastcall*)(std::uint8_t* block, void* unused, std::int32_t state, std::int32_t value);
		using MeleeHitFn = std::int32_t(__fastcall*)(std::uint8_t* creature, void* unused);
		using TileAtFn = std::uint8_t*(__fastcall*)(void* world, void* unused, std::int32_t x, std::int32_t y);
		SetStateFn originalSetState = nullptr;
		MeleeHitFn originalMeleeHit = nullptr;
		std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint32_t> hitCounts;

		template<typename T> T Field(const std::uint8_t* thing, std::uintptr_t offset) { return *reinterpret_cast<const T*>(thing + offset); }
		std::uint8_t* Thing(std::uint16_t tag) {
			if (Memory::Read<std::uint8_t>(Game::Resolve(Game::Address::ThingInUse) + tag) == 0) return nullptr;
			return reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::ThingTable))[tag];
		}
		bool IsCreature(const std::uint8_t* thing) { return thing != nullptr && Field<std::int32_t>(thing, Game::Offset::ThingKind) == 0; }
		std::uint8_t* World() { return Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer)); }
		bool InCombatPit(const std::uint8_t* creature) {
			auto* world = World();
			if (world == nullptr) return false;
			const auto tileAt = reinterpret_cast<TileAtFn>((*reinterpret_cast<std::uintptr_t**>(world))[Game::Offset::WorldTileAt / sizeof(std::uintptr_t)]);
			const auto* tile = tileAt(world, nullptr, Field<std::int32_t>(creature, ThingPosition) >> 12, Field<std::int32_t>(creature, ThingPosition + 4) >> 12);
			if (tile == nullptr) return false;
			const auto roomTag = static_cast<std::uint16_t>(Field<std::uint16_t>(tile, TileRoom) & 0xFFF);
			if (roomTag == 0) return false;
			const auto* room = Thing(roomTag);
			return room != nullptr && room[RoomTypeOffset] == Game::Room::Combat_Pit;
		}
		bool Watched(const std::uint8_t* creature) {
			return IsCreature(creature) && ((Field<std::uint32_t>(creature, CreatureFlags) & PitFighterBit) != 0 || InCombatPit(creature));
		}
		std::string Hex(std::uint32_t value) {
			char text[16]{};
			std::snprintf(text, sizeof(text), "0x%X", static_cast<unsigned>(value));
			return text;
		}
		std::string Owner(const std::uint8_t* creature) {
			const auto tag = Field<std::uint16_t>(creature, ThingOwnerTag);
			if (tag == Memory::Read<std::uint16_t>(Game::Resolve(Game::Address::NeutralPlayerTag))) return "neutral";
			const auto* player = Thing(tag);
			return player != nullptr ? "player " + std::to_string(player[Game::Offset::PlayerNumber]) : "owner tag " + std::to_string(tag);
		}
		std::string Describe(const std::uint8_t* creature) {
			const auto flags = Field<std::uint32_t>(creature, CreatureFlags);
			return "creature " + std::to_string(Field<std::uint16_t>(creature, ThingTag)) + " (type " + std::to_string(creature[Game::Offset::CreatureType])
				+ ", " + Owner(creature) + ", state " + Hex(static_cast<std::uint32_t>(Field<std::int32_t>(creature, Game::Offset::CreatureState)))
				+ ", fighter " + ((flags & PitFighterBit) != 0 ? "yes" : "no") + ", in pit " + (InCombatPit(creature) ? "yes" : "no")
				+ ", held by tag " + std::to_string(Field<std::uint16_t>(creature, CreatureHeldBy)) + ")";
		}
		std::string Turn() {
			auto* world = World();
			if (world == nullptr) return "?";
			return std::to_string(reinterpret_cast<std::uint32_t(__fastcall*)(void*)>(Game::Resolve(Game::Address::WorldGameTurn))(world));
		}

		std::int32_t __fastcall SetStateDetour(std::uint8_t* block, void* unused, std::int32_t state, std::int32_t value) {
			auto* creature = block != nullptr ? Field<std::uint8_t*>(block, StateBlockCreature) : nullptr;
			std::string before;
			const bool watched = creature != nullptr && Watched(creature);
			if (watched) before = Describe(creature);
			const auto result = originalSetState(block, unused, state, value);
			if (watched || (creature != nullptr && Watched(creature))) {
				Logger::Debug("Combat Pit: state " + Hex(static_cast<std::uint32_t>(state)) + " (value " + std::to_string(value) + ", result " + std::to_string(result)
					+ ") for " + (before.empty() ? "a creature" : before) + ", now " + Describe(creature) + ", turn " + Turn() + ".");
			}
			return result;
		}
		std::int32_t __fastcall MeleeHitDetour(std::uint8_t* creature, void* unused) {
			const auto* target = creature != nullptr ? Thing(Field<std::uint16_t>(creature, CreatureMeleeTarget)) : nullptr;
			if (creature != nullptr && IsCreature(creature) && (Watched(creature) || (IsCreature(target) && Watched(target)))) {
				const auto key = std::make_pair(Field<std::uint16_t>(creature, ThingTag), target != nullptr ? Field<std::uint16_t>(target, ThingTag) : std::uint16_t{ 0 });
				const auto count = ++hitCounts[key];
				if (count <= HitsInFull || count % HitsEvery == 0) {
					Logger::Debug("Combat Pit: hit " + std::to_string(count) + " by " + Describe(creature) + " on "
						+ (IsCreature(target) ? Describe(target) : std::string("a non-creature")) + ", turn " + Turn() + ".");
				}
			}
			return originalMeleeHit(creature, unused);
		}

		bool Hook(std::uintptr_t address, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			const auto target = Game::Resolve(address);
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
	}

	void CombatPitLog::Install() {
		if (Logger::GetMinimumLevel() > LogLevel::Debug) return;
		// sub esp, 0x10; push ebx; push ebp; push esi / sub esp, 0x20; push ebx; push ebp; push esi; push edi
		const bool states = Hook(StateBlockSetState, { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56 }, reinterpret_cast<void*>(&SetStateDetour), reinterpret_cast<void**>(&originalSetState));
		const bool hits = Hook(CreatureMeleeHit, { 0x83, 0xEC, 0x20, 0x53, 0x55, 0x56, 0x57 }, reinterpret_cast<void*>(&MeleeHitDetour), reinterpret_cast<void**>(&originalMeleeHit));
		Logger::Debug(std::string("Combat Pit log: on (states ") + (states ? "yes" : "no") + ", hits " + (hits ? "yes" : "no") + ").");
	}
}
