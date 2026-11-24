#include "PossessionActions.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "SessionOptions.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

// See docs/game/possession.md for the game code behind every step.
namespace KeeperForge2 {
	namespace {
		namespace Address = Game::Address;
		namespace Offset = Game::Offset;

		// A thing's own tag.
		constexpr std::uintptr_t ThingTag = 0x4;
		// Thing kinds (Offset::ThingKind).
		constexpr std::int32_t KindCreature = 0;
		constexpr std::int32_t KindObject = 2;
		// Object types (Objects.kwd ids).
		constexpr std::uint8_t ObjectChicken = 9;
		constexpr std::uint8_t ObjectTrainingDummy = 14;
		constexpr std::uint8_t ObjectTrainingTarget = 29;
		constexpr std::uint8_t ObjectPrisonDoor = 109;
		constexpr std::uint8_t ObjectPrisonDoorBar = 116;
		// The bar's lifting animation, as the Prison plays it for an Imp (0x4E29E0).
		constexpr std::uint32_t BarLiftAnimation = 0x2E;
		// Creature states: idle; "in a Prison? then a prisoner" (0x473130); a prisoner; entering a Combat Pit (0x457A70).
		constexpr std::int32_t StateIdle = 1;
		// Walking somewhere (0x471890 sets it; the state on arrival is kept apart).
		constexpr std::int32_t StateWalking = 6;
		constexpr std::int32_t StateImprison = 0x71;
		constexpr std::int32_t StatePrisoner = 0x73;
		constexpr std::int32_t StatePitEnter = 0x89;
		// Creature flags (+ 0x3E6): in a hand, possessed. State flags (+ 0x3EA): dies at no health rather than being
		// knocked out (0x48B770).
		constexpr std::uint32_t FlagInHand = 0x1;
		constexpr std::uint32_t FlagPossessed = 0x80;
		constexpr std::uint32_t FlagDieInstantly = 0x800;
		// The melee bit of a possessed action's data3.
		constexpr std::uint32_t ActionMeleeBit = 0x8000000;
		// The game's actions are 0x12 bytes; the stop action reads only the player tag.
		constexpr std::size_t ActionSize = 0x12;
		// The keeper's "not enough gold to train" event, as the Training Room raises it.
		constexpr std::int32_t TrainingNoGoldEvent = 0x37;
		// A Prison door held open for followers on their way in, at most this long.
		// Loops over the game's lists stop here, in case a list is broken.
		constexpr int ListLimit = 4096;

