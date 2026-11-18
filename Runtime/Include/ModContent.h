#pragma once
#include <cstddef>
#include <cstdint>
// Game content a mod adds (part of the mod SDK, with IMod.h). A mod DLL that exports
//   KEEPERFORGE2_MOD_EXPORT void KeeperForge2_RegisterContent(KeeperForge2::IModContent& content) { ... }
// has it called once, right after its Initialize, before the game has read any of its data. Mods are called in their load
// order (folders sorted by name), so every machine gives the same ids. See docs/api/mods.md.
namespace KeeperForge2 {
	// A creature type a mod added. Its record is the game's creature record (the layout is in docs/game/creature-types.md:
	// name at 0, flags at 0xEFC, attractions at 0xDAC, the art resources' names...), changed in place until the game reads
	// the creature list. The type id at 0xF4B is set by KeeperForge2.
	class ICreatureType {
		public:
			// The type's id in the game (its place in the creature list, from 1).
			virtual std::uint8_t GetId() const = 0;
			virtual std::uint8_t* GetRecord() = 0;
			virtual std::size_t GetRecordSize() const = 0;
		protected:
			~ICreatureType() = default;
	};

	class ICreatureRegistry {
		public:
			// How many creature types the game's own list has (with mods' and Flame packs' data folders), before any added.
			virtual std::size_t GetBaseTypeCount() const = 0;
			// A new type made as a copy of type `from` (an id from 1, a base type or one added before), named `name` (up to 31
			// characters). Null if it cannot be made (no such type, or 255 types already).
			virtual ICreatureType* AddCopy(std::uint8_t from, const char* name) = 0;
			// A new type from a whole record of `size` bytes as a creature list holds it; it is cut or filled with zeros to
			// the list's record size. Null if it cannot be made.
			virtual ICreatureType* AddRecord(const void* record, std::size_t size) = 0;
			// A type added by any mod, by its name; null if none has it.
			virtual ICreatureType* Find(const char* name) = 0;
		protected:
			~ICreatureRegistry() = default;
	};

	// What a mod can add. New kinds of content are added at the end, so mods built against an older copy keep working.
	class IModContent {
		public:
			virtual ICreatureRegistry& Creatures() = 0;
		protected:
			~IModContent() = default;
	};

	using RegisterContentFn = void (*)(IModContent& content);
	inline constexpr const char* RegisterContentExport = "KeeperForge2_RegisterContent";
}
