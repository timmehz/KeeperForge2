#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
// The game's level files (docs/game/level-files.md). A level is a .kwd that lists its other files by path. What we do not
// understand yet is kept byte for byte, so a level saved from here keeps its triggers and everything else as they were.
namespace KeeperForge2::LevelFiles {
	// A file is a run of sections, each with a 12 byte header.
	struct Section {
		std::uint32_t id = 0;
		std::uint32_t second = 0;
		std::vector<std::uint8_t> body;
	};
	struct File {
		std::vector<Section> sections;
		// The first section with this id, or null.
		Section* Find(std::uint32_t id);
		const Section* Find(std::uint32_t id) const;
	};
	std::optional<File> ParseFile(const std::vector<std::uint8_t>& bytes, std::string& error);
	std::vector<std::uint8_t> SerializeFile(const File& file);
	std::optional<File> ReadFile(const std::filesystem::path& path, std::string& error);
	bool WriteFile(const std::filesystem::path& path, const File& file, std::string& error);

	// One tile of a map: its terrain id and the number of the player who owns it. The other two bytes are not understood yet
	// (docs/game/level-files.md), so they are named for where they are.
	struct Tile {
		std::uint8_t terrain = 0;
		std::uint8_t owner = 0;
		std::uint8_t third = 0;
		std::uint8_t fourth = 0;
	};
	struct MapData {
		std::uint32_t headerBlockSize = 0;
		std::uint32_t tilesBlockSize = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::vector<Tile> tiles;
		// Whatever follows the tiles in the section.
		std::vector<std::uint8_t> trailing;
	};
	// A section with the map's id.
	std::optional<MapData> DecodeMap(const Section& section, std::string& error);
	void EncodeMap(const MapData& map, Section& section);

	// A level's things: what it starts with on the map. Records are kept as bytes, whatever their kind, in file order.
	struct ThingRecord {
		std::uint32_t kind = 0;
		std::vector<std::uint8_t> bytes;
	};
	struct ThingsData {
		// The section's first block, as it is but for the count.
		std::vector<std::uint8_t> header;
		std::vector<ThingRecord> records;
	};
	// A section with the things' id.
	std::optional<ThingsData> DecodeThings(const Section& section, std::string& error);
	void EncodeThings(const ThingsData& things, Section& section);

	// A section that is one list of fixed size records, as a level's Players (0xB4) and Variables (0xE6) are: a first block S+1
	// of 0x24 bytes with the count at +8, then block S+2 holding the records. Kept as bytes, all but the count as they were.
	inline constexpr std::uint32_t PlayersSection = 0xB4;
	inline constexpr std::uint32_t VariablesSection = 0xE6;
	struct ListData {
		std::vector<std::uint8_t> header;
		std::uint32_t recordsBlock = 0;
		std::size_t recordSize = 0;
		std::vector<std::vector<std::uint8_t>> records;
		// Whatever follows the records in the section.
		std::vector<std::uint8_t> trailing;
	};
	// recordSize is used when the list is empty, as nothing then says it.
	std::optional<ListData> DecodeList(const Section& section, std::size_t emptyRecordSize, std::string& error);
	void EncodeList(const ListData& list, Section& section);

	// A level's triggers (section 0xD2): a first block 0xD3 of 0x28 bytes with the count of triggers at +8 and of actions at +0xC,
	// then block 0xD4 holding records, each with an 8 byte header: 0xD5 a trigger, 0xD6 an action, 16 bytes each (0x571290)
	// [verified]. docs/game/level-files.md has the fields.
	inline constexpr std::uint32_t TriggersSection = 0xD2;
	inline constexpr std::uint32_t TriggerRecordKind = 0xD5;
	inline constexpr std::uint32_t ActionRecordKind = 0xD6;
	struct TriggersData {
		std::vector<std::uint8_t> header;
		std::vector<ThingRecord> records;
		std::vector<std::uint8_t> trailing;
	};
	std::optional<TriggersData> DecodeTriggers(const Section& section, std::string& error);
	void EncodeTriggers(const TriggersData& triggers, Section& section);

