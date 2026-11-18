#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// KeeperForge2 settings that change the game simulation, so every player must use the same value. In multiplayer
	// the host's value is used: it travels in unused bytes of the lobby's creature list, which the host sends to everyone.
	// Otherwise the value is this machine's config.yml.
	enum class SessionOption {
		// "mercenary-portal-spawns: normal" (neither this nor MercenaryPortalOff = mercenary)
		MercenaryPortalNormal,
		// "mercenary-portal-spawns: off": the portal keeps the game's own rules and the lobby does not list the heroes
		MercenaryPortalOff,
		// "keeper-dwarf-workers: on": keepers' workers are Dwarfs (Create Dwarf and the rest)
		KeeperDwarfWorkers,
		// "mercenary-portal-stone-knights: off"
		StoneKnightsOff,
		// "mercenary-portal-royals: off"
		RoyalsOff,
		// "create-imp-price-rises: off": Create Imp (and Create Dwarf) always costs its base price
		CreateImpFlatPrice,
	};
	class SessionOptions {
		public:
			static void Install();
			// The value the current game uses.
			static bool Get(SessionOption option);
			// This machine's config.yml value, whatever the host chose.
			static bool GetLocal(SessionOption option);
			// Whether the current game uses values received from the host, not this machine's.
			static bool UsesSessionValues();
			// Called at each level start, before the simulation runs (after the values are settled).
			static void OnLevelStart(void (*callback)());
			// For other session values kept in the lobby's creature list: called when this machine's values are written
			// into the list, and when the list holds the values the game will use.
			static void OnLobbyWrite(void (*callback)(std::uint8_t* frontEnd));
			static void OnLobbyRead(void (*callback)(const std::uint8_t* frontEnd));
	};
}
