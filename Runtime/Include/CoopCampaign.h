#pragma once
#include <cstddef>
#include <cstdint>
namespace KeeperForge2 {
	// Campaign levels over the game's own multiplayer. The host picks a level and each player's part in the lobby:
	// sharing the main keeper, the Good player, or the level's second keeper. Set in the game, not in config.yml.
	class CoopCampaign {
		public:
			// After SkirmishSwapKeeper (it adds to the same lobby list), SessionOptions (the choices travel with its list)
			// and every feature with a level start callback.
			static void Install();
			// A campaign level is being played this way, on this machine and every other.
			static bool InSession();
			// In such a game: a person plays this player number. The same answer on every machine, so simulation code may use it.
			static bool HumanPlays(std::uint8_t playerNumber);
			// A lobby network message. True if it was ours.
			static bool OnLobbyMessage(const std::uint8_t* message, std::size_t size);
	};
}
