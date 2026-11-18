#include "CreatureFixes.h"
#include "CoopCampaign.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		// Creature states (Offset::CreatureState / CreatureArrivalState), see docs/game/creature-behavior.md.
		constexpr std::int32_t StateIdle = 1;
		constexpr std::int32_t StateWalkingToRoom = 7;
		constexpr std::int32_t StateMakeHome = 0x50;

		using NewPathFn = void(__fastcall*)(std::uintptr_t creature, void* unused);
		using SetStateFn = void(__fastcall*)(std::uintptr_t creature, void* unused, std::int32_t state);
		using ResetWalksFn = void(__fastcall*)(std::uintptr_t player, void* unused);
		ResetWalksFn originalResetWalks = nullptr;

		template<typename T> T Field(std::uintptr_t thing, std::uintptr_t offset) { return *reinterpret_cast<const T*>(thing + offset); }
		std::uintptr_t Thing(std::uint16_t tag) { return reinterpret_cast<const std::uintptr_t*>(Game::Resolve(Game::Address::ThingTable))[tag]; }

		// The game's own choice of which creatures to reset, except a walk to make a home.
		void __fastcall ResetWalksDetour(std::uintptr_t player, void*) {
			const auto newPath = reinterpret_cast<NewPathFn>(Game::Resolve(Game::Address::CreatureNewPath));
			const auto setState = reinterpret_cast<SetStateFn>(Game::Resolve(Game::Address::CreatureSetState));
			for (std::uintptr_t creature = Thing(Field<std::uint16_t>(player, Game::Offset::PlayerFirstCreature)); creature != 0;
				creature = Thing(Field<std::uint16_t>(creature, Game::Offset::CreatureNextOwned))) {
				const auto state = Field<std::int32_t>(creature, Game::Offset::CreatureState);
				const auto arrival = Field<std::int32_t>(creature, Game::Offset::CreatureArrivalState);
				bool reset = (state >= 3 && state <= 4) || state == StateWalkingToRoom || (state == 6 && arrival == StateIdle);
				if (state == StateWalkingToRoom && arrival == StateMakeHome) reset = false;
				if (!reset) continue;
				newPath(creature, nullptr);
				setState(creature, nullptr, StateIdle);
			}
		}

		// ---- Untagged tiles. Untagging removes the dig or gold task, but a worker already swinging at the tile went on: its
		// state returns at once while its action runs and only checks the task between actions, and the dig is one
		// long action. If the task is gone, the action is stopped first (as the game does when a worker leaves its task,
		// 0x4647D0); the state then finds the task gone and the worker goes back to idle.
		constexpr std::uint8_t TaskDig = 1;
		constexpr std::uint8_t TaskGold = 2;
		using WorkerStateFn = int(__fastcall*)(std::uintptr_t job, void* unused);
		using CreatureFn = int(__fastcall*)(std::uintptr_t creature, void* unused);
		using TaskStillThereFn = int(__fastcall*)(std::uintptr_t list, void* unused, std::uint32_t index, std::uint32_t kind);
		WorkerStateFn originalDigState = nullptr;
		WorkerStateFn originalMineGoldState = nullptr;
		void StopIfTaskGone(std::uintptr_t job, std::uint8_t kind) {
			const auto creature = Field<std::uintptr_t>(job, Game::Offset::WorkerJobCreature);
			if (creature == 0) return;
			if (!reinterpret_cast<CreatureFn>(Game::Resolve(Game::Address::CreatureActionRunning))(creature, nullptr)) return;
			const auto player = Field<std::uintptr_t>(creature, Game::Offset::CreaturePlayer);
			const auto list = player != 0 ? Field<std::uintptr_t>(player, Game::Offset::PlayerTaskList) : 0;
			if (list == 0) return;
			const auto task = Field<std::uint16_t>(job, Game::Offset::WorkerJobTask);
			if (reinterpret_cast<TaskStillThereFn>(Game::Resolve(Game::Address::TaskStillThere))(list, nullptr, task, kind)) return;
			reinterpret_cast<CreatureFn>(Game::Resolve(Game::Address::CreatureStopAction))(creature, nullptr);
		}
		int __fastcall DigStateDetour(std::uintptr_t job, void* unused) {
			StopIfTaskGone(job, TaskDig);
			return originalDigState(job, unused);
		}
		int __fastcall MineGoldStateDetour(std::uintptr_t job, void* unused) {
			StopIfTaskGone(job, TaskGold);
			return originalMineGoldState(job, unused);
		}
		bool InstallUntagFix() {
			const auto dig = Game::Resolve(Game::Address::WorkerDigState);
			const auto gold = Game::Resolve(Game::Address::WorkerMineGoldState);
			// sub esp, 0x38 / push esi / mov esi, ecx (dig) and sub esp, 0x38 / push ebx / push ebp / push esi (gold)
			if (!Memory::Matches(dig, { 0x83, 0xEC, 0x38, 0x56, 0x8B, 0xF1 }) || !Memory::Matches(gold, { 0x83, 0xEC, 0x38, 0x53, 0x55, 0x56 })) return false;
			if (!HookManager::CreateHook(dig, reinterpret_cast<void*>(&DigStateDetour), reinterpret_cast<void**>(&originalDigState))
				|| !HookManager::CreateHook(gold, reinterpret_cast<void*>(&MineGoldStateDetour), reinterpret_cast<void**>(&originalMineGoldState))
				|| !HookManager::EnableHook(dig) || !HookManager::EnableHook(gold)) {
				HookManager::RemoveHook(dig);
				HookManager::RemoveHook(gold);
				return false;
			}
			return true;
		}

		// ---- Possessing a sleeping creature: a creature in a sleep-type state (its state's flags have 4) cannot be
		// attacked (creature + 0x3EA bit 0x40 off, 0x45A8B0), and possessing it leaves that state as it was, so enemies and
		// most traps ignored a possessed sleeper. A possessed creature (+ 0x3E6 bit 0x80) now skips that test; the other
		// tests stay.
		std::uintptr_t attackableResume = 0;
		std::uintptr_t attackableSkip = 0;
		__declspec(naked) void AttackableStateStub() {
			__asm {
				test byte ptr [esi + 0x3E6], 0x80
				jnz possessed
				mov eax, dword ptr [esi + 0x25E]
				jmp dword ptr [attackableResume]
			possessed:
				jmp dword ptr [attackableSkip]
			}
		}
		bool InstallPossessionFix() {
			const auto test = Game::Resolve(Game::Address::AttackableStateTest);
			attackableResume = Game::Resolve(Game::Address::AttackableStateResume);
			attackableSkip = Game::Resolve(Game::Address::AttackableStateSkip);
			// mov eax, [esi+0x25E]; lea eax, [eax+eax*4]; test byte [eax+eax*4+0x6DA8C8], 4; jnz +0x3E
			return Memory::Matches(test, { 0x8B, 0x86, 0x5E, 0x02, 0x00, 0x00, 0x8D, 0x04, 0x80, 0xF6, 0x84, 0x80, 0xC8, 0xA8, 0x6D, 0x00, 0x04, 0x75, 0x3E })
				&& Memory::WriteJump(test, reinterpret_cast<const void*>(&AttackableStateStub), 6);
		}

		// ---- Dig and gold task slots: a task has 3 slots on each of its 4 faces, and a worker looking for work takes a
		// free slot (tag 0) or one whose holder is farther away and working. A slot still holding a worker that died, was
		// given other work or now holds another place was neither, so that face kept one worker fewer for as long as the
		// task lasted. The search's slot reads now clear such a slot first: its holder must be a creature whose job is
		// this task, face and slot. The game's own state rules for taking a slot are unchanged.
		using SlotHolderFn = std::uint32_t(__fastcall*)(std::uintptr_t list, void* unused, std::uint32_t task, std::uint32_t face, std::uint32_t slot);
		using SetSlotHolderFn = std::int32_t(__fastcall*)(std::uintptr_t list, void* unused, std::uint32_t task, std::uint32_t face, std::uint32_t slot, std::uint32_t tag);
		SlotHolderFn originalSlotHolder = nullptr;
		SetSlotHolderFn setSlotHolder = nullptr;
		bool staleSlotLogged = false;
		bool HoldsSlot(std::uint16_t tag, std::uint32_t task, std::uint32_t face, std::uint32_t slot) {
			if (Memory::Read<std::uint8_t>(Game::Resolve(Game::Address::ThingInUse) + tag) == 0) return false;
			const auto creature = Thing(tag);
			if (creature == 0 || Field<std::int32_t>(creature, Game::Offset::ThingKind) != 0) return false;
			const auto job = creature + Game::Offset::CreatureStateBlock;
			return Field<std::uint16_t>(job, Game::Offset::WorkerJobTask) == static_cast<std::uint16_t>(task)
				&& Field<std::uint16_t>(job, Game::Offset::WorkerJobFace) == static_cast<std::uint16_t>(face)
				&& Field<std::uint16_t>(job, Game::Offset::WorkerJobSlot) == static_cast<std::uint16_t>(slot);
		}
		std::uint32_t __fastcall SlotHolderDetour(std::uintptr_t list, void* unused, std::uint32_t task, std::uint32_t face, std::uint32_t slot) {
			const auto result = originalSlotHolder(list, unused, task, face, slot);
			const auto tag = static_cast<std::uint16_t>(result);
			if (tag == 0 || HoldsSlot(tag, task, face, slot)) return result;
			setSlotHolder(list, nullptr, task, face, slot, 0);
			if (!staleSlotLogged) {
				staleSlotLogged = true;
				Logger::Debug("Creature fixes: freed a dig or gold slot held by a creature no longer working it (tag " + std::to_string(tag) + "; first time).");
			}
			return result & 0xFFFF0000u;
		}
		bool InstallTaskSlotFix() {
			const auto holder = Game::Resolve(Game::Address::TaskSlotHolder);
			originalSlotHolder = reinterpret_cast<SlotHolderFn>(holder);
			setSlotHolder = reinterpret_cast<SetSlotHolderFn>(Game::Resolve(Game::Address::TaskSetSlotHolder));
			const auto call = [holder](std::uintptr_t at) {
				const auto rel = static_cast<std::uint32_t>(holder - (at + 5));
				return std::vector<std::uint8_t>{ 0xE8, static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
			};
			const auto dig = Game::Resolve(Game::Address::DigSearchSlotRead);
			const auto gold = Game::Resolve(Game::Address::GoldSearchSlotRead);
			return Memory::Matches(dig, call(dig)) && Memory::Matches(gold, call(gold))
				&& Memory::WriteCall(dig, reinterpret_cast<const void*>(&SlotHolderDetour))
				&& Memory::WriteCall(gold, reinterpret_cast<const void*>(&SlotHolderDetour));
		}

		// ---- Portal table (types past 46). Each player has a per-type portal table of only 46 slots (player + 0xB57), filled
		// with the world's first 46 creature types; the portal's choice looks a type up there and takes slot 0 when it is not
		// found, so a type past 46 (added by a mod) never got the first-arrival boost or a block of its own. The slots are now
		// given to the types that can come through a portal first (keeper Portal creatures, mercenaries, heroes, then the
		// rest), and the per-tick pass (0x4DBD90) takes each slot's type from the slot instead of from the world's list at
		// that place, which is the same for the game's own 46 types.
		constexpr std::size_t PortalSlots = 46;
		constexpr std::size_t PortalSlotSize = 0x12;
		constexpr std::uint32_t KeeperPortalFlag = 0x400;
		constexpr std::uint32_t EvilFlag = 0x80;
		constexpr std::uintptr_t CreatureDataFlags2 = 0x68A;
		constexpr std::uint32_t MercenaryFlag = 8;
		using LevelSetupFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::uint32_t value);
		LevelSetupFn originalLevelSetup = nullptr;
		using WorldCountFn = std::uint32_t(__fastcall*)(void* world, void* unused);
		using WorldTypeAtFn = std::uint8_t(__fastcall*)(void* world, void* unused, std::uint32_t index);
		using WorldDataFn = std::uint8_t*(__fastcall*)(void* world, void* unused, std::uint32_t type);
		bool portalSlotsLogged = false;
		template<typename Fn> Fn WorldMethod(void* world, std::uintptr_t offset) {
			return reinterpret_cast<Fn>((*reinterpret_cast<std::uintptr_t**>(world))[offset / sizeof(std::uintptr_t)]);
		}
		// 0 keeper Portal, 1 mercenary, 2 hero, 3 the rest.
		int PortalGroup(const std::uint8_t* data) {
			if (data == nullptr) return 3;
			const auto flags = *reinterpret_cast<const std::uint32_t*>(data + Game::Offset::CreatureDataFlags);
			if ((flags & KeeperPortalFlag) != 0) return 0;
			if ((data[CreatureDataFlags2] & MercenaryFlag) != 0) return 1;
			if ((flags & EvilFlag) == 0) return 2;
			return 3;
		}
		void OrderPortalSlots(std::uint8_t* player) {
			auto* world = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (world == nullptr || player == nullptr) return;
			auto* table = player + Game::Offset::PlayerPortalTable;
			// Only a table the set-up has just filled: every slot's fields after the type are still 0.
			for (std::size_t slot = 0; slot < PortalSlots; ++slot) {
				const auto* entry = table + slot * PortalSlotSize;
				for (std::size_t i = 1; i < PortalSlotSize; ++i) if (entry[i] != 0) return;
			}
			const auto count = WorldMethod<WorldCountFn>(world, Game::Offset::WorldCreatureTypeCount)(world, nullptr);
			if (count <= PortalSlots) return;
			std::vector<std::uint8_t> groups[4];
			for (std::uint32_t index = 0; index < count; ++index) {
				const auto type = WorldMethod<WorldTypeAtFn>(world, Game::Offset::WorldCreatureTypeAt)(world, nullptr, index);
				const auto* data = WorldMethod<WorldDataFn>(world, Game::Offset::WorldCreatureData)(world, nullptr, type);
				groups[PortalGroup(data)].push_back(type);
			}
			std::vector<std::uint8_t> order;
			for (const auto& group : groups) order.insert(order.end(), group.begin(), group.end());
			for (std::size_t slot = 0; slot < PortalSlots; ++slot) table[slot * PortalSlotSize] = order[slot];
			if (!portalSlotsLogged) {
				portalSlotsLogged = true;
				const auto portalTypes = groups[0].size() + groups[1].size() + groups[2].size();
				if (portalTypes > PortalSlots) {
					Logger::Warning("Creature fixes: " + std::to_string(portalTypes) + " creature types can come through a portal but the portal table has "
						+ std::to_string(PortalSlots) + " places; the last ones use the first place's values.");
				}
				else Logger::Debug("Creature fixes: " + std::to_string(count) + " creature types; the portal table's 46 places go to the " + std::to_string(portalTypes)
					+ " that can come through a portal first.");
			}
		}
		std::int32_t __fastcall LevelSetupDetour(std::uint8_t* player, void* unused, std::uint32_t value) {
			const auto result = originalLevelSetup(player, unused, value);
			OrderPortalSlots(player);
			return result;
		}
		bool InstallPortalSlotsFix() {
			const auto pass = Game::Resolve(Game::Address::PortalPassTypeOfSlot);
			const auto world = static_cast<std::uint32_t>(Game::Resolve(Game::Address::WorldPointer));
			// mov ecx, [world]; push eax; mov edx, [ecx]; call [edx + 0xB8]
			const std::vector<std::uint8_t> expected = { 0x8B, 0x0D, static_cast<std::uint8_t>(world), static_cast<std::uint8_t>(world >> 8),
				static_cast<std::uint8_t>(world >> 16), static_cast<std::uint8_t>(world >> 24), 0x50, 0x8B, 0x11, 0xFF, 0x92, 0xB8, 0x00, 0x00, 0x00 };
			const auto setup = Game::Resolve(Game::Address::PlayerLevelSetup);
			// mov eax, fs:[0]; push -1
			if (!Memory::Matches(pass, expected) || !Memory::Matches(setup, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF })) return false;
			if (!HookManager::CreateHook(setup, reinterpret_cast<void*>(&LevelSetupDetour), reinterpret_cast<void**>(&originalLevelSetup)) || !HookManager::EnableHook(setup)) {
				HookManager::RemoveHook(setup);
				return false;
			}
			// mov edx, [esp + 0x10] (the slot's + 2); xor eax, eax; mov al, [edx - 2] (its type); 6 nops
			Memory::Patch(pass, { 0x8B, 0x54, 0x24, 0x10, 0x33, 0xC0, 0x8A, 0x42, 0xFE, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
			return true;
		}

		// ---- Portal pool. A portal only brings a creature type whose pool number (player + 0x1109) is above 0, and the pool
		// is only set by the level's Variables (kind 1); the script's "make available" changes availability, which the
		// portal never reads. Levels 15a, 15b, 17, 18, 19 and 20 let the keeper have the Temple and the Combat Pit but put no
		// Dark Angels or Black Knights in its pool, so they never came. See docs/game/game-fixes.md.
		struct PoolFix {
			std::uint8_t creature;
			std::uint8_t room;
			const char* name;
		};
		constexpr std::array<PoolFix, 2> PoolFixes = { {
			{ Game::Creature::Dark_Angel, Game::Room::Temple, "Dark Angels (Temple)" },
			{ Game::Creature::Black_Knight, Game::Room::Combat_Pit, "Black Knights (Combat Pit)" },
		} };
		// Pool entries are { type byte, int number }.
		constexpr std::size_t PoolEntrySize = 5;
		// When the keeper has no other pool numbers to go by.
		constexpr std::int32_t DefaultPoolNumber = 20;
		// Per player number: whether its first portal choice in this level has been seen, and which fixes still wait for
		// their room.
		std::array<bool, 256> poolChecked{};
		std::array<std::array<bool, PoolFixes.size()>, 256> poolWaiting{};

		std::uint8_t* PoolEntry(std::uintptr_t player, std::uint8_t type) {
			const auto pool = player + Game::Offset::PlayerCreaturePool;
			const auto count = Field<std::uint32_t>(pool, Game::Offset::CreaturePoolCount);
			auto* entries = Field<std::uint8_t*>(pool, Game::Offset::CreaturePoolEntries);
			for (std::uint32_t i = 0; entries != nullptr && i < count; ++i) {
				if (entries[i * PoolEntrySize] == type) return entries + i * PoolEntrySize;
			}
			return nullptr;
		}
		std::int32_t PoolNumber(const std::uint8_t* entry) { return *reinterpret_cast<const std::int32_t*>(entry + 1); }
		// The largest number in the keeper's pool: the level's own idea of how many of a type may come.
		std::int32_t LevelPoolNumber(std::uintptr_t player) {
			const auto pool = player + Game::Offset::PlayerCreaturePool;
			const auto count = Field<std::uint32_t>(pool, Game::Offset::CreaturePoolCount);
			const auto* entries = Field<const std::uint8_t*>(pool, Game::Offset::CreaturePoolEntries);
			std::int32_t most = 0;
			for (std::uint32_t i = 0; entries != nullptr && i < count; ++i) {
				const auto number = PoolNumber(entries + i * PoolEntrySize);
				if (number > most) most = number;
			}
			return most > 0 ? most : DefaultPoolNumber;
		}
		bool OwnsType(std::uintptr_t player, std::uint8_t type) {
			for (std::uintptr_t creature = Thing(Field<std::uint16_t>(player, Game::Offset::PlayerFirstCreature)); creature != 0;
				creature = Thing(Field<std::uint16_t>(creature, Game::Offset::CreatureNextOwned))) {
				if (Field<std::uint8_t>(creature, Game::Offset::CreatureType) == type) return true;
			}
			return false;
		}
		// The room is researchable or available to the keeper (its room list: { id, state int } entries). The setter
		// 0x4BAAD0 stores the value as given: the level's Variables 4 (available), 3 (researchable) or 1 (not), the
		// script's "make available" 3 or 1. So anything from 2 up counts.
		constexpr std::uint32_t FirstUsableRoomState = 2;
		bool CanHaveRoom(std::uintptr_t player, std::uint8_t room) {
			const auto count = Field<std::uint32_t>(player, Game::Offset::PlayerRoomCount);
			const auto* entries = Field<const std::uint8_t*>(player, Game::Offset::PlayerRooms);
			for (std::uint32_t i = 0; entries != nullptr && i < count; ++i) {
				const auto* entry = entries + i * Game::AvailabilityEntrySize;
				if (entry[0] != room) continue;
				const auto state = *reinterpret_cast<const std::uint32_t*>(entry + 1);
				return state >= FirstUsableRoomState;
			}
			return false;
		}
		// Pools come from the level only in the campaign (and campaign levels played together); in skirmish and
		// multiplayer they are the lobby's numbers, which are left alone.
		bool LevelSetsPools() {
			return Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode)) == Game::PlayModeCampaign || CoopCampaign::InSession();
		}
	}

	void CreatureFixes::OnLevelStart() {
		poolChecked.fill(false);
		for (auto& waiting : poolWaiting) waiting.fill(false);
	}

	void CreatureFixes::BeforePortalChoice(std::uint8_t* room) {
		if (room == nullptr || !LevelSetsPools()) return;
		const auto player = Thing(*reinterpret_cast<const std::uint16_t*>(room + Game::Offset::RoomOwner));
		if (player == 0) return;
		const auto number = Field<std::uint8_t>(player, Game::Offset::PlayerNumber);
		if (number < Game::FirstKeeperPlayer) return;
		// The first choice in a level: nothing has come through a portal yet, so a number of 0 (with none owned, in case
		// this is a loaded game) is one the level never set.
		if (!poolChecked[number]) {
			poolChecked[number] = true;
			for (std::size_t i = 0; i < PoolFixes.size(); ++i) {
				const auto* entry = PoolEntry(player, PoolFixes[i].creature);
				poolWaiting[number][i] = entry != nullptr && PoolNumber(entry) == 0 && !OwnsType(player, PoolFixes[i].creature);
			}
		}
		for (std::size_t i = 0; i < PoolFixes.size(); ++i) {
			if (!poolWaiting[number][i] || !CanHaveRoom(player, PoolFixes[i].room)) continue;
			poolWaiting[number][i] = false;
			auto* entry = PoolEntry(player, PoolFixes[i].creature);
			if (entry == nullptr) continue;
			const auto amount = LevelPoolNumber(player);
			*reinterpret_cast<std::int32_t*>(entry + 1) = amount;
			Logger::Info(std::string("Creature fixes: player ") + std::to_string(number) + " can have " + PoolFixes[i].name
				+ " but the level gave none in its creature pool; " + std::to_string(amount) + " added.");
		}
	}

	void CreatureFixes::Install() {
		if (InstallPortalSlotsFix()) Logger::Debug("Creature fixes: the portal table's places go to the creature types that can come through a portal first.");
		else Logger::Warning("Creature fixes: unexpected game code, creature types past the 46th use the portal table's first place.");
		if (InstallTaskSlotFix()) Logger::Debug("Creature fixes: dig and gold slots held by creatures no longer working them are freed.");
		else Logger::Warning("Creature fixes: unexpected game code, dig and gold slots are not checked.");
		if (InstallPossessionFix()) Logger::Debug("Creature fixes: a possessed creature can be attacked even in a sleep-type state.");
		else Logger::Warning("Creature fixes: unexpected game code, a possessed sleeping creature still cannot be attacked.");
		if (InstallUntagFix()) Logger::Debug("Creature fixes: workers stop digging or mining a tile as soon as it is untagged.");
		else Logger::Warning("Creature fixes: unexpected game code, workers still finish their swing at an untagged tile.");
		const auto target = Game::Resolve(Game::Address::ResetCreatureWalks);
		// xor eax, eax / push esi / mov ax, [ecx + 6] / mov esi, [eax*4 + 0x6EE540]
		if (!Memory::Matches(target, { 0x33, 0xC0, 0x56, 0x66, 0x8B, 0x41, 0x06, 0x8B, 0x34, 0x85, 0x40, 0xE5, 0x6E, 0x00 })) {
			Logger::Warning("Creature fixes: unexpected game code, creatures making a home can still be stopped.");
			return;
		}
		if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&ResetWalksDetour), reinterpret_cast<void**>(&originalResetWalks)) || !HookManager::EnableHook(target)) {
			HookManager::RemoveHook(target);
			Logger::Warning("Creature fixes: failed to hook the creature reset, creatures making a home can still be stopped.");
			return;
		}
		Logger::Debug("Creature fixes: creatures walking to make a home are no longer stopped when the map changes.");
	}
}