		using PossessedActionFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint32_t* action);
		using MeleeReadyFn = std::int32_t(__fastcall*)(std::uint8_t* creature, void* unused, std::uint8_t* data);
		using ThingInFrontFn = std::uint32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint8_t* creature, std::uint32_t* position, std::uint16_t* angles);
		using TileAtFn = std::uint8_t*(__fastcall*)(std::uint8_t* world, void* unused, std::int32_t x, std::int32_t y);
		using GameTurnFn = std::uint32_t(__fastcall*)(std::uint8_t* world, void* unused);
		using CreatureAmountFn = void(__fastcall*)(std::uint8_t* creature, void* unused, std::int32_t amount);
		using SetStateFn = std::int32_t(__fastcall*)(std::uint8_t* block, void* unused, std::int32_t state, std::int32_t value);
		using BlockFn = std::int32_t(__fastcall*)(std::uint8_t* block, void* unused);
		using FollowFn = std::int32_t(__fastcall*)(std::uint8_t* block, void* unused, std::uint32_t leader);
		using ThingFn = void(__fastcall*)(std::uint8_t* thing, void* unused);
		using AnimationFn = void(__fastcall*)(std::uint8_t* object, void* unused, std::uint32_t animation);

		PossessedActionFn originalPossessedAttack = nullptr;
		using TargetFilterFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint8_t* creature, std::uint8_t* thing, std::int32_t flag);
		using TargetReachFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint8_t* creature, std::uint8_t* thing);
		TargetFilterFn originalTargetFilter = nullptr;
		// How far (across, 1/4096 tile) a bed, Training Target or Prison door can be aimed at: a tile and a half.
		constexpr std::int32_t AimReach = 0x1800;
		// A Prison bolt: within this of its tile's nearest edge (a tile).
		constexpr std::int32_t DoorReach = 0x1000;

		template<typename T> T Load(const std::uint8_t* thing, std::uintptr_t offset) { return *reinterpret_cast<const T*>(thing + offset); }
		template<typename T> void Store(std::uint8_t* thing, std::uintptr_t offset, T value) { *reinterpret_cast<T*>(thing + offset) = value; }
		template<typename F> F GameFunction(std::uintptr_t address) { return reinterpret_cast<F>(Game::Resolve(address)); }

		std::uint8_t* Thing(std::uint16_t tag) {
			if (tag == 0 || tag > Game::TagMask || Memory::Read<std::uint8_t>(Game::Resolve(Address::ThingInUse) + tag) == 0) return nullptr;
			return reinterpret_cast<std::uint8_t**>(Game::Resolve(Address::ThingTable))[tag];
		}
		bool IsKind(const std::uint8_t* thing, std::int32_t kind) { return thing != nullptr && Load<std::int32_t>(thing, Offset::ThingKind) == kind; }
		bool IsCreature(const std::uint8_t* thing) { return IsKind(thing, KindCreature); }
		std::uint16_t TagOf(const std::uint8_t* thing) { return Load<std::uint16_t>(thing, ThingTag); }
		// The creature still behind a tag we kept (a tag can be given to a new thing once its creature is gone).
		std::uint8_t* Same(std::uint16_t tag, const std::uint8_t* kept) {
			auto* thing = Thing(tag);
			return thing == kept && IsCreature(thing) ? thing : nullptr;
		}
		std::uint8_t* Block(std::uint8_t* creature) { return creature + Offset::CreatureStateBlock; }
		bool SetState(std::uint8_t* creature, std::int32_t state) { return GameFunction<SetStateFn>(Address::StateBlockSetState)(Block(creature), nullptr, state, 0) != 0; }
		std::uint32_t GameTurn(std::uint8_t* world) { return GameFunction<GameTurnFn>(Address::WorldGameTurn)(world, nullptr); }
		std::uint32_t TurnsPerSecond() {
			const auto value = Memory::Read<std::uint8_t>(Game::Resolve(Address::TurnsPerSecond));
			return value != 0 ? value : 1;
		}
		template<typename F> F WorldMethod(std::uint8_t* world, std::uintptr_t slot) { return reinterpret_cast<F>((*reinterpret_cast<std::uintptr_t**>(world))[slot / sizeof(std::uintptr_t)]); }

		// The room under a thing (nullptr when its tile is not part of one).
		std::uint8_t* RoomAt(std::uint8_t* world, const std::uint8_t* thing) {
			const auto* tile = WorldMethod<TileAtFn>(world, Offset::WorldTileAt)(world, nullptr, Load<std::int32_t>(thing, Offset::ThingPositionX) >> 12, Load<std::int32_t>(thing, Offset::ThingPositionY) >> 12);
			if (tile == nullptr) return nullptr;
			return Thing(static_cast<std::uint16_t>(Load<std::uint16_t>(tile, Offset::TileRoom) & Game::TagMask));
		}
		// Whether a room's tile is inside its edge (0x454110): a Prison's cells, a Combat Pit's floor.
		using TileFn = std::int32_t(__fastcall*)(const std::uint8_t* tile);
		bool RoomHolds(const std::uint8_t* room, const std::uint8_t* thing) {
			int guard = 0;
			for (auto* each = Thing(Load<std::uint16_t>(room, Offset::RoomFirstThing)); each != nullptr && guard < ListLimit; each = Thing(Load<std::uint16_t>(each, Offset::ThingNextInRoom)), ++guard) {
				if (each == thing) return true;
			}
			return false;
		}
		std::uint8_t* RoomObject(std::uint8_t* room, std::uint8_t type) {
			int guard = 0;
			for (auto* each = Thing(Load<std::uint16_t>(room, Offset::RoomFirstThing)); each != nullptr && guard < ListLimit; each = Thing(Load<std::uint16_t>(each, Offset::ThingNextInRoom)), ++guard) {
				if (IsKind(each, KindObject) && each[Offset::ObjectType] == type) return each;
			}
			return nullptr;
		}
		// Every creature of the leader's keeper that follows it.
		template<typename F> void ForEachFollower(const std::uint8_t* leader, F&& visit) {
			const auto* owner = Thing(Load<std::uint16_t>(leader, Offset::CreatureOwner));
			if (owner == nullptr) return;
			const auto leaderTag = TagOf(leader);
			int guard = 0;
			for (auto* creature = Thing(Load<std::uint16_t>(owner, Offset::PlayerFirstCreature)); creature != nullptr && guard < ListLimit; creature = Thing(Load<std::uint16_t>(creature, Offset::CreatureNextOwned)), ++guard) {
				if (creature != leader && IsCreature(creature) && Load<std::uint16_t>(creature, Offset::CreatureFollowing) == leaderTag) visit(creature);
			}
		}
		// A follower stops following, and the leader's count goes down with it (as a follower's death does).
		void StopFollowing(std::uint8_t* creature, std::uint8_t* leader) {
			Store<std::uint16_t>(creature, Offset::CreatureFollowing, 0);
			const auto count = Load<std::int32_t>(leader, Offset::CreatureFollowerCount);
			if (count > 0) Store<std::int32_t>(leader, Offset::CreatureFollowerCount, count - 1);
		}
		void SetDieInstantly(std::uint8_t* creature, bool on) {
			auto flags = Load<std::uint32_t>(creature, Offset::CreatureStateFlags);
			flags = on ? (flags | FlagDieInstantly) : (flags & ~FlagDieInstantly);
			Store<std::uint32_t>(creature, Offset::CreatureStateFlags, flags);
		}
		bool DiesInstantly(const std::uint8_t* creature) { return (Load<std::uint32_t>(creature, Offset::CreatureStateFlags) & FlagDieInstantly) != 0; }
		// Leaves the possessed creature, through the game's own action handler (it reads only the player tag).
		void StopPossession(std::uint8_t* world, std::uint16_t playerTag) {
			std::uint8_t action[ActionSize + 2] = {};
			*reinterpret_cast<std::uint16_t*>(action + Offset::ActionPlayer) = playerTag;
			GameFunction<PossessedActionFn>(Address::StopPossessionAction)(world, nullptr, reinterpret_cast<std::uint32_t*>(action));
		}

		// ---- Training, as the Training Room does it (its per-turn handler 0x47B2D0), so possession gains nothing over it:
		// - experience every turn: the type's training experience at level 1, scaled by the settings' percent table at
		//   other levels, none at or above the room's highest level;
		// - gold every second: the training cost (a negative 16 bit global), taken from the keeper's Treasuries and Heart
		//   (0x4BA420); with too little gold the keeper hears so (event 0x37) and nothing is gained;
		// - every 4 seconds the gold spent shows over the creature (0x50FB70).
		// A hit counts for the turns since the creature's last training hit, at least one second and at most three, and
		// pays for those turns and shows what it paid.
		using TakeGoldFn = std::uint32_t(__fastcall*)(std::uint8_t* player, void* unused, std::int32_t amount, std::int32_t direct, std::int32_t x, std::int32_t y);
		using ShowGoldFn = void(__fastcall*)(std::uint8_t* world, void* unused, std::uint32_t ownerTag, std::int32_t* position, std::int32_t amount, std::int32_t zero);
		using EventFn = void(__fastcall*)(std::uint8_t* events, void* unused, std::int32_t event, std::uint32_t tag, std::int32_t value, std::int32_t value2, std::int32_t value3);
		struct TrainingHit {
			std::uint16_t tag;
			const std::uint8_t* creature;
			std::uint32_t turn;
		};
		std::vector<TrainingHit> trainingHits;
		std::uint32_t TrainingTurns(std::uint8_t* world, std::uint8_t* creature) {
			const auto perSecond = TurnsPerSecond();
			const auto now = GameTurn(world);
			std::erase_if(trainingHits, [](const TrainingHit& kept) { return Same(kept.tag, kept.creature) == nullptr; });
			for (auto& kept : trainingHits) {
				if (kept.creature != creature) continue;
				const auto since = now - kept.turn;
				kept.turn = now;
				return since < perSecond ? perSecond : (since > perSecond * 3 ? perSecond * 3 : since);
			}
			trainingHits.push_back({ TagOf(creature), creature, now });
			return perSecond;
		}
		// Pays for the turns as the Training Room would; false when the keeper cannot.
		bool PayForTraining(std::uint8_t* world, std::uint8_t* creature, std::uint32_t turns) {
			const std::int32_t costPerSecond = -static_cast<std::int32_t>(Memory::Read<std::int16_t>(Game::Resolve(Address::TrainingCostPerSecond)));
			if (costPerSecond <= 0) return true;
			auto* player = Load<std::uint8_t*>(creature, Offset::CreaturePlayer);
			if (player == nullptr) return false;
			const auto charge = static_cast<std::int32_t>(static_cast<std::int64_t>(costPerSecond) * turns / TurnsPerSecond());
			const auto ownerTag = Load<std::uint16_t>(creature, Offset::CreatureOwner);
			if (Load<std::int32_t>(player, Offset::PlayerGold) < charge) {
				GameFunction<EventFn>(Address::PlayerEvent)(world + Offset::WorldEvents, nullptr, TrainingNoGoldEvent, ownerTag, 5, 0, 1);
				Logger::Debug("Possession: training hit by creature " + std::to_string(TagOf(creature)) + ": not enough gold (" + std::to_string(charge) + " needed).");
				return false;
			}
			const auto x = Load<std::int32_t>(creature, Offset::ThingPositionX);
			const auto y = Load<std::int32_t>(creature, Offset::ThingPositionY);
			const auto taken = GameFunction<TakeGoldFn>(Address::PlayerTakeGold)(player, nullptr, -charge, 0, x >> 12, y >> 12);
			if (taken == 0) return false;
			GameFunction<ShowGoldFn>(Address::ShowMoneyText)(world, nullptr, ownerTag, reinterpret_cast<std::int32_t*>(creature + Offset::ThingPositionX), static_cast<std::int32_t>(taken), 0);
			return true;
		}
		void GiveTrainingExperience(std::uint8_t* world, std::uint8_t* creature) {
			const auto level = creature[Offset::CreatureLevel];
			const auto cap = Memory::Read<std::uint8_t>(Game::Resolve(Address::MaxTrainingLevel));
			if (level >= cap) {
				Logger::Debug("Possession: training hit by creature " + std::to_string(TagOf(creature)) + " at level " + std::to_string(level) + ": no experience (the Training Room trains to level " + std::to_string(cap) + ").");
				return;
			}
			const auto* data = Load<const std::uint8_t*>(creature, Offset::CreatureData);
			if (data == nullptr) return;
			const std::int32_t base = Load<std::uint16_t>(data, Offset::CreatureDataTrainingExperience);
			std::int32_t amount = base;
			if (level != 1) {
				const auto* settings = Memory::Read<const std::uint8_t*>(Game::Resolve(Address::GlobalSettingsPointer));
				if (settings == nullptr) return;
				amount = static_cast<std::int32_t>(Load<std::int16_t>(settings, Offset::SettingsLevelTrainingPercent + level * 2u)) * base / 100;
			}
			if (amount < 1) return;
			if (amount > 0xFFFE) amount = 0xFFFE;
			const auto turns = TrainingTurns(world, creature);
			if (!PayForTraining(world, creature, turns)) return;
			amount *= static_cast<std::int32_t>(turns);
			GameFunction<CreatureAmountFn>(Address::CreatureAddExperienceCapped)(creature, nullptr, amount);
			Logger::Debug("Possession: training hit by creature " + std::to_string(TagOf(creature)) + " at level " + std::to_string(level) + ": " + std::to_string(amount) + " experience, now " + std::to_string(Load<std::uint32_t>(creature, Offset::CreatureExperience)) + ".");
		}

		// ---- Ranged training: the game already finds a Training Target near a possessed creature's shot and makes an effect
		// there (0x4ABE54); the caster now trains as for a melee hit, once a shot.
		struct RewardedShot {
			std::uint16_t tag;
			const std::uint8_t* shot;
		};
		std::vector<RewardedShot> rewardedShots;
		void __cdecl OnPossessedShotAtTarget(std::uint8_t* shot, std::uint8_t* world) {
			// Shots that are gone leave the list.
			std::erase_if(rewardedShots, [](const RewardedShot& kept) { return Thing(kept.tag) != kept.shot; });
			const auto tag = TagOf(shot);
			for (const auto& kept : rewardedShots) {
				if (kept.tag == tag && kept.shot == shot) return;
			}
			if (world == nullptr || Load<std::int32_t>(shot, Offset::ShotCastByPossessed) == 0) return;
			auto* caster = Thing(Load<std::uint16_t>(shot, Offset::ShotCastBy));
			if (!IsCreature(caster) || (Load<std::uint32_t>(caster, Offset::CreatureFlags) & FlagPossessed) == 0) return;
			rewardedShots.push_back({ tag, shot });
			Logger::Debug("Possession: creature " + std::to_string(TagOf(caster)) + "'s shot reached a Training Target.");
			GiveTrainingExperience(world, caster);
		}
		// Replaces the effect call: our check first (the registers the call needs kept; ECX is the world), then the game's
		// call itself, with its arguments and our return address on the stack as the call left them.
		__declspec(naked) void TrainingEffectStub() {
			__asm {
				push eax
				push ecx
				push edx
				push ecx
				push esi
				call OnPossessedShotAtTarget
				add esp, 8
				pop edx
				pop ecx
				pop eax
				jmp dword ptr [edx + 0x38C]
			}
		}
		bool InstallRangedTraining() {
			const auto check = Game::Resolve(Address::PossessedShotTrainingCheck);
			const auto call = Game::Resolve(Address::PossessedShotTrainingEffect);
			// cmp byte [eax+0x12C], 0x1D (a Training Target) ... mov edx, [ecx]; push eax; push 1; call [edx+0x38C]
			if (!Memory::Matches(check, { 0x80, 0xB8, 0x2C, 0x01, 0x00, 0x00, 0x1D, 0x75, 0x29 })
				|| !Memory::Matches(call - 5, { 0x8B, 0x11, 0x50, 0x6A, 0x01, 0xFF, 0x92, 0x8C, 0x03, 0x00, 0x00 })) return false;
			if (!Memory::WriteCall(call, reinterpret_cast<const void*>(&TrainingEffectStub))) return false;
			Memory::Patch(call + 5, { 0x90 });
			return true;
		}

		// ---- Bed: possession ends, the creature gets tired (sleep left = its sleep length, as the game does when its time
		// awake is up) and goes to bed.
		void SendToBed(std::uint8_t* world, std::uint16_t playerTag, std::uint8_t* creature, const std::uint8_t* data) {
			StopPossession(world, playerTag);
			Store<std::int32_t>(creature, Offset::CreatureSleepLeft, Load<std::int32_t>(data, Offset::CreatureDataSleepLength));
			const bool going = GameFunction<BlockFn>(Address::StateBlockGoToBed)(Block(creature), nullptr) != 0;
			Logger::Debug(std::string("Possession: creature ") + std::to_string(TagOf(creature)) + (going ? " goes to bed." : " could not go to its bed."));
		}

		// ---- Moving creatures across what the game will not let them walk through: the Prison's doorway (it stays solid for a
		// possessed creature even when open [user: an invisible wall where the door is]) and the Combat Pit's edge (the pit's
		// floor is lower, 0x4545C0 gives 0x400 against 0x1000 [verified in the export]). A creature is put on the other side
		// with the game's own move (0x4B5560, relinked into its new tile) at that tile's floor height, and given a new path.
		using SetPositionFn = void(__fastcall*)(std::uint8_t* thing, void* unused, std::int32_t* position);
		using FloorHeightFn = std::int32_t(__fastcall*)(const std::uint8_t* tile, void* unused, const std::int32_t* position);
		using WalkFn = std::int32_t(__fastcall*)(std::uint8_t* block, void* unused, std::int32_t* position, std::int32_t arrival, std::uint32_t flags);
		using MapSizeFn = std::int32_t(__fastcall*)(std::uint8_t* world);
		using CreatureOnlyFn = void(__fastcall*)(std::uint8_t* creature);
		constexpr std::int32_t TileSize = 0x1000;
		// Walking rules the game passes when it sends a creature somewhere (0x485524).
		constexpr std::uint32_t WalkFlags = 0x80;
		// How near counts as touching a spot (the doorway, where the leader jumped): three quarters of a tile.
		constexpr std::int32_t TouchReach = 0xC00;
		// How far ahead of a creature its facing is looked at (three quarters of a tile).
		constexpr std::int32_t AheadDistance = 0xC00;
		// How far around a spot an inner tile is looked for.
		constexpr std::int32_t InnerSearchTiles = 8;

		std::int32_t TileOf(std::int32_t fixed) { return fixed >> 12; }
		const std::uint8_t* TileAt(std::uint8_t* world, std::int32_t tx, std::int32_t ty) {
			const auto width = WorldMethod<MapSizeFn>(world, Offset::WorldMapWidth)(world);
			const auto height = WorldMethod<MapSizeFn>(world, Offset::WorldMapHeight)(world);
			if (tx < 0 || ty < 0 || tx >= width || ty >= height) return nullptr;
			return WorldMethod<TileAtFn>(world, Offset::WorldTileAt)(world, nullptr, tx, ty);
		}
		// A tile of the room inside its edge: a Prison's cells (where it takes prisoners), a Combat Pit's floor.
		bool InnerTile(std::uint8_t* world, std::int32_t tx, std::int32_t ty, const std::uint8_t* room) {
			const auto* tile = TileAt(world, tx, ty);
			if (tile == nullptr || Thing(static_cast<std::uint16_t>(Load<std::uint16_t>(tile, Offset::TileRoom) & Game::TagMask)) != room) return false;
			return GameFunction<TileFn>(Address::TileTakesPrisoners)(tile) != 0;
		}
		bool OnInnerTile(std::uint8_t* world, const std::uint8_t* thing, const std::uint8_t* room) {
			return InnerTile(world, TileOf(Load<std::int32_t>(thing, Offset::ThingPositionX)), TileOf(Load<std::int32_t>(thing, Offset::ThingPositionY)), room);
		}
		// The middle of the room's inner tile nearest a spot (row order on a tie, the same on every machine).
		bool NearestInnerTile(std::uint8_t* world, const std::uint8_t* room, std::int32_t x, std::int32_t y, std::int32_t& outX, std::int32_t& outY) {
			const auto tx = TileOf(x);
			const auto ty = TileOf(y);
			bool found = false;
			std::int64_t best = 0;
			for (auto cy = ty - InnerSearchTiles; cy <= ty + InnerSearchTiles; ++cy) {
				for (auto cx = tx - InnerSearchTiles; cx <= tx + InnerSearchTiles; ++cx) {
					if (!InnerTile(world, cx, cy, room)) continue;
					const std::int32_t middleX = (cx << 12) + TileSize / 2;
					const std::int32_t middleY = (cy << 12) + TileSize / 2;
					const std::int64_t dx = middleX - x;
					const std::int64_t dy = middleY - y;
					const auto distance = dx * dx + dy * dy;
					if (found && distance >= best) continue;
					found = true;
					best = distance;
					outX = middleX;
					outY = middleY;
				}
			}
			return found;
		}
		// Puts a creature at a spot on the map, on its tile's floor.
		bool MoveTo(std::uint8_t* world, std::uint8_t* creature, std::int32_t x, std::int32_t y) {
			const auto* tile = TileAt(world, TileOf(x), TileOf(y));
			if (tile == nullptr) return false;
			std::int32_t position[3] = { x, y, 0 };
			position[2] = GameFunction<FloorHeightFn>(Address::TileFloorHeight)(tile, nullptr, position);
			GameFunction<SetPositionFn>(Address::ThingSetPosition)(creature, nullptr, position);
			Store<std::int32_t>(creature, Offset::ThingVelocityX, 0);
			Store<std::int32_t>(creature, Offset::ThingVelocityY, 0);
			GameFunction<CreatureOnlyFn>(Address::CreatureNewPath)(creature);
			return true;
		}
		bool WalkTo(std::uint8_t* creature, std::int32_t x, std::int32_t y) {
			std::int32_t position[3] = { x, y, 0 };
			return GameFunction<WalkFn>(Address::StateBlockWalkTo)(Block(creature), nullptr, position, StateIdle, WalkFlags) != 0;
		}
		// The spot a distance ahead of where a creature faces, by the game's own direction tables (16.16; x from the first,
		// y the second's negative, as the possessed melee uses them, 0x514830), so every machine gets the same.
		void Ahead(const std::uint8_t* creature, std::int32_t distance, std::int32_t& x, std::int32_t& y) {
			const auto heading = static_cast<std::uint32_t>(Load<std::uint16_t>(creature, Offset::ThingHeading) & 0x7FF);
			const auto across = Memory::Read<std::int32_t>(Game::Resolve(Address::DirectionTableX) + heading * 4);
			const auto down = Memory::Read<std::int32_t>(Game::Resolve(Address::DirectionTableY) + heading * 4);
			x = Load<std::int32_t>(creature, Offset::ThingPositionX) + static_cast<std::int32_t>((static_cast<std::int64_t>(across) * distance) >> 16);
			y = Load<std::int32_t>(creature, Offset::ThingPositionY) - static_cast<std::int32_t>((static_cast<std::int64_t>(down) * distance) >> 16);
		}
		bool Near(const std::uint8_t* creature, std::int32_t x, std::int32_t y, std::int32_t reach) {
			const std::int64_t dx = Load<std::int32_t>(creature, Offset::ThingPositionX) - x;
			const std::int64_t dy = Load<std::int32_t>(creature, Offset::ThingPositionY) - y;
			return dx * dx + dy * dy <= static_cast<std::int64_t>(reach) * reach;
		}

		// ---- Prison. Hitting the bolt opens the door (as for an Imp) and arms the doorway for a while: a possessed creature
		// that then touches it (comes within TouchReach of the door's tile) is put on the nearest cell tile and made a
		// prisoner, and possession ends. Its followers walk to the spot where it touched, and each that gets there follows
		// it in.
		struct PrisonVisit {
			std::uint16_t room;
			std::uint16_t leader;
			std::uint16_t player;
			std::uint32_t startTurn;
		};
		std::vector<PrisonVisit> prisonVisits;
		// How long the doorway stays armed after the bolt is hit, and how long followers have to reach the spot.
		constexpr std::uint32_t PrisonArmedSeconds = 30;
		constexpr std::uint32_t PrisonFollowSeconds = 30;
		struct PrisonWalker {
			std::uint16_t tag;
			const std::uint8_t* creature;
			std::uint16_t room;
			std::int32_t spotX;
			std::int32_t spotY;
			std::uint32_t untilTurn;
			std::uint32_t nextWalkTurn;
		};
		std::vector<PrisonWalker> prisonWalkers;

		// The Prison a door belongs to: the room on its tile, else one of the keeper's Prisons that holds it.
		std::uint8_t* PrisonOfDoor(std::uint8_t* world, std::uint8_t* door, const std::uint8_t* keeper) {
			auto* room = RoomAt(world, door);
			if (room != nullptr && room[Offset::RoomType] == Game::Room::Prison && RoomHolds(room, door)) return room;
			if (keeper == nullptr) return nullptr;
			int guard = 0;
			for (room = Thing(Load<std::uint16_t>(keeper, Offset::PlayerFirstRoom)); room != nullptr && guard < ListLimit; room = Thing(Load<std::uint16_t>(room, Offset::RoomNextOwned)), ++guard) {
				if (room[Offset::RoomType] == Game::Room::Prison && RoomHolds(room, door)) return room;
			}
			return nullptr;
		}
		// Opens the door as the Prison does for an Imp (0x4E29E0): the bar lifts, the door opens, the countdown starts.
		void OpenPrisonDoor(std::uint8_t* world, std::uint8_t* room, std::uint8_t* player, std::uint8_t* creature) {
			if (Load<std::int32_t>(room, Offset::PrisonDoorOpen) == 0) {
				if (Load<std::int32_t>(room, Offset::PrisonBarLifted) == 0) {
					if (auto* bar = RoomObject(room, ObjectPrisonDoorBar)) {
						GameFunction<AnimationFn>(Address::ObjectSetAnimation)(bar, nullptr, BarLiftAnimation);
						Store<std::int32_t>(room, Offset::PrisonBarLifted, 1);
					}
				}
				GameFunction<ThingFn>(Address::PrisonToggleDoor)(room, nullptr);
				Store<std::int32_t>(room, Offset::PrisonDoorOpen, 1);
			}
			room[Offset::PrisonDoorCountdown] = static_cast<std::uint8_t>(TurnsPerSecond() * 2);
			Logger::Debug("Possession: creature " + std::to_string(TagOf(creature)) + " opened Prison " + std::to_string(TagOf(room)) + "'s door; its doorway takes prisoners for " + std::to_string(PrisonArmedSeconds) + " seconds.");
			const PrisonVisit visit{ TagOf(room), TagOf(creature), TagOf(player), GameTurn(world) };
			for (auto& kept : prisonVisits) {
				if (kept.player == visit.player) { kept = visit; return; }
			}
			prisonVisits.push_back(visit);
		}
		// Within TouchReach of the door's tile (its nearest edge), or of the door itself.
		bool TouchesDoorway(const std::uint8_t* creature, std::uint8_t* room) {
			const auto* door = RoomObject(room, ObjectPrisonDoor);
			if (door == nullptr) return false;
			const auto x = Load<std::int32_t>(creature, Offset::ThingPositionX);
			const auto y = Load<std::int32_t>(creature, Offset::ThingPositionY);
			const std::int32_t left = (Load<std::int32_t>(door, Offset::ThingPositionX) >> 12) << 12;
			const std::int32_t top = (Load<std::int32_t>(door, Offset::ThingPositionY) >> 12) << 12;
			const std::int64_t ex = std::clamp(x, left, left + TileSize - 1) - static_cast<std::int64_t>(x);
			const std::int64_t ey = std::clamp(y, top, top + TileSize - 1) - static_cast<std::int64_t>(y);
			return ex * ex + ey * ey <= static_cast<std::int64_t>(TouchReach) * TouchReach || Near(creature, Load<std::int32_t>(door, Offset::ThingPositionX), Load<std::int32_t>(door, Offset::ThingPositionY), TouchReach);
		}

		// Creatures just made prisoners, looked at again a second later for the log (does the game keep them prisoners?).
		struct NewPrisoner {
			std::uint16_t tag;
			const std::uint8_t* creature;
			std::uint32_t checkTurn;
		};
		std::vector<NewPrisoner> newPrisoners;
		// Puts a creature on the cell tile nearest the doorway and makes it a prisoner of the room's owner: state 0x71 then
		// 0x73 (what the game's own way in, 0x473130, does), whose group's entry (0x47B740) has the room take it only on
		// a cell tile (0x4E5A70 with 0x454110 [verified in game: refused on the edge]); the holder is set to the owner.
		void MakePrisoner(std::uint8_t* world, std::uint8_t* creature, std::uint8_t* room) {
			const auto* door = RoomObject(room, ObjectPrisonDoor);
			const auto fromX = door != nullptr ? Load<std::int32_t>(door, Offset::ThingPositionX) : Load<std::int32_t>(creature, Offset::ThingPositionX);
			const auto fromY = door != nullptr ? Load<std::int32_t>(door, Offset::ThingPositionY) : Load<std::int32_t>(creature, Offset::ThingPositionY);
			std::int32_t cellX = 0;
			std::int32_t cellY = 0;
			if (!NearestInnerTile(world, room, fromX, fromY, cellX, cellY) || !MoveTo(world, creature, cellX, cellY)) {
				Logger::Debug("Possession: no cell tile found in Prison " + std::to_string(TagOf(room)) + " for creature " + std::to_string(TagOf(creature)) + ".");
				return;
			}
			const auto before = Load<std::int32_t>(creature, Offset::CreatureState);
			const bool checked = SetState(creature, StateImprison);
			const bool prisoner = SetState(creature, StatePrisoner);
			if (prisoner) Store<std::uint16_t>(creature, Offset::CreaturePrisonHolder, Load<std::uint16_t>(room, Offset::RoomOwner));
			Logger::Debug("Possession: creature " + std::to_string(TagOf(creature)) + " put in Prison " + std::to_string(TagOf(room)) + " at tile " + std::to_string(TileOf(cellX)) + "," + std::to_string(TileOf(cellY)) + ": state " + std::to_string(before) + ", to 0x71 " + (checked ? "yes" : "refused") + ", to 0x73 " + (prisoner ? "yes" : "refused (the Prison may be full)") + ", now state " + std::to_string(Load<std::int32_t>(creature, Offset::CreatureState)) + ".");
			newPrisoners.push_back({ TagOf(creature), creature, GameTurn(world) + TurnsPerSecond() });
		}
		void CheckNewPrisoners(std::uint32_t turn) {
			for (std::size_t i = 0; i < newPrisoners.size();) {
				const auto& kept = newPrisoners[i];
				if (turn < kept.checkTurn) { ++i; continue; }
				if (auto* creature = Same(kept.tag, kept.creature)) {
					Logger::Debug("Possession: a second later, creature " + std::to_string(kept.tag) + " is in state " + std::to_string(Load<std::int32_t>(creature, Offset::CreatureState)) + ", holder " + std::to_string(Load<std::uint16_t>(creature, Offset::CreaturePrisonHolder)) + ".");
				}
				newPrisoners.erase(newPrisoners.begin() + static_cast<std::ptrdiff_t>(i));
			}
		}
		void VisitOver(const PrisonVisit& visit, const char* why) {
			Logger::Debug("Possession: Prison " + std::to_string(visit.room) + " visit by creature " + std::to_string(visit.leader) + " over: " + why + ".");
		}
		// false when the visit is over.
		bool TickPrisonVisit(std::uint8_t* world, PrisonVisit& visit, std::uint32_t turn) {
			auto* room = Thing(visit.room);
			const auto* player = Thing(visit.player);
			auto* leader = Thing(visit.leader);
			if (room == nullptr || room[Offset::RoomType] != Game::Room::Prison || player == nullptr || !IsCreature(leader)) {
				VisitOver(visit, "the room, keeper or creature is gone");
				return false;
			}
			if (Load<std::uint16_t>(player, Offset::PlayerPossessed) != visit.leader) {
				VisitOver(visit, "the creature is no longer possessed");
				return false;
			}
			if (turn - visit.startTurn >= PrisonArmedSeconds * TurnsPerSecond()) {
				VisitOver(visit, "the doorway was not touched in time");
				return false;
			}
			// A second's grace after the bolt (it is beside the door), then the doorway takes the creature when touched.
			if (turn - visit.startTurn < TurnsPerSecond() || !TouchesDoorway(leader, room)) return true;
			// Touched: its followers are sent to this spot, then it goes in.
			const auto spotX = Load<std::int32_t>(leader, Offset::ThingPositionX);
			const auto spotY = Load<std::int32_t>(leader, Offset::ThingPositionY);
			std::vector<std::uint8_t*> followers;
			ForEachFollower(leader, [&](std::uint8_t* follower) {
				if ((Load<std::uint32_t>(follower, Offset::CreatureFlags) & FlagInHand) == 0) followers.push_back(follower);
			});
			StopPossession(world, visit.player);
			MakePrisoner(world, leader, room);
			const auto perSecond = TurnsPerSecond();
			for (auto* follower : followers) {
				StopFollowing(follower, leader);
				const bool walking = WalkTo(follower, spotX, spotY);
				prisonWalkers.push_back({ TagOf(follower), follower, visit.room, spotX, spotY, turn + PrisonFollowSeconds * perSecond, turn + perSecond });
				Logger::Debug("Possession: follower " + std::to_string(TagOf(follower)) + (walking ? " walks" : " could not set off") + " to Prison " + std::to_string(visit.room) + "'s doorway.");
			}
			Logger::Debug("Possession: creature " + std::to_string(visit.leader) + " touched Prison " + std::to_string(visit.room) + "'s doorway with " + std::to_string(followers.size()) + " follower(s).");
			return false;
		}
		void TickPrisonWalkers(std::uint8_t* world, std::uint32_t turn) {
			for (std::size_t i = 0; i < prisonWalkers.size();) {
				auto& walker = prisonWalkers[i];
				auto* creature = Same(walker.tag, walker.creature);
				auto* room = Thing(walker.room);
				bool done = creature == nullptr || room == nullptr || room[Offset::RoomType] != Game::Room::Prison;
				if (!done && (Load<std::uint32_t>(creature, Offset::CreatureFlags) & (FlagInHand | FlagPossessed)) != 0) done = true;
				if (!done && (Near(creature, walker.spotX, walker.spotY, TouchReach) || TouchesDoorway(creature, room))) {
					MakePrisoner(world, creature, room);
					done = true;
				}
				else if (!done && turn >= walker.untilTurn) {
					Logger::Debug("Possession: follower " + std::to_string(walker.tag) + " did not reach Prison " + std::to_string(walker.room) + "'s doorway in time.");
					done = true;
				}
				else if (!done && turn >= walker.nextWalkTurn && Load<std::int32_t>(creature, Offset::CreatureState) != StateWalking) {
					// Stopped on the way or taken by another job (in game: eating, its lair): sent again.
					WalkTo(creature, walker.spotX, walker.spotY);
					walker.nextWalkTurn = turn + TurnsPerSecond();
				}
				if (done) prisonWalkers.erase(prisonWalkers.begin() + static_cast<std::ptrdiff_t>(i));
				else ++i;
			}
		}

		// ---- Combat Pit, as the Prison: facing the pit's floor from its edge for half a second ends possession and puts the
		// creature on that floor as a pit fighter (the game's own pit entry, state 0x89, which also pairs fighters). Its
		// followers walk to the spot where it jumped; each that gets there (or reaches the pit's edge) is put on the floor the
		// same way. Pit fighters brought in so die rather than being knocked out (to the death) while in the pit, and get
		// their own setting back when they leave it (lifted out) [user: no more possession once in the pit; the old way set
		// everyone idle when possession ended].
		struct PitFighter {
			std::uint16_t tag;
			const std::uint8_t* creature;
			std::uint16_t room;
			bool hadDieInstantly;
		};
		std::vector<PitFighter> pitFighters;
		struct PitWalker {
			std::uint16_t tag;
			const std::uint8_t* creature;
			std::uint16_t room;
			std::int32_t spotX;
			std::int32_t spotY;
			std::uint32_t untilTurn;
			std::uint32_t nextWalkTurn;
		};
		std::vector<PitWalker> pitWalkers;
		constexpr std::uint32_t PitFollowSeconds = 30;
		// Per possessed creature: turns it has faced the floor from the pit's edge.
		struct PitStep {
			std::uint16_t player;
			std::uint32_t turns;
		};
		std::vector<PitStep> pitSteps;
		PitStep& StepOf(std::uint16_t player) {
			for (auto& step : pitSteps) if (step.player == player) return step;
			pitSteps.push_back({ player, 0 });
			return pitSteps.back();
		}

		// Puts a creature on the pit's floor (at a spot, else the floor tile nearest it) and into the pit as a fighter.
		void EnterPit(std::uint8_t* world, std::uint8_t* creature, std::uint8_t* room, const std::int32_t* spot) {
			std::int32_t floorX = 0;
			std::int32_t floorY = 0;
			if (spot != nullptr) {
				floorX = spot[0];
				floorY = spot[1];
			}
			else if (!NearestInnerTile(world, room, Load<std::int32_t>(creature, Offset::ThingPositionX), Load<std::int32_t>(creature, Offset::ThingPositionY), floorX, floorY)) {
				Logger::Debug("Possession: no floor tile found in Combat Pit " + std::to_string(TagOf(room)) + " for creature " + std::to_string(TagOf(creature)) + ".");
				return;
			}
			if (!MoveTo(world, creature, floorX, floorY)) return;
			bool tracked = false;
			for (const auto& fighter : pitFighters) tracked = tracked || (fighter.creature == creature && fighter.tag == TagOf(creature));
			if (!tracked) pitFighters.push_back({ TagOf(creature), creature, TagOf(room), DiesInstantly(creature) });
			SetDieInstantly(creature, true);
			const bool entered = SetState(creature, StatePitEnter);
			Logger::Debug("Possession: creature " + std::to_string(TagOf(creature)) + " put in Combat Pit " + std::to_string(TagOf(room)) + " at tile " + std::to_string(TileOf(floorX)) + "," + std::to_string(TileOf(floorY)) + ": pit entry " + (entered ? "yes" : "refused") + ", now state " + std::to_string(Load<std::int32_t>(creature, Offset::CreatureState)) + ".");
		}
		// The possessed creature faces the floor from the edge long enough: it jumps in, possession ends, followers set off.
		void StepAcrossPitEdge(std::uint8_t* world, std::uint8_t* player, std::uint8_t* leader, std::uint32_t turn) {
			auto& step = StepOf(TagOf(player));
			auto* room = RoomAt(world, leader);
			if (room == nullptr || room[Offset::RoomType] != Game::Room::Combat_Pit || Load<std::uint16_t>(room, Offset::RoomOwner) != Load<std::uint16_t>(leader, Offset::CreatureOwner)
				|| OnInnerTile(world, leader, room)) {
				step.turns = 0;
				return;
			}
			std::int32_t aheadX = 0;
			std::int32_t aheadY = 0;
			Ahead(leader, AheadDistance, aheadX, aheadY);
			if (!InnerTile(world, TileOf(aheadX), TileOf(aheadY), room)) {
				step.turns = 0;
				return;
			}
			const auto perSecond = TurnsPerSecond();
			if (++step.turns < (perSecond + 1) / 2) return;
			step.turns = 0;
			const auto spotX = Load<std::int32_t>(leader, Offset::ThingPositionX);
			const auto spotY = Load<std::int32_t>(leader, Offset::ThingPositionY);
			std::vector<std::uint8_t*> followers;
			ForEachFollower(leader, [&](std::uint8_t* follower) {
				if ((Load<std::uint32_t>(follower, Offset::CreatureFlags) & FlagInHand) == 0) followers.push_back(follower);
			});
			StopPossession(world, TagOf(player));
			const std::int32_t landing[2] = { (TileOf(aheadX) << 12) + TileSize / 2, (TileOf(aheadY) << 12) + TileSize / 2 };
			EnterPit(world, leader, room, landing);
			for (auto* follower : followers) {
				StopFollowing(follower, leader);
				const bool walking = WalkTo(follower, spotX, spotY);
				pitWalkers.push_back({ TagOf(follower), follower, TagOf(room), spotX, spotY, turn + PitFollowSeconds * perSecond, turn + perSecond });
				Logger::Debug("Possession: follower " + std::to_string(TagOf(follower)) + (walking ? " walks" : " could not set off") + " to Combat Pit " + std::to_string(TagOf(room)) + "'s edge.");
			}
			Logger::Debug("Possession: creature " + std::to_string(TagOf(leader)) + " jumped into Combat Pit " + std::to_string(TagOf(room)) + " with " + std::to_string(followers.size()) + " follower(s) to come.");
		}
		void TickPits(std::uint8_t* world, std::uint32_t turn) {
			// Followers on their way: in at the jump spot or the pit's edge.
			for (std::size_t i = 0; i < pitWalkers.size();) {
				auto& walker = pitWalkers[i];
				auto* creature = Same(walker.tag, walker.creature);
				auto* room = Thing(walker.room);
				bool done = creature == nullptr || room == nullptr || room[Offset::RoomType] != Game::Room::Combat_Pit;
				if (!done && (Load<std::uint32_t>(creature, Offset::CreatureFlags) & (FlagInHand | FlagPossessed)) != 0) done = true;
				if (!done && (Near(creature, walker.spotX, walker.spotY, TouchReach) || RoomAt(world, creature) == room)) {
					EnterPit(world, creature, room, nullptr);
					done = true;
				}
				else if (!done && turn >= walker.untilTurn) {
					Logger::Debug("Possession: follower " + std::to_string(walker.tag) + " did not reach Combat Pit " + std::to_string(walker.room) + " in time.");
					done = true;
				}
				else if (!done && turn >= walker.nextWalkTurn && Load<std::int32_t>(creature, Offset::CreatureState) != StateWalking) {
					WalkTo(creature, walker.spotX, walker.spotY);
					walker.nextWalkTurn = turn + TurnsPerSecond();
				}
				if (done) pitWalkers.erase(pitWalkers.begin() + static_cast<std::ptrdiff_t>(i));
				else ++i;
			}
			// Fighters brought in: dead ones leave the list; ones out of the pit (lifted out) get their own setting back.
			for (std::size_t i = 0; i < pitFighters.size();) {
				const auto& fighter = pitFighters[i];
				auto* creature = Same(fighter.tag, fighter.creature);
				if (creature != nullptr && RoomAt(world, creature) == Thing(fighter.room)) {
					++i;
					continue;
				}
				if (creature != nullptr) SetDieInstantly(creature, fighter.hadDieInstantly);
				pitFighters.erase(pitFighters.begin() + static_cast<std::ptrdiff_t>(i));
			}
			// Possessed creatures at a pit's edge.
			int guard = 0;
			for (auto* player = Thing(Load<std::uint16_t>(world, Offset::WorldPlayers)); player != nullptr && guard < ListLimit; player = Thing(Load<std::uint16_t>(player, Offset::PlayerNext)), ++guard) {
				auto* leader = Thing(Load<std::uint16_t>(player, Offset::PlayerPossessed));
				if (IsCreature(leader)) StepAcrossPitEdge(world, player, leader, turn);
			}
		}

		// ---- What the possessed creature can aim at. The game's search for the thing in front keeps only some objects, so
		// beds, the Training Target and the Prison Bolt were never found (or highlighted). These are kept too, when in reach:
		// the Training Target, the creature's own bed, and the bolt of its keeper's Prison.
		bool AimableExtra(std::uint8_t* world, std::uint8_t* creature, std::uint8_t* thing) {
			const auto type = thing[Offset::ObjectType];
			if (type == ObjectTrainingTarget) return true;
			if (TagOf(thing) == Load<std::uint16_t>(creature, Offset::CreatureBed)) {
				const auto* data = Load<const std::uint8_t*>(creature, Offset::CreatureData);
				return data != nullptr && Load<std::int32_t>(data, Offset::CreatureDataSleepLength) != 0;
			}
			if (type == ObjectPrisonDoorBar) {
				const auto ownerTag = Load<std::uint16_t>(creature, Offset::CreatureOwner);
				const auto* room = PrisonOfDoor(world, thing, Thing(ownerTag));
				return room != nullptr && Load<std::uint16_t>(room, Offset::RoomOwner) == ownerTag;
			}
			return false;
		}
		std::int32_t __fastcall TargetFilterDetour(std::uint8_t* world, void* unused, std::uint8_t* creature, std::uint8_t* thing, std::int32_t flag) {
			const auto result = originalTargetFilter(world, unused, creature, thing, flag);
			if (result != 0 || !IsCreature(creature) || !IsKind(thing, KindObject)) return result;
			if ((Load<std::uint32_t>(creature, Offset::CreatureFlags) & FlagPossessed) == 0 || !AimableExtra(world, creature, thing)) return result;
			// The game's reach test wants the two things almost touching (0.3 of their sizes), which a bed or a door the
			// creature cannot walk into does not allow. Ours: within AimReach across of the thing, or (a Prison door, whose
			// place in its wall tile is not known) within a tile of its tile's nearest edge. The search then checks the
			// creature faces it (0x516950) and keeps the nearest.
			const auto cx = Load<std::int32_t>(creature, Offset::ThingPositionX);
			const auto cy = Load<std::int32_t>(creature, Offset::ThingPositionY);
			const auto tx = Load<std::int32_t>(thing, Offset::ThingPositionX);
			const auto ty = Load<std::int32_t>(thing, Offset::ThingPositionY);
			const std::int64_t dx = tx - cx;
			const std::int64_t dy = ty - cy;
			bool inReach = dx * dx + dy * dy <= static_cast<std::int64_t>(AimReach) * AimReach;
			std::int64_t ex = 0;
			std::int64_t ey = 0;
			if (thing[Offset::ObjectType] == ObjectPrisonDoorBar) {
				const std::int32_t left = (tx >> 12) << 12;
				const std::int32_t top = (ty >> 12) << 12;
				ex = std::clamp(cx, left, left + 0xFFF) - static_cast<std::int64_t>(cx);
				ey = std::clamp(cy, top, top + 0xFFF) - static_cast<std::int64_t>(cy);
				inReach = inReach || ex * ex + ey * ey <= static_cast<std::int64_t>(DoorReach) * DoorReach;
			}
			return inReach ? 1 : 0;
		}

		// ---- The possessed creature's melee. The game's own handler finds what is in front (world vtable + 0x334) only
		// when the melee is ready; this asks the same, with the same values, before it runs.
		// ---- The Prison door and its bolt. The search's later tests (facing within 0x155 of the view, 0x516950, and the
		// view line, 0x516670) are made for things the creature can walk up to; a door in the Prison's fence stays a tile
		// or more away, so it was in reach but never chosen [verified in game: log]. When the search finds nothing, the
		// door or bolt of the creature's keeper's Prison is taken if it is within reach (DoorReach of its tile, or AimReach
		// of itself) and within the same 0x155 of where the creature looks, the nearest first. The interface highlights
		// what the search gives, so it is highlighted too.
		using ThingInFrontFn2 = std::uint32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint8_t* creature, std::int32_t* position, std::uint16_t* angles);
		using AngleFn = std::uint32_t(__cdecl*)(std::int32_t dx, std::int32_t dy);
		ThingInFrontFn2 originalThingInFront = nullptr;
		constexpr std::int32_t DoorFacing = 0x155;
		std::uint8_t* PrisonDoorInFront(std::uint8_t* creature, const std::int32_t* position, const std::uint16_t* angles) {
			const auto ownerTag = Load<std::uint16_t>(creature, Offset::CreatureOwner);
			const auto* keeper = Thing(ownerTag);
			if (keeper == nullptr) return nullptr;
			const auto cx = position[0];
			const auto cy = position[1];
			const auto heading = static_cast<std::int32_t>(angles[0] & 0x7FF);
			std::uint8_t* best = nullptr;
			std::int64_t bestDistance = 0;
			int guard = 0;
			for (auto* room = Thing(Load<std::uint16_t>(keeper, Offset::PlayerFirstRoom)); room != nullptr && guard < ListLimit; room = Thing(Load<std::uint16_t>(room, Offset::RoomNextOwned)), ++guard) {
				if (room[Offset::RoomType] != Game::Room::Prison) continue;
				int inner = 0;
				for (auto* thing = Thing(Load<std::uint16_t>(room, Offset::RoomFirstThing)); thing != nullptr && inner < ListLimit; thing = Thing(Load<std::uint16_t>(thing, Offset::ThingNextInRoom)), ++inner) {
					if (!IsKind(thing, KindObject)) continue;
					const auto type = thing[Offset::ObjectType];
					if (type != ObjectPrisonDoorBar) continue;
					const auto tx = Load<std::int32_t>(thing, Offset::ThingPositionX);
					const auto ty = Load<std::int32_t>(thing, Offset::ThingPositionY);
					const std::int64_t dx = tx - cx;
					const std::int64_t dy = ty - cy;
					const auto distance = dx * dx + dy * dy;
					const std::int32_t left = (tx >> 12) << 12;
					const std::int32_t top = (ty >> 12) << 12;
					const std::int64_t ex = std::clamp(cx, left, left + 0xFFF) - static_cast<std::int64_t>(cx);
					const std::int64_t ey = std::clamp(cy, top, top + 0xFFF) - static_cast<std::int64_t>(cy);
					const bool inReach = distance <= static_cast<std::int64_t>(AimReach) * AimReach || ex * ex + ey * ey <= static_cast<std::int64_t>(DoorReach) * DoorReach;
					if (!inReach) continue;
					auto angle = static_cast<std::int32_t>(GameFunction<AngleFn>(Address::GameAngle)(static_cast<std::int32_t>(dx), static_cast<std::int32_t>(dy)) & 0x7FF);
					if (heading - angle > 0x400) angle += 0x800;
					if (angle - heading > 0x400) angle -= 0x800;
					if (angle <= heading - DoorFacing || angle >= heading + DoorFacing) continue;
					if (best == nullptr || distance < bestDistance) {
						best = thing;
						bestDistance = distance;
					}
				}
			}
			return best;
		}
		std::uint32_t __fastcall ThingInFrontDetour(std::uint8_t* world, void* unused, std::uint8_t* creature, std::int32_t* position, std::uint16_t* angles) {
			const auto result = originalThingInFront(world, unused, creature, position, angles);
			if ((result & 0xFFFF) != 0 || !IsCreature(creature) || position == nullptr || angles == nullptr) return result;
			if ((Load<std::uint32_t>(creature, Offset::CreatureFlags) & FlagPossessed) == 0) return result;
			auto* door = PrisonDoorInFront(creature, position, angles);
			if (door == nullptr) return result;
			return (result & 0xFFFF0000u) | TagOf(door);
		}

		// ---- Ranged aim at the Training Target. The possessed creature's ranged abilities aim through their own search
		// (world vtable + 0x338, 0x515520: thiscall (world, creature, position, angles, flag), ret 0x10), which never picks
		// the Training Target, so a shot was neither aimed at it nor highlighted [user: melee works, ranged not even
		// highlighted]. When that search finds nothing, a Training Target of one of the keeper's Training Rooms within
		// RangedReach and within RangedFacing of where the creature looks is given (the one nearest the line of aim). The
		// ability then aims at it, and the shot's flight finds it (0x4ABE54, where the experience is given).
		using RangedInFrontFn = std::uint32_t(__fastcall*)(std::uint8_t* world, void* unused, std::uint8_t* creature, std::int32_t* position, std::uint16_t* angles, std::int32_t flag);
		RangedInFrontFn originalRangedInFront = nullptr;
		constexpr std::int32_t RangedReach = 10 * 0x1000;
		constexpr std::int32_t RangedFacing = 0xAA;
		std::uint8_t* TrainingTargetInView(std::uint8_t* creature, const std::int32_t* position, const std::uint16_t* angles) {
			const auto* keeper = Thing(Load<std::uint16_t>(creature, Offset::CreatureOwner));
			if (keeper == nullptr) return nullptr;
			const auto heading = static_cast<std::int32_t>(angles[0] & 0x7FF);
			std::uint8_t* best = nullptr;
			std::int32_t bestOff = 0;
			int guard = 0;
			for (auto* room = Thing(Load<std::uint16_t>(keeper, Offset::PlayerFirstRoom)); room != nullptr && guard < ListLimit; room = Thing(Load<std::uint16_t>(room, Offset::RoomNextOwned)), ++guard) {
				if (room[Offset::RoomType] != Game::Room::Training_Room) continue;
				int inner = 0;
				for (auto* thing = Thing(Load<std::uint16_t>(room, Offset::RoomFirstThing)); thing != nullptr && inner < ListLimit; thing = Thing(Load<std::uint16_t>(thing, Offset::ThingNextInRoom)), ++inner) {
					if (!IsKind(thing, KindObject) || thing[Offset::ObjectType] != ObjectTrainingTarget) continue;
					const std::int64_t dx = Load<std::int32_t>(thing, Offset::ThingPositionX) - position[0];
					const std::int64_t dy = Load<std::int32_t>(thing, Offset::ThingPositionY) - position[1];
					if (dx * dx + dy * dy > static_cast<std::int64_t>(RangedReach) * RangedReach) continue;
					auto angle = static_cast<std::int32_t>(GameFunction<AngleFn>(Address::GameAngle)(static_cast<std::int32_t>(dx), static_cast<std::int32_t>(dy)) & 0x7FF);
					if (heading - angle > 0x400) angle += 0x800;
					if (angle - heading > 0x400) angle -= 0x800;
					const auto off = angle > heading ? angle - heading : heading - angle;
					if (off >= RangedFacing) continue;
					if (best == nullptr || off < bestOff) {
						best = thing;
						bestOff = off;
					}
				}
			}
			return best;
		}
		std::uint32_t __fastcall RangedInFrontDetour(std::uint8_t* world, void* unused, std::uint8_t* creature, std::int32_t* position, std::uint16_t* angles, std::int32_t flag) {
			const auto result = originalRangedInFront(world, unused, creature, position, angles, flag);
			if ((result & 0xFFFF) != 0 || !IsCreature(creature) || position == nullptr || angles == nullptr) return result;
			if ((Load<std::uint32_t>(creature, Offset::CreatureFlags) & FlagPossessed) == 0) return result;
			auto* target = TrainingTargetInView(creature, position, angles);
			return target != nullptr ? (result & 0xFFFF0000u) | TagOf(target) : result;
		}

		std::int32_t __fastcall PossessedAttackDetour(std::uint8_t* world, void* unused, std::uint32_t* action) {
			if ((action[2] & ActionMeleeBit) == 0) return originalPossessedAttack(world, unused, action);
			const auto playerTag = *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(action) + Offset::ActionPlayer);
			auto* player = Thing(playerTag);
			auto* creature = player != nullptr ? Thing(Load<std::uint16_t>(player, Offset::PlayerPossessed)) : nullptr;
			auto* data = IsCreature(creature) ? Load<std::uint8_t*>(creature, Offset::CreatureData) : nullptr;
			if (data == nullptr || !GameFunction<MeleeReadyFn>(Address::CreatureMeleeReady)(creature, nullptr, data)) return originalPossessedAttack(world, unused, action);
			std::uint32_t position[3] = { action[0] & 0xFFFFF, action[1] & 0xFFFFF, action[2] & 0x3FFF };
			std::uint16_t angles[3] = { static_cast<std::uint16_t>((action[0] >> 20) & 0x7FF), static_cast<std::uint16_t>((action[1] >> 20) & 0x7FF), static_cast<std::uint16_t>((action[2] >> 14) & 0x7FF) };
			const auto targetTag = static_cast<std::uint16_t>(WorldMethod<ThingInFrontFn>(world, Offset::WorldThingInFront)(world, nullptr, creature, position, angles));
			auto* target = Thing(targetTag);
			if (!IsKind(target, KindObject)) return originalPossessedAttack(world, unused, action);
			const auto type = target[Offset::ObjectType];
			Logger::Debug("Possession: creature " + std::to_string(TagOf(creature)) + " hit object " + std::to_string(targetTag) + " (type " + std::to_string(type) + ").");
			if (type == ObjectChicken) {
				// The game eats a chicken only when hungry (or a Bile Demon); otherwise the chicken just hops.
				if (Load<std::int32_t>(creature, Offset::CreatureHunger) == 0 && creature[Offset::CreatureType] != Game::Creature::Bile_Demon) Store<std::int32_t>(creature, Offset::CreatureHunger, 1);
				return originalPossessedAttack(world, unused, action);
			}
			if (type == ObjectTrainingDummy || type == ObjectTrainingTarget) {
				const auto result = originalPossessedAttack(world, unused, action);
				GiveTrainingExperience(world, creature);
				return result;
			}
			if (targetTag == Load<std::uint16_t>(creature, Offset::CreatureBed) && Load<std::int32_t>(data, Offset::CreatureDataSleepLength) != 0) {
				SendToBed(world, playerTag, creature, data);
				return 1;
			}
			if (type == ObjectPrisonDoorBar || type == ObjectPrisonDoor) {
				const auto ownerTag = Load<std::uint16_t>(creature, Offset::CreatureOwner);
				auto* room = PrisonOfDoor(world, target, Thing(ownerTag));
				if (room != nullptr && Load<std::uint16_t>(room, Offset::RoomOwner) == ownerTag) {
					OpenPrisonDoor(world, room, player, creature);
					return 1;
				}
				Logger::Debug("Possession: Prison door " + std::to_string(targetTag) + " hit, but it is not in a Prison of the creature's keeper" + (room != nullptr ? " (the room's owner differs)." : " (no Prison holds it)."));
			}
			return originalPossessedAttack(world, unused, action);
		}

		void OnLevelStart() {
			prisonVisits.clear();
			pitFighters.clear();
			pitWalkers.clear();
			pitSteps.clear();
			prisonWalkers.clear();
			newPrisoners.clear();
			rewardedShots.clear();
			trainingHits.clear();
		}
		bool installed = false;

		struct Expected {
			std::uintptr_t address;
			std::vector<std::uint8_t> bytes;
		};
		bool GameCodeAsExpected() {
			const Expected expected[] = {
				{ Address::PossessedAttackAction, { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x90, 0xD4, 0x64, 0x00 } },
				{ Address::StopPossessionAction, { 0x8B, 0x4C, 0x24, 0x04, 0x33, 0xC0, 0x55, 0x66, 0x8B, 0x41, 0x10 } },
				{ Address::CreatureMeleeReady, { 0x51, 0x56, 0x8B, 0xF1, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x8B, 0x8E, 0x27, 0x04, 0x00, 0x00 } },
				{ Address::CreatureAddExperienceCapped, { 0x51, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x14, 0x85, 0xF6 } },
				{ Address::StateBlockSetState, { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57 } },
				{ Address::StateBlockGoToBed, { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x08, 0x66, 0x83, 0xB8, 0xCB, 0x03, 0x00, 0x00, 0x00 } },
				{ Address::StateBlockFollow, { 0x56, 0x57, 0x66, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1, 0x66, 0x85, 0xFF } },
				{ Address::StateBlockWalkTo, { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x57, 0x8B, 0xF9, 0x55 } },
				{ Address::PrisonToggleDoor, { 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xC0, 0x66, 0x8B, 0x47, 0x0E } },
				{ Address::PossessedTargetFilter, { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x66, 0x8B, 0x47, 0x28 } },
				{ Address::PossessedThingInFront, { 0x83, 0xEC, 0x14, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x24, 0x56, 0x57 } },
				{ Address::PossessedRangedInFront, { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xB3, 0xD4, 0x64, 0x00 } },
				{ Address::GameAngle, { 0x83, 0xEC, 0x04, 0x53, 0x52, 0x8B, 0x44, 0x24, 0x10 } },
				{ Address::PossessedTargetReach, { 0x55, 0x8B, 0xEC, 0x51, 0x53, 0x8B, 0x5D, 0x0C } },
				{ Address::PlayerTakeGold, { 0x83, 0xEC, 0x1C, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x28, 0x8B, 0xD9 } },
				{ Address::ShowMoneyText, { 0x8B, 0x41, 0x04, 0x8B, 0x48, 0x10 } },
				{ Address::PlayerEvent, { 0x81, 0xEC, 0x0C, 0x04, 0x00, 0x00, 0x53, 0x55 } },
				{ Address::ObjectSetAnimation, { 0x8B, 0x44, 0x24, 0x04, 0xC7, 0x81, 0x3E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x81, 0x3A, 0x01, 0x00, 0x00 } },
			};
			for (const auto& each : expected) {
				const auto address = Game::Resolve(each.address);
				// Functions we only call may already carry another module's hook (a jump, e.g. LevelEditor's on 0x50FB70);
				// calling them through it is what the game does too. The two we hook ourselves must be the game's own bytes.
				const bool ours = each.address == Address::PossessedAttackAction || each.address == Address::PossessedTargetFilter || each.address == Address::PossessedThingInFront || each.address == Address::PossessedRangedInFront;
				if (!ours && Memory::Read<std::uint8_t>(address) == 0xE9) continue;
				if (!Memory::Matches(address, each.bytes)) {
					Logger::Warning(std::format("Possession actions: unexpected game code at {:#x}.", each.address));
					return false;
				}
			}
			return true;
		}
	}

	void PossessionActions::Install() {
		if (!GameCodeAsExpected()) {
			Logger::Warning("Possession actions: not installed (the game's code is not as expected).");
			return;
		}
		const auto attack = Game::Resolve(Address::PossessedAttackAction);
		if (!HookManager::CreateHook(attack, reinterpret_cast<void*>(&PossessedAttackDetour), reinterpret_cast<void**>(&originalPossessedAttack)) || !HookManager::EnableHook(attack)) {
			HookManager::RemoveHook(attack);
			Logger::Warning("Possession actions: could not hook the possessed creature's attack.");
			return;
		}
		const auto inFront = Game::Resolve(Address::PossessedThingInFront);
		if (!HookManager::CreateHook(inFront, reinterpret_cast<void*>(&ThingInFrontDetour), reinterpret_cast<void**>(&originalThingInFront)) || !HookManager::EnableHook(inFront)) {
			HookManager::RemoveHook(inFront);
			Logger::Warning("Possession actions: could not hook the thing-in-front search; the Prison door cannot be hit.");
		}
		const auto ranged = Game::Resolve(Address::PossessedRangedInFront);
		if (!HookManager::CreateHook(ranged, reinterpret_cast<void*>(&RangedInFrontDetour), reinterpret_cast<void**>(&originalRangedInFront)) || !HookManager::EnableHook(ranged)) {
			HookManager::RemoveHook(ranged);
			Logger::Warning("Possession actions: could not hook the ranged aim; ranged abilities cannot aim at the Training Target.");
		}
		const auto filter = Game::Resolve(Address::PossessedTargetFilter);
		if (!HookManager::CreateHook(filter, reinterpret_cast<void*>(&TargetFilterDetour), reinterpret_cast<void**>(&originalTargetFilter)) || !HookManager::EnableHook(filter)) {
			HookManager::RemoveHook(filter);
			Logger::Warning("Possession actions: could not hook the possessed creature's aim; beds, the Training Target and Prison doors cannot be hit.");
		}
		if (!InstallRangedTraining()) Logger::Warning("Possession actions: unexpected game code, ranged shots at a Training Target give no experience.");
		SessionOptions::OnLevelStart(&OnLevelStart);
		installed = true;
		Logger::Debug("Possession actions: training hits, bed, chickens, Prison door and Combat Pit are on.");
	}

	void PossessionActions::AfterWorldTick(std::uint8_t* world) {
		if (!installed || world == nullptr) return;
		const auto turn = GameTurn(world);
		for (std::size_t i = 0; i < prisonVisits.size();) {
			if (TickPrisonVisit(world, prisonVisits[i], turn)) ++i;
			else prisonVisits.erase(prisonVisits.begin() + static_cast<std::ptrdiff_t>(i));
		}
		TickPrisonWalkers(world, turn);
		TickPits(world, turn);
		CheckNewPrisoners(turn);
	}
}
