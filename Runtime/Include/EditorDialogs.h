#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
// Windows' own windows for the level editor, in place of the game's menus where there is more to show than they hold
// (docs/game/level-editor.md, Creature types). They run on the game's thread, driven by its own message loop (0x5B2DD0), so
// nothing here runs at the same time as the game and nothing here touches the game: what they change goes back to the level
// editor, which puts it in its document. config.yml's editor_dark_windows gives them dark colors.
namespace KeeperForge2::EditorDialogs {
	struct NamedId {
		std::uint32_t id = 0;
		std::string name;
	};
	// One placed creature, for the window opened on it (a right click on it in the world). Its stats are its type's, shared by
	// every creature of the type unless it is given a type of its own; these are what the level keeps for it alone.
	struct UnitData {
		// The editor's handle for it (its tag in the world).
		std::uint16_t id = 0;
		// Its type id, as its record in the level has it.
		std::uint8_t type = 0;
		std::uint8_t level = 1;
		// Its hit points (exact, as KeeperForge2 keeps them beside the level, or what the level's percent gives).
		std::uint32_t points = 1;
		std::uint16_t gold = 0;
		// Where and whose, for the window's title.
		std::wstring where;
	};
	// What OK or Apply gives back for that creature.
	struct UnitChange {
		std::uint16_t id = 0;
		// Its type id: a new one if it was given stats of its own.
		std::uint8_t type = 0;
		std::uint8_t level = 1;
		std::uint32_t points = 1;
		std::uint16_t gold = 0;
		// It becomes a dead body.
		bool dead = false;
	};
	// What the creature properties window shows and edits. A copy: the window keeps its own until it closes.
	struct CreatureDialogData {
		// Set when the window is for one placed creature rather than a type from the side panel.
		std::optional<UnitData> unit;
		// A creature's most health at each level, as a percent of its type's Health (index the level; level 1 is 100), from the
		// game's settings, for the window's hit points and percent.
		std::array<std::int32_t, 11> levelHealthPercent{};
		// For a type from the side panel: the level new creatures of each type (by id less one) are placed at, and whether they are
		// placed as dead bodies. The editor's placing settings: changed at once through setPlacingDefaults, as "Places as" is.
		std::vector<std::uint8_t> placeLevels;
		std::vector<bool> placeDead;
		std::function<void(std::uint8_t type, std::uint8_t level, bool dead)> setPlacingDefaults;
		// The level's creature list, by type id less one.
		std::vector<std::vector<std::uint8_t>> records;
		// For the lists that name other things by id: objects (Lair Object), effects, rooms (Attractions).
		std::vector<NamedId> objects;
		std::vector<NamedId> effects;
		std::vector<NamedId> rooms;
		// CreatureSpells.kwd, for Available Attacks.
		std::vector<NamedId> spells;
		// The lowest type id Delete Type may take away. Ids are places in the list, so only the last type can go, and only one at
		// or after this (the game's own types are known by id in its code).
		std::uint32_t firstDeletable = 0xFFFFFFFF;
		// Asked before deleting a type: what in the level uses it, or empty if nothing does. Called on the game's thread.
		std::function<std::string(std::uint8_t type)> deleteProblem;
		// What a placed creature of each type (by id less one) may be besides its plain kind, for the window's "Places as" list,
		// which only shows for a type that has one: a Unique type, or the mercenary textures heroes have.
		enum class Variant : std::uint8_t { None, Unique, Mercenary };
		std::vector<Variant> variants;
		// Which types place as their variant now, by id less one.
		std::vector<bool> placesVariant;
		// Called when "Places as" changes: the editor's placing setting, not the level's data, so it takes effect at once.
		std::function<void(std::uint8_t type, bool variant)> setPlacesVariant;
	};
	// Opens the creature properties window on this type (an id from 1), over the game's window, for one placed creature if data
	// has one. If it is open already, it turns to this type (and creature, or none) and comes to the front, keeping its own copy
	// of the list and whatever was being changed there. False if it could not be made.
	bool OpenCreatureProperties(CreatureDialogData data, std::uint8_t type);
	// The whole list as given back with OK or Apply, if it was given back since the last call: changed, renamed, added or
	// deleted types.
	std::optional<std::vector<std::vector<std::uint8_t>>> TakeCreatureList();
	// The change to the placed creature the window is open on, if OK or Apply gave one since the last call. The editor takes the
	// creature list first, as a new type the change names comes with it.
	std::optional<UnitChange> TakeUnitChange();
	// The editor made that creature again (or turned it into a dead body): the window follows the new one, or, with none, goes
	// back to showing its type.
	void SetUnit(std::optional<UnitData> unit);
	// The editor's handle of the placed creature the window is open on, if it is open on one.
	std::optional<std::uint16_t> UnitId();
	// That creature as it is now (after undo or redo): its row (level, gold, health) shows these, leaving the type's fields as
	// they are; with none (it is gone), the window goes back to showing its type. Nothing if the window is not on a creature.
	void RefreshUnit(std::optional<UnitData> unit);
	// Whether the window's creature is the editor's chosen (outlined) one, shown in its title as "[Outlined]". Cheap to call
	// every tick: the title only changes when this does.
	void SetUnitSelected(bool selected);
	bool CreaturePropertiesOpen();
	// Closes the creature properties window (and any window it has open): a change to its creature not yet taken is forgotten,
	// and a list already given back with OK or Apply stays. With keepEdits, what it held (its working copy of the list, the
	// fields changed and not yet applied, its creature's row, where it was) is kept for ReopenCreatureProperties; without, it is
	// dropped, as Cancel drops it. The smaller windows' own fields are not kept.
	void CloseCreatureProperties(bool keepEdits = false);
	// Opens the window as OpenCreatureProperties does, then puts back what the last CloseCreatureProperties(true) kept, if the
	// level's list is still the one it was working from (else it opens as new). What was kept is used once.
	bool ReopenCreatureProperties(CreatureDialogData data, std::uint8_t type);
	// Drops what the last close kept (the creature it was for will not come back).
	void ForgetKeptEdits();
	// Closes every window without giving anything back, and forgets what was given back and not yet taken (a level is ending).
	void CloseAll();
}
