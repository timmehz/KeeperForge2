#include "ModApiHost.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "PatternScanner.h"
#include "SessionOptions.h"
#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		template<typename T> T Load(const std::uint8_t* at) {
			T value{};
			std::memcpy(&value, at, sizeof(T));
			return value;
		}
		std::uint8_t* World() { return *reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::WorldPointer)); }
		// A thing by its tag (players, creatures and rooms are all things), or null.
		std::uint8_t* Thing(std::uint16_t tag) {
			tag &= Game::TagMask;
			if (tag == 0) return nullptr;
			return reinterpret_cast<std::uint8_t* const*>(Game::Resolve(Game::Address::ThingTable))[tag];
		}
		template<typename T> T WorldMethod(const std::uint8_t* world, std::uintptr_t slot) {
			return reinterpret_cast<T>((*reinterpret_cast<const std::uintptr_t* const*>(world))[slot / sizeof(std::uintptr_t)]);
		}
		using WorldIntFn = std::int32_t(__fastcall*)(const std::uint8_t* world);
		using TileAtFn = std::uint8_t*(__fastcall*)(std::uint8_t* world, void* unused, std::int32_t x, std::int32_t y);
		using GameTurnFn = std::uint32_t(__fastcall*)(const std::uint8_t* world);
		// The first player in the world's list, and the next.
		std::uint8_t* FirstPlayer(const std::uint8_t* world) { return Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); }
		std::uint8_t* NextPlayer(const std::uint8_t* player) { return Thing(Load<std::uint16_t>(player + Game::Offset::PlayerNext)); }
		std::uint8_t PlayerNumberOfTag(std::uint16_t tag) {
			const auto* player = Thing(tag);
			return player != nullptr ? player[Game::Offset::PlayerNumber] : std::uint8_t{ 0 };
		}
		// A thing's tag is at + 4; ThingKind (0 a creature) is also which world list it is on (9 none: a removed creature in
		// the pool, still marked in use).
		constexpr std::uintptr_t ThingTag = 0x4;
		void FillCreature(const std::uint8_t* creature, ModCreature& out) {
			out.tag = Load<std::uint16_t>(creature + ThingTag);
			out.type = creature[Game::Offset::CreatureType];
			out.level = creature[Game::Offset::CreatureLevel];
			out.owner = PlayerNumberOfTag(Load<std::uint16_t>(creature + Game::Offset::CreatureOwner));
			out.health = Load<std::int32_t>(creature + Game::Offset::CreatureHealth);
			out.x = Load<std::int32_t>(creature + Game::Offset::ThingX);
			out.y = Load<std::int32_t>(creature + Game::Offset::ThingY);
		}

		// ---- The shared random number. At each game turn (and level start) it starts from the game's synced seed and the
		// turn, so it is the same on every machine; it never steps the game's own seed.
		std::uint32_t randomState = 0;
		void StartRandom() {
			const auto* world = World();
			if (world == nullptr) return;
			const auto seed = Load<std::uint32_t>(world + Game::Offset::WorldGameSeed);
			const auto turn = reinterpret_cast<GameTurnFn>(Game::Resolve(Game::Address::WorldGameTurn))(world);
			randomState = seed ^ (turn * 0x9E3779B9u) ^ 0x4B463252u;
		}
		std::uint32_t NextRandom() {
			randomState += 0x9E3779B9u;
			std::uint32_t z = randomState;
			z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
			z = (z ^ (z >> 13)) * 0xC2B2AE35u;
			return z ^ (z >> 16);
		}

		class GameData final : public IModGame {
			public:
				bool InLevel() const override { return World() != nullptr; }
				std::int32_t GetPlayMode() const override { return Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode)); }
				std::uint32_t GetGameTurn() const override {
					const auto* world = World();
					return world != nullptr ? reinterpret_cast<GameTurnFn>(Game::Resolve(Game::Address::WorldGameTurn))(world) : 0;
				}
				std::uint8_t GetLocalPlayer() const override {
					return World() != nullptr ? Memory::Read<std::uint8_t>(Game::Resolve(Game::Address::LocalPlayerNumber)) : std::uint8_t{ 0 };
				}
				bool GetPlayer(std::uint8_t number, ModPlayer& out) const override {
					const auto* world = World();
					if (world == nullptr) return false;
					for (const auto* player = FirstPlayer(world); player != nullptr; player = NextPlayer(player)) {
						if (player[Game::Offset::PlayerNumber] != number) continue;
						out.number = number;
						out.mana = Load<std::int32_t>(player + Game::Offset::PlayerMana);
						out.heartX = Load<std::uint16_t>(player + Game::Offset::PlayerHeartX);
						out.heartY = Load<std::uint16_t>(player + Game::Offset::PlayerHeartY);
						return true;
					}
					return false;
				}
				std::uint32_t GetMapWidth() const override {
					const auto* world = World();
					return world != nullptr ? static_cast<std::uint32_t>(WorldMethod<WorldIntFn>(world, Game::Offset::WorldMapWidth)(world)) : 0;
				}
				std::uint32_t GetMapHeight() const override {
					const auto* world = World();
					return world != nullptr ? static_cast<std::uint32_t>(WorldMethod<WorldIntFn>(world, Game::Offset::WorldMapHeight)(world)) : 0;
				}
				bool GetTile(std::uint32_t x, std::uint32_t y, ModTile& out) const override {
					auto* world = World();
					if (world == nullptr || x >= GetMapWidth() || y >= GetMapHeight()) return false;
					const auto* tile = WorldMethod<TileAtFn>(world, Game::Offset::WorldTileAt)(world, nullptr, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
					if (tile == nullptr) return false;
					out.terrain = tile[Game::Offset::TileTerrain];
					out.owner = PlayerNumberOfTag(Load<std::uint16_t>(tile + Game::Offset::TileOwner));
					const auto* room = Thing(Load<std::uint16_t>(tile + Game::Offset::TileRoom));
					out.roomType = room != nullptr ? room[Game::Offset::RoomType] : std::uint8_t{ 0 };
					return true;
				}
				void ForEachCreature(std::uint8_t owner, ModCreatureFn callback, void* context) const override {
					const auto* world = World();
					if (world == nullptr || callback == nullptr) return;
					for (const auto* player = FirstPlayer(world); player != nullptr; player = NextPlayer(player)) {
						if (owner != 0 && player[Game::Offset::PlayerNumber] != owner) continue;
						for (const auto* creature = Thing(Load<std::uint16_t>(player + Game::Offset::PlayerFirstCreature)); creature != nullptr;
							creature = Thing(Load<std::uint16_t>(creature + Game::Offset::CreatureNextOwned))) {
							ModCreature found;
							FillCreature(creature, found);
							callback(found, context);
						}
					}
				}
				bool GetCreature(std::uint16_t tag, ModCreature& out) const override {
					if (World() == nullptr || tag == 0 || tag > Game::TagMask) return false;
					if (Memory::Read<std::uint8_t>(Game::Resolve(Game::Address::ThingInUse) + tag) == 0) return false;
					const auto* thing = Thing(tag);
					if (thing == nullptr || Load<std::int32_t>(thing + Game::Offset::ThingKind) != 0) return false;
					FillCreature(thing, out);
					return true;
				}
				std::uint32_t Random(std::uint32_t bound) override { return bound == 0 ? 0 : NextRandom() % bound; }
		};
		GameData gameData;

		class ModApiImpl;
		struct Callback {
			ModApiImpl* owner = nullptr;
			ModEventFn function = nullptr;
			void* context = nullptr;
		};
		std::vector<Callback> levelStartCallbacks;
		std::vector<Callback> gameTurnCallbacks;

		class ModLog final : public IModLog {
			public:
				std::string prefix;
				void Trace(const char* message) override { Logger::Trace(prefix + Text(message)); }
				void Debug(const char* message) override { Logger::Debug(prefix + Text(message)); }
				void Info(const char* message) override { Logger::Info(prefix + Text(message)); }
				void Warning(const char* message) override { Logger::Warning(prefix + Text(message)); }
				void Error(const char* message) override { Logger::ErrorAndContinue(prefix + Text(message)); }
			private:
				static std::string Text(const char* message) { return message != nullptr ? message : ""; }
		};

		class ModHooks final : public IModHooks {
			public:
				std::string modName;
				std::vector<std::uintptr_t> targets;
				std::uintptr_t FindPattern(const char* pattern) override {
					if (pattern == nullptr) return 0;
					const auto found = PatternScanner::FindUnique(pattern);
					if (found == 0) Logger::Debug("Mod API: " + modName + "'s pattern \"" + pattern + "\" matches no single place in the game.");
					return found;
				}
				bool Hook(std::uintptr_t target, void* detour, void** original) override {
					const auto base = Memory::GetModuleBase();
					if (target < base || target >= base + Memory::GetModuleSize()) {
						Logger::Warning("Mod API: " + modName + " asked to hook a place outside the game's exe; only the game's own code can be hooked.");
						return false;
					}
					const auto where = " at game offset 0x" + Hex(target - base);
					if (!HookManager::CreateHook(target, detour, original)) {
						Logger::Warning("Mod API: " + modName + " could not hook" + where + " (already hooked by KeeperForge2 or a mod, or not code that can be hooked).");
						return false;
					}
					if (!HookManager::EnableHook(target)) {
						HookManager::RemoveHook(target);
						Logger::Warning("Mod API: " + modName + " could not turn on its hook" + where + ".");
						return false;
					}
					targets.push_back(target);
					Logger::Debug("Mod API: " + modName + " hooked" + where + ".");
					return true;
				}
				bool Unhook(std::uintptr_t target) override {
					const auto place = std::ranges::find(targets, target);
					if (place == targets.end()) return false;
					targets.erase(place);
					return HookManager::RemoveHook(target);
				}
				void UnhookAll() {
					for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
						if (!HookManager::RemoveHook(*it)) Logger::Warning("Mod API: could not take back " + modName + "'s hook at game offset 0x" + Hex(*it - Memory::GetModuleBase()) + ".");
					}
					targets.clear();
				}
			private:
				static std::string Hex(std::uintptr_t value) {
					constexpr const char* Digits = "0123456789ABCDEF";
					std::string text;
					do {
						text.insert(text.begin(), Digits[value & 15]);
						value >>= 4;
					} while (value != 0);
					return text;
				}
		};

		class ModEvents final : public IModEvents {
			public:
				ModApiImpl* owner = nullptr;
				void OnLevelStart(ModEventFn callback, void* context) override {
					if (callback != nullptr) levelStartCallbacks.push_back({ owner, callback, context });
				}
				void OnGameTurn(ModEventFn callback, void* context) override {
					if (callback != nullptr) gameTurnCallbacks.push_back({ owner, callback, context });
				}
		};

		class ModApiImpl final : public IModApi {
			public:
				std::string modName;
				ModLog log;
				ModHooks hooks;
				ModEvents events;
				explicit ModApiImpl(const std::string& name) : modName(name) {
					log.prefix = name + ": ";
					hooks.modName = name;
					events.owner = this;
				}
				IModLog& Log() override { return log; }
				IModHooks& Hooks() override { return hooks; }
				IModEvents& Events() override { return events; }
				IModGame& Game() override { return gameData; }
				const char* GetVersion() const override { return KEEPERFORGE2_VERSION; }
		};
		std::vector<std::unique_ptr<ModApiImpl>> apis;

		// A mod's callback; what it throws is logged and the game goes on.
		void Run(const Callback& callback, const char* what) {
			try {
				callback.function(callback.context);
			}
			catch (const std::exception& error) {
				Logger::ErrorAndContinue("Mod API: " + callback.owner->modName + "'s " + what + " callback failed: " + error.what());
			}
			catch (...) {
				Logger::ErrorAndContinue("Mod API: " + callback.owner->modName + "'s " + what + " callback failed.");
			}
		}
		void OnLevelStart() {
			StartRandom();
			// A copy: a callback may register another.
			const auto callbacks = levelStartCallbacks;
			for (const auto& callback : callbacks) Run(callback, "level start");
		}
	}

	void ModApiHost::Install() { SessionOptions::OnLevelStart(&OnLevelStart); }

	IModApi& ModApiHost::Create(const std::string& modName) {
		apis.push_back(std::make_unique<ModApiImpl>(modName));
		return *apis.back();
	}

	void ModApiHost::Release(IModApi& api) {
		const auto place = std::ranges::find_if(apis, [&api](const auto& kept) { return kept.get() == &api; });
		if (place == apis.end()) return;
		auto* impl = place->get();
		impl->hooks.UnhookAll();
		std::erase_if(levelStartCallbacks, [impl](const Callback& callback) { return callback.owner == impl; });
		std::erase_if(gameTurnCallbacks, [impl](const Callback& callback) { return callback.owner == impl; });
		apis.erase(place);
	}

	void ModApiHost::BeforeGameTurn() {
		StartRandom();
		if (gameTurnCallbacks.empty()) return;
		const auto callbacks = gameTurnCallbacks;
		for (const auto& callback : callbacks) Run(callback, "game turn");
	}
}
