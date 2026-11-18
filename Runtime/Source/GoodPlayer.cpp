#include "GoodPlayer.h"
#include "CoopCampaign.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "ModCheck.h"
#include "SessionOptions.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace KeeperForge2 {
	namespace {
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }
		// Game structures are packed, so their fields are read and written byte-wise.
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		using PlayerTickFn = std::int32_t(__fastcall*)(std::uint8_t* player);
		using SpellStateFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::uint32_t spell);
		using SetSpellStateFn = void(__fastcall*)(std::uint8_t* player, void* unused, std::uint32_t spell, std::uint32_t state);
		using CastFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::uint32_t spell, void* position, std::int32_t flag);
		using InterfaceSetupFn = void(__fastcall*)(std::uint8_t* playerInterface);
		using GetStringFn = const std::uint8_t*(__fastcall*)(void* table, void* unused, std::uint32_t id);
		using GameTurnFn = std::uint32_t(__fastcall*)(void* world);
		using GetSpellDataFn =std::uint8_t*(__fastcall*)(void* world, void* unused, std::uint32_t spell);
		using TextToWideFn = std::int32_t(__cdecl*)(const std::uint8_t* text, wchar_t* out, std::int32_t size);
		using WideToTextFn = std::int32_t(__cdecl*)(const wchar_t* text, void* out, std::int32_t size);

		constexpr std::int32_t TextBufferSize = 0x200;

		PlayerTickFn originalPlayerTick = nullptr;
		CastFn originalCast = nullptr;
		InterfaceSetupFn originalInterfaceSetup = nullptr;
		GetStringFn originalGetString = nullptr;

		// Set when the interface was made for a player with Dwarf workers; only the screen reads it, never the game
		// simulation.
		bool interfaceUsesDwarfs = false;
		std::uint32_t nameText = 0;
		std::uint32_t descriptionText = 0;
		std::mutex textMutex;
		// Renamed texts by the game's original string. An empty entry means the text is used as it is.
		std::unordered_map<const std::uint8_t*, std::vector<std::uint8_t>> renamedTexts;

		std::uint8_t* World() { return *reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::WorldPointer)); }
		std::uint8_t* Thing(std::uint16_t tag) { return reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::ThingTable))[tag]; }
		bool IsGood(const std::uint8_t* player) { return player != nullptr && player[Game::Offset::PlayerNumber] == Game::GoodPlayer; }
		std::uint8_t* SpellData(std::uint32_t spell) {
			auto* world = World();
			if (world == nullptr) return nullptr;
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(world);
			return reinterpret_cast<GetSpellDataFn>(vtable[Game::Offset::WorldGetSpellData / sizeof(std::uintptr_t)])(world, nullptr, spell);
		}
		// The player this machine controls. Only a human ever controls the Good player (Swap to Good Player).
		// In a co-op campaign this decides how the world runs, so it has to be the same on every machine: whether anyone
		// plays that player, not whether this machine does.
		bool ControlledHere(const std::uint8_t* player) {
			if (CoopCampaign::InSession()) return CoopCampaign::HumanPlays(player[Game::Offset::PlayerNumber]);
			const auto* world = World();
			return world != nullptr && Load<std::uint16_t>(world + Game::Offset::WorldLocalPlayerTag) == Load<std::uint16_t>(player + Game::Offset::PlayerTag);
		}

		// Players whose workers are Dwarfs: the Good player a human controls, and every keeper when keeper-dwarf-workers
		// is on (a session option, so all machines agree).
		bool UsesDwarfWorkers(const std::uint8_t* player) {
			if (player == nullptr) return false;
			if (IsGood(player)) return ControlledHere(player);
			return player[Game::Offset::PlayerNumber] >= Game::FirstKeeperPlayer && SessionOptions::Get(SessionOption::KeeperDwarfWorkers);
		}
		// The player's count of creatures of one type, from its per-type list (entries: type, ..., count at + 9).
		std::int32_t CreatureCount(const std::uint8_t* player, std::uint8_t type) {
			const auto count = Load<std::uint32_t>(player + Game::Offset::PlayerCreatureTypeCount);
			const auto* entries = *reinterpret_cast<const std::uint8_t* const*>(player + Game::Offset::PlayerCreatureTypes);
			for (std::uint32_t i = 0; entries != nullptr && i < count; ++i) {
				const auto* entry = entries + i * Game::CreatureCountEntrySize;
				if (entry[0] == type) return Load<std::int32_t>(entry + Game::CreatureCountValue);
			}
			return 0;
		}
		// One of the world's players (not just any thing with that tag).
		const std::uint8_t* PlayerByTag(std::uint16_t tag) {
			const auto* world = World();
			if (world == nullptr || tag == 0) return nullptr;
			for (const auto* player = Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); player != nullptr; player = Thing(Load<std::uint16_t>(player + Game::Offset::PlayerNext))) {
				if (Load<std::uint16_t>(player + Game::Offset::PlayerTag) == tag) return player;
			}
			return nullptr;
		}
		// Every Imp made for a player with Dwarf workers is made as a Dwarf instead: the starting Imps placed by the map,
		// and any made later. Saved games load their creatures another way, so they keep what they had.
		using CreateCreatureFn = std::int32_t(__fastcall*)(void* list, void* unused, std::uint32_t type, std::uint32_t owner, void* position, void* out);
		CreateCreatureFn originalCreateCreature = nullptr;
		std::int32_t __fastcall CreateCreatureDetour(void* list, void* unused, std::uint32_t type, std::uint32_t owner, void* position, void* out) {
			if ((type & 0xFF) == Game::Creature::Imp && UsesDwarfWorkers(PlayerByTag(static_cast<std::uint16_t>(owner)))) {
				type = (type & ~0xFFu) | Game::Creature::Dwarf;
			}
			return originalCreateCreature(list, unused, type, owner, position, out);
		}

		// The heart makes free workers (a free Create Imp) while the player has fewer Imps than the minimum. For a player
		// with Dwarf workers its Dwarfs count too, or the heart would never stop.
		std::uintptr_t topUpResume = 0;
		std::int32_t __cdecl WorkerCount(const std::uint8_t* player, const std::uint8_t* impEntry) {
			const auto imps = Load<std::int32_t>(impEntry + Game::CreatureCountValue);
			return UsesDwarfWorkers(player) ? imps + CreatureCount(player, Game::Creature::Dwarf) : imps;
		}
		// Replaces "mov eax, [eax + 9]; xor ecx, ecx" with EAX = the Imp entry, ESI = the player.
		__declspec(naked) void WorkerCountStub() {
			__asm {
				push ecx
				push edx
				push eax
				push esi
				call WorkerCount
				add esp, 8
				pop edx
				pop ecx
				xor ecx, ecx
				jmp dword ptr [topUpResume]
			}
		}

		// The player tick returns early for the Good player: it only sets its mana to the maximum. The keeper part it
		// skips does the gold count from treasuries, room building and removal, research, the timed hand actions and
		// more. A Good player this machine controls goes on through all of it like a keeper (the stub below), keeping
		// the heroes' unlimited mana. Every other Good player is left as it was.
		enum TickPath : std::int32_t { GoodReturn = 0, KeeperTick = 1 };
		std::uintptr_t tickKeeperPart = 0;
		std::uintptr_t tickGoodReturn = 0;
		bool tickStubInstalled = false;
		std::int32_t __cdecl ChooseTickPath(std::uint8_t* player) {
			if (Load<std::uint16_t>(player + Game::Offset::PlayerTag) != Memory::Read<std::uint16_t>(Game::Resolve(Game::Address::GoodPlayerTag))) return KeeperTick;
			if (!ControlledHere(player)) return GoodReturn;
			Store<std::int32_t>(player + Game::Offset::PlayerMana, Game::UnlimitedMana);
			return KeeperTick;
		}
		// Replaces "cmp ax, [Good player tag]; jnz" with EDI = the player.
		__declspec(naked) void TickPathStub() {
			__asm {
				push ecx
				push edx
				push eax
				push edi
				call ChooseTickPath
				add esp, 4
				cmp eax, 1
				pop eax
				pop edx
				pop ecx
				jb goodReturn
				jmp dword ptr [tickKeeperPart]
			goodReturn:
				jmp dword ptr [tickGoodReturn]
			}
		}

		// The Good player starts a level with the rooms, doors, traps and spells the map gives the keepers: what a keeper
		// starts with is available, what a keeper can research is researchable (Wizards research it in a Library, and
		// spell upgrades only come once nothing is left to research, as for a keeper). Copied during the first game turns
		// only, after the map's start-up scripts and before any keeper can have researched something. Each list is
		// filled in the same order for every player, with the state as an int at +1 of each entry.
		struct AvailabilityList { std::uintptr_t count; std::uintptr_t entries; std::size_t entrySize; };
		constexpr AvailabilityList AvailabilityLists[] = {
			{ Game::Offset::PlayerRoomCount, Game::Offset::PlayerRooms, Game::AvailabilityEntrySize },
			{ Game::Offset::PlayerDoorCount, Game::Offset::PlayerDoors, Game::AvailabilityEntrySize },
			{ Game::Offset::PlayerTrapCount, Game::Offset::PlayerTraps, Game::AvailabilityEntrySize },
			{ Game::Offset::PlayerSpellCount, Game::Offset::PlayerSpells, Game::SpellEntrySize },
		};
		constexpr std::uint32_t AvailabilityCopyTurns = 20;
		void CopyKeeperAvailability(std::uint8_t* good) {
			auto* world = World();
			if (world == nullptr || GameFunction<GameTurnFn>(Game::Address::WorldGameTurn)(world) > AvailabilityCopyTurns) return;
			for (const auto& list : AvailabilityLists) {
				const auto count = Load<std::uint32_t>(good + list.count);
				auto* entries = *reinterpret_cast<std::uint8_t**>(good + list.entries);
				if (entries == nullptr) continue;
				for (std::uint32_t i = 0; i < count; ++i) {
					auto* entry = entries + i * list.entrySize;
					auto best = Load<std::int32_t>(entry + 1);
					for (auto* keeper = Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); keeper != nullptr; keeper = Thing(Load<std::uint16_t>(keeper + Game::Offset::PlayerNext))) {
						if (keeper[Game::Offset::PlayerNumber] < Game::FirstKeeperPlayer || Load<std::uint32_t>(keeper + list.count) != count) continue;
						const auto* keeperEntry = *reinterpret_cast<const std::uint8_t* const*>(keeper + list.entries);
						if (keeperEntry == nullptr) continue;
						keeperEntry += i * list.entrySize;
						if (keeperEntry[0] != entry[0]) continue;
						const auto state = Load<std::int32_t>(keeperEntry + 1);
						if (state <= static_cast<std::int32_t>(Game::StateAvailable) && state > best) best = state;
					}
					if (best >= static_cast<std::int32_t>(Game::StateResearchable)) Store<std::int32_t>(entry + 1, best);
				}
			}
		}

		// Create Imp's price is its base cost times the player's count of the spell's creature type (+0x43) above a free
		// number, worked out the same way by the spell panel and the cast. With keeper-dwarf-workers every keeper's
		// Create Imp is Create Dwarf, so the spell's type is the Dwarf for the whole game: the price rises with each
		// Dwarf as Create Imp's does with each Imp, and the panel shows what the cast takes. Without it the type is put
		// back. A session option, so every machine has the same spell data.
		std::int32_t createImpType = -1;
		std::int32_t createImpUpgradedType = -1;
		void UpdateCreateImpType() {
			auto* data = SpellData(Game::CreateImpSpell);
			if (data == nullptr) return;
			const auto type = Load<std::int32_t>(data + Game::Offset::SpellCreatureType);
			const auto upgradedType = Load<std::int32_t>(data + Game::Offset::SpellUpgradedCreatureType);
			if ((type & 0xFF) != Game::Creature::Dwarf) createImpType = type;
			if ((upgradedType & 0xFF) != Game::Creature::Dwarf) createImpUpgradedType = upgradedType;
			if (SessionOptions::Get(SessionOption::KeeperDwarfWorkers)) {
				if ((type & 0xFF) != Game::Creature::Dwarf) Store<std::int32_t>(data + Game::Offset::SpellCreatureType, (type & ~0xFF) | Game::Creature::Dwarf);
				if ((upgradedType & 0xFF) != Game::Creature::Dwarf) Store<std::int32_t>(data + Game::Offset::SpellUpgradedCreatureType, (upgradedType & ~0xFF) | Game::Creature::Dwarf);
				return;
			}
			if ((type & 0xFF) == Game::Creature::Dwarf && createImpType >= 0) Store<std::int32_t>(data + Game::Offset::SpellCreatureType, createImpType);
			if ((upgradedType & 0xFF) == Game::Creature::Dwarf && createImpUpgradedType >= 0) Store<std::int32_t>(data + Game::Offset::SpellUpgradedCreatureType, createImpUpgradedType);
		}

		// create-imp-price-rises: off makes Create Imp (and Create Dwarf, the same spell) cost its base price however many
		// workers the keeper has. At each of the game's seven price sums the count part becomes 1, so every one (the panel,
		// the affordability check, the cast) agrees. A session option, set at each level start, so all machines agree.
		constexpr std::uint8_t PriceIncOriginal[] = { 0x2B, 0xC1, 0x40 };       // sub eax, ecx; inc eax
		constexpr std::uint8_t PriceIncFlat[] = { 0x33, 0xC0, 0x40 };           // xor eax, eax; inc eax
		constexpr std::uint8_t PriceAddOriginal[] = { 0x2B, 0xD1, 0x03, 0xC2 }; // sub edx, ecx; add eax, edx
		constexpr std::uint8_t PriceAddFlat[] = { 0x33, 0xC0, 0x40, 0x90 };     // xor eax, eax; inc eax; nop
		bool flatPriceUsable = false;
		bool flatPriceOn = false;
		template<std::size_t N> std::vector<std::uint8_t> Bytes(const std::uint8_t (&bytes)[N]) { return { bytes, bytes + N }; }
		void ApplyCreateImpPrice() {
			const bool flat = SessionOptions::Get(SessionOption::CreateImpFlatPrice);
			if (!flatPriceUsable || flat == flatPriceOn) return;
			for (const auto site : Game::Address::CreateImpPriceIncSites) Memory::Patch(Game::Resolve(site), flat ? Bytes(PriceIncFlat) : Bytes(PriceIncOriginal));
			for (const auto site : Game::Address::CreateImpPriceAddSites) Memory::Patch(Game::Resolve(site), flat ? Bytes(PriceAddFlat) : Bytes(PriceAddOriginal));
			flatPriceOn = flat;
			Logger::Debug(flat ? "Good player: Create Imp keeps its base price." : "Good player: Create Imp's price rises with each worker again.");
		}

		void UpdateDwarfDropRules();
		std::int32_t __fastcall PlayerTickDetour(std::uint8_t* player) {
			UpdateCreateImpType();
			UpdateDwarfDropRules();
			const std::int32_t result = originalPlayerTick(player);
			if (!IsGood(player)) return result;
			const bool controlled = ControlledHere(player);
			// Only a human gives the Good player a timed action. Without the keeper part, finish it here.
			if ((!controlled || !tickStubInstalled) && Load<std::int32_t>(player + Game::Offset::PlayerAction) != 0) {
				GameFunction<PlayerTickFn>(Game::Address::PlayerActionTick)(player);
			}
			if (!controlled) return result;
			// The keeper part works out mana income and caps it; the heroes keep their unlimited mana.
			Store<std::int32_t>(player + Game::Offset::PlayerMana, Game::UnlimitedMana);
			CopyKeeperAvailability(player);
			// Create Imp (cast as Create Dwarf below) is always available to the human playing the heroes.
			if (GameFunction<SpellStateFn>(Game::Address::PlayerSpellState)(player, nullptr, Game::CreateImpSpell) != static_cast<std::int32_t>(Game::SpellAvailable)) {
				GameFunction<SetSpellStateFn>(Game::Address::SetPlayerSpellState)(player, nullptr, Game::CreateImpSpell, Game::SpellAvailable);
			}
			return result;
		}

		// Create Imp spawns the creature type in its spell data. For the Good player, and for keepers with Dwarf workers,
		// the type the cast gives the spell's effect is the Dwarf (plain or upgraded spell). The price is worked out from
		// the spell data's type too, before that point, and is left as the spell panel shows it.
		bool castingDwarf = false;
		std::uintptr_t castCreatureMake = 0;
		std::uintptr_t castCreatureNone = 0;
		std::uint32_t __cdecl CastCreatureType(std::uint32_t type) {
			return castingDwarf ? (type & ~0xFFu) | Game::Creature::Dwarf : type;
		}
		// Replaces "test al, al; jz <no creature>" with EDI = the creature type.
		__declspec(naked) void CastCreatureStub() {
			__asm {
				push eax
				push ecx
				push edx
				push edi
				call CastCreatureType
				add esp, 4
				mov edi, eax
				pop edx
				pop ecx
				pop eax
				test al, al
				jz none
				jmp dword ptr [castCreatureMake]
			none:
				jmp dword ptr [castCreatureNone]
			}
		}
		std::int32_t __fastcall CastDetour(std::uint8_t* player, void* unused, std::uint32_t spell, void* position, std::int32_t flag) {
			if ((spell & 0xFF) != Game::CreateImpSpell || !(IsGood(player) || UsesDwarfWorkers(player))) return originalCast(player, unused, spell, position, flag);
			castingDwarf = true;
			const std::int32_t result = originalCast(player, unused, spell, position, flag);
			castingDwarf = false;
			return result;
		}

		// The Workers row shows the first creature type the game marks as a worker (the Imp). For a player with Dwarf
		// workers it shows Dwarfs.
		void __fastcall InterfaceSetupDetour(std::uint8_t* playerInterface) {
			originalInterfaceSetup(playerInterface);
			// Multiplayer: a notice in the game's chat when the host's KeeperForge2 or mods differ.
			ModCheck::OnInterfaceReady(playerInterface);
			const auto* player = Thing(Load<std::uint16_t>(playerInterface + Game::Offset::InterfacePlayerTag));
			interfaceUsesDwarfs = UsesDwarfWorkers(player);
			if (!interfaceUsesDwarfs) return;
			playerInterface[Game::Offset::InterfaceWorkerType] = Game::Creature::Dwarf;
			if (const auto* data = SpellData(Game::CreateImpSpell)) {
				nameText = Load<std::uint16_t>(data + Game::Offset::SpellNameText);
				descriptionText = Load<std::uint16_t>(data + Game::Offset::SpellDescriptionText);
			}
			Logger::Debug(IsGood(player) ? "Good player: Dwarfs are your workers, and Create Imp is Create Dwarf." : "Keeper Dwarf workers: Dwarfs are your workers, and Create Imp is Create Dwarf.");
		}

		// When a creature looks for something to do, the game sends every creature of the Good player to its hero
		// behavior before anything else: Dwarfs never take dig or claim jobs, and a Wizard that finished its research
		// does not go back to the Library. For the Good player a human controls, its creatures go on like a keeper's
		// (workers to the worker jobs, the rest to the keeper idle), except those with a hero order to carry out.
		// Heroes in normal games are left alone.
		enum NextPath : std::int32_t { HeroPath = 0, NotGoodPath = 1, KeeperPath = 2 };
		std::uintptr_t nextGood = 0;
		std::uintptr_t nextNotGood = 0;
		std::uintptr_t nextWorker = 0;
		// A defeated player: one of the world's players, with "killed by" set.
		bool IsDefeatedPlayer(std::uint16_t tag) {
			const auto* world = World();
			if (world == nullptr || tag == 0) return false;
			for (const auto* player = Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); player != nullptr; player = Thing(Load<std::uint16_t>(player + Game::Offset::PlayerNext))) {
				if (Load<std::uint16_t>(player + Game::Offset::PlayerTag) == tag) return Load<std::uint16_t>(player + Game::Offset::PlayerKilledBy) != 0;
			}
			return false;
		}
		bool HasHeroJob(const std::uint8_t* creature) { return Load<std::int32_t>(creature + Game::Offset::CreatureGoodJob) != 0; }
		std::int32_t __cdecl ChooseNextPath(std::uint8_t* creature) {
			const auto owner = Load<std::uint16_t>(creature + Game::Offset::CreatureOwner);
			if (owner != Memory::Read<std::uint16_t>(Game::Resolve(Game::Address::GoodPlayerTag))) return NotGoodPath;
			const auto* player = Thing(owner);
			if (player == nullptr || !ControlledHere(player)) return HeroPath;
			// Hero jobs aimed at a player (attacking their dungeon, for one) are dropped once that player is defeated, so
			// the creatures stop crowding a destroyed heart. With no job, the hero behavior leaves them idle like any
			// creature.
			if (HasHeroJob(creature) && IsDefeatedPlayer(Load<std::uint16_t>(creature + Game::Offset::CreatureGoodJobPlayer))) {
				Store<std::int32_t>(creature + Game::Offset::CreatureGoodJob, 0);
				Store<std::uint16_t>(creature + Game::Offset::CreatureGoodJobPlayer, 0);
			}
			const auto* data = *reinterpret_cast<const std::uint8_t* const*>(creature + Game::Offset::CreatureData);
			const bool worker = data != nullptr && (Load<std::uint32_t>(data + Game::Offset::CreatureDataFlags) & Game::CreatureWorkerFlag) != 0;
			if (worker) return KeeperPath;
			// A hero order still to carry out (its own, or its party leader's) keeps the hero behavior. Otherwise the
			// creature behaves like a keeper's: it goes back to the job it was dropped on (a Wizard keeps researching),
			// sleeps, eats, trains and so on. The hero behavior would only leave it doing nothing.
			if (HasHeroJob(creature)) return HeroPath;
			if ((creature[Game::Offset::CreaturePartyFlags] & Game::PartyFollowerFlag) != 0) {
				const auto* leader = Thing(Load<std::uint16_t>(creature + Game::Offset::CreaturePartyLeader));
				if (leader != nullptr && HasHeroJob(leader)) return HeroPath;
			}
			return KeeperPath;
		}
		// Replaces "cmp cx, [Good player tag]; jnz" with EAX = the creature.
		__declspec(naked) void NextPathStub() {
			__asm {
				push ecx
				push edx
				push eax
				push eax
				call ChooseNextPath
				add esp, 4
				cmp eax, 1
				pop eax
				pop edx
				pop ecx
				jb hero
				je notGood
				jmp dword ptr [nextWorker]
			hero:
				jmp dword ptr [nextGood]
			notGood:
				jmp dword ptr [nextNotGood]
			}
		}

		// The "Receive Imps" special makes a ring of the first creature type marked as a worker (the Imp) around its
		// player's heart. For a player with Dwarf workers the ring is of Dwarfs: for the call, every other type's worker
		// mark is hidden. The heroes also have no heart (the ring went off the map's corner and the game crashed), so for
		// the Good player the ring is made around the special itself, kept away from the map's edges.
		using ReceiveWorkersFn = std::int32_t(__fastcall*)(std::uint8_t* special, void* unused, std::uint8_t* player);
		using WorldIntFn = std::int32_t(__fastcall*)(void* world);
		using WorldTypeFn = std::uint8_t(__fastcall*)(void* world);
		using WorldNextTypeFn = std::uint8_t(__fastcall*)(void* world, void* unused, std::uint32_t type);
		using WorldCreatureDataFn = std::uint8_t*(__fastcall*)(void* world, void* unused, std::uint32_t type);
		ReceiveWorkersFn originalReceiveWorkers = nullptr;
		template<typename T> T WorldMethod(const std::uint8_t* world, std::uintptr_t offset) {
			return reinterpret_cast<T>((*reinterpret_cast<const std::uintptr_t* const*>(world))[offset / sizeof(std::uintptr_t)]);
		}
		// With keeper-dwarf-workers, Dwarfs follow the Imp's drop rules: they can be dropped into the Dungeon Heart to be
		// taken away and cannot be dropped into a Portal or Mercenary Portal. The rules come from the creature type's
		// data (CreatureImpDropFlag), so the Dwarf type gets the Imp's flag while the option is on and its own back when
		// it is off. A session option, so every machine has the same data.
		std::int32_t dwarfDropFlagOriginal = -1;
		void UpdateDwarfDropRules() {
			auto* world = World();
			if (world == nullptr) return;
			auto* data = WorldMethod<WorldCreatureDataFn>(world, Game::Offset::WorldCreatureData)(world, nullptr, Game::Creature::Dwarf);
			if (data == nullptr) return;
			const auto flags = Load<std::uint32_t>(data + Game::Offset::CreatureDataFlags);
			const bool on = SessionOptions::Get(SessionOption::KeeperDwarfWorkers);
			if (on) {
				if (dwarfDropFlagOriginal < 0) dwarfDropFlagOriginal = static_cast<std::int32_t>(flags & Game::CreatureImpDropFlag);
				if ((flags & Game::CreatureImpDropFlag) == 0) Store<std::uint32_t>(data + Game::Offset::CreatureDataFlags, flags | Game::CreatureImpDropFlag);
				return;
			}
			if (dwarfDropFlagOriginal < 0) return;
			Store<std::uint32_t>(data + Game::Offset::CreatureDataFlags, (flags & ~Game::CreatureImpDropFlag) | static_cast<std::uint32_t>(dwarfDropFlagOriginal));
			dwarfDropFlagOriginal = -1;
		}
		constexpr std::int32_t RingClearance = 3;
		std::int32_t __fastcall ReceiveWorkersDetour(std::uint8_t* special, void* unused, std::uint8_t* player) {
			auto* world = World();
			if (world == nullptr || !UsesDwarfWorkers(player)) return originalReceiveWorkers(special, unused, player);
			const bool noHeart = IsGood(player);
			const auto heartX = Load<std::uint16_t>(player + Game::Offset::PlayerHeartX);
			const auto heartY = Load<std::uint16_t>(player + Game::Offset::PlayerHeartY);
			if (noHeart) {
				const auto width = WorldMethod<WorldIntFn>(world, Game::Offset::WorldMapWidth)(world);
				const auto height = WorldMethod<WorldIntFn>(world, Game::Offset::WorldMapHeight)(world);
				const auto x = std::clamp<std::int32_t>(Load<std::int32_t>(special + Game::Offset::ThingPositionX) >> 12, RingClearance, (std::max)(RingClearance, width - 1 - RingClearance));
				const auto y = std::clamp<std::int32_t>(Load<std::int32_t>(special + Game::Offset::ThingPositionY) >> 12, RingClearance, (std::max)(RingClearance, height - 1 - RingClearance));
				Store<std::uint16_t>(player + Game::Offset::PlayerHeartX, static_cast<std::uint16_t>(x));
				Store<std::uint16_t>(player + Game::Offset::PlayerHeartY, static_cast<std::uint16_t>(y));
			}
			std::vector<std::pair<std::uint8_t*, std::uint32_t>> hidden;
			const auto count = WorldMethod<WorldIntFn>(world, Game::Offset::WorldCreatureTypeCount)(world);
			auto type = WorldMethod<WorldTypeFn>(world, Game::Offset::WorldFirstCreatureType)(world);
			for (std::int32_t i = 0; i < count; ++i) {
				auto* data = WorldMethod<WorldCreatureDataFn>(world, Game::Offset::WorldCreatureData)(world, nullptr, type);
				if (data != nullptr && type != Game::Creature::Dwarf) {
					const auto flags = Load<std::uint32_t>(data + Game::Offset::CreatureDataFlags);
					if ((flags & Game::CreatureWorkerFlag) != 0) {
						hidden.emplace_back(data, flags);
						Store<std::uint32_t>(data + Game::Offset::CreatureDataFlags, flags & ~Game::CreatureWorkerFlag);
					}
				}
				type = WorldMethod<WorldNextTypeFn>(world, Game::Offset::WorldNextCreatureType)(world, nullptr, type);
			}
			const std::int32_t result = originalReceiveWorkers(special, unused, player);
			for (const auto& [data, flags] : hidden) Store<std::uint32_t>(data + Game::Offset::CreatureDataFlags, flags);
			if (noHeart) {
				Store<std::uint16_t>(player + Game::Offset::PlayerHeartX, heartX);
				Store<std::uint16_t>(player + Game::Offset::PlayerHeartY, heartY);
			}
			return result;
		}

		bool IsLetter(wchar_t c) { return std::iswalpha(c) != 0; }
		// "Imp" -> "Dwarf" as whole words (plurals and "an Imp" too), keeping the case.
		std::wstring ImpsToDwarfs(const std::wstring& text) {
			std::wstring out;
			for (std::size_t i = 0; i < text.size();) {
				const bool wordStart = i == 0 || !IsLetter(text[i - 1]);
				if (wordStart && i + 3 <= text.size() && std::towlower(text[i]) == L'i' && std::towlower(text[i + 1]) == L'm' && std::towlower(text[i + 2]) == L'p') {
					std::size_t end = i + 3;
					if (end < text.size() && std::towlower(text[end]) == L's') ++end;
					if (end == text.size() || !IsLetter(text[end])) {
						const std::size_t size = out.size();
						if (size >= 3 && out[size - 1] == L' ' && std::towlower(out[size - 2]) == L'n' && std::towlower(out[size - 3]) == L'a' && (size == 3 || !IsLetter(out[size - 4]))) {
							out.erase(size - 2, 1);
						}
						out += std::iswupper(text[i + 1]) ? L"DWARF" : std::iswupper(text[i]) ? L"Dwarf" : L"dwarf";
						if (end > i + 3) out += text[i + 3];
						i = end;
						continue;
					}
				}
				out += text[i++];
			}
			return out;
		}
		// Builds the renamed copy in the game's text layout: a header (type 1, length), the text, then a zero dword.
		std::vector<std::uint8_t> Rename(const std::uint8_t* text) {
			wchar_t wide[TextBufferSize]{};
			if (GameFunction<TextToWideFn>(Game::Address::TextToWide)(text, wide, TextBufferSize) != 1) return {};
			const std::wstring original(wide);
			const std::wstring renamed = ImpsToDwarfs(original);
			if (renamed == original) return {};
			std::uint8_t converted[TextBufferSize * 2]{};
			if (GameFunction<WideToTextFn>(Game::Address::ConvertText)(renamed.c_str(), converted, sizeof(converted) - 1) != 1) return {};
			const std::size_t length = std::strlen(reinterpret_cast<const char*>(converted)) + 1;
			std::vector<std::uint8_t> entry(4 + ((length + 3) & ~std::size_t{ 3 }) + 4, 0);
			Store<std::uint32_t>(entry.data(), 1 | static_cast<std::uint32_t>(length << 8));
			std::memcpy(entry.data() + 4, converted, length);
			return entry;
		}
		const std::uint8_t* __fastcall GetStringDetour(void* table, void* unused, std::uint32_t id) {
			const std::uint8_t* text = originalGetString(table, unused, id);
			if (!interfaceUsesDwarfs || text == nullptr || id == 0 || (id != nameText && id != descriptionText)) return text;
			std::lock_guard<std::mutex> lock(textMutex);
			auto found = renamedTexts.find(text);
			if (found == renamedTexts.end()) found = renamedTexts.emplace(text, Rename(text)).first;
			return found->second.empty() ? text : found->second.data() + 4;
		}

		bool Hook(std::uintptr_t target, void* detour, void** original) {
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
	}

	void GoodPlayer::Install() {
		const auto playerTick = Game::Resolve(Game::Address::PlayerTick);
		if (!Memory::Matches(playerTick, { 0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xED })
			|| !Memory::Matches(Game::Resolve(Game::Address::PlayerActionTick), { 0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB, 0x57, 0x8B, 0x86, 0x86, 0x01, 0x00, 0x00 })
			|| !Memory::Matches(Game::Resolve(Game::Address::PlayerSpellState), { 0x8B, 0x51, 0x6E })
			|| !Memory::Matches(Game::Resolve(Game::Address::SetPlayerSpellState), { 0x8B, 0x41, 0x6E })
			|| !Hook(playerTick, reinterpret_cast<void*>(&PlayerTickDetour), reinterpret_cast<void**>(&originalPlayerTick))) {
			Logger::Warning("Good player: could not hook the player tick, the hand will not pick things up and there is no Create Dwarf.");
		}
		const auto goodTag = static_cast<std::uint32_t>(Game::Resolve(Game::Address::GoodPlayerTag));
		const auto tickCheck = Game::Resolve(Game::Address::PlayerTickGoodCheck);
		std::vector<std::uint8_t> tickPattern = { 0x66, 0x3B, 0x05 };
		for (int shift = 0; shift < 32; shift += 8) tickPattern.push_back(static_cast<std::uint8_t>(goodTag >> shift));
		tickPattern.insert(tickPattern.end(), { 0x75, 0x14, 0xC7, 0x87, 0x82, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x7F });
		tickGoodReturn = Game::Resolve(Game::Address::PlayerTickGoodReturn);
		tickKeeperPart = Game::Resolve(Game::Address::PlayerTickKeeperPart);
		tickStubInstalled = originalPlayerTick != nullptr && Memory::Matches(tickCheck, tickPattern)
			&& Memory::Matches(tickKeeperPart, { 0x66, 0x39, 0xAF, 0x32, 0x01, 0x00, 0x00 })
			&& Memory::WriteJump(tickCheck, reinterpret_cast<const void*>(&TickPathStub), 9);
		if (!tickStubInstalled) Logger::Warning("Good player: unexpected player tick code, the heroes cannot build rooms or count gold.");
		const auto createCreature = Game::Resolve(Game::Address::CreateCreature);
		if (!Memory::Matches(createCreature, { 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x54, 0x24, 0x0C, 0x50 })
			|| !Hook(createCreature, reinterpret_cast<void*>(&CreateCreatureDetour), reinterpret_cast<void**>(&originalCreateCreature))) {
			Logger::Warning("Good player: could not hook creature creation, starting Imps stay Imps with keeper-dwarf-workers.");
		}
		const auto topUpCount = Game::Resolve(Game::Address::WorkerTopUpCount);
		topUpResume = Game::Resolve(Game::Address::WorkerTopUpResume);
		if (!Memory::Matches(topUpCount, { 0x8B, 0x40, 0x09, 0x33, 0xC9, 0x8A, 0x0D })
			|| !Memory::WriteJump(topUpCount, reinterpret_cast<const void*>(&WorkerCountStub), 5)) {
			Logger::Warning("Good player: unexpected worker top-up code, a heart may keep making Dwarfs for keepers with Dwarf workers.");
		}
		const auto cast = Game::Resolve(Game::Address::CastKeeperSpell);
		if (!Memory::Matches(cast, { 0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x14, 0x55, 0x56, 0x57, 0x8B, 0xF9 })
			|| !Hook(cast, reinterpret_cast<void*>(&CastDetour), reinterpret_cast<void**>(&originalCast))) {
			Logger::Warning("Good player: could not hook spell casting, Create Dwarf will make Imps.");
		}
		else {
			const auto castCheck = Game::Resolve(Game::Address::CastCreatureCheck);
			castCreatureMake = Game::Resolve(Game::Address::CastCreatureMake);
			castCreatureNone = Game::Resolve(Game::Address::CastCreatureNone);
			if (!Memory::Matches(castCheck, { 0x84, 0xC0, 0x0F, 0x84, 0x13, 0x01, 0x00, 0x00 })
				|| !Memory::WriteJump(castCheck, reinterpret_cast<const void*>(&CastCreatureStub), 8)) {
				Logger::Warning("Good player: unexpected spell cast code, Create Dwarf may make Imps.");
			}
		}
		const auto nextCheck = Game::Resolve(Game::Address::CreatureNextGoodCheck);
		std::vector<std::uint8_t> nextPattern = { 0x66, 0x3B, 0x0D };
		for (int shift = 0; shift < 32; shift += 8) nextPattern.push_back(static_cast<std::uint8_t>(goodTag >> shift));
		nextPattern.insert(nextPattern.end(), { 0x75, 0x17, 0x53, 0x6A, 0x5A });
		nextGood = Game::Resolve(Game::Address::CreatureNextGood);
		nextNotGood = Game::Resolve(Game::Address::CreatureNextNotGood);
		nextWorker = Game::Resolve(Game::Address::CreatureNextWorker);
		if (!Memory::Matches(nextCheck, nextPattern)
			|| !Memory::Matches(nextWorker, { 0x8B, 0x88, 0x70, 0x03, 0x00, 0x00, 0x8B, 0x89, 0x86, 0x06, 0x00, 0x00, 0xF6, 0xC1, 0x01 })
			|| !Memory::WriteJump(nextCheck, reinterpret_cast<const void*>(&NextPathStub), 9)) {
			Logger::Warning("Good player: unexpected creature code, Dwarfs will not dig or claim.");
		}
		flatPriceUsable = true;
		for (const auto site : Game::Address::CreateImpPriceIncSites) flatPriceUsable = flatPriceUsable && Memory::Matches(Game::Resolve(site), Bytes(PriceIncOriginal));
		for (const auto site : Game::Address::CreateImpPriceAddSites) flatPriceUsable = flatPriceUsable && Memory::Matches(Game::Resolve(site), Bytes(PriceAddOriginal));
		if (flatPriceUsable) SessionOptions::OnLevelStart(&ApplyCreateImpPrice);
		else Logger::Warning("Good player: unexpected Create Imp price code, create-imp-price-rises: off does nothing.");
		const auto receiveWorkers = Game::Resolve(Game::Address::ReceiveWorkersSpecial);
		if (!Memory::Matches(receiveWorkers, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x3C, 0x89, 0x4D, 0xDC, 0x53 })
			|| !Hook(receiveWorkers, reinterpret_cast<void*>(&ReceiveWorkersDetour), reinterpret_cast<void**>(&originalReceiveWorkers))) {
			Logger::Warning("Good player: could not hook the Receive Imps special, it may crash for the heroes.");
		}
		const auto interfaceSetup = Game::Resolve(Game::Address::PlayerInterfaceSetup);
		if (!Memory::Matches(interfaceSetup, { 0x6A, 0xFF, 0x68, 0xF0, 0x96, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 })
			|| !Hook(interfaceSetup, reinterpret_cast<void*>(&InterfaceSetupDetour), reinterpret_cast<void**>(&originalInterfaceSetup))) {
			Logger::Warning("Good player: could not hook the interface setup, the Workers row shows Imps.");
			return;
		}
		const auto getString = Game::Resolve(Game::Address::TextGetString);
		if (!Memory::Matches(getString, { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x51, 0x08, 0x3B, 0xC2, 0x72, 0x05 })
			|| !Memory::Matches(Game::Resolve(Game::Address::TextToWide), { 0x51, 0x56, 0x8B, 0x74, 0x24, 0x10 })
			|| !Hook(getString, reinterpret_cast<void*>(&GetStringDetour), reinterpret_cast<void**>(&originalGetString))) {
			Logger::Warning("Good player: could not hook text lookup, the spell keeps the name Create Imp.");
		}
	}
}
