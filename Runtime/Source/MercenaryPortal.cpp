#include "MercenaryPortal.h"
#include "Config.h"
#include "CreatureFixes.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "SessionOptions.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace KeeperForge2 {
	namespace {
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// Who the portal brings, and the rooms each needs (any size) by default: config.yml can change them. Creature and
		// room type ids: see docs/game/mercenary-portal.md.
		using Rooms = std::array<std::uint8_t, 2>; // 0 = none
		struct PortalCreature {
			const char* name;
			std::uint8_t type;
			bool royal;
			const char* roomsKey;
			Rooms defaultRooms;
		};
		using namespace Game;
		constexpr const char* RoyalRoomsKey = "mercenary-portal-royal-room-ids";
		constexpr PortalCreature PortalCreatures[] = {
			{ "Wizard", Creature::Wizard, false, "mercenary-portal-wizard-room-ids", { Room::Library, 0 } },
			{ "Guard", Creature::Guard, false, "mercenary-portal-guard-room-ids", { Room::Training_Room, 0 } },
			{ "Giant", Creature::Giant, false, "mercenary-portal-giant-room-ids", { Room::Work_Shop, 0 } },
			{ "Thief", Creature::Thief, false, "mercenary-portal-thief-room-ids", { Room::Casino, 0 } },
			{ "Knight", Creature::Knight, false, "mercenary-portal-knight-room-ids", { Room::Combat_Pit, 0 } },
			{ "Elven Archer", Creature::Elven_Archer, false, "mercenary-portal-elven-archer-room-ids", { Room::Guard_Room, 0 } },
			{ "Monk", Creature::Monk, false, "mercenary-portal-monk-room-ids", { Room::Graveyard, 0 } },
			{ "Fairy", Creature::Fairy, false, "mercenary-portal-fairy-room-ids", { Room::Temple, 0 } },
			{ "Royal Guard", Creature::Royal_Guard, false, "mercenary-portal-royal-guard-room-ids", { Room::Torture_Chamber, Room::Training_Room } },
			{ "Stone Knight", Creature::Stone_Knight, false, "mercenary-portal-stone-knight-room-ids", { Room::Torture_Chamber, Room::Combat_Pit } },
			// One of these per keeper per game. By default the first room is the Lair, which gives each the same weight,
			// so the game's pick among them is even.
			{ "Lord of the Land", Creature::Lord_Of_The_Land, true, RoyalRoomsKey, { Room::Lair, Room::Hatchery } },
			{ "King Reginald", Creature::King_Reginald, true, RoyalRoomsKey, { Room::Lair, Room::Hatchery } },
			{ "Prince Tristran", Creature::Prince_Tristran, true, RoyalRoomsKey, { Room::Lair, Room::Hatchery } },
			{ "Prince Felix", Creature::Prince_Felix, true, RoyalRoomsKey, { Room::Lair, Room::Hatchery } },
			{ "Prince Balder", Creature::Prince_Balder, true, RoyalRoomsKey, { Room::Lair, Room::Hatchery } },
		};
		constexpr std::size_t PortalCreatureCount = std::size(PortalCreatures);
		bool IsRoyal(std::uint8_t type) {
			for (const auto& creature : PortalCreatures) {
				if (creature.type == type) return creature.royal;
			}
			return false;
		}

		// The rooms by config.yml, and the ones the host sent.
		std::array<Rooms, PortalCreatureCount> localRooms{};
		std::array<Rooms, PortalCreatureCount> sessionRooms{};
		const Rooms& RoomsOf(const PortalCreature& creature) {
			const auto index = static_cast<std::size_t>(&creature - PortalCreatures);
			return SessionOptions::UsesSessionValues() ? sessionRooms[index] : localRooms[index];
		}
		struct RoomInfo {
			std::uint8_t id;
			const char* name;
		};
		constexpr RoomInfo KnownRooms[] = {
			{ Room::Lair, "Lair" }, { Room::Hatchery, "Hatchery" }, { Room::Dungeon_Heart, "Dungeon Heart" }, { Room::Portal, "Portal" },
			{ Room::Library, "Library" }, { Room::Training_Room, "Training Room" }, { Room::Treasury, "Treasury" }, { Room::Work_Shop, "Work Shop" },
			{ Room::Casino, "Casino" }, { Room::Guard_Room, "Guard Room" }, { Room::Wooden_Bridge, "Wooden Bridge" }, { Room::Prison, "Prison" },
			{ Room::Torture_Chamber, "Torture Chamber" }, { Room::Stone_Bridge, "Stone Bridge" }, { Room::Graveyard, "Graveyard" },
			{ Room::Combat_Pit, "Combat Pit" }, { Room::Temple, "Temple" }, { Room::Hero_Gate_Final, "Hero Gate [Final]" },
			{ Room::Hero_Gate_2x2, "Hero Gate [2x2]" }, { Room::Hero_Gate_3x1, "Hero Gate [3x1]" }, { Room::Hero_Portal, "Hero Portal" },
		};
		// "6" or "12 + 16". False if it is not one or two known room ids.
		bool ParseRooms(const std::string& text, Rooms& rooms) {
			rooms = {};
			std::size_t count = 0;
			std::string number;
			const auto addNumber = [&] {
				if (number.empty() || number.size() > 3 || count == rooms.size()) return false;
				const int id = std::stoi(number);
				number.clear();
				for (const auto& known : KnownRooms) {
					if (known.id == id) {
						rooms[count++] = known.id;
						return true;
					}
				}
				return false;
			};
			for (const char c : text) {
				if (c == '+') {
					if (!addNumber()) return false;
				}
				else if (std::isdigit(static_cast<unsigned char>(c))) { number += c; }
				else if (c != ' ' && c != '\t') { return false; }
			}
			return addNumber();
		}
		void LoadRooms() {
			for (std::size_t i = 0; i < PortalCreatureCount; ++i) {
				const auto& creature = PortalCreatures[i];
				const std::string text = Config::GetString(creature.roomsKey, "");
				if (ParseRooms(text, localRooms[i])) continue;
				localRooms[i] = creature.defaultRooms;
				// The royals share a key: once is enough.
				if (i == 0 || std::string_view(creature.roomsKey) != PortalCreatures[i - 1].roomsKey) {
					Logger::Warning(std::string("Mercenary Portal: ") + creature.roomsKey + " '" + text + "' in config.yml is not one or two known room ids, using the default.");
				}
			}
			sessionRooms = localRooms;
		}

		std::uint8_t* World() { return *reinterpret_cast<std::uint8_t**>(Resolve(Address::WorldPointer)); }
		std::uint8_t* Thing(std::uint16_t tag) { return reinterpret_cast<std::uint8_t**>(Resolve(Address::ThingTable))[tag]; }
		using CreatureDataFn = std::uint8_t*(__fastcall*)(void* world, void* unused, std::uint32_t type);
		std::uint8_t* CreatureData(std::uint8_t type) {
			auto* world = World();
			if (world == nullptr) return nullptr;
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(world);
			return reinterpret_cast<CreatureDataFn>(vtable[Offset::WorldCreatureData / sizeof(std::uintptr_t)])(world, nullptr, type);
		}

		// Players (by player number) that have had their royal this game. Every machine sees the same game, so they
		// all agree; cleared at each level start.
		std::array<bool, 256> royalGiven{};

		// While one Mercenary Portal's creature choice runs: its player, and the creature data of our creatures.
		struct ActivePortal {
			bool active = false;
			bool royalAllowed = false;
			std::unordered_map<const std::uint8_t*, const PortalCreature*> creatures;
		};
		ActivePortal portal;

		bool GameMercenary(const std::uint8_t* data) { return (data[Offset::CreatureDataPortalFlags] & PortalMercenaryFlag) != 0; }

		// Replaces "test byte [data + 0x68A], 8; jz skip": may this creature type come through? Outside a Mercenary
		// Portal's choice it is the game's own test.
		std::int32_t __cdecl PortalCreatureAllowed(const std::uint8_t* data) {
			if (!portal.active) return GameMercenary(data) ? 1 : 0;
			const auto found = portal.creatures.find(data);
			if (found != portal.creatures.end()) {
				if (found->second->royal) return portal.royalAllowed ? 1 : 0;
				return found->second->type == Creature::Stone_Knight && SessionOptions::Get(SessionOption::StoneKnightsOff) ? 0 : 1;
			}
			return !SessionOptions::Get(SessionOption::MercenaryPortalNormal) && GameMercenary(data) ? 1 : 0;
		}
		std::uintptr_t portalAccept = 0;
		std::uintptr_t portalReject = 0;
		__declspec(naked) void PortalCreatureStub() {
			__asm {
				push ecx
				push edx
				push eax
				push eax
				call PortalCreatureAllowed
				add esp, 4
				test eax, eax
				pop eax
				pop edx
				pop ecx
				jz reject
				jmp dword ptr [portalAccept]
			reject:
				jmp dword ptr [portalReject]
			}
		}

		// Everything from a Mercenary Portal arrives the Mercenary Portal's way (out between the pillars, with its effect)
		// and is made a mercenary at the end. With mercenary-portal-spawns: normal the arrival stays, but the creature is
		// not made a mercenary, so heroes come as normal heroes.
		std::int32_t mercenaryFlag = 1;
		std::uintptr_t mercenaryResume = 0;
		void __cdecl SetMercenaryFlag() { mercenaryFlag = SessionOptions::Get(SessionOption::MercenaryPortalNormal) ? 0 : 1; }
		// Replaces "push 1; xor al, 1; mov ecx, esi": the 1 is IsMercenary for the call that follows.
		__declspec(naked) void MercenaryFlagStub() {
			__asm {
				push eax
				push ecx
				push edx
				call SetMercenaryFlag
				pop edx
				pop ecx
				pop eax
				xor al, 1
				mov ecx, esi
				push dword ptr [mercenaryFlag]
				jmp dword ptr [mercenaryResume]
			}
		}

		// Making a creature a mercenary also draws it with the mercenary textures (texture bank 1). These types have
		// none, so they came out white: they keep the mercenary mark but are drawn with their normal textures.
		bool HasMercenaryTextures(std::uint8_t type) {
			switch (type) {
				case Creature::King_Reginald:
				case Creature::Lord_Of_The_Land:
				case Creature::Prince_Balder:
				case Creature::Prince_Felix:
				case Creature::Prince_Tristran:
				case Creature::Stone_Knight:
				case Creature::Dwarf:
					return false;
				default:
					return true;
			}
		}
		using SetMercenaryFn = void(__fastcall*)(std::uint8_t* creature, void* unused, std::int32_t on);
		SetMercenaryFn originalSetMercenary = nullptr;
		void __fastcall SetMercenaryDetour(std::uint8_t* creature, void* unused, std::int32_t on) {
			originalSetMercenary(creature, unused, on);
			if (on != 0 && !HasMercenaryTextures(creature[Offset::CreatureType])) creature[Offset::CreatureTextureBank] = 0;
		}

		// The Hand of Evil draws a creature it holds with a texture bank of its own, set the same way when it is picked
		// up: mercenaries without mercenary textures keep the normal ones there too.
		constexpr std::uint32_t MercenaryStateFlag = 0x200;
		std::int32_t __cdecl HandUsesMercenaryTextures(const std::uint8_t* creature) {
			return (Load<std::uint32_t>(creature + Offset::CreatureStateFlags) & MercenaryStateFlag) != 0 && HasMercenaryTextures(creature[Offset::CreatureType]) ? 1 : 0;
		}
		std::uintptr_t handNormal = 0;
		std::uintptr_t handMercenary = 0;
		__declspec(naked) void HandMercenaryStub() {
			__asm {
				push eax
				call HandUsesMercenaryTextures
				add esp, 4
				test eax, eax
				jnz mercenary
				jmp dword ptr [handNormal]
			mercenary:
				jmp dword ptr [handMercenary]
			}
		}

		// The creature data's attraction list: two entries of { room type, minimum size, kind } (kind 1 = room) and
		// their count. Saved, replaced with our rooms for the portal's choice, and put back after.
		struct SavedAttraction {
			std::uint8_t* data;
			std::array<std::uint8_t, CreatureAttractionBytes> entries;
			std::uint8_t count;
		};

		// For the log: what stands between the player and each of our creatures. Written only when it changes, so a
		// test shows at once whether the map's creature list, a missing room or our rules keep a creature out.
		using InPoolFn = std::int32_t(__fastcall*)(const std::uint8_t* pool, void* unused, std::uint32_t type);
		std::uint32_t RoomSize(const std::uint8_t* player, std::uint8_t roomType) {
			std::uint32_t size = 0;
			for (auto* room = Thing(Load<std::uint16_t>(player + Offset::PlayerFirstRoom)); room != nullptr; room = Thing(Load<std::uint16_t>(room + Offset::RoomNextOwned))) {
				if (room[Offset::RoomType] == roomType) size += Load<std::uint16_t>(room + Offset::RoomSize);
			}
			return size;
		}
		const char* RoomName(std::uint8_t roomType) {
			for (const auto& known : KnownRooms) {
				if (known.id == roomType) return known.name;
			}
			return "?";
		}
		std::array<std::string, 256> lastStatus;
		std::array<bool, 256> portalSeen{};
		void LogStatus(const std::uint8_t* player, bool royalAllowed) {
			const auto number = player[Offset::PlayerNumber];
			const auto inPool = reinterpret_cast<InPoolFn>(Resolve(Address::CreaturePoolHas));
			const bool stoneKnightsOff = SessionOptions::Get(SessionOption::StoneKnightsOff);
			std::string notListed;
			std::string waiting;
			std::string ready;
			for (const auto& creature : PortalCreatures) {
				if (creature.royal && !royalAllowed) continue;
				if (creature.type == Creature::Stone_Knight && stoneKnightsOff) continue;
				const std::string name = std::string(creature.name) + " (" + std::to_string(creature.type) + ")";
				// Royals do not use the pool.
				if (!creature.royal && inPool(player + Offset::PlayerCreaturePool, nullptr, creature.type) == 0) {
					notListed += (notListed.empty() ? "" : ", ") + name;
					continue;
				}
				std::string missing;
				for (const auto roomType : RoomsOf(creature)) {
					if (roomType != 0 && RoomSize(player, roomType) == 0) missing += std::string(missing.empty() ? "" : " + ") + RoomName(roomType);
				}
				if (missing.empty()) ready += (ready.empty() ? "" : ", ") + name;
				else waiting += (waiting.empty() ? "" : ", ") + name + " needs " + missing;
			}
			const std::string status = "ready: " + (ready.empty() ? std::string("none") : ready)
				+ "; waiting for rooms: " + (waiting.empty() ? std::string("none") : waiting)
				+ "; none left in the creature list (lobby/map number, minus those alive): " + (notListed.empty() ? std::string("none") : notListed)
				+ "; royal: " + (SessionOptions::Get(SessionOption::RoyalsOff) ? "off" : royalAllowed ? "not had yet" : "already had");
			if (status == lastStatus[number]) return;
			lastStatus[number] = status;
			Logger::Debug("Mercenary Portal (player " + std::to_string(number) + "): " + status + ".");
		}

		bool PlayerHasRoyal(const std::uint8_t* player) {
			for (auto* creature = Thing(Load<std::uint16_t>(player + Offset::PlayerFirstCreature)); creature != nullptr; creature = Thing(Load<std::uint16_t>(creature + Offset::CreatureNextOwned))) {
				if (IsRoyal(creature[Offset::CreatureType])) return true;
			}
			return false;
		}

		// ---- The lobby's creature list (skirmish and multiplayer setup): each row sets how many of a creature type each
		// keeper may get; at level start it becomes every keeper's creature pool. The game lists only creatures that come
		// through the keeper Portal, so the heroes are added, starting at HeroLobbyDefault each. For heroes the number is
		// how many may be alive at a time (see the portal below); the royals are not listed (one per keeper per game).
		//
		// A joining player ignores the host's message unless the lists are the same size, so a setting cannot simply leave
		// rows out on one machine. Every machine keeps the rows sorted (the game's, the heroes, the Stone Knight last) with
		// room for all of them, and the list's count hides the rows at the end. A joining player takes its count from the
		// size of the host's message.
		bool IsListedHero(std::uint8_t type) {
			for (const auto& creature : PortalCreatures) {
				if (creature.type == type) return !creature.royal;
			}
			return false;
		}
		int RowClass(std::uint8_t type) { return type == Creature::Stone_Knight ? 2 : IsListedHero(type) ? 1 : 0; }
		std::uint32_t allRows = 0;
		bool rowsCanBeHidden = false;
		std::uint32_t hostRowBytes = 0;
		void SetShownRows(std::uint8_t* frontEnd, std::uint32_t rows) {
			Store<std::uint32_t>(frontEnd + Offset::LobbyCreatureCount, rows);
			Store<std::uint32_t>(reinterpret_cast<std::uint8_t*>(Resolve(Address::GameCreatureListCount)), rows);
		}
		std::uint32_t RowsToShow(const std::uint8_t* types, std::uint32_t rows) {
			if (!rowsCanBeHidden) return rows;
			const int highestClass = SessionOptions::GetLocal(SessionOption::MercenaryPortalOff) ? 0 : SessionOptions::GetLocal(SessionOption::StoneKnightsOff) ? 1 : 2;
			std::uint32_t shown = 0;
			for (std::uint32_t i = 0; i < rows; ++i) {
				if (RowClass(types[i]) <= highestClass) ++shown;
			}
			return shown;
		}
		// The host's message is a different size from what this machine's list expects: take the host's number of rows.
		std::int32_t __cdecl AcceptHostRows(std::uint8_t* frontEnd, std::uint32_t received, std::uint32_t expected) {
			const auto difference = static_cast<std::int32_t>(received) - static_cast<std::int32_t>(expected);
			const auto rowSize = static_cast<std::int32_t>(CreatureListEntrySize);
			if (difference % rowSize != 0) return 0;
			const auto rows = static_cast<std::int32_t>(Load<std::uint32_t>(frontEnd + Offset::LobbyCreatureCount)) + difference / rowSize;
			if (rows <= 0 || static_cast<std::uint32_t>(rows) > allRows) return 0;
			SetShownRows(frontEnd, static_cast<std::uint32_t>(rows));
			hostRowBytes = static_cast<std::uint32_t>(rows * rowSize);
			return 1;
		}
		std::uintptr_t hostListAccepted = 0;
		std::uintptr_t hostListRejected = 0;
		// Replaces "cmp eax, edx; jnz <ignore the message>". EBX is the creature list's bytes, used for copying it.
		__declspec(naked) void HostListSizeStub() {
			__asm {
				cmp eax, edx
				je accepted
				push edx
				push eax
				push ebp
				call AcceptHostRows
				add esp, 12
				test eax, eax
				jz rejected
				mov ebx, dword ptr [hostRowBytes]
			accepted:
				jmp dword ptr [hostListAccepted]
			rejected:
				jmp dword ptr [hostListRejected]
			}
		}
		// Each hero's row holds its rooms in its free bytes, and the first row (always one of the game's) the royals'.
		constexpr std::size_t RoyalRoomsByte = CreatureListFreeByte + 1;
		// Calls use(creature's index, its rooms' bytes in the list) for each portal creature the list holds.
		template<typename Use> void ForEachRoomsInList(const std::uint8_t* frontEnd, Use use) {
			auto* rows = *reinterpret_cast<std::uint8_t* const*>(frontEnd + Offset::LobbyCreatureList);
			const auto* types = *reinterpret_cast<const std::uint8_t* const*>(frontEnd + Offset::LobbyCreatureTypes);
			if (rows == nullptr || types == nullptr) return;
			const auto shown = Load<std::uint32_t>(frontEnd + Offset::LobbyCreatureCount);
			for (std::size_t i = 0; i < PortalCreatureCount; ++i) {
				if (PortalCreatures[i].royal) {
					use(i, rows + RoyalRoomsByte);
					continue;
				}
				for (std::uint32_t row = 0; row < shown; ++row) {
					if (types[row] == PortalCreatures[i].type) use(i, rows + row * CreatureListEntrySize + CreatureListFreeByte);
				}
			}
		}
		void WriteRoomsToList(std::uint8_t* frontEnd) {
			// A player who joined someone's game earlier may still have that host's rows.
			const auto* types = *reinterpret_cast<const std::uint8_t* const*>(frontEnd + Offset::LobbyCreatureTypes);
			if (rowsCanBeHidden && types != nullptr && allRows != 0) SetShownRows(frontEnd, RowsToShow(types, allRows));
			ForEachRoomsInList(frontEnd, [](std::size_t i, std::uint8_t* bytes) { std::memcpy(bytes, localRooms[i].data(), localRooms[i].size()); });
		}
		void ReadRoomsFromList(const std::uint8_t* frontEnd) {
			sessionRooms = localRooms;
			ForEachRoomsInList(frontEnd, [](std::size_t i, const std::uint8_t* bytes) { std::memcpy(sessionRooms[i].data(), bytes, sessionRooms[i].size()); });
		}
		// Called by the creature data loader for each creature record, cdecl (record, front end): adds the record's type
		// and name to the front end's lists when it is a Portal creature.
		using LobbyRecordFn = std::int32_t(__cdecl*)(const std::uint8_t* record, std::uint8_t* frontEnd);
		LobbyRecordFn originalLobbyRecord = nullptr;
		std::int32_t __cdecl LobbyRecordDetour(const std::uint8_t* record, std::uint8_t* frontEnd) {
			const auto before = Load<std::uint32_t>(frontEnd + Offset::LobbyCreatureCount);
			const std::int32_t result = originalLobbyRecord(record, frontEnd);
			const auto type = record[Offset::CreatureRecordType];
			auto count = Load<std::uint32_t>(frontEnd + Offset::LobbyCreatureCount);
			if (count == before && !IsListedHero(type)) return result;
			auto* types = *reinterpret_cast<std::uint8_t**>(frontEnd + Offset::LobbyCreatureTypes);
			auto* names = *reinterpret_cast<std::uint8_t**>(frontEnd + Offset::LobbyCreatureNames);
			if (types == nullptr || names == nullptr) return result;
			if (count == before) {
				// The lists are made with room for every record, so one more always fits.
				types[before] = type;
				Store<std::uint16_t>(names + before * 2, Load<std::uint16_t>(record + Offset::CreatureRecordName));
				count = before + 1;
				Store<std::uint32_t>(frontEnd + Offset::LobbyCreatureCount, count);
			}
			// Into its place in the sorted rows.
			for (std::uint32_t at = count - 1; at > 0 && RowClass(types[at - 1]) > RowClass(types[at]); --at) {
				std::swap(types[at - 1], types[at]);
				const auto name = Load<std::uint16_t>(names + at * 2);
				Store<std::uint16_t>(names + at * 2, Load<std::uint16_t>(names + (at - 1) * 2));
				Store<std::uint16_t>(names + (at - 1) * 2, name);
			}
			allRows = (std::max)(allRows, count);
			return result;
		}
		// The game's settings make the lobby's list (count entries of { type, count, ... }), all zero: always with room for
		// every row, then only the rows this machine's settings show are counted. Our heroes start at HeroLobbyDefault.
		using MakeCreatureListFn = void(__fastcall*)(void* gameConfig, void* unused, std::uint8_t** frontEndList, std::uint32_t count);
		MakeCreatureListFn originalMakeCreatureList = nullptr;
		void __fastcall MakeCreatureListDetour(void* gameConfig, void* unused, std::uint8_t** frontEndList, std::uint32_t count) {
			// The saved-game reader has no front end list.
			if (frontEndList == nullptr || count == 0) {
				originalMakeCreatureList(gameConfig, unused, frontEndList, count);
				return;
			}
			const std::uint32_t rows = (std::max)(allRows, count);
			originalMakeCreatureList(gameConfig, unused, frontEndList, rows);
			if (*frontEndList == nullptr) return;
			auto* frontEnd = reinterpret_cast<std::uint8_t*>(frontEndList) - Offset::LobbyCreatureList;
			const auto* types = *reinterpret_cast<const std::uint8_t* const*>(frontEnd + Offset::LobbyCreatureTypes);
			if (types == nullptr) return;
			for (std::uint32_t i = 0; i < rows; ++i) {
				if (IsListedHero(types[i])) (*frontEndList)[i * CreatureListEntrySize + 1] = HeroLobbyDefault;
			}
			SetShownRows(frontEnd, RowsToShow(types, rows));
		}

		// ---- The player's creature pool, as the portal reads it: count + 0xC, entries + 0x10 of { type, int number }.
		std::uint8_t* PoolEntry(std::uint8_t* player, std::uint8_t type) {
			auto* pool = player + Offset::PlayerCreaturePool;
			const auto count = Load<std::uint32_t>(pool + Offset::CreaturePoolCount);
			auto* entries = *reinterpret_cast<std::uint8_t**>(pool + Offset::CreaturePoolEntries);
			for (std::uint32_t i = 0; entries != nullptr && i < count; ++i) {
				if (entries[i * CreatureListEntrySize] == type) return entries + i * CreatureListEntrySize;
			}
			return nullptr;
		}
		std::uint32_t OwnedOfType(const std::uint8_t* player, std::uint8_t type) {
			std::uint32_t owned = 0;
			for (auto* creature = Thing(Load<std::uint16_t>(player + Offset::PlayerFirstCreature)); creature != nullptr; creature = Thing(Load<std::uint16_t>(creature + Offset::CreatureNextOwned))) {
				if (creature[Offset::CreatureType] == type) ++owned;
			}
			return owned;
		}
		// Each keeper's hero limits, taken from its pool the first time its portal runs in a level (the pool holds the
		// lobby's or the map's numbers). The game takes one off the pool for each arrival; for heroes the pool is set to
		// "limit minus how many it has" for the portal's choice and put back to the limit after, so the limit counts the
		// heroes alive at a time.
		std::array<std::array<std::int32_t, 256>, 256> heroLimits{};
		std::array<bool, 256> limitsTaken{};

		// The pool check runs before the creature test, and the royals are not in any pool: while a royal is allowed
		// during a Mercenary Portal's choice, it counts as in the pool.
		using PoolHasFn = std::int32_t(__fastcall*)(const std::uint8_t* pool, void* unused, std::uint32_t type);
		PoolHasFn originalPoolHas = nullptr;
		std::int32_t __fastcall PoolHasDetour(const std::uint8_t* pool, void* unused, std::uint32_t type) {
			if (portal.active && portal.royalAllowed && IsRoyal(static_cast<std::uint8_t>(type))) return 1;
			return originalPoolHas(pool, unused, type);
		}

		using PortalTickFn = std::int32_t(__fastcall*)(std::uint8_t* room, void* unused, std::int32_t flag);
		PortalTickFn originalPortalTick = nullptr;
		std::int32_t __fastcall PortalTickDetour(std::uint8_t* room, void* unused, std::int32_t flag) {
			// Every portal (keeper and Mercenary): creature pools the level forgot (CreatureFixes).
			CreatureFixes::BeforePortalChoice(room);
			auto* player = room[Offset::RoomType] == Room::Hero_Portal ? Thing(Load<std::uint16_t>(room + Offset::RoomOwner)) : nullptr;
			if (player == nullptr || SessionOptions::Get(SessionOption::MercenaryPortalOff)) return originalPortalTick(room, unused, flag);
			const auto number = player[Offset::PlayerNumber];
			if (!portalSeen[number]) {
				portalSeen[number] = true;
				Logger::Debug("Mercenary Portal: player " + std::to_string(number) + " has a working Mercenary Portal.");
			}
			if (!limitsTaken[number]) {
				limitsTaken[number] = true;
				for (const auto& creature : PortalCreatures) {
					const auto* entry = PoolEntry(player, creature.type);
					heroLimits[number][creature.type] = entry != nullptr ? Load<std::int32_t>(entry + 1) : 0;
				}
			}
			// The pool as the portal will read it: heroes up to their limit alive at once. Royals do not use the pool
			// (PoolHasDetour).
			std::vector<std::pair<std::uint8_t*, std::int32_t>> pools;
			for (const auto& creature : PortalCreatures) {
				auto* entry = PoolEntry(player, creature.type);
				if (entry == nullptr) continue;
				if (creature.royal) continue;
				pools.emplace_back(entry, heroLimits[number][creature.type]);
				const auto limit = heroLimits[number][creature.type];
				const auto owned = static_cast<std::int32_t>(OwnedOfType(player, creature.type));
				Store<std::int32_t>(entry + 1, limit > owned ? limit - owned : 0);
			}
			const bool royalAllowed = !royalGiven[number] && !SessionOptions::Get(SessionOption::RoyalsOff);
			LogStatus(player, royalAllowed);
			portal.active = true;
			portal.royalAllowed = royalAllowed;
			portal.creatures.clear();
			std::vector<SavedAttraction> saved;
			for (const auto& creature : PortalCreatures) {
				auto* data = CreatureData(creature.type);
				if (data == nullptr) continue;
				SavedAttraction save{ data, {}, data[Offset::CreatureDataAttractionCount] };
				std::memcpy(save.entries.data(), data + Offset::CreatureDataAttraction, save.entries.size());
				saved.push_back(save);
				std::uint8_t count = 0;
				for (const auto roomType : RoomsOf(creature)) {
					if (roomType == 0) continue;
					auto* entry = data + Offset::CreatureDataAttraction + count * CreatureAttractionEntrySize;
					entry[0] = roomType;
					entry[1] = 1;
					entry[2] = AttractionKindRoom;
					++count;
				}
				data[Offset::CreatureDataAttractionCount] = count;
				portal.creatures[data] = &creature;
			}
			const std::int32_t result = originalPortalTick(room, unused, flag);
			for (const auto& save : saved) {
				std::memcpy(save.data + Offset::CreatureDataAttraction, save.entries.data(), save.entries.size());
				save.data[Offset::CreatureDataAttractionCount] = save.count;
			}
			portal.active = false;
			for (const auto& [entry, value] : pools) Store<std::int32_t>(entry + 1, value);
			if (!royalGiven[number] && PlayerHasRoyal(player)) {
				royalGiven[number] = true;
				Logger::Debug("Mercenary Portal: player " + std::to_string(number) + " has had its royal.");
			}
			return result;
		}

		void OnLevelStart() {
			CreatureFixes::OnLevelStart();
			royalGiven.fill(false);
			portalSeen.fill(false);
			limitsTaken.fill(false);
			for (auto& status : lastStatus) status.clear();
			std::string missing;
			for (const auto& creature : PortalCreatures) {
				if (CreatureData(creature.type) == nullptr) missing += std::string(missing.empty() ? "" : ", ") + creature.name + " (" + std::to_string(creature.type) + ")";
			}
			if (!missing.empty()) Logger::Warning("Mercenary Portal: no creature data for " + missing + "; they cannot come.");
		}

		bool Hook(std::uintptr_t target, void* detour, void** original) {
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
	}

	void MercenaryPortal::Install() {
		LoadRooms();
		const auto tick = Resolve(Address::PortalAttraction);
		const auto test = Resolve(Address::MercenaryPortalCreatureTest);
		portalAccept = Resolve(Address::PortalCreatureAccepted);
		portalReject = Resolve(Address::PortalCreatureRejected);
		// "test byte [eax + 0x68A], 8; jz <skip>" (the jz goes to the rejected path), then "mov edx, [ebp-0x14]".
		const auto rel = static_cast<std::uint32_t>(portalReject - (test + 13));
		const std::vector<std::uint8_t> testPattern = { 0xF6, 0x80, 0x8A, 0x06, 0x00, 0x00, 0x08, 0x0F, 0x84,
			static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
		if (!Memory::Matches(tick, { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x76, 0xCD, 0x64, 0x00 })
			|| !Memory::Matches(test, testPattern)
			|| !Memory::Matches(portalAccept, { 0x8B, 0x55, 0xEC, 0x80, 0x7A, 0x18, 0x19 })) {
			Logger::Warning("Mercenary Portal: unexpected game code, the portal keeps the game's own rules.");
			return;
		}
		if (!Hook(tick, reinterpret_cast<void*>(&PortalTickDetour), reinterpret_cast<void**>(&originalPortalTick))) {
			Logger::Warning("Mercenary Portal: could not hook the portal, it keeps the game's own rules.");
			return;
		}
		if (!Memory::WriteJump(test, reinterpret_cast<const void*>(&PortalCreatureStub), 13)) {
			HookManager::RemoveHook(tick);
			Logger::Warning("Mercenary Portal: could not patch the portal's creature test, it keeps the game's own rules.");
			return;
		}
		const auto mercenaryFlagSite = Resolve(Address::PortalMercenaryFlag);
		mercenaryResume = Resolve(Address::PortalMercenaryResume);
		if (!Memory::Matches(mercenaryFlagSite, { 0x6A, 0x01, 0x34, 0x01, 0x8B, 0xCE })
			|| !Memory::Matches(mercenaryResume, { 0x88, 0x86, 0xF3, 0x00, 0x00, 0x00, 0xE8 })
			|| !Memory::WriteJump(mercenaryFlagSite, reinterpret_cast<const void*>(&MercenaryFlagStub), 6)) {
			Logger::Warning("Mercenary Portal: unexpected arrival code, heroes always arrive as mercenaries.");
		}
		const auto setMercenary = Resolve(Address::SetCreatureMercenary);
		if (!Memory::Matches(setMercenary, { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xB1, 0xEA, 0x03, 0x00, 0x00 })
			|| !Hook(setMercenary, reinterpret_cast<void*>(&SetMercenaryDetour), reinterpret_cast<void**>(&originalSetMercenary))) {
			Logger::Warning("Mercenary Portal: unexpected mercenary code, royals, Stone Knights and Dwarfs may be drawn white as mercenaries.");
		}
		// EAX, ECX and EDX are loaded again after the test, so the stub may use them.
		const auto handTest = Resolve(Address::HandMercenaryTest);
		handNormal = Resolve(Address::HandTexturesNormal);
		handMercenary = Resolve(Address::HandTexturesMercenary);
		if (!Memory::Matches(handTest, { 0x8B, 0x88, 0xEA, 0x03, 0x00, 0x00, 0xF6, 0xC5, 0x02, 0x75, 0x09 })
			|| !Memory::Matches(handNormal, { 0xC6, 0x87, 0x64, 0x10, 0x00, 0x00, 0x00, 0xEB, 0x07 })
			|| !Memory::Matches(handMercenary, { 0xC6, 0x87, 0x64, 0x10, 0x00, 0x00, 0x01 })
			|| !Memory::WriteJump(handTest, reinterpret_cast<const void*>(&HandMercenaryStub), 11)) {
			Logger::Warning("Mercenary Portal: unexpected hand code, royals, Stone Knights and Dwarfs may be drawn white in the hand as mercenaries.");
		}
		const auto poolHas = Resolve(Address::CreaturePoolHas);
		if (!Memory::Matches(poolHas, { 0x8B, 0x54, 0x24, 0x04, 0x56, 0x8B, 0x71, 0x0C })
			|| !Hook(poolHas, reinterpret_cast<void*>(&PoolHasDetour), reinterpret_cast<void**>(&originalPoolHas))) {
			Logger::Warning("Mercenary Portal: could not hook the creature pool check, the royals cannot come.");
		}
		const auto lobbyRecord = Resolve(Address::LobbyCreatureRecord);
		const auto makeList = Resolve(Address::MakeCreatureList);
		if (!Memory::Matches(lobbyRecord, { 0x8B, 0x4C, 0x24, 0x04, 0x53, 0x56, 0x8B, 0x81, 0xEC, 0x12, 0x00, 0x00 })
			|| !Memory::Matches(makeList, { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57, 0x85, 0xF6, 0x8B, 0xD9 })
			|| !Hook(lobbyRecord, reinterpret_cast<void*>(&LobbyRecordDetour), reinterpret_cast<void**>(&originalLobbyRecord))) {
			Logger::Warning("Mercenary Portal: unexpected lobby code, the heroes are not in the lobby's creature list.");
		}
		else if (!Hook(makeList, reinterpret_cast<void*>(&MakeCreatureListDetour), reinterpret_cast<void**>(&originalMakeCreatureList))) {
			Logger::Warning("Mercenary Portal: could not hook the lobby's creature list, the heroes start at 0 there.");
		}
		else {
			// "cmp eax, edx; jnz" to the rejected path.
			const auto sizeCheck = Resolve(Address::LobbyHostListSizeCheck);
			hostListAccepted = Resolve(Address::LobbyHostListAccepted);
			hostListRejected = Resolve(Address::LobbyHostListRejected);
			rowsCanBeHidden = Memory::Matches(sizeCheck, { 0x3B, 0xC2, 0x0F, 0x85, 0xED, 0x02, 0x00, 0x00 })
				&& Memory::WriteJump(sizeCheck, reinterpret_cast<const void*>(&HostListSizeStub), 8);
			if (!rowsCanBeHidden) {
				Logger::Warning("Mercenary Portal: unexpected lobby message code, the heroes stay in the lobby's creature list even when turned off.");
			}
		}
		SessionOptions::OnLobbyWrite(&WriteRoomsToList);
		SessionOptions::OnLobbyRead(&ReadRoomsFromList);
		SessionOptions::OnLevelStart(&OnLevelStart);
		if (SessionOptions::Get(SessionOption::MercenaryPortalOff)) {
			Logger::Info("Mercenary Portal: off, the game's own rules (in multiplayer the host's setting is used).");
			return;
		}
		Logger::Info(std::string("Mercenary Portal: heroes by room; Stone Knights ") + (SessionOptions::Get(SessionOption::StoneKnightsOff) ? "off" : "on")
			+ "; one royal per keeper " + (SessionOptions::Get(SessionOption::RoyalsOff) ? "off" : "on")
			+ "; the game's own mercenaries " + (SessionOptions::Get(SessionOption::MercenaryPortalNormal) ? "off" : "on") + " (in multiplayer the host's settings are used).");
	}
}
