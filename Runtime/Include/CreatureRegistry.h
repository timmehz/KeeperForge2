#pragma once
#include "LevelFiles.h"
#include "ModContent.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
namespace KeeperForge2 {
	// Creature types added by mods (ModContent.h). They are put at the end of the shared creature list: when the game reads
	// Data\editor\Creatures.kwd it gets KeeperForge2\cache\Creatures.kwd, the list it would have read (with mods' and Flame
	// packs' data folders) with the added types after it. A level that carries its own creature list gets a copy of that
	// list with the added types after its own (LevelListFor), and our level editor sees them the same way (AddTo).
	class CreatureRegistry {
		public:
			// What mods are handed in KeeperForge2_RegisterContent.
			static IModContent& Content();
			// For a path under the game's Data folder: the merged list's path when it is the shared creature list and mods
			// added types, else empty. Made the first time it is asked for.
			static std::filesystem::path ListFor(const std::string& underData);
			// For any file the game opens: when it is a level's own creature list (a "...Creatures.kld" or "...Globals.kld"
			// with a creature section) and mods added types it lacks, a copy with them added (KeeperForge2\cache\levels),
			// else empty. Made again at each open, as the level editor may have changed the file.
			static std::filesystem::path LevelListFor(const std::filesystem::path& path);
			// Adds the mods' types to a creature list (a level's own, or the shared one as read): each one the list has no
			// type of that name for, after the list's own types, with its id set to its place there. `what` names the list
			// in the log. Returns how many were added.
			static std::size_t AddTo(LevelFiles::ListData& list, const std::string& what);
			// How many types the shared list has with the mods' types (0 if it could not be read).
			static std::size_t SharedTypeCount();
			// For a type a mod added (by name): the stock type it was copied from, through copies of copies (0 if none or
			// not a copy).
			static std::uint8_t CopiedFrom(const std::string& name);
			// The mod being registered now, for the log.
			static void SetCurrentMod(const std::string& name);
	};
}