	// One entry of the list of files in a level's .kwd. The path is relative to the game folder.
	struct LevelResource {
		std::uint32_t type = 0;
		std::uint32_t second = 0;
		std::array<char, 0x40> path{};
		std::string Path() const;
		// False if it does not fit.
		bool SetPath(const std::string& value);
	};
	struct LevelData {
		// Everything before the list, kept as it is: header, description, and the list's block header.
		std::vector<std::uint8_t> before;
		std::vector<LevelResource> resources;
		// Everything after the list.
		std::vector<std::uint8_t> after;
	};
	// A section with the level's id.
	std::optional<LevelData> DecodeLevel(const Section& section, std::string& error);
	void EncodeLevel(const LevelData& level, Section& section);
	// The wide texts at the start of the level's description (block 0xDF), by their place and length in characters (0x56C100
	// copies them out) [verified]. What each is for is from their sizes and the official editor's fields [unverified]; the first
	// holds the level's file name in stock levels.
	enum class LevelText { Name, Description, Author, Email, Information };
	std::size_t LevelTextLength(LevelText text);
	std::wstring GetLevelText(const LevelData& level, LevelText text);
	// False if the level has no room for it or it is too long.
	bool SetLevelText(LevelData& level, LevelText text, const std::wstring& value);
	// The level's own trigger tree: the first trigger's id, at +0x1180 of the description (0x51E6D0 hands it to 0x518080, which
	// keeps it at world +0x7310) [verified], or 0 for none.
	std::uint16_t LevelTriggerRoot(const LevelData& level);
	// False if the level's description has no room for it.
	bool SetLevelTriggerRoot(LevelData& level, std::uint16_t root);

	// A creature list (section 0xAA): Creatures.kwd, or a level's own copy. One record per creature type, a type's id being its place
	// in the list from 1 (also at CreatureOffset::TypeId). docs/game/creature-types.md has the layout and where each fact is from.
	inline constexpr std::uint32_t CreaturesSection = 0xAA;
	// The record size the game reads: its reader (0x56A250) has one buffer of this size, and its converter (0x568E30) reads nothing
	// past it [verified]. A longer record is read in at its own size, past the buffer, so records are never made longer here.
	// Creatures.kwd's are this size; the official editor's newer ones are 0x15A1 bytes.
	inline constexpr std::size_t GameCreatureRecordSize = 0x1549;
	namespace CreatureOffset {
		inline constexpr std::size_t Name = 0;                       // 32 characters
		inline constexpr std::size_t NameLength = 32;
		inline constexpr std::size_t OlhiEffect = 0xCFD;             // 16 bit effect id, 0 none
		inline constexpr std::size_t TranslationSoundCategory = 0xD17; // 32 characters
		inline constexpr std::size_t CloneCreature = 0xD3B;          // creature id, 0 none
		inline constexpr std::size_t IntroCameraPath = 0xD3E;        // byte
		inline constexpr std::size_t Attractions = 0xDAC;            // two of { 32 bit kind, 32 bit value }
		inline constexpr std::size_t AttractionCount = 2;
		inline constexpr std::size_t AttractionSize = 8;
		inline constexpr std::size_t Flags = 0xEFC;                  // 32 bits, CreatureFlag
		inline constexpr std::size_t JobClass = 0xF02;               // byte: Artisan, Thinker, Fighter, Scout
		inline constexpr std::size_t FightStyle = 0xF03;             // byte: Non-Fighter, Blitzer, Blocker, Flanker, Support
		inline constexpr std::size_t EntranceEffect = 0xF36;         // 16 bit effect id
		inline constexpr std::size_t SlapEffect = 0xF3E;             // 16 bit effect id
		inline constexpr std::size_t DeathEffect = 0xF40;            // 16 bit effect id
		inline constexpr std::size_t TypeId = 0xF4B;                 // byte
		inline constexpr std::size_t LairObject = 0xF52;             // object id, 0 none
		inline constexpr std::size_t DeathFallDirection = 0xF54;     // byte: North, North-East ... North-West
		inline constexpr std::size_t SoundCategory = 0xF56;          // 32 characters
		inline constexpr std::size_t ArmorType = 0xF76;             // byte: None, Flesh, Rock, Wood, Metal 1, Metal 2, Magic, Glass
		inline constexpr std::size_t UniqueNameTextId = 0x14DF;      // 16 bit
		inline constexpr std::size_t SoundCategoryLength = 32;
	}
	// How a number is kept in a creature record. Fixed is 32 bits counting 1/4096ths (tiles, seconds, tiles per second).
	enum class CreatureNumber : std::uint8_t { U8, U16, S16, U32, Fixed };
	struct CreatureAttribute {
		const char* name;
		std::size_t offset;
		CreatureNumber kind;
	};
	// The numbers the official editor's Attribute list shows, in its order.
	const std::vector<CreatureAttribute>& CreatureAttributes();
	// Bits of the 32 bit flags at CreatureOffset::Flags.
	namespace CreatureFlag {
		inline constexpr std::uint32_t IsWorker = 1u << 0;
		inline constexpr std::uint32_t CanBePickedUp = 1u << 1;
		inline constexpr std::uint32_t CanBeSlapped = 1u << 2;
		inline constexpr std::uint32_t AlwaysFlee = 1u << 3;
		inline constexpr std::uint32_t CanWalkOnWater = 1u << 4;
		inline constexpr std::uint32_t CanWalkOnLava = 1u << 5;
		inline constexpr std::uint32_t IsEvil = 1u << 7;
		inline constexpr std::uint32_t IsImmuneToTurncoat = 1u << 9;
		inline constexpr std::uint32_t AvailableViaPortal = 1u << 10;
		inline constexpr std::uint32_t CanFly = 1u << 13;
		inline constexpr std::uint32_t Horny = 1u << 14;
		inline constexpr std::uint32_t GenerateDeadBody = 1u << 15;
		inline constexpr std::uint32_t CanBeHypnotised = 1u << 16;
		inline constexpr std::uint32_t IsImmuneToChicken = 1u << 17;
		inline constexpr std::uint32_t IsFearless = 1u << 18;
		inline constexpr std::uint32_t CanBeElectrocuted = 1u << 19;
		inline constexpr std::uint32_t NeedBodyForFightIdle = 1u << 20;
		// Set means the creature does not train when idle.
		inline constexpr std::uint32_t NotTrainWhenIdle = 1u << 21;
		inline constexpr std::uint32_t OnlyAttackableByHorny = 1u << 22;
		inline constexpr std::uint32_t CanBeResurrected = 1u << 23;
		inline constexpr std::uint32_t DoesntGetAngryWithEnemies = 1u << 24;
		inline constexpr std::uint32_t FreesFriendsOnJailBreak = 1u << 25;
		inline constexpr std::uint32_t RevealsAdjacentTraps = 1u << 26;
		inline constexpr std::uint32_t IsUnique = 1u << 27;
		inline constexpr std::uint32_t MpdRandomInvader = 1u << 30;
		inline constexpr std::uint32_t IsMale = 1u << 31;
	}

