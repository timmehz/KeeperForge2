#include "LevelFiles.h"
#include "CreatureRegistry.h"
#include "Logger.h"
#include "ModData.h"
#include "Paths.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace KeeperForge2::LevelFiles {
	namespace {
		constexpr std::size_t SectionHeaderSize = 12;
		constexpr std::size_t BlockHeaderSize = 8;
		// A list's first block: its id and size, then 7 dwords of which the first is usually the record count.
		constexpr std::size_t ListHeaderBlockSize = 0x24;
		constexpr std::size_t ListCountOffset = 8;
		constexpr std::size_t RecordNameSize = 32;
		constexpr std::uint32_t TerrainSection = 0x6E;
		// In a Terrain.kwd record. The game reads it here (0x56ED20).
		constexpr std::size_t TerrainIdOffset = 0x1D6;
		constexpr std::size_t TerrainIconOffset = 0x196;

		constexpr std::uint32_t MapSection = 100;
		constexpr std::uint32_t MapHeaderBlock = 0x65;
		constexpr std::uint32_t MapTilesBlock = 0x66;
		constexpr std::size_t MapHeaderBlockSize = 0x10;
		constexpr std::size_t MapWidthOffset = 8;
		constexpr std::size_t MapHeightOffset = 12;
		constexpr std::size_t TileSize = 4;

		constexpr std::uint32_t LevelSection = 0xDC;
		constexpr std::uint32_t ThingsSection = 0xBE;
		constexpr std::uint32_t ThingsHeaderBlock = 0xBF;
		constexpr std::uint32_t ThingsRecordsBlock = 0xC0;
		constexpr std::size_t ThingsCountOffset = 8;
		constexpr std::uint32_t LevelHeaderBlock = 0xDD;
		constexpr std::uint32_t LevelListBlock = 0xDE;
		constexpr std::uint32_t LevelDescriptionBlock = 0xDF;
		constexpr std::size_t LevelResourceCountOffset = 8;
		constexpr std::size_t LevelResourceSize = 0x48;
		constexpr std::size_t LevelResourcePathOffset = 8;

		std::uint32_t U32(const std::uint8_t* at) { std::uint32_t value = 0; std::memcpy(&value, at, sizeof(value)); return value; }
		std::uint16_t U16(const std::uint8_t* at) { std::uint16_t value = 0; std::memcpy(&value, at, sizeof(value)); return value; }
		void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
			out.insert(out.end(), bytes, bytes + sizeof(value));
		}
		std::string Hex(std::uint32_t value) {
			constexpr char digits[] = "0123456789ABCDEF";
			std::string text;
			do { text.insert(text.begin(), digits[value & 0xF]); value >>= 4; } while (value != 0);
			return "0x" + text;
		}
		std::string Lower(std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}
		bool EndsWith(const std::string& text, const std::string& end) {
			return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
		}
		std::optional<std::vector<std::uint8_t>> ReadBytes(const fs::path& path, std::string& error) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				error = "could not open " + path.string();
				return std::nullopt;
			}
			std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			return bytes;
		}
		// Written to a temporary name first, so a failure never leaves half a file under the real one.
		bool WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes, std::string& error) {
			std::error_code code;
			fs::create_directories(path.parent_path(), code);
			auto temporary = path;
			temporary += ".tmp";
			{
				std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
				if (!file) {
					error = "could not create " + temporary.string();
					return false;
				}
				file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (!file) {
					error = "could not write " + temporary.string();
					return false;
				}
			}
			fs::rename(temporary, path, code);
			if (code) {
				error = "could not name " + path.string() + " (" + code.message() + ")";
				fs::remove(temporary, code);
				return false;
			}
			return true;
		}
	}

	Section* File::Find(std::uint32_t id) {
		for (auto& section : sections) { if (section.id == id) return &section; }
		return nullptr;
	}
	const Section* File::Find(std::uint32_t id) const {
		for (const auto& section : sections) { if (section.id == id) return &section; }
		return nullptr;
	}

	std::optional<File> ParseFile(const std::vector<std::uint8_t>& bytes, std::string& error) {
		File file;
		std::size_t at = 0;
		while (at < bytes.size()) {
			if (bytes.size() - at < SectionHeaderSize) {
				error = "a section header is cut off at " + std::to_string(at);
				return std::nullopt;
			}
			Section section;
			section.id = U32(bytes.data() + at);
			section.second = U32(bytes.data() + at + 4);
			// The size in the header counts the header too.
			const std::size_t whole = U32(bytes.data() + at + 8);
			if (whole < SectionHeaderSize) {
				error = "section " + Hex(section.id) + " says it has " + std::to_string(whole) + " bytes, less than its header";
				return std::nullopt;
			}
			const std::size_t size = whole - SectionHeaderSize;
			at += SectionHeaderSize;
			if (size > bytes.size() - at) {
				error = "section " + Hex(section.id) + " says it has " + std::to_string(size) + " bytes, " + std::to_string(bytes.size() - at) + " left";
				return std::nullopt;
			}
			section.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
			at += size;
			file.sections.push_back(std::move(section));
		}
		return file;
	}
	std::vector<std::uint8_t> SerializeFile(const File& file) {
		std::vector<std::uint8_t> bytes;
		for (const auto& section : file.sections) {
			PutU32(bytes, section.id);
			PutU32(bytes, section.second);
			PutU32(bytes, static_cast<std::uint32_t>(section.body.size() + SectionHeaderSize));
			bytes.insert(bytes.end(), section.body.begin(), section.body.end());
		}
		return bytes;
	}
	std::optional<File> ReadFile(const fs::path& path, std::string& error) {
		const auto bytes = ReadBytes(path, error);
		if (!bytes) return std::nullopt;
		auto file = ParseFile(*bytes, error);
		if (!file) error = path.string() + ": " + error;
		return file;
	}
	bool WriteFile(const fs::path& path, const File& file, std::string& error) { return WriteBytes(path, SerializeFile(file), error); }

	std::optional<ThingsData> DecodeThings(const Section& section, std::string& error) {
		const auto& body = section.body;
		if (body.size() < 16 || U32(body.data()) != ThingsHeaderBlock) {
			error = "the things section does not start with its header block";
			return std::nullopt;
		}
		const std::size_t headerSize = std::size_t{ 8 } + U32(body.data() + 4);
		if (headerSize < ThingsCountOffset + 4 || headerSize > body.size() || body.size() - headerSize < 8 || U32(body.data() + headerSize) != ThingsRecordsBlock) {
			error = "the things section has no block of records after its header";
			return std::nullopt;
		}
		ThingsData things;
		things.header.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(headerSize));
		const std::size_t count = U32(body.data() + ThingsCountOffset);
		const std::size_t recordsSize = U32(body.data() + headerSize + 4);
		std::size_t at = headerSize + 8;
		if (recordsSize != body.size() - at) {
			error = "the things section's records say " + std::to_string(recordsSize) + " bytes, " + std::to_string(body.size() - at) + " are there";
			return std::nullopt;
		}
		for (std::size_t i = 0; i < count; ++i) {
			if (body.size() - at < 8) {
				error = "the things section ends before record " + std::to_string(i) + " of " + std::to_string(count);
				return std::nullopt;
			}
			ThingRecord record;
			record.kind = U32(body.data() + at);
			const std::size_t size = U32(body.data() + at + 4);
			at += 8;
			if (size > body.size() - at) {
				error = "thing record " + std::to_string(i) + " goes past the end of the section";
				return std::nullopt;
			}
			record.bytes.assign(body.begin() + static_cast<std::ptrdiff_t>(at), body.begin() + static_cast<std::ptrdiff_t>(at + size));
			at += size;
			things.records.push_back(std::move(record));
		}
		if (at != body.size()) {
			error = "the things section has " + std::to_string(body.size() - at) + " bytes after its records";
			return std::nullopt;
		}
		return things;
	}
	void EncodeThings(const ThingsData& things, Section& section) {
		std::vector<std::uint8_t> records;
		for (const auto& record : things.records) {
			PutU32(records, record.kind);
			PutU32(records, static_cast<std::uint32_t>(record.bytes.size()));
			records.insert(records.end(), record.bytes.begin(), record.bytes.end());
		}
		auto& body = section.body;
		body = things.header;
		const auto count = static_cast<std::uint32_t>(things.records.size());
		std::memcpy(body.data() + ThingsCountOffset, &count, sizeof(count));
		PutU32(body, ThingsRecordsBlock);
		PutU32(body, static_cast<std::uint32_t>(records.size()));
		body.insert(body.end(), records.begin(), records.end());
	}

	std::optional<MapData> DecodeMap(const Section& section, std::string& error) {
		const auto& body = section.body;
		if (body.size() < MapHeaderBlockSize + BlockHeaderSize) {
			error = "map section too short";
			return std::nullopt;
		}
		if (U32(body.data()) != MapHeaderBlock || U32(body.data() + MapHeaderBlockSize) != MapTilesBlock) {
			error = "map blocks not where expected";
			return std::nullopt;
		}
		MapData map;
		map.headerBlockSize = U32(body.data() + 4);
		map.width = U32(body.data() + MapWidthOffset);
		map.height = U32(body.data() + MapHeightOffset);
		map.tilesBlockSize = U32(body.data() + MapHeaderBlockSize + 4);
		if (!MapSizeFits(map.width, map.height)) {
			error = "impossible map size " + std::to_string(map.width) + "x" + std::to_string(map.height);
			return std::nullopt;
		}
		const std::size_t count = static_cast<std::size_t>(map.width) * map.height;
		const std::size_t start = MapHeaderBlockSize + BlockHeaderSize;
		if (body.size() - start < count * TileSize) {
			error = "map section too short for " + std::to_string(map.width) + "x" + std::to_string(map.height);
			return std::nullopt;
		}
		map.tiles.resize(count);
		for (std::size_t i = 0; i < count; ++i) {
			const auto* tile = body.data() + start + i * TileSize;
			map.tiles[i] = { tile[0], tile[1], tile[2], tile[3] };
		}
		map.trailing.assign(body.begin() + static_cast<std::ptrdiff_t>(start + count * TileSize), body.end());
		return map;
	}
	void EncodeMap(const MapData& map, Section& section) {
		auto& body = section.body;
		body.clear();
		PutU32(body, MapHeaderBlock);
		PutU32(body, map.headerBlockSize);
		PutU32(body, map.width);
		PutU32(body, map.height);
		PutU32(body, MapTilesBlock);
		PutU32(body, map.tilesBlockSize);
		for (const auto& tile : map.tiles) {
			body.push_back(tile.terrain);
			body.push_back(tile.owner);
			body.push_back(tile.third);
			body.push_back(tile.fourth);
		}
		body.insert(body.end(), map.trailing.begin(), map.trailing.end());
	}

	std::string LevelResource::Path() const { return std::string(path.data(), strnlen(path.data(), path.size())); }
	bool LevelResource::SetPath(const std::string& value) {
		if (value.size() >= path.size()) return false;
		path.fill(0);
		std::memcpy(path.data(), value.data(), value.size());
		return true;
	}

	std::optional<LevelData> DecodeLevel(const Section& section, std::string& error) {
		const auto& body = section.body;
		if (body.size() < ListHeaderBlockSize + BlockHeaderSize) {
			error = "level section too short";
			return std::nullopt;
		}
		if (U32(body.data()) != LevelHeaderBlock || U32(body.data() + ListHeaderBlockSize) != LevelDescriptionBlock) {
			error = "level blocks not where expected";
			return std::nullopt;
		}
		const std::size_t resourceCount = U16(body.data() + LevelResourceCountOffset);
		const std::size_t descriptionSize = U32(body.data() + ListHeaderBlockSize + 4);
		const std::size_t descriptionStart = ListHeaderBlockSize + BlockHeaderSize;
		if (descriptionSize > body.size() - descriptionStart || body.size() - descriptionStart - descriptionSize < BlockHeaderSize) {
			error = "level description runs past the end";
			return std::nullopt;
		}
		const std::size_t listHeader = descriptionStart + descriptionSize;
		if (U32(body.data() + listHeader) != LevelListBlock) {
			error = "level list block not where expected";
			return std::nullopt;
		}
		const std::size_t listStart = listHeader + BlockHeaderSize;
		if ((body.size() - listStart) / LevelResourceSize < resourceCount) {
			error = "level says it has " + std::to_string(resourceCount) + " files, with no room for them";
			return std::nullopt;
		}
		LevelData level;
		level.before.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(listStart));
		level.resources.resize(resourceCount);
		for (std::size_t i = 0; i < resourceCount; ++i) {
			const auto* entry = body.data() + listStart + i * LevelResourceSize;
			level.resources[i].type = U32(entry);
			level.resources[i].second = U32(entry + 4);
			std::memcpy(level.resources[i].path.data(), entry + LevelResourcePathOffset, level.resources[i].path.size());
		}
		level.after.assign(body.begin() + static_cast<std::ptrdiff_t>(listStart + resourceCount * LevelResourceSize), body.end());
		return level;
	}
	void EncodeLevel(const LevelData& level, Section& section) {
		auto& body = section.body;
		body = level.before;
		for (const auto& resource : level.resources) {
			PutU32(body, resource.type);
			PutU32(body, resource.second);
			body.insert(body.end(), resource.path.begin(), resource.path.end());
		}
		body.insert(body.end(), level.after.begin(), level.after.end());
	}

	std::optional<ListData> DecodeList(const Section& section, std::size_t emptyRecordSize, std::string& error) {
		const auto& body = section.body;
		if (body.size() < ListHeaderBlockSize + BlockHeaderSize || U32(body.data()) != section.id + 1 || U32(body.data() + ListHeaderBlockSize) != section.id + 2) {
			error = "section " + Hex(section.id) + ": list blocks not where expected";
			return std::nullopt;
		}
		const std::size_t count = U32(body.data() + ListCountOffset);
		const std::size_t size = U32(body.data() + ListHeaderBlockSize + 4);
		const std::size_t start = ListHeaderBlockSize + BlockHeaderSize;
		if (size > body.size() - start || (count != 0 && size % count != 0) || (count == 0 && size != 0)) {
			error = "section " + Hex(section.id) + ": " + std::to_string(count) + " records in " + std::to_string(size) + " bytes does not fit";
			return std::nullopt;
		}
		ListData list;
		list.header.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(ListHeaderBlockSize));
		list.recordsBlock = section.id + 2;
		list.recordSize = count != 0 ? size / count : emptyRecordSize;
		for (std::size_t i = 0; i < count; ++i) {
			const auto* record = body.data() + start + i * list.recordSize;
			list.records.emplace_back(record, record + list.recordSize);
		}
		list.trailing.assign(body.begin() + static_cast<std::ptrdiff_t>(start + size), body.end());
		return list;
	}
	void EncodeList(const ListData& list, Section& section) {
		auto& body = section.body;
		body = list.header;
		const auto count = static_cast<std::uint32_t>(list.records.size());
		std::memcpy(body.data() + ListCountOffset, &count, sizeof(count));
		PutU32(body, list.recordsBlock);
		PutU32(body, static_cast<std::uint32_t>(list.records.size() * list.recordSize));
		for (const auto& record : list.records) {
			body.insert(body.end(), record.begin(), record.end());
			// A record made shorter or longer than the list's is put right, so the block's size stays true.
			if (record.size() < list.recordSize) body.insert(body.end(), list.recordSize - record.size(), std::uint8_t{ 0 });
			else if (record.size() > list.recordSize) body.resize(body.size() - (record.size() - list.recordSize));
		}
		body.insert(body.end(), list.trailing.begin(), list.trailing.end());
	}

	namespace {
		// Within the description block's body, which starts after the level's first block and the description's block header.
		constexpr std::size_t DescriptionStart = ListHeaderBlockSize + BlockHeaderSize;
		struct LevelTextPlace { std::size_t offset; std::size_t length; };
		LevelTextPlace PlaceOf(LevelText text) {
			switch (text) {
				case LevelText::Name: return { 0, 0x40 };
				case LevelText::Description: return { 0x80, 0x400 };
				case LevelText::Author: return { 0x880, 0x40 };
				case LevelText::Email: return { 0x900, 0x40 };
				case LevelText::Information: return { 0x980, 0x400 };
			}
			return { 0, 0 };
		}
	}
	std::optional<TriggersData> DecodeTriggers(const Section& section, std::string& error) {
		constexpr std::size_t HeaderSize = 0x28;
		constexpr std::uint32_t RecordsBlock = 0xD4;
		const auto& body = section.body;
		if (body.size() < HeaderSize + BlockHeaderSize || U32(body.data()) != section.id + 1 || U32(body.data() + HeaderSize) != RecordsBlock) {
			error = "the triggers section's blocks are not where expected";
			return std::nullopt;
		}
		const std::size_t count = std::size_t{ U32(body.data() + 8) } + U32(body.data() + 0xC);
		TriggersData triggers;
		triggers.header.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(HeaderSize + BlockHeaderSize));
		std::size_t at = HeaderSize + BlockHeaderSize;
		for (std::size_t i = 0; i < count; ++i) {
			if (body.size() - at < BlockHeaderSize) {
				error = "the triggers section ends before record " + std::to_string(i) + " of " + std::to_string(count);
				return std::nullopt;
			}
			ThingRecord record;
			record.kind = U32(body.data() + at);
			const std::size_t size = U32(body.data() + at + 4);
			at += BlockHeaderSize;
			if (size > body.size() - at) {
				error = "trigger record " + std::to_string(i) + " goes past the end of the section";
				return std::nullopt;
			}
			record.bytes.assign(body.begin() + static_cast<std::ptrdiff_t>(at), body.begin() + static_cast<std::ptrdiff_t>(at + size));
			at += size;
			triggers.records.push_back(std::move(record));
		}
		triggers.trailing.assign(body.begin() + static_cast<std::ptrdiff_t>(at), body.end());
		return triggers;
	}
	void EncodeTriggers(const TriggersData& triggers, Section& section) {
		std::vector<std::uint8_t> records;
		std::uint32_t triggerCount = 0;
		std::uint32_t actionCount = 0;
		for (const auto& record : triggers.records) {
			PutU32(records, record.kind);
			PutU32(records, static_cast<std::uint32_t>(record.bytes.size()));
			records.insert(records.end(), record.bytes.begin(), record.bytes.end());
			if (record.kind == TriggerRecordKind) ++triggerCount;
			else ++actionCount;
		}
		auto& body = section.body;
		body = triggers.header;
		std::memcpy(body.data() + 8, &triggerCount, sizeof(triggerCount));
		std::memcpy(body.data() + 0xC, &actionCount, sizeof(actionCount));
		// The records block's own size is left as the file had it: in stock files it does not follow the records in any one way
		// (a survey of all 67), and the game's reader skips it (0x571290).
		body.insert(body.end(), records.begin(), records.end());
		body.insert(body.end(), triggers.trailing.begin(), triggers.trailing.end());
	}

	std::size_t LevelTextLength(LevelText text) { return PlaceOf(text).length; }
	namespace {
		// Where the description ends in the level's bytes, as old files have a shorter one.
		std::size_t DescriptionEnd(const LevelData& level) {
			if (level.before.size() < DescriptionStart) return 0;
			return (std::min)(level.before.size(), DescriptionStart + std::size_t{ U32(level.before.data() + ListHeaderBlockSize + 4) });
		}
	}
	std::wstring GetLevelText(const LevelData& level, LevelText text) {
		const auto place = PlaceOf(text);
		const auto start = DescriptionStart + place.offset;
		const auto end = DescriptionEnd(level);
		std::wstring value;
		for (std::size_t i = 0; i < place.length && start + i * 2 + 1 < end; ++i) {
			const auto c = static_cast<wchar_t>(level.before[start + i * 2] | (level.before[start + i * 2 + 1] << 8));
			if (c == 0) break;
			value += c;
		}
		return value;
	}
	bool SetLevelText(LevelData& level, LevelText text, const std::wstring& value) {
		const auto place = PlaceOf(text);
		const auto start = DescriptionStart + place.offset;
		// One character is kept for the end.
		if (value.size() >= place.length || DescriptionEnd(level) < start + place.length * 2) return false;
		std::fill(level.before.begin() + static_cast<std::ptrdiff_t>(start), level.before.begin() + static_cast<std::ptrdiff_t>(start + place.length * 2), std::uint8_t{ 0 });
		for (std::size_t i = 0; i < value.size(); ++i) {
			level.before[start + i * 2] = static_cast<std::uint8_t>(value[i] & 0xFF);
			level.before[start + i * 2 + 1] = static_cast<std::uint8_t>(value[i] >> 8);
		}
		return true;
	}

	std::uint16_t LevelTriggerRoot(const LevelData& level) {
		constexpr std::size_t RootOffset = 0x1180;
		const auto at = DescriptionStart + RootOffset;
		return at + 2 <= DescriptionEnd(level) ? U16(level.before.data() + at) : std::uint16_t{ 0 };
	}
	bool SetLevelTriggerRoot(LevelData& level, std::uint16_t root) {
		constexpr std::size_t RootOffset = 0x1180;
		const auto at = DescriptionStart + RootOffset;
		if (at + 2 > DescriptionEnd(level)) return false;
		std::memcpy(level.before.data() + at, &root, sizeof(root));
		return true;
	}

	const std::vector<CreatureAttribute>& CreatureAttributes() {
		// Where the official editor keeps each one in the record, and how it shows it (docs/game/creature-types.md). The game's own
		// converter 0x568E30 reads these places.
		using enum CreatureNumber;
		static const std::vector<CreatureAttribute> attributes{
			{ "Experience Points For Next Level", 0xF00, U16 },
			{ "Experience Points (Per Second)", 0xF04, U16 },
			{ "Experience Points From Training (Per Second)", 0xF06, U16 },
			{ "Research Points (Per Second)", 0xF08, U16 },
			{ "Manufacture Points (Per Second)", 0xF0A, U16 },
			{ "Height (Tiles)", 0xEA4, Fixed },
			{ "Eye Height (Tiles)", 0xEB0, Fixed },
			{ "Health", 0xF0C, U16 },
			{ "Health From Chicken", 0xF0E, U16 },
			{ "Fear", 0xF10, U16 },
			{ "Threat", 0xF12, U16 },
			{ "Mana Generated By Prayer", 0xF18, S16 },
			{ "Pay", 0xF1C, U16 },
			{ "Max Gold Held", 0xF1E, U16 },
			{ "Speed (Tiles Per Second)", 0xEB4, Fixed },
			{ "Run Speed (Tiles Per Second)", 0xEB8, Fixed },
			{ "Shuffle Speed (Tiles Per Second)", 0xD37, Fixed },
			{ "Distance Can See (Tiles)", 0xEC8, Fixed },
			{ "Distance Can Hear (Tiles)", 0xECC, Fixed },
			{ "Slap Damage", 0xF16, S16 },
			{ "Hunger Rate", 0xEBC, Fixed },
			{ "Hunger Fill", 0xF4E, U8 },
			{ "Time Awake (Seconds)", 0xEC0, U32 },
			{ "Sleep Duration (Seconds)", 0xEC4, U32 },
			{ "Mood Unhappy Threshold", 0xF4F, U8 },
			{ "Decompose Value", 0xF22, U16 },
			{ "Anger (No Lair)", 0xF28, S16 },
			{ "Anger (No Food)", 0xF2A, S16 },
			{ "Anger (No Pay)", 0xF2C, S16 },
			{ "Anger (No Work)", 0xF2E, S16 },
			{ "Anger (Slap)", 0xF30, S16 },
			{ "Anger (In Hand)", 0xF32, S16 },
			{ "Initial Gold Held", 0xF34, U16 },
			{ "Torture Time To Convert (Seconds)", 0xEF4, Fixed },
			{ "Stun Duration (Seconds)", 0xED0, Fixed },
			{ "Guard Duration (Seconds)", 0xED4, Fixed },
			{ "Idle Duration (Seconds)", 0xED8, Fixed },
			{ "Slap Fearless Duration (Seconds)", 0xEDC, Fixed },
			{ "Possession Mana Cost (Per Second)", 0xEE8, U16 },
			{ "Own Land Health Increase (Per Second)", 0xEEA, U16 },
			{ "Torture Health Change (Per Second)", 0x107F, S16 },
			{ "Torture Mood Change (Per Second)", 0x1081, S16 },
			{ "Perception Range [ Fog Of War ]", 0xD01, Fixed },
		};
		return attributes;
	}

	std::optional<DefinitionTable> ReadDefinitions(const fs::path& path, std::uint32_t sectionId, std::string& error) {
		const auto file = ReadFile(path, error);
		if (!file) return std::nullopt;
		const auto* section = file->Find(sectionId);
		if (section == nullptr) {
			error = path.string() + " has no section " + Hex(sectionId);
			return std::nullopt;
		}
		const auto& body = section->body;
		if (body.size() < ListHeaderBlockSize + BlockHeaderSize || U32(body.data()) != sectionId + 1 || U32(body.data() + ListHeaderBlockSize) != sectionId + 2) {
			error = path.string() + ": list blocks not where expected";
			return std::nullopt;
		}
		const std::size_t count = U32(body.data() + ListCountOffset);
		const std::size_t size = U32(body.data() + ListHeaderBlockSize + 4);
		const std::size_t start = ListHeaderBlockSize + BlockHeaderSize;
		if (count == 0 || size > body.size() - start || size % count != 0 || size / count < RecordNameSize) {
			error = path.string() + ": " + std::to_string(count) + " records in " + std::to_string(size) + " bytes does not fit";
			return std::nullopt;
		}
		DefinitionTable table;
		table.recordSize = size / count;
		for (std::size_t i = 0; i < count; ++i) {
			const auto* record = body.data() + start + i * table.recordSize;
			const auto* name = reinterpret_cast<const char*>(record);
			table.names.emplace_back(name, strnlen(name, RecordNameSize));
			table.records.emplace_back(record, record + table.recordSize);
		}
		return table;
	}

	fs::path CreatureHealthPath(const fs::path& thingsPath) {
		auto path = thingsPath;
		path.replace_extension(".health");
		return path;
	}
	std::vector<CreatureHealth> ReadCreatureHealth(const fs::path& path) {
		std::vector<CreatureHealth> entries;
		std::ifstream file(path);
		std::string line;
		while (std::getline(file, line)) {
			if (line.empty() || line[0] == '#') continue;
			std::istringstream fields(line);
			long long x = 0, y = 0, owner = 0, type = 0, level = 0, points = 0;
			if (!(fields >> x >> y >> owner >> type >> level >> points)) continue;
			if (owner < 0 || owner > 0xFF || type < 1 || type > 0xFF || level < 1 || level > 0xFF || points < 1 || points > 0x7FFFFFFF) continue;
			entries.push_back({ static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::uint8_t>(owner), static_cast<std::uint8_t>(type),
				static_cast<std::uint8_t>(level), static_cast<std::uint32_t>(points) });
		}
		return entries;
	}
	bool WriteCreatureHealth(const fs::path& path, const std::vector<CreatureHealth>& entries, std::string& error) {
		std::error_code code;
		// None left: no file, so a level without any keeps none.
		if (entries.empty()) {
			fs::remove(path, code);
			return true;
		}
		std::string text = "# KeeperForge2: exact hit points of placed creatures: x y owner type level points\n";
		for (const auto& entry : entries) {
			text += std::to_string(entry.x) + " " + std::to_string(entry.y) + " " + std::to_string(entry.owner) + " " + std::to_string(entry.type) + " "
				+ std::to_string(entry.level) + " " + std::to_string(entry.points) + "\n";
		}
		return WriteBytes(path, std::vector<std::uint8_t>(text.begin(), text.end()), error);
	}

	std::wstring SavedLevelName(const std::wstring& level) {
		// The game makes this <base>Data\editor\Maps\<name>.kwd, so three folders up is the game folder.
		return L"..\\..\\..\\" + Paths::Maps().wstring() + L"\\" + level;
	}
	bool HasSavedCopy(const std::wstring& level) {
		std::error_code code;
		return fs::is_regular_file(Paths::Maps() / (level + L".kwd"), code);
	}

	bool CopyLevel(const fs::path& kwdPath, bool makeCopy, bool& copyMade, std::string& error) {
		copyMade = false;
		const auto originalKwd = ReadBytes(kwdPath, error);
		if (!originalKwd) return false;
		auto kwd = ParseFile(*originalKwd, error);
		if (!kwd) return false;
		if (SerializeFile(*kwd) != *originalKwd) {
			error = kwdPath.string() + " does not write back the same";
			return false;
		}
		auto* levelSection = kwd->Find(LevelSection);
		if (levelSection == nullptr) {
			error = kwdPath.string() + " has no level section";
			return false;
		}
		auto level = DecodeLevel(*levelSection, error);
		if (!level) return false;
		auto decoded = *levelSection;
		EncodeLevel(*level, decoded);
		if (decoded.body != levelSection->body) {
			error = kwdPath.string() + ": its level section does not write back the same";
			return false;
		}

		// Each of the level's own files. Shared ones such as Data\Editor\Terrain stay where they are.
		struct LevelFileCopy { fs::path path; std::vector<std::uint8_t> bytes; };
		std::vector<LevelFileCopy> copies;
		for (auto& resource : level->resources) {
			const auto path = resource.Path();
			const auto lower = Lower(path);
			if (!EndsWith(lower, ".kld") || lower.find("\\maps\\") == std::string::npos) continue;
			auto bytes = ReadBytes(path, error);
			if (!bytes) return false;
			auto file = ParseFile(*bytes, error);
			if (!file) {
				error = path + ": " + error;
				return false;
			}
			if (SerializeFile(*file) != *bytes) {
				error = path + " does not write back the same";
				return false;
			}
			if (const auto* mapSection = file->Find(MapSection)) {
				const auto map = DecodeMap(*mapSection, error);
				if (!map) {
					error = path + ": " + error;
					return false;
				}
				auto encoded = *mapSection;
				EncodeMap(*map, encoded);
				if (encoded.body != mapSection->body) {
					error = path + ": its map does not write back the same";
					return false;
				}
			}
			if (const auto* thingsSection = file->Find(ThingsSection)) {
				const auto things = DecodeThings(*thingsSection, error);
				if (!things) {
					error = path + ": " + error;
					return false;
				}
				auto encoded = *thingsSection;
				EncodeThings(*things, encoded);
				if (encoded.body != thingsSection->body) {
					error = path + ": its things do not write back the same";
					return false;
				}
			}
			const auto name = fs::path(path).filename();
			const auto copyPath = (Paths::Maps() / name).string();
			if (!resource.SetPath(copyPath)) {
				error = copyPath + " is too long for the level's file list";
				return false;
			}
			copies.push_back({ Paths::Maps() / name, std::move(*bytes) });
		}
		if (copies.empty()) {
			error = kwdPath.string() + " lists none of its own files";
			return false;
		}

		if (!makeCopy) return true;
		// A copy already there may be someone's work, so it is left alone.
		std::error_code code;
		const auto copyKwd = Paths::Maps() / kwdPath.filename();
		if (fs::exists(copyKwd, code)) return true;
		for (const auto& copy : copies) {
			if (!WriteBytes(copy.path, copy.bytes, error)) return false;
		}
		EncodeLevel(*level, *levelSection);
		// Last, as the game finds a level by this file: one that is there has everything it lists.
		if (!WriteFile(copyKwd, *kwd, error)) return false;
		copyMade = true;
		return true;
	}

	std::optional<LoadedMap> LoadMap(const fs::path& kwdPath, std::string& error) {
		const auto kwd = ReadFile(kwdPath, error);
		if (!kwd) return std::nullopt;
		const auto* levelSection = kwd->Find(LevelSection);
		if (levelSection == nullptr) {
			error = kwdPath.string() + " has no level section";
			return std::nullopt;
		}
		const auto level = DecodeLevel(*levelSection, error);
		if (!level) return std::nullopt;
		for (const auto& resource : level->resources) {
			if (resource.type != MapSection) continue;
			LoadedMap loaded;
			loaded.path = resource.Path();
			auto file = ReadFile(loaded.path, error);
			if (!file) return std::nullopt;
			const auto* mapSection = file->Find(MapSection);
			if (mapSection == nullptr) {
				error = loaded.path.string() + " has no map section";
				return std::nullopt;
			}
			auto map = DecodeMap(*mapSection, error);
			if (!map) return std::nullopt;
			loaded.file = std::move(*file);
			loaded.map = std::move(*map);
			// Without its things the map can still be painted, so this only leaves creatures out.
			for (const auto& other : level->resources) {
				if (other.type != ThingsSection) continue;
				std::string thingsError;
				loaded.thingsPath = other.Path();
				auto thingsFile = ReadFile(loaded.thingsPath, thingsError);
				const auto* thingsSection = thingsFile ? thingsFile->Find(ThingsSection) : nullptr;
				if (thingsSection != nullptr) loaded.things = DecodeThings(*thingsSection, thingsError);
				if (loaded.things) loaded.thingsFile = std::move(*thingsFile);
				else error = thingsError.empty() ? loaded.thingsPath.string() + " has no things section" : thingsError;
				loaded.creatureHealth = ReadCreatureHealth(CreatureHealthPath(loaded.thingsPath));
				break;
			}
			// Players and variables the same way: without them the rest can still be edited. Player records are 0xCD bytes and
			// variables 0x10 in current files (0x56DAD0, 0x5716E0) [verified].
			constexpr std::size_t PlayerRecordSize = 0xCD;
			constexpr std::size_t VariableRecordSize = 0x10;
			const auto loadList = [&](std::uint32_t type, std::size_t emptySize, fs::path& path, File& listFile, std::optional<ListData>& list) {
				for (const auto& other : level->resources) {
					if (other.type != type) continue;
					std::string listError;
					path = other.Path();
					auto read = ReadFile(path, listError);
					const auto* section = read ? read->Find(type) : nullptr;
					if (section != nullptr) list = DecodeList(*section, emptySize, listError);
					if (list) listFile = std::move(*read);
					else Logger::Warning("Level files: " + (listError.empty() ? path.string() + " has no section " + Hex(type) : listError));
					return;
				}
			};
			loadList(PlayersSection, PlayerRecordSize, loaded.playersPath, loaded.playersFile, loaded.players);
			loadList(VariablesSection, VariableRecordSize, loaded.variablesPath, loaded.variablesFile, loaded.variables);
			for (const auto& other : level->resources) {
				if (other.type != TriggersSection) continue;
				std::string triggersError;
				loaded.triggersPath = other.Path();
				auto read = ReadFile(loaded.triggersPath, triggersError);
				const auto* section = read ? read->Find(TriggersSection) : nullptr;
				if (section != nullptr) loaded.triggers = DecodeTriggers(*section, triggersError);
				if (loaded.triggers) loaded.triggersFile = std::move(*read);
				if (!loaded.triggers) Logger::Warning("Level files: " + (triggersError.empty() ? loaded.triggersPath.string() + " has no triggers section" : triggersError));
				break;
			}
			// Its creature types: the list the level names for them, or a Globals file (type 0) holding one. The game adds .kwd to a
			// path with no extension (0x56C960), as the shared Data\Editor\Creatures is listed.
			for (const auto& other : level->resources) {
				if (other.type != CreaturesSection && other.type != 0) continue;
				fs::path path = other.Path();
				if (!path.has_extension()) path += ".kwd";
				const bool shared = Lower(path.string()).find("\\maps\\") == std::string::npos;
				// The shared list as the game reads it: from a mod's or Flame pack's data folder when one has it.
				if (const auto lower = Lower(path.string()); shared && (lower.starts_with("data\\") || lower.starts_with("data/"))) {
					if (const auto found = ModData::Find(path.string().substr(5)); !found.empty()) path = found;
				}
				std::string creaturesError;
				auto read = ReadFile(path, creaturesError);
				const auto* section = read ? read->Find(CreaturesSection) : nullptr;
				if (section == nullptr) {
					if (other.type == CreaturesSection) Logger::Warning("Level files: " + (creaturesError.empty() ? path.string() + " has no creature list" : creaturesError));
					continue;
				}
				// A shared file of every list is not the level's to split; none is listed by a stock level.
				if (shared && other.type != CreaturesSection) continue;
				loaded.creatures = DecodeList(*section, GameCreatureRecordSize, creaturesError);
				if (!loaded.creatures) {
					Logger::Warning("Level files: " + creaturesError);
					break;
				}
				// The mods' creature types, as the game will have them in this level (after the list's own). Not a change of
				// the level: they are only written into it if its creature types are changed and saved.
				CreatureRegistry::AddTo(*loaded.creatures, path.string());
				loaded.creaturesFile = std::move(*read);
				loaded.creaturesPath = path;
				loaded.creaturesResourceType = other.type;
				loaded.creaturesShared = shared;
				break;
			}
			loaded.level = *level;
			return loaded;
		}
		error = kwdPath.string() + " lists no map";
		return std::nullopt;
	}
	fs::path LevelPath(const std::wstring& level) {
		return HasSavedCopy(level) ? Paths::Maps() / (level + L".kwd") : fs::path("Data\\editor\\maps") / (level + L".kwd");
	}
	namespace {
		const fs::path StockLevels = "Data\\editor\\maps";
		// Levels the editor made, one name a line, so they are saved where they are and a stock level is never written.
		fs::path EditorLevelList() { return Paths::Maps() / "editor-levels.txt"; }
		std::vector<std::wstring> ReadEditorLevels() {
			std::vector<std::wstring> names;
			std::ifstream file(EditorLevelList());
			std::string line;
			while (std::getline(file, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
				if (!line.empty()) names.emplace_back(line.begin(), line.end());
			}
			return names;
		}
		std::string Narrow(const std::wstring& text) {
			std::string out;
			for (const auto c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
			return out;
		}
		std::wstring LowerWide(std::wstring text) {
			for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
			return text;
		}
		// Where a level's own file goes when saved: next to its .kwd for a level the editor made, else KeeperForge2\maps.
		fs::path SaveFolder(const std::wstring& level) { return IsEditorLevel(level) ? StockLevels : Paths::Maps(); }
	}
	bool IsEditorLevel(const std::wstring& level) {
		const auto wanted = LowerWide(level);
		for (const auto& name : ReadEditorLevels()) {
			if (LowerWide(name) == wanted) return true;
		}
		return false;
	}
	namespace {
		// The creature list as it is to be written: the level's own file as read, or, when the level listed the shared one, a file
		// holding only its creature list (the shared file's other contents, if any, are not the level's to copy).
		std::optional<File> CreatureFileToWrite(const LoadedMap& loaded, std::string& error) {
			const auto* section = loaded.creaturesFile.Find(CreaturesSection);
			if (section == nullptr || !loaded.creatures) {
				error = "the loaded creature list has no section";
				return std::nullopt;
			}
			File out;
			if (loaded.creaturesShared) out.sections.push_back(*section);
			else out = loaded.creaturesFile;
			EncodeList(*loaded.creatures, *out.Find(CreaturesSection));
			return out;
		}
		// The level's own creature file's name: <level>Creatures.kld, as its other files are named.
		std::string OwnCreatureFileName(const std::wstring& level) { return Narrow(level) + "Creatures.kld"; }
		// Points the entry of this type in a .kwd's file list at a new path.
		bool PointLevelFileAt(const fs::path& kwdPath, std::uint32_t type, const std::string& path, std::string& error) {
			auto kwd = ReadFile(kwdPath, error);
			if (!kwd) return false;
			auto* levelSection = kwd->Find(LevelSection);
			auto level = levelSection != nullptr ? DecodeLevel(*levelSection, error) : std::nullopt;
			if (!level) {
				if (error.empty()) error = kwdPath.string() + " has no level section";
				return false;
			}
			bool found = false;
			for (auto& resource : level->resources) {
				if (resource.type != type) continue;
				if (!resource.SetPath(path)) {
					error = path + " is too long for the level's file list";
					return false;
				}
				found = true;
				break;
			}
			if (!found) {
				error = kwdPath.string() + " lists no file of type " + Hex(type);
				return false;
			}
			EncodeLevel(*level, *levelSection);
			return WriteFile(kwdPath, *kwd, error);
		}
	}

	bool SaveMap(const std::wstring& level, LoadedMap& loaded, std::string& error) {
		if (!IsEditorLevel(level) && !HasSavedCopy(level)) {
			bool copyMade = false;
			if (!CopyLevel(StockLevels / (level + L".kwd"), true, copyMade, error)) return false;
		}
		auto* section = loaded.file.Find(MapSection);
		if (section == nullptr) {
			error = "the loaded map has no map section";
			return false;
		}
		EncodeMap(loaded.map, *section);
		const auto path = SaveFolder(level) / loaded.path.filename();
		if (!WriteFile(path, loaded.file, error)) return false;
		loaded.path = path;
		if (loaded.things) {
			auto* thingsSection = loaded.thingsFile.Find(ThingsSection);
			if (thingsSection == nullptr) {
				error = "the loaded things have no things section";
				return false;
			}
			EncodeThings(*loaded.things, *thingsSection);
			const auto thingsPath = SaveFolder(level) / loaded.thingsPath.filename();
				if (!WriteFile(thingsPath, loaded.thingsFile, error)) return false;
				loaded.thingsPath = thingsPath;
				// Always beside the things saved, as a first save of a stock level puts them in a new folder.
				if (!WriteCreatureHealth(CreatureHealthPath(thingsPath), loaded.creatureHealth, error)) return false;
				loaded.creatureHealthChanged = false;
		}
		const auto saveList = [&](std::uint32_t type, fs::path& path, File& listFile, const std::optional<ListData>& list) {
			if (!list) return true;
			auto* listSection = listFile.Find(type);
			if (listSection == nullptr) {
				error = "the loaded " + Hex(type) + " list has no section";
				return false;
			}
			EncodeList(*list, *listSection);
			const auto target = SaveFolder(level) / path.filename();
			if (!WriteFile(target, listFile, error)) return false;
			path = target;
			return true;
		};
		if (!saveList(PlayersSection, loaded.playersPath, loaded.playersFile, loaded.players)) return false;
		if (!saveList(VariablesSection, loaded.variablesPath, loaded.variablesFile, loaded.variables)) return false;
		if (loaded.triggersChanged && loaded.triggers) {
			auto* triggersSection = loaded.triggersFile.Find(TriggersSection);
			if (triggersSection == nullptr) {
				error = "the loaded triggers have no section";
				return false;
			}
			EncodeTriggers(*loaded.triggers, *triggersSection);
			const auto target = SaveFolder(level) / loaded.triggersPath.filename();
			if (!WriteFile(target, loaded.triggersFile, error)) return false;
			loaded.triggersPath = target;
			loaded.triggersChanged = false;
		}
		if (loaded.creaturesChanged && loaded.creatures) {
			auto out = CreatureFileToWrite(loaded, error);
			if (!out) return false;
			const auto target = SaveFolder(level) / (loaded.creaturesShared ? fs::path(OwnCreatureFileName(level)) : loaded.creaturesPath.filename());
			if (!WriteFile(target, *out, error)) return false;
			// The level listed the shared list: its file list now names its own, which the game loads in its place.
			if (loaded.creaturesShared && !PointLevelFileAt(SaveFolder(level) / (level + L".kwd"), loaded.creaturesResourceType, target.string(), error)) return false;
			loaded.creaturesFile = std::move(*out);
			loaded.creaturesPath = target;
			loaded.creaturesShared = false;
			loaded.creaturesChanged = false;
		}
		if (loaded.levelChanged && loaded.level) {
			// Into the .kwd that is saved to, whose file list is its own (a copy's points at the copy's files): only its
			// description is taken from the one being edited.
			const auto kwdPath = SaveFolder(level) / (level + L".kwd");
			auto kwd = ReadFile(kwdPath, error);
			if (!kwd) return false;
			auto* levelSection = kwd->Find(LevelSection);
			auto target = levelSection != nullptr ? DecodeLevel(*levelSection, error) : std::nullopt;
			if (!target) {
				if (error.empty()) error = kwdPath.string() + " has no level section";
				return false;
			}
			const auto end = DescriptionEnd(*loaded.level);
			if (DescriptionEnd(*target) != end) {
				error = kwdPath.string() + "'s description is not the size of the one edited";
				return false;
			}
			std::copy(loaded.level->before.begin() + static_cast<std::ptrdiff_t>(DescriptionStart), loaded.level->before.begin() + static_cast<std::ptrdiff_t>(end),
				target->before.begin() + static_cast<std::ptrdiff_t>(DescriptionStart));
			EncodeLevel(*target, *levelSection);
			if (!WriteFile(kwdPath, *kwd, error)) return false;
			loaded.levelChanged = false;
		}
		return true;
	}

	bool SaveLevelAs(const fs::path& sourceKwd, const std::wstring& name, LoadedMap& loaded, std::string& error) {
		// Shared names in the game's folder: one the editor did not make is a stock level, and is never written.
		const auto newKwd = StockLevels / (name + L".kwd");
		std::error_code code;
		if (fs::exists(newKwd, code) && !IsEditorLevel(name)) {
			error = "a level called " + Narrow(name) + " already exists and was not made by the editor";
			return false;
		}
		auto kwd = ReadFile(sourceKwd, error);
		if (!kwd) return false;
		auto* levelSection = kwd->Find(LevelSection);
		if (levelSection == nullptr) {
			error = sourceKwd.string() + " has no level section";
			return false;
		}
		auto level = DecodeLevel(*levelSection, error);
		if (!level) return false;
		// The description as edited, then its name set below.
		if (loaded.level && DescriptionEnd(*loaded.level) == DescriptionEnd(*level)) {
			std::copy(loaded.level->before.begin() + static_cast<std::ptrdiff_t>(DescriptionStart), loaded.level->before.begin() + static_cast<std::ptrdiff_t>(DescriptionEnd(*loaded.level)),
				level->before.begin() + static_cast<std::ptrdiff_t>(DescriptionStart));
		}
		// The description starts with the level's file name, wide (room for far more than this before anything else).
		constexpr std::size_t DescriptionNameOffset = ListHeaderBlockSize + BlockHeaderSize;
		constexpr std::size_t DescriptionNameBytes = 0x80;
		const std::wstring fileName = name + L".kwd";
		if (level->before.size() < DescriptionNameOffset + DescriptionNameBytes || (fileName.size() + 1) * 2 > DescriptionNameBytes) {
			error = "the level's description has no room for the name " + Narrow(name);
			return false;
		}
		std::fill(level->before.begin() + DescriptionNameOffset, level->before.begin() + DescriptionNameOffset + DescriptionNameBytes, std::uint8_t{ 0 });
		for (std::size_t i = 0; i < fileName.size(); ++i) {
			level->before[DescriptionNameOffset + i * 2] = static_cast<std::uint8_t>(fileName[i] & 0xFF);
			level->before[DescriptionNameOffset + i * 2 + 1] = static_cast<std::uint8_t>(fileName[i] >> 8);
		}
		// Each of the level's own files gets the new name in place of the old one in front of its kind ("Map", "Things" ...).
		const auto oldName = Lower(sourceKwd.stem().string());
		auto* mapSection = loaded.file.Find(MapSection);
		if (mapSection == nullptr) {
			error = "the loaded map has no map section";
			return false;
		}
		EncodeMap(loaded.map, *mapSection);
		if (loaded.things) {
			if (auto* thingsSection = loaded.thingsFile.Find(ThingsSection)) EncodeThings(*loaded.things, *thingsSection);
		}
		if (loaded.players) {
			if (auto* section = loaded.playersFile.Find(PlayersSection)) EncodeList(*loaded.players, *section);
		}
		if (loaded.variables) {
			if (auto* section = loaded.variablesFile.Find(VariablesSection)) EncodeList(*loaded.variables, *section);
		}
		const bool triggersWritten = loaded.triggersChanged && loaded.triggers && loaded.triggersFile.Find(TriggersSection) != nullptr;
		if (triggersWritten) EncodeTriggers(*loaded.triggers, *loaded.triggersFile.Find(TriggersSection));
		struct Written { fs::path path; std::vector<std::uint8_t> bytes; std::uint32_t type; };
		std::vector<Written> files;
		for (auto& resource : level->resources) {
			const auto path = resource.Path();
			const auto lower = Lower(path);
			if (!EndsWith(lower, ".kld") || lower.find("\\maps\\") == std::string::npos) continue;
			auto stem = fs::path(path).stem().string();
			const auto kind = Lower(stem).starts_with(oldName) ? stem.substr(oldName.size()) : stem;
			const auto newFile = Narrow(name) + kind + ".kld";
			std::vector<std::uint8_t> bytes;
			if (resource.type == MapSection) bytes = SerializeFile(loaded.file);
			else if (resource.type == ThingsSection && loaded.things) bytes = SerializeFile(loaded.thingsFile);
			else if (resource.type == PlayersSection && loaded.players) bytes = SerializeFile(loaded.playersFile);
			else if (resource.type == VariablesSection && loaded.variables) bytes = SerializeFile(loaded.variablesFile);
			else if (resource.type == TriggersSection && triggersWritten) bytes = SerializeFile(loaded.triggersFile);
			else if (resource.type == loaded.creaturesResourceType && loaded.creatures && loaded.creaturesChanged && !loaded.creaturesShared) {
				auto out = CreatureFileToWrite(loaded, error);
				if (!out) return false;
				bytes = SerializeFile(*out);
			}
			else {
				auto read = ReadBytes(path, error);
				if (!read) return false;
				bytes = std::move(*read);
			}
			if (!resource.SetPath("Data\\Editor\\Maps\\" + newFile)) {
				error = newFile + " is too long for the level's file list";
				return false;
			}
			files.push_back({ StockLevels / newFile, std::move(bytes), resource.type });
		}
		// Creature types changed from the shared list: the new level gets its own file of them, as Save does.
		const bool ownCreaturesMade = loaded.creatures && loaded.creaturesChanged && loaded.creaturesShared;
		if (ownCreaturesMade) {
			auto out = CreatureFileToWrite(loaded, error);
			if (!out) return false;
			const auto newFile = OwnCreatureFileName(name);
			bool pointed = false;
			for (auto& resource : level->resources) {
				if (resource.type != CreaturesSection) continue;
				if (!resource.SetPath("Data\\Editor\\Maps\\" + newFile)) {
					error = newFile + " is too long for the level's file list";
					return false;
				}
				pointed = true;
				break;
			}
			if (!pointed) {
				error = sourceKwd.string() + " lists no creature list";
				return false;
			}
			files.push_back({ StockLevels / newFile, SerializeFile(*out), CreaturesSection });
		}
		for (const auto& file : files) {
			if (!WriteBytes(file.path, file.bytes, error)) return false;
		}
		EncodeLevel(*level, *levelSection);
		// Last, as the game finds a level by this file.
		if (!WriteFile(newKwd, *kwd, error)) return false;
		if (!IsEditorLevel(name)) {
			std::ofstream list(EditorLevelList(), std::ios::app);
			list << Narrow(name) << "\n";
		}
		for (const auto& file : files) {
			if (file.type == MapSection) loaded.path = file.path;
			if (file.type == ThingsSection) {
				loaded.thingsPath = file.path;
				if (!WriteCreatureHealth(CreatureHealthPath(file.path), loaded.creatureHealth, error)) return false;
				loaded.creatureHealthChanged = false;
			}
			if (file.type == PlayersSection) loaded.playersPath = file.path;
			if (file.type == VariablesSection) loaded.variablesPath = file.path;
			if (file.type == TriggersSection) loaded.triggersPath = file.path;
			if (loaded.creatures && file.type == loaded.creaturesResourceType && (ownCreaturesMade || !loaded.creaturesShared)) loaded.creaturesPath = file.path;
		}
		if (loaded.creatures && (ownCreaturesMade || (loaded.creaturesChanged && !loaded.creaturesShared))) {
			if (auto out = CreatureFileToWrite(loaded, error)) loaded.creaturesFile = std::move(*out);
			loaded.creaturesShared = false;
			loaded.creaturesChanged = false;
		}
		loaded.triggersChanged = false;
		loaded.level = *level;
		loaded.levelChanged = false;
		return true;
	}

	bool MapSizeFits(std::uint32_t width, std::uint32_t height) {
		// The game has no largest size: its map is width * height tiles of 16 bytes, allocated as one 32-bit size (0x44E9E0), with a
		// grid of a byte and one of two bytes a tile beside it (0x4D1840). So the only limit is that this still fits in 32 bits.
		if (width == 0 || height == 0) return false;
		constexpr std::uint64_t GameTileBytes = 0x10;
		return static_cast<std::uint64_t>(width) * height * GameTileBytes + 4 <= 0xFFFFFFFFull;
	}

	bool MakeBlankLevel(const std::wstring& name, std::uint32_t width, std::uint32_t height, std::string& error) {
		if (width < SmallestNewMapSide || height < SmallestNewMapSide) {
			error = "a new map is at least " + std::to_string(SmallestNewMapSide) + " tiles each way, for its edge and keeper 1's heart";
			return false;
		}
		if (!MapSizeFits(width, height)) {
			error = std::to_string(width) + "x" + std::to_string(height) + " is more tiles than the game can hold";
			return false;
		}
		std::error_code code;
		if (fs::exists(StockLevels / (name + L".kwd"), code)) {
			error = "a level called " + Narrow(name) + " already exists";
			return false;
		}
		// Rules and players from a stock level with four keepers and no triggers, so that nothing in it waits for things that
		// are not on the new map. The original, never a saved copy of it.
		const auto templateKwd = StockLevels / (std::wstring(NewMapTemplate) + L".kwd");
		auto loaded = LoadMap(templateKwd, error);
		if (!loaded) return false;
		const auto& old = loaded->map;
		MapData map;
		map.headerBlockSize = old.headerBlockSize;
		// The tiles block's size counts the tiles; whatever else it counts is kept as in the template.
		const auto oldTileBytes = static_cast<std::int64_t>(old.width) * old.height * TileSize;
		const auto newTileBytes = static_cast<std::int64_t>(width) * height * TileSize;
		map.tilesBlockSize = static_cast<std::uint32_t>(static_cast<std::int64_t>(old.tilesBlockSize) - oldTileBytes + newTileBytes);
		map.width = width;
		map.height = height;
		map.trailing = old.trailing;
		// Terrain ids from Terrain.kwd [verified: the check's log]. The third byte is 1 on every stock tile; the fourth is 0 or 1 and
		// not understood, 1 on most.
		constexpr std::uint8_t ImpenetrableRock = 1;
		constexpr std::uint8_t Rock = 2;
		constexpr std::uint8_t ReinforcedWall = 9;
		constexpr std::uint8_t DungeonHeart = 14;
		constexpr std::uint8_t Neutral = 2;
		constexpr std::uint8_t FirstKeeper = 3;
		map.tiles.assign(static_cast<std::size_t>(width) * height, Tile{ Rock, Neutral, 1, 1 });
		const auto at = [&](std::uint32_t x, std::uint32_t y) -> Tile& { return map.tiles[static_cast<std::size_t>(y) * width + x]; };
		for (std::uint32_t x = 0; x < width; ++x) {
			at(x, 0).terrain = ImpenetrableRock;
			at(x, height - 1).terrain = ImpenetrableRock;
		}
		for (std::uint32_t y = 0; y < height; ++y) {
			at(0, y).terrain = ImpenetrableRock;
			at(width - 1, y).terrain = ImpenetrableRock;
		}
		// Keeper 1's heart in the middle as stock levels have theirs: 5 by 5 heart tiles inside a ring of the keeper's reinforced
		// wall, with the room's thing at its center tile. [verified by eye: Small and Swiss Cheese]
		// In the middle, but never past tile 251: a room keeps its tiles' x and y in bytes (0x4ECB70), and on a 1024x512 map a heart at
		// 512,256 crashed level load at 0x4ECF7B, where a neighbor's y wrapped round to -1 (tested). The heart's ring reaches 3 further.
		constexpr std::uint32_t LastRoomCenter = 251;
		const auto centerX = (std::min)(width / 2, LastRoomCenter);
		const auto centerY = (std::min)(height / 2, LastRoomCenter);
		for (std::uint32_t y = centerY - 3; y <= centerY + 3; ++y) {
			for (std::uint32_t x = centerX - 3; x <= centerX + 3; ++x) {
				const bool ring = x == centerX - 3 || x == centerX + 3 || y == centerY - 3 || y == centerY + 3;
				at(x, y) = Tile{ ring ? ReinforcedWall : DungeonHeart, FirstKeeper, 1, 1 };
			}
		}
		loaded->map = std::move(map);
		if (loaded->things) {
			// Only the heart, taken from the template's own so that the bytes not understood are as the game's: +0 x, +4 y, +0x12 the
			// room type, +0x13 the player.
			constexpr std::uint32_t RoomRecord = 0xCC;
			constexpr std::size_t RoomRecordSize = 0x14;
			constexpr std::size_t RoomTypeOffset = 0x12;
			constexpr std::size_t RoomPlayerOffset = 0x13;
			std::optional<ThingRecord> heart;
			for (const auto& record : loaded->things->records) {
				if (record.kind == RoomRecord && record.bytes.size() == RoomRecordSize && record.bytes[RoomTypeOffset] == DungeonHeart && record.bytes[RoomPlayerOffset] == FirstKeeper) {
					heart = record;
					break;
				}
			}
			loaded->things->records.clear();
			if (heart) {
				std::memcpy(heart->bytes.data(), &centerX, sizeof(centerX));
				std::memcpy(heart->bytes.data() + 4, &centerY, sizeof(centerY));
				loaded->things->records.push_back(std::move(*heart));
			}
			else {
				error = Narrow(std::wstring(NewMapTemplate)) + " has no heart for keeper 1 to copy";
				return false;
			}
		}
		if (!SaveLevelAs(templateKwd, name, *loaded, error)) return false;
		std::string thumbnailError;
		if (!WriteThumbnail(loaded->map, name, thumbnailError)) Logger::Warning("Level files: no thumbnail for " + Narrow(name) + ": " + thumbnailError);
		return true;
	}

	bool WriteThumbnail(const MapData& map, const std::wstring& name, std::string& error) {
		// The game's own are <name>144.bmp in Data\editor\maps\Thumbnails, read at 0x53F7F0. 144 pixels a side, 24 bits.
		// [unverified: their size and depth, as the game's own could not be opened here]
		constexpr std::int32_t Side = 144;
		struct Color { std::uint8_t r, g, b; };
		const auto ownerColor = [](std::uint8_t owner) -> Color {
			switch (owner) {
				case 1: return { 230, 230, 230 };
				case 3: return { 200, 40, 40 };
				case 4: return { 50, 90, 220 };
				case 5: return { 40, 170, 60 };
				case 6: return { 220, 200, 40 };
				default: return { 150, 130, 100 };
			}
		};
		const auto dim = [](Color color, int percent) -> Color {
			return { static_cast<std::uint8_t>(color.r * percent / 100), static_cast<std::uint8_t>(color.g * percent / 100), static_cast<std::uint8_t>(color.b * percent / 100) };
		};
		const auto tileColor = [&](const Tile& tile) -> Color {
			switch (tile.terrain) {
				case 1: return { 30, 26, 24 };
				case 2: return { 105, 70, 40 };
				case 3: return { 150, 115, 75 };
				case 4: return { 40, 80, 170 };
				case 5: return { 230, 100, 20 };
				case 6: return { 235, 195, 40 };
				case 7: return { 120, 225, 225 };
				case 8: return dim(ownerColor(tile.owner), 55);
				case 9: return dim(ownerColor(tile.owner), 35);
				default: return ownerColor(tile.owner);
			}
		};
		const std::int32_t width = static_cast<std::int32_t>(map.width);
		const std::int32_t height = static_cast<std::int32_t>(map.height);
		const std::int32_t longest = (std::max)(width, height);
		if (longest == 0) {
			error = "the map has no tiles";
			return false;
		}
		// The whole map, keeping its shape, centered on black.
		const std::int32_t shownWidth = width * Side / longest;
		const std::int32_t shownHeight = height * Side / longest;
		const std::int32_t left = (Side - shownWidth) / 2;
		const std::int32_t top = (Side - shownHeight) / 2;
		constexpr std::uint32_t RowBytes = Side * 3;
		constexpr std::uint32_t PixelBytes = RowBytes * Side;
		std::vector<std::uint8_t> bytes;
		const auto put16 = [&bytes](std::uint16_t value) { bytes.push_back(static_cast<std::uint8_t>(value)); bytes.push_back(static_cast<std::uint8_t>(value >> 8)); };
		const auto put32 = [&bytes](std::uint32_t value) { PutU32(bytes, value); };
		put16(0x4D42);
		put32(14 + 40 + PixelBytes);
		put32(0);
		put32(14 + 40);
		put32(40);
		put32(Side);
		put32(Side);
		put16(1);
		put16(24);
		put32(0);
		put32(PixelBytes);
		put32(2835);
		put32(2835);
		put32(0);
		put32(0);
		// Bottom row first, blue green red.
		for (std::int32_t y = Side - 1; y >= 0; --y) {
			for (std::int32_t x = 0; x < Side; ++x) {
				Color color{ 0, 0, 0 };
				if (x >= left && x < left + shownWidth && y >= top && y < top + shownHeight) {
					const auto tileX = (x - left) * width / shownWidth;
					const auto tileY = (y - top) * height / shownHeight;
					color = tileColor(map.tiles[static_cast<std::size_t>(tileY) * map.width + static_cast<std::size_t>(tileX)]);
				}
				bytes.push_back(color.b);
				bytes.push_back(color.g);
				bytes.push_back(color.r);
			}
		}
		return WriteBytes(StockLevels / "Thumbnails" / (name + L"144.bmp"), bytes, error);
	}
	std::vector<TerrainType> ReadTerrainTypes(std::string& error) {
		std::vector<TerrainType> types;
		const auto table = ReadDefinitions("Data\\editor\\Terrain.kwd", TerrainSection, error);
		if (!table) return types;
		if (table->recordSize <= TerrainIdOffset) {
			error = "Terrain.kwd has records of " + std::to_string(table->recordSize) + " bytes, too short to hold an id";
			return types;
		}
		for (std::size_t i = 0; i < table->names.size(); ++i) {
			if (table->names[i].empty()) continue;
			const auto& record = table->records[i];
			const auto icon = static_cast<std::uint16_t>(record[TerrainIconOffset] | (record[TerrainIconOffset + 1] << 8));
			types.push_back({ record[TerrainIdOffset], table->names[i], icon });
		}
		std::sort(types.begin(), types.end(), [](const TerrainType& a, const TerrainType& b) { return a.id < b.id; });
		return types;
	}

	void CheckAllLevels(bool makeCopies) {
		std::error_code code;
		const fs::path stock = "Data\\editor\\maps";
		std::size_t passed = 0;
		std::size_t failed = 0;
		std::size_t made = 0;
		for (const auto& entry : fs::directory_iterator(stock, code)) {
			if (Lower(entry.path().extension().string()) != ".kwd") continue;
			bool copyMade = false;
			std::string error;
			if (CopyLevel(entry.path(), makeCopies, copyMade, error)) {
				++passed;
				if (copyMade) ++made;
			}
			else {
				++failed;
				Logger::Warning("Level files: " + entry.path().filename().string() + " failed: " + error);
			}
		}
		if (code) Logger::Warning("Level files: could not list " + stock.string() + " (" + code.message() + ").");
		Logger::Info("Level files: " + std::to_string(passed) + " levels would write back byte for byte, " + std::to_string(failed) + " failed. "
			+ std::to_string(made) + " new copies in " + Paths::Maps().string() + ".");

		std::string terrainError;
		std::string terrainIds;
		for (const auto& terrain : ReadTerrainTypes(terrainError)) terrainIds += (terrainIds.empty() ? "" : ", ") + std::to_string(terrain.id) + " " + terrain.name;
		if (!terrainError.empty()) Logger::Warning("Level files: " + terrainError);
		Logger::Debug("Level files: terrain ids: " + terrainIds + ".");

		struct Shared { const char* file; std::uint32_t section; };
		for (const auto& shared : { Shared{ "Terrain.kwd", 0x6E }, Shared{ "Rooms.kwd", 0x78 }, Shared{ "Traps.kwd", 0x82 }, Shared{ "Doors.kwd", 0x8C },
			Shared{ "KeeperSpells.kwd", 0x96 }, Shared{ "Creatures.kwd", 0xAA }, Shared{ "Objects.kwd", 0xF0 } }) {
			std::string error;
			const auto table = ReadDefinitions(fs::path("Data\\editor") / shared.file, shared.section, error);
			if (!table) {
				Logger::Warning("Level files: " + error);
				continue;
			}
			std::string names;
			for (const auto& name : table->names) names += (names.empty() ? "" : ", ") + name;
			Logger::Debug("Level files: " + std::string(shared.file) + ", " + std::to_string(table->names.size()) + " records of " + std::to_string(table->recordSize) + " bytes: " + names + ".");
		}
	}
}
