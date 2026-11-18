#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace KeeperForge2 {
	// Game data from mods and Flame resource packs: a mod folder (KeeperForge2\mods\<mod>\data\) or a Flame pack
	// (flame\resources\<pack>\data\) holding files at the same paths as the game's Data folder is searched before the
	// game's own files, for meshes, engine textures, sprites, front-end art, paths, editor data (creatures, levels...),
	// text, textures and the palette. Mods win over Flame packs; among mods the later one (sorted by folder name) wins.
	// Files the game opens by path for reading (level files, Creatures.kwd and the other shared lists) are taken from the
	// same folders when they hold a file of that name. Sounds and music are not covered yet. See docs/api/mods.md.
	class ModData {
		public:
			// Before the game reads its command line (the runtime's start).
			static void Install();
			// The file the game gets for this path under its Data folder (such as "editor\Creatures.kwd"): a mod's or
			// Flame pack's copy if one has it, else the game's own; empty if there is none.
			static std::filesystem::path Find(const std::string& underData);
			// The data folders in use (mods' and Flame packs'), highest priority first.
			static std::vector<std::filesystem::path> DataFolders();
	};
}