	// A shared list such as Terrain.kwd or Traps.kwd: fixed size records, each starting with its name.
	struct DefinitionTable {
		std::size_t recordSize = 0;
		std::vector<std::string> names;
		std::vector<std::vector<std::uint8_t>> records;
	};
	std::optional<DefinitionTable> ReadDefinitions(const std::filesystem::path& path, std::uint32_t sectionId, std::string& error);

	// The name a level's saved copy goes by in the game's level name setting, which is joined to Data\editor\Maps\.
	std::wstring SavedLevelName(const std::wstring& level);
	bool HasSavedCopy(const std::wstring& level);
	// Reads a stock level and all its files, checks they would be written back byte for byte, and, if asked and it has none
	// already, makes its copy in KeeperForge2\maps. False with the reason if anything failed.
	bool CopyLevel(const std::filesystem::path& kwdPath, bool makeCopy, bool& copyMade, std::string& error);
	// CopyLevel for every stock level, with the results and the shared lists in the log.
	void CheckAllLevels(bool makeCopies);

	// A placed creature's exact hit points, which the level's own record cannot hold: it keeps a whole percent of the creature's
	// most health (docs/game/level-files.md, Things). Kept in a file of our own beside the level's Things file
	// (<things file's name>.health, a line "x y owner type level points" each), and applied by KeeperForge2 when the level loads.
	// A creature is found by the first five.
	struct CreatureHealth {
		std::int32_t x = 0;
		std::int32_t y = 0;
		std::uint8_t owner = 0;
		std::uint8_t type = 0;
		std::uint8_t level = 0;
		std::uint32_t points = 0;
		bool operator==(const CreatureHealth&) const = default;
	};
	std::filesystem::path CreatureHealthPath(const std::filesystem::path& thingsPath);
	std::vector<CreatureHealth> ReadCreatureHealth(const std::filesystem::path& path);
	bool WriteCreatureHealth(const std::filesystem::path& path, const std::vector<CreatureHealth>& entries, std::string& error);

