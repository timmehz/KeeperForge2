#pragma once
#include <cstddef>
#include <cstdint>
// What KeeperForge2 offers a mod beyond content (part of the mod SDK, with IMod.h and ModContent.h): its log, hooks of
// the game's functions, events and the game's state. A mod DLL that exports
//   KEEPERFORGE2_MOD_EXPORT void KeeperForge2_UseApi(KeeperForge2::IModApi& api) { ... }
// has it called once, right before its Initialize. The api, and everything it hands out, stays valid until the mod's
// Shutdown has returned. All of it is for the game's own thread (the mod's Initialize, its events, its hooks' detours),
// unless it says otherwise. See docs/api/mods.md.
//
// Interfaces only grow at their end, so a mod built against an older copy of this file keeps working.
namespace KeeperForge2 {
	// The KeeperForge2 log (KeeperForge2\Logs). Each line starts with the mod's name. Any thread.
	class IModLog {
		public:
			virtual void Trace(const char* message) = 0;
			virtual void Debug(const char* message) = 0;
			virtual void Info(const char* message) = 0;
			virtual void Warning(const char* message) = 0;
			// Logged as an error, without closing the game (as KeeperForge2's own errors do).
			virtual void Error(const char* message) = 0;
		protected:
			~IModLog() = default;
	};

	// Hooks of the game's functions, through KeeperForge2's own hook engine. A mod never writes the game's code itself, and
	// never names a raw address: it finds a place by a byte pattern, then hooks it.
	class IModHooks {
		public:
			// The start of the only place in the game's exe matching a pattern such as "8B 44 ?? 04" (hex bytes, ? or ?? for
			// any byte). 0 when there is none, more than one, or the pattern cannot be read: a pattern that matches
			// several places is not specific enough to hook.
			virtual std::uintptr_t FindPattern(const char* pattern) = 0;
			// Sends calls of the game's function at target to detour; *original is then a function that runs the game's
			// own code. False if the place cannot be hooked or is hooked already (by KeeperForge2 or another mod); the log
			// says why. Best made in Initialize, before the game runs.
			virtual bool Hook(std::uintptr_t target, void* detour, void** original) = 0;
			// Takes back a hook this mod made. Every hook a mod made is taken back when it shuts down.
			virtual bool Unhook(std::uintptr_t target) = 0;
		protected:
			~IModHooks() = default;
	};

	// Called with the context the mod gave when it registered.
	using ModEventFn = void (*)(void* context);

	// When things happen in the game. Simulation events (level start, game turns) come at the same point on every
	// machine of a multiplayer game, so what a mod does in them stays in step if it reads only the game's state and
	// IModGame::Random.
	class IModEvents {
		public:
			// At each level start, before the simulation runs (multiplayer settings settled).
			virtual void OnLevelStart(ModEventFn callback, void* context) = 0;
			// Before each game turn of a level being played (not while our level editor holds the level).
			virtual void OnGameTurn(ModEventFn callback, void* context) = 0;
		protected:
			~IModEvents() = default;
	};

	// A player of the level being played.
	struct ModPlayer {
		// 1 the Good player (heroes), 2 neutral, 3 and up the keepers.
		std::uint8_t number = 0;
		std::int32_t mana = 0;
		// Where its Dungeon Heart is, in tiles (the game keeps 1, 1 when it has none).
		std::uint16_t heartX = 0;
		std::uint16_t heartY = 0;
	};
	// A creature of the level being played.
	struct ModCreature {
		// Its tag: the game's handle for it while it lives (tags are used again after it is gone).
		std::uint16_t tag = 0;
		// Its type: the place in the creature list from 1 (docs/game/creatures.md; mods' types after the stock ones).
		std::uint8_t type = 0;
		std::uint8_t level = 0;
		// Its owner's player number (as ModPlayer::number).
		std::uint8_t owner = 0;
		std::int32_t health = 0;
		// Where it is, in 1/4096ths of a tile (tile = x >> 12).
		std::int32_t x = 0;
		std::int32_t y = 0;
	};
	// A tile of the map.
	struct ModTile {
		// Its terrain's id (Terrain.kwd), its owner's player number (0 none), and its room's type (Rooms.kwd; 0 none).
		std::uint8_t terrain = 0;
		std::uint8_t owner = 0;
		std::uint8_t roomType = 0;
	};
	using ModCreatureFn = void (*)(const ModCreature& creature, void* context);

	// The game's state, read only. Everything but GetPlayMode is about the level being played: without one the counts are
	// 0 and the functions return false.
	class IModGame {
		public:
			// Whether a level is being played (its world exists).
			virtual bool InLevel() const = 0;
			// 1 campaign, 2 skirmish, 3 multiplayer.
			virtual std::int32_t GetPlayMode() const = 0;
			// The level's game turn (0 before the first).
			virtual std::uint32_t GetGameTurn() const = 0;
			// The player number this machine plays (0 when unknown).
			virtual std::uint8_t GetLocalPlayer() const = 0;
			virtual bool GetPlayer(std::uint8_t number, ModPlayer& out) const = 0;
			virtual std::uint32_t GetMapWidth() const = 0;
			virtual std::uint32_t GetMapHeight() const = 0;
			virtual bool GetTile(std::uint32_t x, std::uint32_t y, ModTile& out) const = 0;
			// Every creature a player owns (number 0: every player's), in the game's own order, the same on every machine.
			virtual void ForEachCreature(std::uint8_t owner, ModCreatureFn callback, void* context) const = 0;
			virtual bool GetCreature(std::uint16_t tag, ModCreature& out) const = 0;
			// The shared random number: 0 to bound - 1 (0 for bound 0). The same on every machine when every machine
			// calls it alike: only from simulation events and the game's simulation code, never for what only this
			// machine shows. Started again at each game turn from the game's own synced seed (which it never changes).
			virtual std::uint32_t Random(std::uint32_t bound) = 0;
		protected:
			~IModGame() = default;
	};

	class IModApi {
		public:
			virtual IModLog& Log() = 0;
			virtual IModHooks& Hooks() = 0;
			virtual IModEvents& Events() = 0;
			virtual IModGame& Game() = 0;
			// The KeeperForge2 version, such as "0.1.0".
			virtual const char* GetVersion() const = 0;
		protected:
			~IModApi() = default;
	};

	using UseApiFn = void (*)(IModApi& api);
	inline constexpr const char* UseApiExport = "KeeperForge2_UseApi";
}
