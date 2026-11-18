#include "CoopHands.h"
#include "CoopCampaign.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <intrin.h>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }
		bool On() { return CoopCampaign::InSession(); }
		std::uint8_t* World() { return *reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::WorldPointer)); }
		std::uint8_t* Thing(std::uint16_t tag) { return reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::ThingTable))[tag]; }
		std::uint16_t LocalTag() { return Load<std::uint16_t>(World() + Game::Offset::WorldLocalPlayerTag); }
		constexpr std::size_t TagCount = 4096;
		constexpr std::size_t HandSize = 64;

		// 1 and up, one for each machine; 0 is nobody's (the game's or a script's own pick-ups).
		std::uint8_t localOrigin = 1;

		// ---- Actions carry which machine made them, in bits the game does not use: a thing's tag has 12 bits, and a
		// drop's direction 11. Every machine reads the same action, so all agree on whose hand a thing is in.
		constexpr std::int32_t ActionPossess = 55;
		constexpr std::int32_t ActionFirstPickup = 57;
		constexpr std::int32_t ActionPickupMany = 58;
		constexpr std::int32_t ActionLastPickup = 60;
		constexpr std::int32_t ActionDrop = 61;
		constexpr std::int32_t ActionMarkDig = 40;
		constexpr std::int32_t ActionUnmarkDig = 41;
		constexpr std::uint32_t PossessSpellKind = 2;
		constexpr std::uint32_t PickupOriginShift = 12;
		constexpr std::uint32_t PickupOriginMask = 0xF000;
		constexpr std::uint32_t DropOriginShift = 28;
		constexpr std::uint32_t DropKeptBits = 0x07FFFFFF;
		constexpr std::uint32_t PossessMark = 0x504F5300;
		constexpr std::size_t ActionSize = 0x12;
		bool IsPickup(std::int32_t kind) { return kind >= ActionFirstPickup && kind <= ActionLastPickup; }

		using PushActionFn = std::int32_t(__fastcall*)(void* session, void* unused, const std::uint8_t* action);
		PushActionFn originalPushAction = nullptr;
		std::int32_t __fastcall PushActionDetour(void* session, void* unused, const std::uint8_t* action) {
			if (!On()) return originalPushAction(session, unused, action);
			const auto kind = Load<std::int32_t>(action + Game::Offset::ActionKind);
			std::array<std::uint8_t, ActionSize> tagged{};
			std::memcpy(tagged.data(), action, tagged.size());
			if (IsPickup(kind)) {
				Store<std::uint32_t>(tagged.data(), Load<std::uint32_t>(action) | (static_cast<std::uint32_t>(localOrigin) << PickupOriginShift));
			}
			else if (kind == ActionDrop) {
				Store<std::uint32_t>(tagged.data() + Game::Offset::ActionData3, (Load<std::uint32_t>(action + Game::Offset::ActionData3) & DropKeptBits) | (static_cast<std::uint32_t>(localOrigin) << DropOriginShift));
			}
			else if (kind == ActionPossess && (Load<std::uint32_t>(action) & 0xFF) == PossessSpellKind) {
				Store<std::uint32_t>(tagged.data() + Game::Offset::ActionData3, PossessMark | localOrigin);
			}
			return originalPushAction(session, unused, tagged.data());
		}

		// ---- Whose hand: only a thing the keeper really took gets an owner.
		std::array<std::uint8_t, TagCount> owners{};
		std::uint8_t pickingOrigin = 0;
		std::int32_t pickingKind = 0;
		bool VisibleTo(std::uint16_t tag, std::uint8_t origin) { return tag < TagCount && (owners[tag] == 0 || owners[tag] == origin); }
		std::uint16_t* Hand(std::uint8_t* player) { return reinterpret_cast<std::uint16_t*>(player + Game::Offset::PlayerHand); }
		std::uint32_t HandCount(const std::uint8_t* player) { return player[Game::Offset::PlayerHandCount]; }
		std::int32_t LatestOf(std::uint8_t* player, std::uint8_t origin) {
			auto* hand = Hand(player);
			for (auto i = static_cast<std::int32_t>(HandCount(player)) - 1; i >= 0; --i) {
				if (VisibleTo(hand[i], origin)) return i;
			}
			return -1;
		}
		std::uint32_t CountOf(std::uint8_t* player, std::uint8_t origin) {
			std::uint32_t count = 0;
			for (std::uint32_t i = 0; i < HandCount(player); ++i) {
				if (VisibleTo(Hand(player)[i], origin)) ++count;
			}
			return count;
		}

		// The pick-up handlers take the action itself: the mark comes off before the game reads the tag.
		using ActionHandlerFn = std::int32_t(__stdcall*)(const std::uint8_t* action);
		std::array<ActionHandlerFn, 4> originalPickups{};
		std::int32_t RunPickup(ActionHandlerFn original, const std::uint8_t* action) {
			const auto kind = Load<std::int32_t>(action + Game::Offset::ActionKind);
			if (!On() || !IsPickup(kind)) return original(action);
			std::array<std::uint8_t, ActionSize> plain{};
			std::memcpy(plain.data(), action, plain.size());
			const auto data = Load<std::uint32_t>(action);
			pickingOrigin = static_cast<std::uint8_t>((data & PickupOriginMask) >> PickupOriginShift);
			pickingKind = kind;
			Store<std::uint32_t>(plain.data(), data & ~PickupOriginMask);
			const std::int32_t result = original(plain.data());
			pickingOrigin = 0;
			pickingKind = 0;
			return result;
		}
		template<std::size_t Index> std::int32_t __stdcall PickupDetour(const std::uint8_t* action) { return RunPickup(originalPickups[Index], action); }

		bool PossessedBlocks(const std::uint8_t* player, std::uint16_t tag) { return tag != 0 && Load<std::uint16_t>(player + Game::Offset::PlayerPossessed) == tag; }
		// A pick-up that takes time keeps its machine in the timed action's unused second value.
		using PlayerActionFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::int32_t type, std::uint32_t data1, std::uint32_t data2, std::uint32_t data3);
		PlayerActionFn originalPlayerAction = nullptr;
		std::int32_t __fastcall PlayerActionDetour(std::uint8_t* player, void* unused, std::int32_t type, std::uint32_t data1, std::uint32_t data2, std::uint32_t data3) {
			if (On() && type >= 1 && type <= 3) {
				if (PossessedBlocks(player, static_cast<std::uint16_t>(data1))) return 0;
				if (pickingKind == (type == 1 ? ActionFirstPickup : type + ActionFirstPickup)) data2 = pickingOrigin;
			}
			return originalPlayerAction(player, unused, type, data1, data2, data3);
		}
		constexpr std::size_t ThingKind = 0x12;
		using TakeThingFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::uint32_t tag);
		TakeThingFn originalTakeThing = nullptr;
		std::int32_t __fastcall TakeThingDetour(std::uint8_t* player, void* unused, std::uint32_t tag) {
			if (!On()) return originalTakeThing(player, unused, tag);
			const auto thing = static_cast<std::uint16_t>(tag);
			if (PossessedBlocks(player, thing)) return 0;
			const auto type = Load<std::int32_t>(player + Game::Offset::PlayerActionType);
			std::uint8_t origin = 0;
			// Finishing a timed pick-up can happen while another machine's action is being run.
			if (type >= 1 && type <= 3 && static_cast<std::uint16_t>(Load<std::uint32_t>(player + Game::Offset::PlayerActionData1)) == thing) origin = static_cast<std::uint8_t>(Load<std::uint32_t>(player + Game::Offset::PlayerActionData2));
			else if (pickingKind == ActionPickupMany) origin = pickingOrigin;
			const std::int32_t result = originalTakeThing(player, unused, tag);
			if (result != 0 && thing < TagCount) {
				owners[thing] = origin;
				const auto* taken = Thing(thing);
				const bool creature = taken != nullptr && Load<std::int32_t>(taken + ThingKind) == 0;
				Logger::Debug("Co-op hands: player " + std::to_string(player[Game::Offset::PlayerNumber]) + " took thing " + std::to_string(thing) + (creature ? " (creature type " + std::to_string(taken[Game::Offset::CreatureType]) + ")" : std::string()) + " for machine " + std::to_string(origin) + ", this machine is " + std::to_string(localOrigin) + ", " + std::to_string(HandCount(player)) + " in the hand.");
			}
			return result;
		}
		// A drop takes the last thing in the list, so this machine's own last thing is moved there first.
		using DropFn = std::int32_t(__fastcall*)(std::uint8_t* player);
		DropFn originalDrop = nullptr;
		std::int32_t __fastcall DropDetour(std::uint8_t* player) {
			if (!On()) return originalDrop(player);
			const auto origin = static_cast<std::uint8_t>(Load<std::uint32_t>(player + Game::Offset::PlayerActionData3) >> DropOriginShift);
			if (origin == 0) return originalDrop(player);
			const auto index = LatestOf(player, origin);
			if (index < 0) return 0;
			auto* hand = Hand(player);
			const auto chosen = hand[index];
			const auto count = HandCount(player);
			for (auto i = static_cast<std::uint32_t>(index) + 1; i < count; ++i) hand[i - 1] = hand[i];
			hand[count - 1] = chosen;
			return originalDrop(player);
		}

		// What the local interface asks: only these callers are about the picture and cursor. The world's own code
		// and the computer players go on seeing the whole list.
		constexpr std::array<std::uintptr_t, 10> InterfaceHandCallers = { 0x0040C043, 0x0040C14A, 0x0040D04A, 0x0040D07C, 0x0040D156, 0x0040D6A8, 0x0040D6C5, 0x0040D6D1, 0x0040E03D, 0x0040E459 };
		constexpr std::array<std::uintptr_t, 2> CursorHandCallers = { 0x0040AF59, 0x0040E4B3 };
		constexpr std::uintptr_t HandTickCaller = 0x0040E17A;
		template<std::size_t N> bool Contains(const std::array<std::uintptr_t, N>& list, std::uintptr_t value) {
			for (const auto item : list) if (item == value) return true;
			return false;
		}
		bool AsksForLocalHand(std::uintptr_t caller, std::uint32_t tag) {
			return On() && Contains(InterfaceHandCallers, caller) && static_cast<std::uint16_t>(tag) == LocalTag();
		}
		using HandAnyFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint32_t tag);
		using HandAtFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint32_t tag, std::uint32_t index, std::uint16_t* out);
		HandAnyFn originalHasThings = nullptr;
		HandAnyFn originalThingCount = nullptr;
		HandAtFn originalThingAt = nullptr;
		std::int32_t __fastcall HasThingsDetour(std::uint8_t* world, void* unused, std::uint32_t tag) {
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			if (!AsksForLocalHand(caller, tag)) return originalHasThings(world, unused, tag);
			return CountOf(Thing(static_cast<std::uint16_t>(tag)), localOrigin) != 0 ? 1 : 0;
		}
		std::int32_t __fastcall ThingCountDetour(std::uint8_t* world, void* unused, std::uint32_t tag) {
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			if (!AsksForLocalHand(caller, tag)) return originalThingCount(world, unused, tag);
			return static_cast<std::int32_t>(CountOf(Thing(static_cast<std::uint16_t>(tag)), localOrigin));
		}
		std::int32_t __fastcall ThingAtDetour(std::uint8_t* world, void* unused, std::uint32_t tag, std::uint32_t index, std::uint16_t* out) {
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			if (!AsksForLocalHand(caller, tag)) return originalThingAt(world, unused, tag, index, out);
			auto* player = Thing(static_cast<std::uint16_t>(tag));
			for (std::uint32_t i = 0; i < HandCount(player); ++i) {
				if (!VisibleTo(Hand(player)[i], localOrigin)) continue;
				if (index-- != 0) continue;
				*out = Hand(player)[i];
				return 1;
			}
			return 0;
		}
		using PlayerThingAtFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::uint32_t index, std::uint16_t* out);
		PlayerThingAtFn originalPlayerThingAt = nullptr;
		std::int32_t __fastcall PlayerThingAtDetour(std::uint8_t* player, void* unused, std::uint32_t index, std::uint16_t* out) {
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			if (!On() || !Contains(CursorHandCallers, caller) || Load<std::uint16_t>(player + Game::Offset::PlayerTag) != LocalTag()) return originalPlayerThingAt(player, unused, index, out);
			const auto latest = LatestOf(player, localOrigin);
			if (latest < 0) return 0;
			*out = Hand(player)[latest];
			return 1;
		}
		// The interface's list of things being picked up: one the other player got first is no longer on its way here.
		using HasThingFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::uint32_t tag);
		HasThingFn originalHasThing = nullptr;
		std::int32_t __fastcall HasThingDetour(std::uint8_t* player, void* unused, std::uint32_t tag) {
			const std::int32_t result = originalHasThing(player, unused, tag);
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			if (result == 0 || !On() || caller != HandTickCaller) return result;
			return VisibleTo(static_cast<std::uint16_t>(tag), localOrigin) ? result : 0;
		}

		// Owners go with the world when it is saved, as a machine that fell behind gets its world from another's save.
		constexpr std::uint32_t SavedOwnersMark = 0x31444E48;
		struct SavedOwners {
			std::uint32_t mark = SavedOwnersMark;
			std::array<std::uint8_t, HandSize> slots{};
		};
		using FileIoFn = std::int32_t*(__cdecl*)(std::int32_t* status, void* file, void* data, std::uint32_t size, std::int32_t* done);
		using PlayerFileFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, void** file);
		PlayerFileFn originalSavePlayer = nullptr;
		PlayerFileFn originalLoadPlayer = nullptr;
		std::int32_t __fastcall SavePlayerDetour(std::uint8_t* player, void* unused, void** file) {
			const std::int32_t result = originalSavePlayer(player, unused, file);
			if (result == 0 || !On() || HandCount(player) > HandSize) return result;
			SavedOwners saved;
			for (std::uint32_t i = 0; i < HandCount(player); ++i) {
				const auto tag = Hand(player)[i];
				saved.slots[i] = tag < TagCount ? owners[tag] : 0;
			}
			std::int32_t status = 0;
			std::int32_t done = 0;
			GameFunction<FileIoFn>(Game::Address::FileWrite)(&status, *file, &saved, sizeof(saved), &done);
			return status >= 0 && done == sizeof(saved) ? 1 : 0;
		}
		std::int32_t __fastcall LoadPlayerDetour(std::uint8_t* player, void* unused, void** file) {
			const std::int32_t result = originalLoadPlayer(player, unused, file);
			if (result == 0 || !On() || HandCount(player) > HandSize) return result;
			SavedOwners saved;
			std::int32_t status = 0;
			std::int32_t done = 0;
			GameFunction<FileIoFn>(Game::Address::FileRead)(&status, *file, &saved, sizeof(saved), &done);
			if (status < 0 || done != sizeof(saved) || saved.mark != SavedOwnersMark) return 0;
			for (std::uint32_t i = 0; i < HandCount(player); ++i) {
				const auto tag = Hand(player)[i];
				if (tag < TagCount) owners[tag] = saved.slots[i];
			}
			return 1;
		}

		// ---- Possession. The cast is one action on every machine, so it says which machine cast it. A shot made during
		// the cast remembers that, and when it reaches the creature, only that machine's camera goes in.
		constexpr std::uint32_t CameraKeeperModeA = 1;
		constexpr std::uint32_t CameraKeeperModeB = 2;
		constexpr std::uint32_t CameraPathMode = 18;
		std::array<std::uint32_t, TagCount> shotOrigins{};
		std::uint32_t castingOrigin = 0;
		std::uint32_t enteringOrigin = 0;
		std::uint16_t ownKeeper = 0;
		std::uint16_t ownCreature = 0;
		bool releasePending = false;
		bool ValidPossessOrigin(std::uint32_t value) { return value > PossessMark && value <= PossessMark + 8; }
		ActionHandlerFn originalCast = nullptr;
		std::int32_t __stdcall CastDetour(const std::uint8_t* action) {
			if (!On() || (Load<std::uint32_t>(action) & 0xFF) != PossessSpellKind) return originalCast(action);
			const auto mark = Load<std::uint32_t>(action + Game::Offset::ActionData3);
			castingOrigin = ValidPossessOrigin(mark) ? mark : 0;
			std::array<std::uint8_t, ActionSize> plain{};
			std::memcpy(plain.data(), action, plain.size());
			Store<std::uint32_t>(plain.data() + Game::Offset::ActionData3, 0);
			const std::int32_t result = originalCast(plain.data());
			castingOrigin = 0;
			return result;
		}
		using MakeShotFn = std::int32_t(__fastcall*)(void* list, void* unused, std::int32_t type, std::int32_t keeper, void* position, std::uint16_t* tag, std::uint8_t** shot, std::int32_t flags);
		MakeShotFn originalMakeShot = nullptr;
		std::int32_t __fastcall MakeShotDetour(void* list, void* unused, std::int32_t type, std::int32_t keeper, void* position, std::uint16_t* tag, std::uint8_t** shot, std::int32_t flags) {
			const std::int32_t result = originalMakeShot(list, unused, type, keeper, position, tag, shot, flags);
			if (result != 0 && On() && shot != nullptr && *shot != nullptr) {
				const auto made = Load<std::uint16_t>(*shot + Game::Offset::PlayerTag);
				if (made < TagCount) shotOrigins[made] = castingOrigin;
			}
			return result;
		}
		using ShotReachedFn = std::int32_t(__fastcall*)(std::uint8_t* shot);
		ShotReachedFn originalShotReached = nullptr;
		std::int32_t __fastcall ShotReachedDetour(std::uint8_t* shot) {
			if (!On()) return originalShotReached(shot);
			const auto tag = Load<std::uint16_t>(shot + Game::Offset::PlayerTag);
			if (tag < TagCount) {
				enteringOrigin = shotOrigins[tag];
				shotOrigins[tag] = 0;
			}
			const std::int32_t result = originalShotReached(shot);
			enteringOrigin = 0;
			return result;
		}
		// The two messages that put the local camera into a creature, { creature, ?, keeper }.
		using BridgeHandlerFn = std::int32_t(__fastcall*)(void* bridge, void* unused, std::uint16_t* data);
		BridgeHandlerFn originalEnterA = nullptr;
		BridgeHandlerFn originalEnterB = nullptr;
		bool MayEnter(const std::uint16_t* data) {
			if (!On() || data[2] != LocalTag()) return true;
			if (enteringOrigin != PossessMark + localOrigin) return false;
			ownKeeper = data[2];
			ownCreature = data[0];
			releasePending = false;
			return true;
		}
		std::int32_t __fastcall EnterADetour(void* bridge, void* unused, std::uint16_t* data) { return MayEnter(data) ? originalEnterA(bridge, unused, data) : 1; }
		std::int32_t __fastcall EnterBDetour(void* bridge, void* unused, std::uint16_t* data) { return MayEnter(data) ? originalEnterB(bridge, unused, data) : 1; }

		std::uint8_t* Bridge(const std::uint8_t* world) { return *reinterpret_cast<std::uint8_t* const*>(*reinterpret_cast<const std::uint8_t* const*>(world + Game::Offset::WorldSession) + Game::Offset::SessionBridge); }
		std::uint8_t* BridgeCamera(std::uint8_t* bridge) {
			const auto getCamera = (*reinterpret_cast<const std::uintptr_t* const*>(bridge))[Game::Offset::BridgeGetCamera / sizeof(std::uintptr_t)];
			return reinterpret_cast<std::uint8_t*(__fastcall*)(std::uint8_t*)>(getCamera)(bridge);
		}
		// The creature died or was left, by either machine: this machine's camera comes back out.
		void ObservePossession(std::uint8_t* world) {
			if (ownCreature != 0) {
				const auto* keeper = Thing(ownKeeper);
				if (keeper == nullptr || Load<std::uint16_t>(keeper + Game::Offset::PlayerPossessed) != ownCreature) {
					ownCreature = 0;
					releasePending = true;
				}
			}
			if (!releasePending) return;
			auto* bridge = Bridge(world);
			if (bridge == nullptr) return;
			auto* camera = BridgeCamera(bridge);
			if (camera == nullptr) return;
			releasePending = false;
			auto mode = Load<std::uint32_t>(camera + Game::Offset::CameraMode);
			// A path holds what comes after it.
			if (mode == CameraPathMode) {
				mode = Load<std::uint32_t>(camera + Game::Offset::CameraDeferredMode);
				if (mode == 0) mode = Load<std::uint32_t>(camera + Game::Offset::CameraSavedMode);
			}
			if (mode != CameraKeeperModeA && mode != CameraKeeperModeB) return;
			std::uint32_t release[] = { 0, 0, LocalTag() };
			GameFunction<std::int32_t(__fastcall*)(void*, void*, std::uint32_t*)>(Game::Address::BridgeLeaveCreature)(bridge, nullptr, release);
		}

		// Local movement in a creature changes a global the computer players then read, which would differ by machine: a
		// desync in any multiplayer game with computer players, as only one machine is in the creature.
		using LocalMovementFn = void*(__fastcall*)(void* controller);
		LocalMovementFn originalLocalMovement = nullptr;
		void* __fastcall LocalMovementDetour(void* controller) {
			auto& capability = *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::PilotNavigation));
			const auto kept = capability;
			void* result = originalLocalMovement(controller);
			if (Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode)) == Game::PlayModeMultiplayer) capability = kept;
			return result;
		}

		// ---- Health flowers: while the other machine is in a creature, the game would treat this view as in one too.
		bool __cdecl PossessionShown(const std::uint8_t* keeper) {
			const bool possessed = Load<std::uint16_t>(keeper + Game::Offset::PlayerPossessed) != 0;
			if (!possessed || !On()) return possessed;
			auto* world = World();
			auto* bridge = world != nullptr ? Bridge(world) : nullptr;
			auto* camera = bridge != nullptr ? BridgeCamera(bridge) : nullptr;
			if (camera == nullptr) return possessed;
			switch (Load<std::uint32_t>(camera + Game::Offset::CameraMode)) {
				// The game's keeper views (0x44D870).
				case 3: case 4: case 5: case 9: case 13: case 14: case 15: return false;
				default: return true;
			}
		}
		// In place of "cmp word [ebp + 0x134], 0".
		__declspec(naked) void FlowerCompareStub() {
			__asm {
				push eax
				pushfd
				pushad
				push ebp
				call PossessionShown
				add esp, 4
				mov byte ptr [esp + 28], al
				popad
				popfd
				cmp al, 0
				pop eax
				ret
			}
		}
		// In place of "cmp word [eax + 0x134], cx", with CX zero.
		__declspec(naked) void MeshCompareStub() {
			__asm {
				push eax
				pushfd
				pushad
				push eax
				call PossessionShown
				add esp, 4
				mov byte ptr [esp + 28], al
				popad
				popfd
				cmp al, 0
				pop eax
				ret
			}
		}

		bool Hook(std::uintptr_t address, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			const auto target = Game::Resolve(address);
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
		void* Pointer(auto function) { return reinterpret_cast<void*>(function); }
	}

	void CoopHands::Reset() {
		owners.fill(0);
		shotOrigins.fill(0);
		pickingOrigin = 0;
		pickingKind = 0;
		castingOrigin = 0;
		enteringOrigin = 0;
		ownKeeper = 0;
		ownCreature = 0;
		releasePending = false;
		// The lobby slot the game gave this machine: different on each.
		const auto slot = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::MyLobbySlot));
		localOrigin = static_cast<std::uint8_t>(slot >= 0 && slot < static_cast<std::int32_t>(Game::PlayerSlotCount) ? slot + 1 : 1);
	}

	void CoopHands::BeforeWorldTick(std::uint8_t* world, const std::uint8_t* actions) {
		// Marks made by the other machine are drawn on this one too, as its own are when they are made. Drawn twice for
		// its own, which changes nothing.
		auto* bridge = Bridge(world);
		if (bridge == nullptr || actions == nullptr) return;
		const auto count = actions[Game::Offset::ActionListCount];
		const auto local = LocalTag();
		for (std::uint32_t i = 0; i < count && i < Game::ActionListCapacity; ++i) {
			const auto* action = actions + Game::Offset::ActionListActions + i * ActionSize;
			const auto kind = Load<std::int32_t>(action + Game::Offset::ActionKind);
			if ((kind != ActionMarkDig && kind != ActionUnmarkDig) || Load<std::uint16_t>(action + Game::Offset::ActionPlayer) != local) continue;
			std::array<std::uint8_t, ActionSize> copy{};
			std::memcpy(copy.data(), action, copy.size());
			GameFunction<std::int32_t(__fastcall*)(void*, void*, void*)>(kind == ActionMarkDig ? Game::Address::BridgeShowDigMark : Game::Address::BridgeHideDigMark)(bridge, nullptr, copy.data());
		}
	}
	void CoopHands::AfterWorldTick(std::uint8_t* world) { ObservePossession(world); }

	bool CoopHands::Install() {
		// Checked first, as nothing should change if any of these differ.
		const auto flower = Game::Resolve(Game::Address::FlowerPossessedCompare);
		const auto mesh = Game::Resolve(Game::Address::MeshPossessedCompare);
		const std::vector<std::uint8_t> flowerBytes = { 0x66, 0x83, 0xBD, 0x34, 0x01, 0x00, 0x00, 0x00 };
		const std::vector<std::uint8_t> meshBytes = { 0x66, 0x39, 0x88, 0x34, 0x01, 0x00, 0x00 };
		if (!Memory::Matches(flower, flowerBytes) || !Memory::Matches(mesh, meshBytes)
			|| !Memory::Matches(Game::Resolve(Game::Address::BridgeLeaveCreature), { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1, 0x57 })) return false;
		const bool hooked =
			Hook(Game::Address::SessionPushAction, { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xC1, 0x1C }, Pointer(&PushActionDetour), reinterpret_cast<void**>(&originalPushAction))
			&& Hook(Game::Address::ActionPickup1, { 0x8B, 0x4C, 0x24, 0x04, 0x33, 0xD2, 0x56 }, Pointer(&PickupDetour<0>), reinterpret_cast<void**>(&originalPickups[0]))
			&& Hook(Game::Address::ActionPickup2, { 0x83, 0xEC, 0x0C, 0x8B, 0x44, 0x24, 0x10 }, Pointer(&PickupDetour<1>), reinterpret_cast<void**>(&originalPickups[1]))
			&& Hook(Game::Address::ActionPickup3, { 0x8B, 0x4C, 0x24, 0x04, 0x33, 0xD2, 0x66, 0x8B, 0x01 }, Pointer(&PickupDetour<2>), reinterpret_cast<void**>(&originalPickups[2]))
			&& Hook(Game::Address::ActionPickup4, { 0x8B, 0x4C, 0x24, 0x04, 0x33, 0xD2, 0x66, 0x8B, 0x01 }, Pointer(&PickupDetour<3>), reinterpret_cast<void**>(&originalPickups[3]))
			&& Hook(Game::Address::PlayerTimedAction, { 0x53, 0x8B, 0x5C, 0x24, 0x08 }, Pointer(&PlayerActionDetour), reinterpret_cast<void**>(&originalPlayerAction))
			&& Hook(Game::Address::PlayerTakeThing, { 0x83, 0xEC, 0x0C, 0x33, 0xC0 }, Pointer(&TakeThingDetour), reinterpret_cast<void**>(&originalTakeThing))
			&& Hook(Game::Address::PlayerDropThing, { 0x83, 0xEC, 0x10, 0x8B, 0x81, 0x96, 0x01, 0x00, 0x00 }, Pointer(&DropDetour), reinterpret_cast<void**>(&originalDrop))
			&& Hook(Game::Address::WorldHasThingsInHand, { 0x8B, 0x54, 0x24, 0x04, 0x8B, 0x01, 0x52 }, Pointer(&HasThingsDetour), reinterpret_cast<void**>(&originalHasThings))
			&& Hook(Game::Address::WorldThingsInHandCount, { 0x8B, 0x54, 0x24, 0x04, 0x8B, 0x01, 0x52 }, Pointer(&ThingCountDetour), reinterpret_cast<void**>(&originalThingCount))
			&& Hook(Game::Address::WorldThingInHand, { 0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x08 }, Pointer(&ThingAtDetour), reinterpret_cast<void**>(&originalThingAt))
			&& Hook(Game::Address::PlayerThingInHand, { 0x8B, 0x44, 0x24, 0x04, 0x33, 0xD2 }, Pointer(&PlayerThingAtDetour), reinterpret_cast<void**>(&originalPlayerThingAt))
			&& Hook(Game::Address::PlayerHasThing, { 0x33, 0xD2, 0x33, 0xC0, 0x8A, 0x91, 0xBF, 0x01, 0x00, 0x00 }, Pointer(&HasThingDetour), reinterpret_cast<void**>(&originalHasThing))
			&& Hook(Game::Address::PlayerSave, { 0x83, 0xEC, 0x10, 0x8D, 0x44, 0x24, 0x00 }, Pointer(&SavePlayerDetour), reinterpret_cast<void**>(&originalSavePlayer))
			&& Hook(Game::Address::PlayerLoad, { 0x83, 0xEC, 0x10, 0x8D, 0x44, 0x24, 0x00 }, Pointer(&LoadPlayerDetour), reinterpret_cast<void**>(&originalLoadPlayer))
			&& Hook(Game::Address::ActionCastSpell, { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x08, 0x66, 0x8B, 0x50, 0x04 }, Pointer(&CastDetour), reinterpret_cast<void**>(&originalCast))
			&& Hook(Game::Address::CreatureListMakeShot, { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x8B, 0xF9 }, Pointer(&MakeShotDetour), reinterpret_cast<void**>(&originalMakeShot))
			&& Hook(Game::Address::ShotReachedTarget, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x60, 0xBF, 0x64, 0x00, 0x50 }, Pointer(&ShotReachedDetour), reinterpret_cast<void**>(&originalShotReached))
			&& Hook(Game::Address::BridgeEnterCreatureA, { 0x8B, 0x44, 0x24, 0x04, 0x53, 0x56, 0x8B, 0xF1, 0x66, 0x8B, 0x58, 0x04 }, Pointer(&EnterADetour), reinterpret_cast<void**>(&originalEnterA))
			&& Hook(Game::Address::BridgeEnterCreatureB, { 0x8B, 0x44, 0x24, 0x04, 0x53, 0x56, 0x8B, 0xF1, 0x66, 0x8B, 0x58, 0x04 }, Pointer(&EnterBDetour), reinterpret_cast<void**>(&originalEnterB))
			&& Hook(Game::Address::InterfaceLocalMovement, { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xD8, 0x92, 0x64, 0x00 }, Pointer(&LocalMovementDetour), reinterpret_cast<void**>(&originalLocalMovement));
		if (!hooked) return false;
		if (!Memory::WriteCall(flower, reinterpret_cast<const void*>(&FlowerCompareStub)) || !Memory::WriteCall(mesh, reinterpret_cast<const void*>(&MeshCompareStub))) return false;
		Memory::Fill(flower + 5, 0x90, flowerBytes.size() - 5);
		Memory::Fill(mesh + 5, 0x90, meshBytes.size() - 5);
		return true;
	}
}