	// A level's map, loaded to be changed and saved.
	struct LoadedMap {
		std::filesystem::path path;
		File file;
		MapData map;
		// Its things, if the level lists a file of them.
		std::filesystem::path thingsPath;
		File thingsFile;
		std::optional<ThingsData> things;
		// Its players and variables, if it lists files of them.
		std::filesystem::path playersPath;
		File playersFile;
		std::optional<ListData> players;
		std::filesystem::path variablesPath;
		File variablesFile;
		std::optional<ListData> variables;
		// Its triggers. Written back only when changed, so a level whose triggers were never touched keeps its file as it was.
		std::filesystem::path triggersPath;
		File triggersFile;
		std::optional<TriggersData> triggers;
		bool triggersChanged = false;
		// The .kwd's level section, for its description. Written back on saving when changed.
		std::optional<LevelData> level;
		bool levelChanged = false;
		// Its creature types: the creature list the level lists, which is the shared Creatures.kwd unless it has its own. A changed
		// shared list is saved as the level's own <name>Creatures.kld, and the level's file list pointed at it; the shared file is
		// never written. Only written when changed.
		std::filesystem::path creaturesPath;
		File creaturesFile;
		std::optional<ListData> creatures;
		bool creaturesShared = false;
		// The entry of the level's file list that holds them: CreaturesSection, or 0 for a Globals file holding every shared list
		// (as Furnace_hero has).
		std::uint32_t creaturesResourceType = CreaturesSection;
		bool creaturesChanged = false;
		// Exact hit points of placed creatures (CreatureHealth), written beside the Things file when changed.
		std::vector<CreatureHealth> creatureHealth;
		bool creatureHealthChanged = false;
	};
	// The level's .kwd says where its map file is.
	std::optional<LoadedMap> LoadMap(const std::filesystem::path& kwdPath, std::string& error);
	// The level's copy in KeeperForge2\maps if it has one, else the original.
	std::filesystem::path LevelPath(const std::wstring& level);
	// Into the level's copy, which is made first if it has none. The original is never written. A level the editor made
	// is saved where it is, in Data\editor\maps.
	bool SaveMap(const std::wstring& level, LoadedMap& loaded, std::string& error);
	// A level made by the editor (Save as), listed in KeeperForge2\maps\editor-levels.txt.
	bool IsEditorLevel(const std::wstring& level);
	// The level, with the loaded map and things, as a new level of this name in Data\editor\maps, all its own files renamed.
	// Refuses a name a stock level has. The loaded paths become the new files'.
	bool SaveLevelAs(const std::filesystem::path& sourceKwd, const std::wstring& name, LoadedMap& loaded, std::string& error);
	// Whether the game can hold a map this size. It has no largest map of its own, so only an impossible size is refused.
	bool MapSizeFits(std::uint32_t width, std::uint32_t height);
	// The stock level a new map takes its rules and players from: four keepers and no triggers.
	inline constexpr const wchar_t* NewMapTemplate = L"Swiss Cheese";
	// Room for the Impenetrable Rock edge and keeper 1's heart with its wall.
	inline constexpr std::uint32_t SmallestNewMapSide = 9;
	// A new level of this name in Data\editor\maps, with a thumbnail: a width by height map of Rock inside an edge of Impenetrable
	// Rock, keeper 1's Dungeon Heart in the middle, and no other things. Refuses a name any level has.
	bool MakeBlankLevel(const std::wstring& name, std::uint32_t width, std::uint32_t height, std::string& error);
	// A picture of the map for the game's level list: Data\editor\maps\Thumbnails\<name>144.bmp.
	bool WriteThumbnail(const MapData& map, const std::wstring& name, std::string& error);

	struct TerrainType {
		std::uint8_t id = 0;
		std::string name;
		// Its cell in TerrainIcons.bmp. Rooms hold no useful one here, as they use RoomIcons.bmp.
		std::uint16_t icon = 0;
	};
	// Every terrain in Data\editor\Terrain.kwd, by id.
	std::vector<TerrainType> ReadTerrainTypes(std::string& error);
}
