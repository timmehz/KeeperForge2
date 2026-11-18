#pragma once
#include "ModApi.h"
#include <string>
namespace KeeperForge2 {
	// The runtime's side of ModApi.h: one IModApi for each mod, its hooks and its event callbacks.
	class ModApiHost {
		public:
			// Registers for level starts (SessionOptions). Before CoopCampaign::Install, whose callback must be the last.
			static void Install();
			// The api handed to a mod; kept until Release.
			static IModApi& Create(const std::string& modName);
			// After the mod's Shutdown, before its DLL is unloaded: takes back its hooks and forgets its callbacks.
			static void Release(IModApi& api);
			// Before each game turn of a level being played (CoopCampaign's world tick hook).
			static void BeforeGameTurn();
	};
}
