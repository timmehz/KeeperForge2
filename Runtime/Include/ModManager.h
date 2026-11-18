#pragma once
#include "IMod.h"
#include <memory>
#include <vector>
namespace KeeperForge2 {
	class ModManager {
		public:
			// Loads every mod DLL in KeeperForge2\mods\<folder>\. Mods only ever come from there: each is its own
			// project, and KeeperForge2 has none of its own.
			static bool Initialize();
			static void Shutdown();
			static bool IsInitialized();
			static std::size_t GetModCount();
		private:
			struct LoadedMod;
			static std::vector<std::unique_ptr<LoadedMod>> mods;
			static void LoadModFolders();
			static bool Start(std::unique_ptr<LoadedMod> loaded);
	};
}
