#include "CreatureRegistry.h"
#include "LevelFiles.h"
#include "Logger.h"
#include "ModData.h"
#include "Paths.h"
#include "StringUtils.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace KeeperForge2 {
	namespace {
		// The shared list, as a path under the game's Data folder.
		constexpr const char* SharedListPath = "editor\\Creatures.kwd";
		// Type ids are bytes; 0 is no type.
		constexpr std::size_t MostTypes = 255;
		constexpr std::size_t NameSize = LevelFiles::CreatureOffset::NameLength;

		class AddedType final : public ICreatureType {
			public:
				std::uint8_t id = 0;
				// The stock type it is a copy of (through copies of copies), 0 if it is not a copy.
				std::uint8_t copiedFrom = 0;
				std::string mod;
				std::vector<std::uint8_t> record;
				std::uint8_t GetId() const override { return id; }
				std::uint8_t* GetRecord() override { return record.data(); }
				std::size_t GetRecordSize() const override { return record.size(); }
				std::string Name() const {
					const auto* text = reinterpret_cast<const char*>(record.data() + LevelFiles::CreatureOffset::Name);
					return std::string(text, strnlen(text, NameSize));
				}
		};

		// The shared list the game would read, read once when the first type is added.
		struct BaseList {
			fs::path path;
			LevelFiles::File file;
			LevelFiles::ListData list;
		};
		std::optional<BaseList> base;
		bool baseTried = false;
		std::vector<std::unique_ptr<AddedType>> added;
		std::string currentMod = "a mod";
		fs::path mergedPath;
		bool mergedTried = false;
		// Set once any list with the added types has been made: a type added after that would be missing from it.
		bool listsInUse = false;

		const BaseList* Base() {
			if (baseTried) return base ? &*base : nullptr;
			baseTried = true;
			const auto path = ModData::Find(SharedListPath);
			if (path.empty()) {
				Logger::Warning("Creature registry: the game's creature list (Data\\editor\\Creatures.kwd) was not found; mods cannot add creature types.");
				return nullptr;
			}
			std::string error;
			auto file = LevelFiles::ReadFile(path, error);
			const auto* section = file ? file->Find(LevelFiles::CreaturesSection) : nullptr;
			auto list = section ? LevelFiles::DecodeList(*section, LevelFiles::GameCreatureRecordSize, error) : std::nullopt;
			if (!list) {
				Logger::Warning("Creature registry: could not read the creature list " + path.string() + " (" + (error.empty() ? "no creature section" : error)
					+ "); mods cannot add creature types.");
				return nullptr;
			}
			base = BaseList{ path, std::move(*file), std::move(*list) };
			return &*base;
		}

		// The record of a type by id: a base type or one added before.
		const std::vector<std::uint8_t>* RecordOf(std::uint8_t id) {
			const auto* list = Base();
			if (list == nullptr || id == 0) return nullptr;
			if (id <= list->list.records.size()) return &list->list.records[id - 1u];
			for (const auto& type : added) if (type->id == id) return &type->record;
			return nullptr;
		}

		AddedType* Add(std::vector<std::uint8_t> record, const std::string& how) {
			const auto* list = Base();
			if (list == nullptr) return nullptr;
			if (listsInUse) {
				Logger::Warning("Creature registry: " + currentMod + " added a creature type after the game read its creature list; it is left out.");
				return nullptr;
			}
			const auto total = list->list.records.size() + added.size();
			if (total >= MostTypes) {
				Logger::Warning("Creature registry: " + currentMod + " could not add a creature type, there are already " + std::to_string(total) + ".");
				return nullptr;
			}
			record.resize(list->list.recordSize, 0);
			auto type = std::make_unique<AddedType>();
			type->id = static_cast<std::uint8_t>(total + 1);
			type->mod = currentMod;
			type->record = std::move(record);
			type->record[LevelFiles::CreatureOffset::TypeId] = type->id;
			Logger::Info("Creature registry: " + currentMod + " added creature type " + std::to_string(type->id) + " \"" + type->Name() + "\" (" + how + ").");
			added.push_back(std::move(type));
			return added.back().get();
		}

		class Registry final : public ICreatureRegistry {
			public:
				std::size_t GetBaseTypeCount() const override {
					const auto* list = Base();
					return list != nullptr ? list->list.records.size() : 0;
				}
				ICreatureType* AddCopy(std::uint8_t from, const char* name) override {
					const auto* source = RecordOf(from);
					if (source == nullptr) {
						Logger::Warning("Creature registry: " + currentMod + " asked to copy creature type " + std::to_string(from) + ", which does not exist.");
						return nullptr;
					}
					auto record = *source;
					if (name != nullptr) {
						std::fill_n(record.begin() + LevelFiles::CreatureOffset::Name, NameSize, std::uint8_t{ 0 });
						const auto length = std::min<std::size_t>(std::strlen(name), NameSize - 1);
						std::memcpy(record.data() + LevelFiles::CreatureOffset::Name, name, length);
					}
					auto* type = Add(std::move(record), "a copy of type " + std::to_string(from));
					if (type != nullptr) {
						type->copiedFrom = from;
						for (const auto& other : added) if (other->id == from) type->copiedFrom = other->copiedFrom;
					}
					return type;
				}
				ICreatureType* AddRecord(const void* record, std::size_t size) override {
					if (record == nullptr || size == 0) return nullptr;
					const auto* bytes = static_cast<const std::uint8_t*>(record);
					return Add(std::vector<std::uint8_t>(bytes, bytes + size), "its own record");
				}
				ICreatureType* Find(const char* name) override {
					if (name == nullptr) return nullptr;
					const auto wanted = StringUtils::ToLower(name);
					for (const auto& type : added) if (StringUtils::ToLower(type->Name()) == wanted) return type.get();
					return nullptr;
				}
		};
		Registry registry;

		class ModContentImpl final : public IModContent {
			public:
				ICreatureRegistry& Creatures() override { return registry; }
		};
		ModContentImpl content;

		// The base list with the added types after it, written to KeeperForge2\cache\Creatures.kwd.
		fs::path MakeMerged() {
			mergedTried = true;
			listsInUse = true;
			const auto* list = Base();
			if (list == nullptr) return {};
			auto merged = list->list;
			for (const auto& type : added) merged.records.push_back(type->record);
			auto file = list->file;
			auto* section = file.Find(LevelFiles::CreaturesSection);
			if (section == nullptr) return {};
			LevelFiles::EncodeList(merged, *section);
			std::error_code error;
			const auto folder = fs::absolute(Paths::Root() / "cache", error);
			fs::create_directories(folder, error);
			const auto path = folder / "Creatures.kwd";
			std::string writeError;
			if (!LevelFiles::WriteFile(path, file, writeError)) {
				Logger::Warning("Creature registry: could not write " + path.string() + " (" + writeError + "); the game reads its creature list without the added types.");
				return {};
			}
			Logger::Info("Creature registry: the game's creature list has " + std::to_string(list->list.records.size()) + " types and " + std::to_string(added.size())
				+ " added by mods (from " + list->path.string() + ").");
			return path;
		}
	}

	IModContent& CreatureRegistry::Content() { return content; }

	void CreatureRegistry::SetCurrentMod(const std::string& name) { currentMod = name; }

	fs::path CreatureRegistry::ListFor(const std::string& underData) {
		if (added.empty()) return {};
		if (StringUtils::ToLower(underData) != StringUtils::ToLower(SharedListPath)) return {};
		if (!mergedTried) mergedPath = MakeMerged();
		return mergedPath;
	}

	std::size_t CreatureRegistry::AddTo(LevelFiles::ListData& list, const std::string& what) {
		listsInUse = true;
		std::size_t count = 0;
		for (const auto& type : added) {
			const auto name = StringUtils::ToLower(type->Name());
			// Already there: a list our editor saved with the mods' types in it (or a type of the level's own by that name).
			const bool present = std::ranges::any_of(list.records, [&name](const std::vector<std::uint8_t>& record) {
				const auto* text = reinterpret_cast<const char*>(record.data());
				return StringUtils::ToLower(std::string(text, strnlen(text, (std::min)(record.size(), NameSize)))) == name;
			});
			if (present) continue;
			if (list.records.size() >= MostTypes) {
				Logger::Warning("Creature registry: " + what + " already has " + std::to_string(list.records.size()) + " creature types; \"" + type->Name()
					+ "\" and the mods' types after it are left out.");
				break;
			}
			auto record = type->record;
			if (list.recordSize != 0) record.resize(list.recordSize, 0);
			if (record.size() <= LevelFiles::CreatureOffset::TypeId) continue;
			const auto id = static_cast<std::uint8_t>(list.records.size() + 1);
			record[LevelFiles::CreatureOffset::TypeId] = id;
			// Ids follow the list's own types. A level with types of its own past the shared ones moves the mods' types on.
			if (id != type->id) {
				Logger::Warning("Creature registry: in " + what + " the mod type \"" + type->Name() + "\" is type " + std::to_string(id) + " (it is "
					+ std::to_string(type->id) + " elsewhere), as that list has creature types of its own.");
			}
			list.records.push_back(std::move(record));
			++count;
		}
		return count;
	}

	fs::path CreatureRegistry::LevelListFor(const fs::path& path) {
		if (added.empty()) return {};
		const auto fileName = StringUtils::ToLower(path.filename().string());
		if (!fileName.ends_with("creatures.kld") && !fileName.ends_with("globals.kld")) return {};
		std::string error;
		auto file = LevelFiles::ReadFile(path, error);
		auto* section = file ? file->Find(LevelFiles::CreaturesSection) : nullptr;
		if (section == nullptr) return {};
		auto list = LevelFiles::DecodeList(*section, LevelFiles::GameCreatureRecordSize, error);
		if (!list) {
			Logger::Warning("Creature registry: could not read the level's creature list " + path.string() + " (" + error + "); it is read without the mods' types.");
			return {};
		}
		if (AddTo(*list, path.string()) == 0) return {};
		LevelFiles::EncodeList(*list, *section);
		// One cache file per level list, by the order they were first opened, so two levels' lists of the same name do not meet.
		static std::vector<std::string> seen;
		const auto key = StringUtils::ToLower(path.lexically_normal().string());
		auto place = std::ranges::find(seen, key);
		if (place == seen.end()) place = seen.insert(seen.end(), key);
		std::error_code fsError;
		const auto folder = fs::absolute(Paths::Root() / "cache" / "levels", fsError);
		fs::create_directories(folder, fsError);
		const auto target = folder / (std::to_string(place - seen.begin()) + "-" + path.filename().string());
		if (!LevelFiles::WriteFile(target, *file, error)) {
			Logger::Warning("Creature registry: could not write " + target.string() + " (" + error + "); the level reads its creature list without the mods' types.");
			return {};
		}
		Logger::Info("Creature registry: the level's creature list " + path.string() + " is read with the mods' creature types added.");
		return target;
	}

	std::size_t CreatureRegistry::SharedTypeCount() {
		const auto* list = Base();
		return list != nullptr ? list->list.records.size() + added.size() : 0;
	}

	std::uint8_t CreatureRegistry::CopiedFrom(const std::string& name) {
		const auto wanted = StringUtils::ToLower(name);
		for (const auto& type : added) if (StringUtils::ToLower(type->Name()) == wanted) return type->copiedFrom;
		return 0;
	}
}
