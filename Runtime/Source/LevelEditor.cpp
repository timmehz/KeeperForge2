#include "LevelEditor.h"
#include "Config.h"
#include "CreatureRegistry.h"
#include "EditorDialogs.h"
#include "EditorPanel.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "LevelFiles.h"
#include "Logger.h"
#include "Memory.h"
#include "Renderer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }
		// Game structures are packed, so their fields are read and written byte-wise.
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// ---- The Single Player menu.
		constexpr std::int32_t EditorButtonId = 9100;
		constexpr std::int32_t ContinueCampaignButtonId = 0x237;
		constexpr std::int32_t SkirmishButtonId = 8;
		constexpr std::int32_t LoadGameButtonId = 7;
		constexpr std::int32_t ButtonListEnd = -1;
		constexpr std::size_t MaxButtons = 32;
		// Continue Campaign ends at 760. Editor takes the next row, and the rows below move down, keeping the game's
		// larger step before Skirmish.
		constexpr std::int16_t EditorButtonY = 768;
		constexpr std::int16_t SkirmishButtonY = 1032;
		constexpr std::int16_t LoadGameButtonY = 1212;
		// The game bakes this window's text images in list order: New Campaign 0, Continue 1, Skirmish 2, Load Game 3. Ours
		// goes last in the list, so theirs keep their numbers, whatever its place on screen.
		constexpr std::int32_t EditorImageIndex = 4;
		// A text id no game text uses: the baker asks for the button's text by id, and this one gets ours.
		constexpr std::int32_t EditorTextId = 60000;
		// The high word of a menu click's second argument is the button's image number.
		constexpr std::uint32_t ClickImageShift = 16;
		constexpr std::size_t ClickSecondArgument = Game::Offset::ButtonConfigClickArgument + 4;

		using ClickFn = void(__cdecl*)(std::int32_t, std::int32_t, std::uint8_t*);
		ClickFn gameSkirmishClick = nullptr;
		std::vector<std::uint8_t> buttonList;
		bool installed = false;
		// The Editor button was pressed, and the next skirmish started is a level to edit.
		bool armed = false;
		bool inSession = false;
		bool revealPending = false;
		bool revealUsable = false;
		// The minimap stayed black unless the map was revealed again a little after the first tick (tested: this filled it in).
		// Mercenaries from the level's list are checked again a little later too, as the first try did not last (tested).
		constexpr std::uint32_t RevealAgainAfterTicks = 8;
		std::uint32_t revealAgainIn = 0;
		constexpr std::uint32_t MercenaryRecheckAfterTicks = 4;
		std::uint32_t mercenaryRecheckIn = 0;

		// Once a run, the first time the editor is opened, the level files are checked (and nothing written), so a log from
		// any use of the editor says whether they can be read and saved.
		bool levelsChecked = false;
		void __cdecl OnEditorClick(std::int32_t first, std::int32_t second, std::uint8_t* frontEnd) {
			armed = true;
			if (!levelsChecked) {
				levelsChecked = true;
				LevelFiles::CheckAllLevels(false);
			}
			gameSkirmishClick(first, second, frontEnd);
		}
		void __cdecl OnSkirmishClick(std::int32_t first, std::int32_t second, std::uint8_t* frontEnd) {
			armed = false;
			gameSkirmishClick(first, second, frontEnd);
		}

		using TextByIdFn = const char*(__cdecl*)(std::int32_t id);
		using ConvertTextFn = void(__cdecl*)(const wchar_t* text, void* out, std::int32_t size);
		TextByIdFn originalTextById = nullptr;
		// The baker keeps the pointer while it works, so the text stays here.
		std::array<char, 0x40> editorText{};
		const char* __cdecl TextByIdDetour(std::int32_t id) {
			// The second tab holds terrain in a session. The game's button config names both ids: the first its hover text.
			constexpr std::int32_t SpellsTabHoverText = 2157;
			constexpr std::int32_t SpellsTabText = 175;
			if (inSession && (id == SpellsTabHoverText || id == SpellsTabText)) {
				static decltype(editorText) terrainTabText{};
				if (terrainTabText[0] == 0) GameFunction<ConvertTextFn>(Game::Address::ConvertText)(L"Terrain", terrainTabText.data(), static_cast<std::int32_t>(terrainTabText.size()));
				return terrainTabText.data();
			}
			if (id != EditorTextId) return originalTextById(id);
			if (editorText[0] == 0) GameFunction<ConvertTextFn>(Game::Address::ConvertText)(L"Editor", editorText.data(), static_cast<std::int32_t>(editorText.size()));
			return editorText.data();
		}

		// Copies the menu's list with the rows below Continue Campaign moved down, and Editor before the end marker.
		bool BuildButtonList(const std::uint8_t* buttons) {
			std::size_t count = 0;
			const std::uint8_t* skirmish = nullptr;
			bool continueFound = false;
			bool loadFound = false;
			for (; count < MaxButtons; ++count) {
				const auto* entry = buttons + count * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				if (id == ButtonListEnd) break;
				if (id == EditorButtonId) return false;
				if (id == SkirmishButtonId) skirmish = entry;
				if (id == ContinueCampaignButtonId) continueFound = true;
				if (id == LoadGameButtonId) loadFound = true;
			}
			if (count == MaxButtons || skirmish == nullptr || !continueFound || !loadFound) return false;
			gameSkirmishClick = reinterpret_cast<ClickFn>(Load<std::uint32_t>(skirmish + Game::Offset::ButtonConfigClick));

			buttonList.assign((count + 2) * Game::ButtonConfigSize, 0);
			std::memcpy(buttonList.data(), buttons, count * Game::ButtonConfigSize);
			for (std::size_t index = 0; index < count; ++index) {
				auto* entry = buttonList.data() + index * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				if (id == SkirmishButtonId) {
					Store<std::int16_t>(entry + Game::Offset::ButtonConfigY, SkirmishButtonY);
					Store<std::uint32_t>(entry + Game::Offset::ButtonConfigClick, reinterpret_cast<std::uint32_t>(&OnSkirmishClick));
				}
				else if (id == LoadGameButtonId) {
					Store<std::int16_t>(entry + Game::Offset::ButtonConfigY, LoadGameButtonY);
				}
			}
			// A copy of Skirmish, so it is drawn with the game's own images and glow, and opens the same way.
			auto* editor = buttonList.data() + count * Game::ButtonConfigSize;
			std::memcpy(editor, skirmish, Game::ButtonConfigSize);
			Store<std::int32_t>(editor + Game::Offset::ButtonConfigId, EditorButtonId);
			Store<std::int16_t>(editor + Game::Offset::ButtonConfigY, EditorButtonY);
			Store<std::uint32_t>(editor + Game::Offset::ButtonConfigClick, reinterpret_cast<std::uint32_t>(&OnEditorClick));
			Store<std::int32_t>(editor + Game::Offset::ButtonConfigTextId, EditorTextId);
			Store<std::int32_t>(editor + Game::Offset::ButtonConfigTextIndex, EditorImageIndex);
			const auto second = Load<std::uint32_t>(editor + ClickSecondArgument);
			Store<std::uint32_t>(editor + ClickSecondArgument, (second & 0xFFFF) | (static_cast<std::uint32_t>(EditorImageIndex) << ClickImageShift));
			std::memcpy(editor + Game::ButtonConfigSize, buttons + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			return true;
		}

		// ---- Holding the world. Actions that are not camera moves or leaving the level are dropped, or clicks would dig,
		// build and pick up things in a world that is not running.
		constexpr std::int32_t ActionLeaveLevel = 1;
		constexpr std::int32_t ActionMoveCamera = 4;
		constexpr std::int32_t ActionCameraZoom = 7;
		constexpr std::int32_t ActionCameraTurn = 8;
		constexpr std::int32_t ActionLeaveLevelOther = 125;
		constexpr std::size_t ActionListHeader = 0x11;
		constexpr std::size_t ActionSize = 0x12;
		constexpr std::size_t MaxActions = 0xFF;
		std::array<std::uint8_t, ActionListHeader + MaxActions * ActionSize> keptActions{};
		// Each kind dropped is logged once, as a later stage will want to know what was asked for.
		std::array<bool, 0x100> droppedLogged{};
		std::int32_t savedMenuOpen = 0;
		std::uint8_t* heldSession = nullptr;
		// The turn drawing sees is kept still, as it is under a menu: animations and in-between positions go by it.
		bool turnHeld = false;
		std::uint32_t heldTurn = 0;

		bool KeepAction(std::int32_t kind) {
			return kind == ActionLeaveLevel || kind == ActionLeaveLevelOther || kind == ActionMoveCamera || kind == ActionCameraZoom || kind == ActionCameraTurn;
		}

		// Time into the current turn, thiscall (communication). Drawing makes its 0..256 blend between turns from it. In a
		// session it reads as one whole turn, which is the things' own current positions, so nothing moves between turns.
		using TimeIntoTurnFn = std::uint32_t(__fastcall*)(const std::uint8_t* communication);
		TimeIntoTurnFn originalTimeIntoTurn = nullptr;
		std::uint32_t __fastcall TimeIntoTurnDetour(const std::uint8_t* communication) {
			if (!inSession) return originalTimeIntoTurn(communication);
			const auto* session = Load<const std::uint8_t*>(communication + Game::Offset::CommunicationSession);
			return 1000 / Load<std::uint32_t>(session + Game::Offset::SessionTurnsPerSecond);
		}

		// The options menu opening or closing, cdecl (0, 0, interface). Outside multiplayer it asks for the pause through an
		// action and takes the session's menu field for whether it is open. Tested: with that field set for real, none of
		// the menu's buttons worked in a held world. Multiplayer's menu pauses nothing and keeps its own open flag, and the
		// world is held anyway, so a level being edited uses that one. The mode is only read once in there.
		using ToggleOptionsFn = void(__cdecl*)(std::int32_t, std::int32_t, std::uint8_t* playerInterface);
		ToggleOptionsFn originalToggleOptions = nullptr;
		void __cdecl ToggleOptionsDetour(std::int32_t first, std::int32_t second, std::uint8_t* playerInterface) {
			if (!inSession) {
				originalToggleOptions(first, second, playerInterface);
				return;
			}
			auto& mode = *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::PlayMode));
			const auto saved = mode;
			mode = Game::PlayModeMultiplayer;
			originalToggleOptions(first, second, playerInterface);
			mode = saved;
		}

		// ---- Painting terrain. What gets saved is the level's own map, kept here and changed along with the world.
		std::wstring documentLevel;
		std::optional<LevelFiles::LoadedMap> document;
		bool documentChecked = false;
		std::vector<LevelFiles::TerrainType> terrains;
		std::size_t terrainChoice = 0;
		// Player numbers: 1 the Good player, 2 neutral, 3 and up the keepers.
		constexpr std::uint8_t FirstOwner = 1;
		constexpr std::uint8_t LastOwner = 7;
		constexpr std::uint8_t NeutralOwner = 2;
		std::uint8_t ownerChoice = Game::FirstKeeperPlayer;

		struct TileChange {
			std::int32_t x;
			std::int32_t y;
			LevelFiles::Tile before;
		};
		std::vector<TileChange> stroke;
		// A creature placed (undoing removes it), deleted (undoing brings it back) or changed (undoing puts back this record),
		// and its place in the level's list.
		enum class CreatureChangeKind { Placed, Deleted, Changed };
		struct CreatureChange {
			CreatureChangeKind kind;
			std::size_t index;
			LevelFiles::ThingRecord record;
		};
		// One step of undo: tiles and things changed together, each list in the order it was done. Undoing goes back through the
		// tiles, then back through the things, so a thing comes back after the ground it stands on.
		struct UndoStep {
			std::vector<TileChange> tiles;
			std::vector<CreatureChange> things;
			// The level's exact hit points (LevelFiles::CreatureHealth) as they were before the step, when it changed them.
			std::optional<std::vector<LevelFiles::CreatureHealth>> health;
		};
		std::vector<UndoStep> undoSteps;
		// The exact hit points as of the last step pushed (or the level's load, or the last undo or redo): a step pushed while
		// they differ from these takes these along, so undoing it puts them back.
		std::vector<LevelFiles::CreatureHealth> healthAtLastStep;
		// What undo took back, for redo. Any new change clears it.
		std::vector<UndoStep> redoSteps;
		constexpr std::size_t MaxUndoSteps = 256;
		bool unsavedChanges = false;
		// Leaving with changes not saved is stopped once, with a warning; leaving again goes. A new change asks again.
		bool leaveWarned = false;
		// Things taken off tiles the stroke painted (as rock), which go into the stroke's undo step with its tiles.
		std::vector<CreatureChange> strokeThings;
		// What a left click does: paints, places the thing chosen on the panel, or puts down a thing being moved.
		enum class Tool { Terrain, Creature, Trap, Door, Object, ActionPoint, Move, DeadBody, EffectGenerator };
		Tool tool = Tool::Terrain;
		// How terrain goes down: a square brush dragged, a box dragged out and filled, a fill of the area of the same tile, or
		// copying a box and pasting it.
		enum class PaintMode { Brush, Box, Fill, Copy, Paste };
		PaintMode paintMode = PaintMode::Brush;
		// Brush sides, in tiles.
		constexpr std::array<std::int32_t, 4> BrushSizes{ 1, 3, 5, 7 };
		std::size_t brushChoice = 0;
		// The most tiles one box or fill paints, as each is the game's whole tile change.
		constexpr std::size_t MostTilesAtOnce = 16384;
		// Moving: the thing (by tag) or action point (by number) chosen from its menu, and the tool to go back to.
		std::uint16_t moveTag = 0;
		std::uint8_t moveActionPoint = 0;
		Tool toolBeforeMove = Tool::Terrain;
		// Copied with Copy: the box's tiles (none where a tile stays as it is, the heart and hero gates) and its things, with
		// places counted from the box's top left.
		struct Clipboard {
			std::int32_t width = 0;
			std::int32_t height = 0;
			std::vector<std::optional<LevelFiles::Tile>> tiles;
			std::vector<LevelFiles::ThingRecord> things;
		};
		std::optional<Clipboard> clipboard;
		// The Delete key, carried out after the world tick.
		bool deleteAtPointerWanted = false;
		// The panel shows and changes what each keeper may have, in place of choosing what to place.
		bool availabilityMode = false;
		std::uint8_t creatureChoice = 1;
		std::uint8_t trapChoice = 1;
		std::uint8_t doorChoice = 1;
		std::uint8_t objectChoice = 1;
		// Objects.kwd's names, by type less one [verified: types are the place in the file from 1, also at +0x35A of each record].
		std::vector<std::string> objectNames;
		// Keeper spells for a spell book, by id less one (KeeperSpells.kwd's order from 1, as Create Imp is 4).
		std::vector<std::string> keeperSpellNames;
		// For the creature properties window: Effects.kwd and Rooms.kwd by id less one [verified: Warlock's slap and entrance effects
		// 19 and 165 are its own as the official editor shows them; room ids match rooms.md].
		std::vector<std::string> effectNames;
		std::vector<std::string> roomNames;
		// CreatureSpells.kwd by id less one [verified: Warlock's attacks 1, 14 and 2 are its fireball, heal and firebomb].
		std::vector<std::string> creatureSpellNames;
		// Items at the ends of the Workshop and Objects lists that go from one to the other, and the action point tool.
		constexpr std::size_t ObjectsPageItem = 2000;
		constexpr std::size_t WorkshopPageItem = 2001;
		constexpr std::size_t ActionPointItem = 2002;
		constexpr std::size_t DeadBodyItem = 2003;
		constexpr std::size_t EffectGeneratorItem = 2004;
		// What a right click on a spell book or gold on the panel has chosen: an index into the lists below.
		std::size_t spellBookChoice = 0;
		std::size_t goldChoice = 3;
		constexpr std::array<std::int32_t, 7> GoldAmounts{ 50, 100, 250, 500, 1000, 2500, 5000 };
		// A box is dragged out from the tile pressed on to the tile let go on: an action point, a box of terrain, or what to copy.
		enum class DragPurpose { ActionPoint, PaintBox, Copy };
		DragPurpose dragPurpose = DragPurpose::ActionPoint;
		bool actionPointDragging = false;
		std::int32_t actionPointStartX = 0;
		std::int32_t actionPointStartY = 0;
		std::int32_t actionPointEndX = 0;
		std::int32_t actionPointEndY = 0;
		std::uint8_t actionPointDeleteWanted = 0;
		// By id less one, as ids follow the file's order [verified: stock levels' records read as sensible things].
		std::vector<std::string> creatureNames;
		std::vector<std::string> trapNames;
		std::vector<std::string> doorNames;
		// The Workshop tab holds both, so a door's item value is its place in its file plus this.
		constexpr std::size_t DoorItemBase = 1000;
		// Variants, by item value, cycled by a right click on the panel: traps and doors as blueprints, and creatures as their
		// Unique or mercenary kind.
		std::map<std::size_t, bool> blueprintItems;
		std::map<std::size_t, bool> alternateCreatures;
		// A creature's id to its Unique's id, from the names in Creatures.kwd.
		std::map<std::uint8_t, std::uint8_t> uniqueOfCreature;
		// A room tile's sale, asked for from the menu.
		bool roomSaleWanted = false;
		std::int32_t roomSaleX = 0;
		std::int32_t roomSaleY = 0;
		bool creaturesUsable = false;
		std::uint16_t deleteWanted = 0;
		// The creature chosen with a left click on it: Delete deletes it, and holding the button down drags it, following the
		// pointer, to another tile, where letting go puts it (or puts it back where it was if it cannot stand there).
		std::uint16_t selectedCreature = 0;
		std::uint16_t clickCreature = 0;
		bool creatureDragging = false;
		std::int32_t dragFromX = 0;
		std::int32_t dragFromY = 0;
		std::int32_t dragToX = 0;
		std::int32_t dragToY = 0;
		// While dragging, the creature is made again on each tile it can stand on under the pointer, so it follows it; its record
		// in the list stays as it was until the button is let go. Its record then, the tile it is shown on, and its hit points.
		std::optional<LevelFiles::ThingRecord> dragRecord;
		std::int32_t dragShownX = 0;
		std::int32_t dragShownY = 0;
		std::int32_t dragPoints = 0;
		// Esc while dragging: put it back where it was and let go of it.
		bool dragCancelWanted = false;
		// The creature window was open on the dragged creature: it follows it once the drag ends (made again, it has a new tag).
		bool dragWindowFollows = false;
		// The record of the creature the creature window was open on when taking that creature away closed it (Delete, rock painted
		// over it, made a dead body): undo or redo that brings it back opens the window on it again. Forgotten when a creature window
		// is opened or the level changes.
		std::optional<LevelFiles::ThingRecord> closedWindowRecord;
		// Placing settings for new creatures of a type, from the creature window opened on the side panel: their level, and
		// whether they go down as dead bodies.
		std::map<std::uint8_t, std::uint8_t> placeLevels;
		std::set<std::uint8_t> placeDeadTypes;
		// How many creature types the world was made with: a type past these (added since, or a creature's own) is not in the
		// game's data until the level is loaded again, so nothing of it is made in the world before then.
		std::size_t loadedCreatureTypes = 0;
		std::uint16_t levelChangeTag = 0;
		std::uint8_t levelChangeTo = 0;
		// Asked for from the interface's input, and carried out in the world tick, the one place the game changes the world.
		bool leftDown = false;
		bool clickWaiting = false;
		std::int32_t clickX = 0;
		std::int32_t clickY = 0;
		bool undoWanted = false;
		bool redoWanted = false;
		bool saveWanted = false;
		// Save as: typing a name on the palette, then saving under it in Data\editor\maps.
		bool naming = false;
		std::string nameText;
		bool saveAsWanted = false;
		constexpr std::size_t MaxLevelName = 40;
		// What the typed text is for. A new map asks for its name, then its width, then its height, one at a time.
		enum class Prompt { SaveAsName, NewMapName, NewMapWidth, NewMapHeight, LevelText, Gold, ActionPointName, TriggerField };
		Prompt prompt = Prompt::SaveAsName;
		// For LevelText the row of LevelTextRows, for Gold the player record.
		std::size_t promptIndex = 0;
		constexpr std::size_t MaxGoldDigits = 7;
		// Typed for a trigger or action: bytes 0 to 7 by their place, then its value and its repeat count.
		constexpr std::size_t FieldValue = 8;
		constexpr std::size_t FieldRepeat = 9;
		constexpr std::array<std::pair<LevelFiles::LevelText, const wchar_t*>, 5> LevelTextRows{ {
			{ LevelFiles::LevelText::Name, L"Name" }, { LevelFiles::LevelText::Description, L"Description" }, { LevelFiles::LevelText::Author, L"Author" },
			{ LevelFiles::LevelText::Email, L"Email" }, { LevelFiles::LevelText::Information, L"Information" } } };
		// Digits for a side; no largest size, as the game has none (LevelFiles::MapSizeFits refuses only what cannot fit).
		constexpr std::size_t MaxSideDigits = 6;
		std::string newMapName;
		std::uint32_t newMapWidth = 0;
		std::uint32_t newMapHeight = 0;
		bool newMapWanted = false;
		// New map with changes not saved warns once; a second click goes ahead.
		bool newMapWarned = false;
		// Testing: the level plays for real from its saved files, then is loaded again to edit.
		bool testing = false;
		bool testWanted = false;
		bool stopTestWanted = false;
		// The game's level load knows nothing of the editor's mercenary mark, so it is put on them after the test's load.
		bool testMercenariesPending = false;

		std::uint8_t* VirtualOf(std::uint8_t* object, std::uintptr_t slot) { return Load<std::uint8_t*>(Load<std::uint8_t*>(object) + slot); }
		const std::uint8_t* PlayerThing(std::uint16_t tag) { return reinterpret_cast<const std::uint8_t* const*>(Game::Resolve(Game::Address::ThingTable))[tag]; }
		std::wstring Widen(const std::string& text) {
			std::wstring wide;
			for (const auto c : text) wide += static_cast<wchar_t>(static_cast<unsigned char>(c));
			return wide;
		}

		// Shown on the palette's last row, and in the log.
		std::string statusText;
		void Say(const std::string& text) {
			Logger::Debug("Editor: " + text);
			statusText = text;
		}
		std::string TerrainName() { return terrains.empty() ? std::string("none") : terrains[terrainChoice].name; }
		void LogChoice() { Logger::Debug("Editor: painting " + TerrainName() + ", owner " + std::to_string(ownerChoice) + "."); }
		std::string OwnerName(std::uint8_t number) {
			if (number == Game::GoodPlayer) return "Heroes";
			if (number == NeutralOwner) return "Neutral";
			return "Keeper " + std::to_string(number - Game::FirstKeeperPlayer + 1);
		}

		// The terrains on the palette, by place in the list. Rooms are on the side panel's Rooms tab instead. Worked out once
		// the world is there to say which are rooms.
		std::vector<std::size_t> paletteEntries;
		// What makes no sense to paint: the game's own markers, and what its files say is not used.
		bool PaletteHides(const std::string& name) {
			for (const auto* hidden : { "<not used>", "FrontEnd", "Fog Of War", "Edge of Map", "Claim Scan", "Spare Door" }) {
				if (name.find(hidden) != std::string::npos) return true;
			}
			return false;
		}
		// What is being painted, chosen in any way, shows on its tab.
		void ShowChoiceOnPanel() {
			tool = Tool::Terrain;
			const bool room = std::find(paletteEntries.begin(), paletteEntries.end(), terrainChoice) == paletteEntries.end();
			EditorPanel::Choose(room ? EditorPanel::Tab::Rooms : EditorPanel::Tab::Terrain, terrainChoice);
		}

		void RefreshPanelIcons();
		// The panel after the owner changes: its icons, or what is available to the new one.
		void RefreshPanelForOwner();
		// The owner chosen by the heart on the top bar, and the keys: keepers 1 to 4, then the heroes, then neutral.
		constexpr std::array<std::uint8_t, 6> OwnerOrder{ 3, 4, 5, 6, Game::GoodPlayer, NeutralOwner };
		void StepOwner(std::int32_t step) {
			const auto found = std::find(OwnerOrder.begin(), OwnerOrder.end(), ownerChoice);
			const auto count = static_cast<std::int32_t>(OwnerOrder.size());
			const auto place = found == OwnerOrder.end() ? count - step : static_cast<std::int32_t>(found - OwnerOrder.begin());
			ownerChoice = OwnerOrder[static_cast<std::size_t>((place + step + count) % count)];
			RefreshPanelForOwner();
			Logger::Debug("Editor: painting " + TerrainName() + ", owner " + std::to_string(ownerChoice) + ".");
		}

		// Which picture goes with what, by looking at the bitmaps (docs/game/level-editor.md). Sheets are in file order at the
		// start and end in a "?" for everything else. Places in the file, for what does not follow that.
		constexpr std::array<std::int8_t, 16> TrapCells{ 0, 1, 3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 12, 3, 2, 11 };
		constexpr std::array<std::int8_t, 8> DoorCells{ 0, 1, 2, 3, 4, 5, 5, 6 };
		constexpr std::array<std::int8_t, 46> CreatureCells{ 0, 29, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 29, 1, 26, 27, 28, 29,
			11, 6, 4, 9, 5, 8, 3, 10, 23, 2, 7, 21, 22, 25, 30 };
		constexpr std::int32_t RoomQuestionMarkCell = 17;
		// The bitmap's owner colors: red, purple, green, yellow for keepers 1 to 4, then white for anyone else.
		std::int32_t OwnerColorSlot(std::uint8_t owner) {
			return owner >= Game::FirstKeeperPlayer && owner < Game::FirstKeeperPlayer + 4 ? owner - static_cast<std::int32_t>(Game::FirstKeeperPlayer) : 4;
		}
		// Its cell in TerrainIcons.bmp: the one its record names, which for owned terrain is the first of a run in the owner
		// colors. Claimed Vault's run has no white, so anyone else's shows the unclaimed Mana Vault just before it.
		std::int32_t TerrainCell(const LevelFiles::TerrainType& terrain, std::uint8_t owner) {
			constexpr std::uint8_t ClaimedPathId = 8;
			constexpr std::uint8_t ReinforcedWallId = 9;
			constexpr std::uint8_t ClaimedVaultId = 32;
			const auto slot = OwnerColorSlot(owner);
			if (terrain.id == ClaimedPathId || terrain.id == ReinforcedWallId) return terrain.icon + slot;
			if (terrain.id == ClaimedVaultId) return slot < 4 ? terrain.icon + slot : terrain.icon - 1;
			return terrain.icon;
		}

		// The names in a shared list, as panel items, with the cells for them.
		template<std::size_t Size>
		std::vector<EditorPanel::Item> ListItems(const char* file, std::uint32_t section, const wchar_t* sheet, const std::array<std::int8_t, Size>& cells) {
			std::string error;
			const auto table = LevelFiles::ReadDefinitions(std::string("Data\\editor\\") + file, section, error);
			std::vector<EditorPanel::Item> items;
			if (!table) {
				Logger::Warning("Editor: " + error);
				return items;
			}
			for (std::size_t i = 0; i < table->names.size(); ++i) {
				const std::int32_t cell = i < cells.size() ? cells[i] : cells.back();
				if (!table->names[i].empty() && !PaletteHides(table->names[i])) items.push_back({ table->names[i], sheet, cell, i });
			}
			return items;
		}

		using TileAtFn = std::uint8_t*(__fastcall*)(std::uint8_t* world, void* unused, std::int32_t x, std::int32_t y);
		using TerrainDataFn = const std::uint8_t*(__fastcall*)(std::uint8_t* world, void* unused, std::int32_t terrain);
		using RoomDataFn = const std::uint8_t*(__fastcall*)(std::uint8_t* world, void* unused, std::int32_t type);
		using RoomTypeFn = std::uint8_t(__fastcall*)(std::uint8_t* world, void* unused, std::int32_t terrain);
		using SetTileFn = void(__fastcall*)(std::uint8_t* map, void* unused, std::int32_t x, std::int32_t y, std::int32_t terrain, std::int32_t ownerTag, std::int32_t first, std::int32_t second);
		using RoomsTileFn = void(__fastcall*)(std::uint8_t* rooms, void* unused, std::int32_t x, std::int32_t y, std::int32_t x2, std::int32_t y2);
		using RoomsAddFn = void(__fastcall*)(std::uint8_t* rooms, void* unused, std::int32_t type, std::int32_t ownerTag, std::int32_t x, std::int32_t y, std::int32_t x2, std::int32_t y2);
		using PlayerRemoveFn = void(__fastcall*)(const std::uint8_t* player, void* unused, std::int32_t x, std::int32_t y, std::int32_t zero);
		using PlayerAddFn = void(__fastcall*)(const std::uint8_t* player, void* unused, std::int32_t x, std::int32_t y, std::int32_t type, std::int32_t turn, std::int32_t first, std::int32_t second, std::int32_t third);

		// Selling in the editor pays nothing: every price is this percent of a cost.
		struct NothingForSale {
			std::uint8_t* percent = reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::SellPercent));
			std::uint8_t kept = *percent;
			NothingForSale() { *percent = 0; }
			~NothingForSale() { *percent = kept; }
		};
		// Tiles painted, sold or undone since the last stroke ended. The rooms at and beside them are built again in one go then.
		std::vector<std::pair<std::int32_t, std::int32_t>> roomSeeds;
		// The game's refresh of the rooms beside a tile (RoomsRefreshAround), which its own terrain changes run after setting a tile.
		bool roomsRefreshUsable = false;
		void RefreshRoomsAround(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			if (!roomsRefreshUsable) return;
			using RefreshFn = void(__fastcall*)(std::uint8_t* rooms, void* unused, std::int32_t x, std::int32_t y);
			GameFunction<RefreshFn>(Game::Address::RoomsRefreshAround)(world + Game::Offset::WorldRooms, nullptr, x, y);
		}
		// What a level script's "change terrain" does, without its effect and sound: nothing would ever play them out here.
		bool PaintTile(std::uint8_t* world, std::int32_t x, std::int32_t y, std::uint8_t terrain, std::uint8_t ownerNumber) {
			roomSeeds.push_back({ x, y });
			const auto number = ownerNumber == 0 ? NeutralOwner : ownerNumber;
			const auto* owner = Load<const std::uint8_t*>(world + Game::Offset::WorldPlayerTable) + (number - 1) * Game::PlayerTableEntrySize;
			const auto ownerTag = Load<std::uint16_t>(owner + 4);
			if (ownerTag == 0) {
				Say("This level has no " + OwnerName(number));
				return false;
			}
			auto* tile = reinterpret_cast<TileAtFn>(VirtualOf(world, Game::Offset::WorldTileAt))(world, nullptr, x, y);
			if (tile == nullptr) return false;
			auto* map = world + Game::Offset::WorldMap;
			auto* rooms = world + Game::Offset::WorldRooms;
			const auto neutralTag = static_cast<std::int32_t>(*reinterpret_cast<const std::uint16_t*>(Game::Resolve(Game::Address::NeutralPlayerTag)));
			if ((Load<std::uint16_t>(tile + Game::Offset::TileRoom) & Game::TagMask) != 0) {
				const NothingForSale free;
				const auto* roomOwner = PlayerThing(Load<std::uint16_t>(tile + Game::Offset::TileOwner) & Game::TagMask);
				GameFunction<RoomsTileFn>(Game::Address::RoomsBeforeRemoveTile)(rooms, nullptr, x, y, x, y);
				if (roomOwner != nullptr) GameFunction<PlayerRemoveFn>(Game::Address::PlayerRemoveRoomTile)(roomOwner, nullptr, x, y, 0);
				GameFunction<RoomsTileFn>(Game::Address::RoomsAfterRemoveTile)(rooms, nullptr, x, y, x, y);
			}
			const auto setTile = GameFunction<SetTileFn>(Game::Address::MapSetTile);
			const auto* data = reinterpret_cast<TerrainDataFn>(VirtualOf(world, Game::Offset::WorldTerrainData))(world, nullptr, terrain);
			if ((data[Game::Offset::TerrainFlags] & Game::TerrainIsRoomFlag) == 0) {
				setTile(map, nullptr, x, y, terrain, ownerTag, 1, 1);
				// The rooms beside it get their wall props and objects for the new wall or floor, as when the game digs or reinforces.
				RefreshRoomsAround(world, x, y);
				return true;
			}
			const auto type = GameFunction<RoomTypeFn>(Game::Address::WorldRoomTypeOfTerrain)(world, nullptr, terrain);
			const auto* roomData = reinterpret_cast<RoomDataFn>(VirtualOf(world, Game::Offset::WorldRoomData))(world, nullptr, type);
			const auto flags = Load<std::uint16_t>(roomData + Game::Offset::RoomDataFlags);
			if ((flags & Game::RoomOverWaterFlag) != 0) setTile(map, nullptr, x, y, world[Game::Offset::WorldWaterTerrain], neutralTag, 1, 1);
			else if ((flags & Game::RoomOverLavaFlag) != 0) setTile(map, nullptr, x, y, world[Game::Offset::WorldLavaTerrain], neutralTag, 1, 1);
			else setTile(map, nullptr, x, y, terrain, ownerTag, 1, 1);
			GameFunction<RoomsAddFn>(Game::Address::RoomsBeforeAddTile)(rooms, nullptr, type, ownerTag, x, y, x, y);
			const auto turn = Load<std::uint32_t>(Load<std::uint8_t*>(world + Game::Offset::WorldSession) + Game::Offset::SessionGameTurn);
			GameFunction<PlayerAddFn>(Game::Address::PlayerAddRoomTile)(owner, nullptr, x, y, type, static_cast<std::int32_t>(turn), 1, 1, 8);
			GameFunction<RoomsAddFn>(Game::Address::RoomsAfterAddTile)(rooms, nullptr, type, ownerTag, x, y, x, y);
			RefreshRoomsAround(world, x, y);
			return true;
		}

		constexpr const char* MapEdgeMessage = "The map's edge cannot be changed";
		bool DocumentReady() { return document.has_value() && !terrains.empty(); }
		LevelFiles::Tile* DocumentTile(std::int32_t x, std::int32_t y) {
			if (!document || x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= document->map.width || static_cast<std::uint32_t>(y) >= document->map.height) return nullptr;
			return &document->map.tiles[static_cast<std::size_t>(y) * document->map.width + static_cast<std::size_t>(x)];
		}
		// The game's tile change looks at each neighbor with no check that it is on the map, so a tile on the edge crashes
		// it. Real levels keep this ring solid, and the game never changes it.
		bool OnMapEdge(std::int32_t x, std::int32_t y) {
			return x <= 0 || y <= 0 || static_cast<std::uint32_t>(x) + 1 >= document->map.width || static_cast<std::uint32_t>(y) + 1 >= document->map.height;
		}
		std::string TerrainNameOf(std::uint8_t id) {
			for (const auto& type : terrains) {
				if (type.id == id) return type.name;
			}
			return {};
		}
		// The Dungeon Heart and hero gates are things in the level's list as well as tiles (level load builds them from the
		// thing), so painting one over or painting one in would leave the two disagreeing (a heart's object left standing).
		bool IsListedRoomTerrain(std::uint8_t id) {
			const auto name = TerrainNameOf(id);
			return name.find("Dungeon Heart") != std::string::npos || name.find("Hero Gate") != std::string::npos;
		}
		constexpr const char* ListedRoomMessage = "Dungeon Hearts and Hero Gates cannot be painted, they are things of the level";
		constexpr const char* RoomTooFarMessage = "Rooms cannot go past tile 254 across or down: the game keeps room tiles' places in bytes";
		bool IsRoomTerrain(std::uint8_t* world, std::uint8_t terrain) {
			const auto* data = reinterpret_cast<TerrainDataFn>(VirtualOf(world, Game::Offset::WorldTerrainData))(world, nullptr, terrain);
			return (data[Game::Offset::TerrainFlags] & Game::TerrainIsRoomFlag) != 0;
		}
		void ClearThingsForTerrain(std::uint8_t* world, std::int32_t x, std::int32_t y, std::uint8_t terrain);
		// One tile of a stroke, box, fill or paste. False if it could not change (not that it was already so).
		bool PaintTo(std::uint8_t* world, std::int32_t x, std::int32_t y, std::uint8_t terrain, std::uint8_t owner) {
			auto* tile = DocumentTile(x, y);
			if (tile == nullptr || !DocumentReady()) return false;
			if (OnMapEdge(x, y)) {
				if (statusText != MapEdgeMessage) Say(MapEdgeMessage);
				return false;
			}
			if (tile->terrain == terrain && tile->owner == owner) return true;
			if (IsListedRoomTerrain(tile->terrain) || IsListedRoomTerrain(terrain)) {
				if (statusText != ListedRoomMessage) Say(ListedRoomMessage);
				return false;
			}
			// A room keeps its tiles' x and y in bytes (0x4ECB70), and the game looks at each one's neighbors from those, so a room
			// tile past 254 would be taken for one near the map's other edge (a crash at 0x4ECF7B when loading such a map, tested).
			constexpr std::int32_t LastRoomTile = 254;
			if ((x > LastRoomTile || y > LastRoomTile) && IsRoomTerrain(world, terrain)) {
				if (statusText != RoomTooFarMessage) Say(RoomTooFarMessage);
				return false;
			}
			const auto before = *tile;
			if (!PaintTile(world, x, y, terrain, owner)) return false;
			tile->terrain = terrain;
			tile->owner = owner;
			stroke.push_back({ x, y, before });
			ClearThingsForTerrain(world, x, y, terrain);
			unsavedChanges = true;
			return true;
		}
		// The brush: a square of its size around the tile.
		void PaintChosen(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			if (!DocumentReady()) return;
			const auto reach = BrushSizes[brushChoice] / 2;
			for (auto dy = -reach; dy <= reach; ++dy) {
				for (auto dx = -reach; dx <= reach; ++dx) {
					if (DocumentTile(x + dx, y + dy) != nullptr) PaintTo(world, x + dx, y + dy, terrains[terrainChoice].id, ownerChoice);
				}
			}
		}
		// Parts of the view the game has marked as seen can be lost with a tile's change, so it is all lifted again.
		void RevealMap(std::uint8_t* world) {
			if (!revealUsable) return;
			using RevealFn = void(__fastcall*)(std::uint8_t* map);
			GameFunction<RevealFn>(Game::Address::MapRevealAll)(world + Game::Offset::WorldMap);
		}
		// A room built a tile at a time ends up with other objects than one built in one go (a 5x5 Hatchery had 3 pens, not 4). The
		// game re-places a room's objects for the whole room on every change, but walking its chain of tiles, which is in the order
		// they were added: a Hatchery puts a pen on each middle tile far enough from the last ones until it has one for every 6
		// tiles (0x4DE220), so the order decides how many fit. Built in one go the chain runs row by row, as the add goes along each
		// row in turn (0x4F04F0) and puts each tile at the end (0x4ECB70). So the chain is put in that order here and the room
		// refreshed as the game refreshes it. Nothing is sold, so nothing in the room is lost and nothing plays.
		std::vector<std::pair<std::int32_t, std::int32_t>> RoomTiles(std::uint8_t* world, const std::uint8_t* room) {
			const auto tileAt = reinterpret_cast<TileAtFn>(VirtualOf(world, Game::Offset::WorldTileAt));
			std::vector<std::pair<std::int32_t, std::int32_t>> tiles;
			std::int32_t x = room[Game::Offset::RoomFirstTile];
			std::int32_t y = room[Game::Offset::RoomFirstTile + 1];
			// A chain longer than the map would be a loop.
			const std::size_t most = document ? static_cast<std::size_t>(document->map.width) * document->map.height : 0;
			while ((x != 0 || y != 0) && tiles.size() < most) {
				const auto* tile = tileAt(world, nullptr, x, y);
				if (tile == nullptr) break;
				tiles.push_back({ x, y });
				x = tile[Game::Offset::TileNextInRoom];
				y = tile[Game::Offset::TileNextInRoom + 1];
			}
			return tiles;
		}
		void PutRoomInBuildOrder(std::uint8_t* world, std::uint8_t* room) {
			auto tiles = RoomTiles(world, room);
			if (tiles.size() < 2) return;
			std::sort(tiles.begin(), tiles.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
			const auto tileAt = reinterpret_cast<TileAtFn>(VirtualOf(world, Game::Offset::WorldTileAt));
			for (std::size_t i = 0; i < tiles.size(); ++i) {
				auto* tile = tileAt(world, nullptr, tiles[i].first, tiles[i].second);
				const bool last = i + 1 == tiles.size();
				tile[Game::Offset::TileNextInRoom] = last ? std::uint8_t{ 0 } : static_cast<std::uint8_t>(tiles[i + 1].first);
				tile[Game::Offset::TileNextInRoom + 1] = last ? std::uint8_t{ 0 } : static_cast<std::uint8_t>(tiles[i + 1].second);
			}
			room[Game::Offset::RoomFirstTile] = static_cast<std::uint8_t>(tiles.front().first);
			room[Game::Offset::RoomFirstTile + 1] = static_cast<std::uint8_t>(tiles.front().second);
			room[Game::Offset::RoomLastTile] = static_cast<std::uint8_t>(tiles.back().first);
			room[Game::Offset::RoomLastTile + 1] = static_cast<std::uint8_t>(tiles.back().second);
			// The refresh is for the rooms beside a tile, so it is asked about the tile right of the first, which is beside it.
			RefreshRoomsAround(world, tiles.front().first + 1, tiles.front().second);
		}
		// Every room at or beside a tile changed since last time, as the world has them.
		void RebuildTouchedRooms(std::uint8_t* world) {
			if (roomSeeds.empty()) return;
			auto seeds = std::move(roomSeeds);
			roomSeeds.clear();
			if (!DocumentReady() || !roomsRefreshUsable) return;
			const auto tileAt = reinterpret_cast<TileAtFn>(VirtualOf(world, Game::Offset::WorldTileAt));
			const auto terrainName = [&](std::uint8_t id) -> std::string {
				for (const auto& type : terrains) {
					if (type.id == id) return type.name;
				}
				return {};
			};
			std::unordered_set<std::uint16_t> done;
			for (const auto& [seedX, seedY] : seeds) {
				const std::array<std::pair<std::int32_t, std::int32_t>, 5> around{ { { seedX, seedY }, { seedX + 1, seedY }, { seedX - 1, seedY }, { seedX, seedY + 1 }, { seedX, seedY - 1 } } };
				for (const auto& [x, y] : around) {
					if (DocumentTile(x, y) == nullptr) continue;
					const auto* tile = tileAt(world, nullptr, x, y);
					if (tile == nullptr) continue;
					const auto roomTag = static_cast<std::uint16_t>(Load<std::uint16_t>(tile + Game::Offset::TileRoom) & Game::TagMask);
					if (roomTag == 0 || !done.insert(roomTag).second) continue;
					// Rooms are in the thing table by their tag too [verified: 0x454110 finds a tile's room there].
					auto* room = reinterpret_cast<std::uint8_t* const*>(Game::Resolve(Game::Address::ThingTable))[roomTag];
					if (room == nullptr) continue;
					// The heart and hero gates are made by level load from their things and are always whole, so they stay as they are.
					const auto name = terrainName(tile[Game::Offset::TileTerrain]);
					if (name.find("Dungeon Heart") != std::string::npos || name.find("Hero Gate") != std::string::npos) continue;
					PutRoomInBuildOrder(world, room);
				}
			}
			if (!done.empty()) Logger::Debug("Editor: " + std::to_string(done.size()) + " room(s) put in build order and refreshed.");
		}
		void PushStep(std::vector<UndoStep>& steps, UndoStep step) {
			if (step.tiles.empty() && step.things.empty() && !step.health) return;
			if (steps.size() == MaxUndoSteps) steps.erase(steps.begin());
			steps.push_back(std::move(step));
		}
		// A new change: what was undone can no longer be redone, and leaving warns again. An empty step still goes in when the
		// exact hit points changed (a change of them alone).
		void PushUndo(UndoStep step) {
			if (document && document->creatureHealth != healthAtLastStep) {
				step.health = std::exchange(healthAtLastStep, document->creatureHealth);
			}
			if (step.tiles.empty() && step.things.empty() && !step.health) return;
			PushStep(undoSteps, std::move(step));
			redoSteps.clear();
			leaveWarned = false;
		}
		void EndStroke(std::uint8_t* world) {
			RebuildTouchedRooms(world);
			if (stroke.empty() && strokeThings.empty()) return;
			const bool tiles = !stroke.empty();
			PushUndo({ std::move(stroke), std::move(strokeThings) });
			stroke.clear();
			strokeThings.clear();
			if (tiles) RevealMap(world);
		}

		// ---- Creatures. They are made as level load makes them, from the same bytes the level's file holds, so a creature
		// placed here and one loaded later are the same. The world and the level's list change together.
		constexpr std::uint32_t NeutralCreatureRecord = 0xC6;
		constexpr std::uint32_t HeroCreatureRecord = 0xC7;
		constexpr std::uint32_t KeeperCreatureRecord = 0xC8;
		constexpr std::uint32_t TrapRecord = 0xC3;
		constexpr std::uint32_t DoorRecord = 0xC4;
		constexpr std::uint32_t ObjectRecord = 0xC2;
		constexpr std::uint32_t ActionPointRecord = 0xC5;
		bool IsCreatureKind(std::uint32_t kind) { return kind == NeutralCreatureRecord || kind == HeroCreatureRecord || kind == KeeperCreatureRecord; }
		constexpr std::uint8_t TerrainSolidFlag = 0x01;
		// Set for water, lava and impenetrable rock among others, none of which a trap or door stands on [verified in Terrain.kwd].
		constexpr std::uint8_t TerrainNoBuildFlag = 0x02;
		std::uint8_t* ThingOfTag(std::uint16_t tag) { return reinterpret_cast<std::uint8_t* const*>(Game::Resolve(Game::Address::ThingTable))[tag]; }
		bool IsCreature(std::uint16_t tag) {
			if (tag == 0 || tag > Game::TagMask) return false;
			if (reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::ThingInUse))[tag] == 0) return false;
			const auto* thing = ThingOfTag(tag);
			// A removed creature waits in the pool with its tag, vtable and last position, so only its list tells them apart.
			constexpr std::int32_t CreaturesList = 0;
			return thing != nullptr && Load<std::uintptr_t>(thing) == Game::Resolve(Game::Address::CreatureVtable) && Load<std::int32_t>(thing + Game::Offset::ThingList) == CreaturesList;
		}
		// Traps and doors are freed when removed, so unlike creatures one in the table is always real.
		bool ThingHasVtable(std::uint16_t tag, std::uintptr_t vtable) {
			if (tag == 0 || tag > Game::TagMask) return false;
			if (reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::ThingInUse))[tag] == 0) return false;
			const auto* thing = ThingOfTag(tag);
			return thing != nullptr && Load<std::uintptr_t>(thing) == vtable;
		}
		bool IsTrap(std::uint16_t tag) { return ThingHasVtable(tag, Game::Resolve(Game::Address::TrapVtable)); }
		bool IsDoor(std::uint16_t tag) { return ThingHasVtable(tag, Game::Resolve(Game::Address::DoorVtable)); }
		// Objects are freed when removed too [verified: 0x4B7B10 destroys and frees them], and are on their own list.
		bool IsObject(std::uint16_t tag) {
			return ThingHasVtable(tag, Game::Resolve(Game::Address::ObjectVtable)) && Load<std::int32_t>(ThingOfTag(tag) + Game::Offset::ThingList) == Game::Offset::ObjectsList;
		}
		std::uint8_t TrapOrDoorOwner(const std::uint8_t* thing) {
			const auto* player = PlayerThing(Load<std::uint16_t>(thing + Game::Offset::ThingOwnerTag) & Game::TagMask);
			return player != nullptr ? player[Game::Offset::PlayerNumber] : NeutralOwner;
		}
		std::string NameFrom(const std::vector<std::string>& names, std::uint8_t id) {
			if (id == 0 || id > names.size()) return "Thing " + std::to_string(id);
			return names[id - std::size_t{ 1 }];
		}
		std::string TrapOrDoorName(std::uint16_t tag) {
			const auto type = ThingOfTag(tag)[Game::Offset::TrapDoorType];
			return IsDoor(tag) ? NameFrom(doorNames, type) : NameFrom(trapNames, type);
		}
		std::int32_t CreatureTileX(const std::uint8_t* creature) { return Load<std::int32_t>(creature + Game::Offset::ThingX) >> 12; }
		std::int32_t CreatureTileY(const std::uint8_t* creature) { return Load<std::int32_t>(creature + Game::Offset::ThingY) >> 12; }
		std::uint8_t CreatureOwner(const std::uint8_t* creature) {
			const auto* player = Load<const std::uint8_t*>(creature + Game::Offset::ThingPlayer);
			return player != nullptr ? player[Game::Offset::PlayerNumber] : NeutralOwner;
		}
		// The one on this tile, preferring the one of this type, owner and level, for those given.
		std::uint16_t CreatureOnTile(std::int32_t x, std::int32_t y, std::uint8_t type = 0, std::uint8_t owner = 0, std::uint8_t level = 0) {
			std::uint16_t found = 0;
			std::int32_t foundScore = -1;
			for (std::uint16_t tag = 1; tag <= Game::TagMask; ++tag) {
				if (!IsCreature(tag)) continue;
				const auto* creature = ThingOfTag(tag);
				if (CreatureTileX(creature) != x || CreatureTileY(creature) != y) continue;
				const auto score = ((owner == 0 || CreatureOwner(creature) == owner) ? 4 : 0) + ((type == 0 || creature[Game::Offset::CreatureType] == type) ? 2 : 0)
					+ ((level == 0 || creature[Game::Offset::CreatureLevel] == level) ? 1 : 0);
				if (score > foundScore) {
					found = tag;
					foundScore = score;
				}
			}
			return found;
		}
		// The trap or door on this tile, of either kind unless one is asked for.
		std::uint16_t TrapOrDoorOnTile(std::int32_t x, std::int32_t y, std::uint32_t recordKind = 0) {
			for (std::uint16_t tag = 1; tag <= Game::TagMask; ++tag) {
				const bool trap = IsTrap(tag);
				const bool door = !trap && IsDoor(tag);
				if (!trap && !door) continue;
				if (recordKind == TrapRecord && !trap) continue;
				if (recordKind == DoorRecord && !door) continue;
				const auto* thing = ThingOfTag(tag);
				if (CreatureTileX(thing) == x && CreatureTileY(thing) == y) return tag;
			}
			return 0;
		}
		std::string CreatureName(std::uint8_t type) {
			if (type == 0 || type > creatureNames.size()) return "Creature " + std::to_string(type);
			auto name = creatureNames[type - std::size_t{ 1 }];
			while (!name.empty() && name.back() == ' ') name.pop_back();
			return name;
		}
		// Field places by kind, from the converters the game runs on these bytes (0x56F330, 0x56F430, 0x56F540).
		struct CreatureFields {
			std::int32_t x;
			std::int32_t y;
			std::uint8_t type;
			std::uint8_t owner;
			std::uint8_t level;
		};
		constexpr std::size_t CreatureRecordLevel = 0xE;
		std::optional<CreatureFields> ReadCreatureRecord(const LevelFiles::ThingRecord& record) {
			const auto* bytes = record.bytes.data();
			const auto size = record.bytes.size();
			if (record.kind == NeutralCreatureRecord && size >= 0x18) return CreatureFields{ Load<std::int32_t>(bytes), Load<std::int32_t>(bytes + 4), bytes[0x16], NeutralOwner, bytes[CreatureRecordLevel] };
			if (record.kind == HeroCreatureRecord && size >= 0x20) return CreatureFields{ Load<std::int32_t>(bytes), Load<std::int32_t>(bytes + 4), bytes[0x1C], Game::GoodPlayer, bytes[CreatureRecordLevel] };
			if (record.kind == KeeperCreatureRecord && size >= 0x1C) return CreatureFields{ Load<std::int32_t>(bytes), Load<std::int32_t>(bytes + 4), bytes[0x1A], bytes[0x1B], bytes[CreatureRecordLevel] };
			if (record.kind == TrapRecord && size >= 0x10) return CreatureFields{ Load<std::int32_t>(bytes), Load<std::int32_t>(bytes + 4), bytes[0xD], bytes[0xE], 0 };
			if (record.kind == DoorRecord && size >= 0x14) return CreatureFields{ Load<std::int32_t>(bytes), Load<std::int32_t>(bytes + 4), bytes[0xE], bytes[0xF], 0 };
			// +0x16 the type, +0x17 the player [verified: 0x56F730 copies them to what 0x520BB0 reads as those].
			if (record.kind == ObjectRecord && size >= 0x18) return CreatureFields{ Load<std::int32_t>(bytes), Load<std::int32_t>(bytes + 4), bytes[0x16], bytes[0x17], 0 };
			return std::nullopt;
		}
		// A trap's or door's place in the level's list.
		std::optional<std::size_t> FindTrapOrDoorRecord(std::uint16_t tag) {
			if (!document || !document->things) return std::nullopt;
			const auto kind = IsDoor(tag) ? DoorRecord : TrapRecord;
			const auto* thing = ThingOfTag(tag);
			const auto& records = document->things->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				if (records[i].kind != kind) continue;
				const auto fields = ReadCreatureRecord(records[i]);
				if (fields && fields->x == CreatureTileX(thing) && fields->y == CreatureTileY(thing)) return i;
			}
			return std::nullopt;
		}
		// A creature's place in the level's list. Nothing moves in a held world, so it is where the file says. A Unique creature
		// is its base type in the world, so type only prefers, and level tells apart those standing together.
		std::optional<std::size_t> FindCreatureRecord(const std::uint8_t* creature) {
			if (!document || !document->things) return std::nullopt;
			const auto x = CreatureTileX(creature);
			const auto y = CreatureTileY(creature);
			const auto owner = CreatureOwner(creature);
			std::optional<std::size_t> place;
			std::int32_t placeScore = -1;
			const auto& records = document->things->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				if (!IsCreatureKind(records[i].kind)) continue;
				const auto fields = ReadCreatureRecord(records[i]);
				if (!fields || fields->x != x || fields->y != y || fields->owner != owner) continue;
				const auto score = (fields->type == creature[Game::Offset::CreatureType] ? 2 : 0) + (fields->level == creature[Game::Offset::CreatureLevel] ? 1 : 0);
				if (score > placeScore) {
					place = i;
					placeScore = score;
				}
			}
			return place;
		}
		// Level 1, full health, and for the rest what most of the stock levels' creatures of that kind have.
		LevelFiles::ThingRecord MakeCreatureRecord(std::int32_t x, std::int32_t y, std::uint8_t type, std::uint8_t owner) {
			constexpr std::uint8_t UsualFlags = 9;
			constexpr std::int32_t FullHealth = 100;
			LevelFiles::ThingRecord record;
			if (owner == Game::GoodPlayer) {
				record.kind = HeroCreatureRecord;
				record.bytes.assign(0x20, 0);
				Store<std::int32_t>(record.bytes.data() + 0x14, FullHealth);
				record.bytes[0x1A] = 3;
				record.bytes[0x1B] = 23;
				record.bytes[0x1C] = type;
				record.bytes[0x1E] = 0xFF;
			}
			else if (owner == NeutralOwner) {
				record.kind = NeutralCreatureRecord;
				record.bytes.assign(0x18, 0);
				Store<std::int32_t>(record.bytes.data() + 0x10, FullHealth);
				record.bytes[0x16] = type;
			}
			else {
				record.kind = KeeperCreatureRecord;
				record.bytes.assign(0x1C, 0);
				Store<std::int32_t>(record.bytes.data() + 0x10, FullHealth);
				record.bytes[0x1A] = type;
				record.bytes[0x1B] = owner;
			}
			Store<std::int32_t>(record.bytes.data(), x);
			Store<std::int32_t>(record.bytes.data() + 4, y);
			record.bytes[CreatureRecordLevel] = 1;
			record.bytes[0xF] = UsualFlags;
			return record;
		}
		LevelFiles::ThingRecord MakeTrapRecord(std::int32_t x, std::int32_t y, std::uint8_t type, std::uint8_t owner) {
			// Above 0 is a finished trap, 0 a blueprint for imps to build (0x520E50). Most stock traps have 4.
			constexpr std::uint8_t UsualBuilt = 4;
			LevelFiles::ThingRecord record;
			record.kind = TrapRecord;
			record.bytes.assign(0x10, 0);
			Store<std::int32_t>(record.bytes.data(), x);
			Store<std::int32_t>(record.bytes.data() + 4, y);
			record.bytes[0xC] = UsualBuilt;
			record.bytes[0xD] = type;
			record.bytes[0xE] = owner;
			return record;
		}
		LevelFiles::ThingRecord MakeDoorRecord(std::int32_t x, std::int32_t y, std::uint8_t type, std::uint8_t owner) {
			LevelFiles::ThingRecord record;
			record.kind = DoorRecord;
			record.bytes.assign(0x14, 0);
			Store<std::int32_t>(record.bytes.data(), x);
			Store<std::int32_t>(record.bytes.data() + 4, y);
			record.bytes[0xE] = type;
			record.bytes[0xF] = owner;
			return record;
		}
		// In a creature's flags, a bit nothing in the game reads [verified: the make functions read the others, and no stock level
		// sets it]: this one is a mercenary, which the file has no other way to say. Only the editor knows this for now.
		constexpr std::uint8_t RecordMercenaryFlag = 0x02;
		constexpr std::size_t CreatureRecordFlags = 0xF;
		constexpr std::uint32_t MercenaryStateFlag = 0x200;
		// The first of this kind on the tile not yet a mercenary.
		void MakeMercenaryOnTile(const CreatureFields& fields) {
			for (std::uint16_t tag = 1; tag <= Game::TagMask; ++tag) {
				if (!IsCreature(tag)) continue;
				auto* creature = ThingOfTag(tag);
				if (CreatureTileX(creature) != fields.x || CreatureTileY(creature) != fields.y || creature[Game::Offset::CreatureType] != fields.type || CreatureOwner(creature) != fields.owner) continue;
				if ((Load<std::uint32_t>(creature + Game::Offset::CreatureStateFlags) & MercenaryStateFlag) != 0) continue;
				using SetFn = void(__fastcall*)(std::uint8_t* creature, void* unused, std::int32_t on);
				GameFunction<SetFn>(Game::Address::SetCreatureMercenary)(creature, nullptr, 1);
				return;
			}
		}
		bool IsCreatureRecord(const LevelFiles::ThingRecord& record) { return record.kind == NeutralCreatureRecord || record.kind == HeroCreatureRecord || record.kind == KeeperCreatureRecord; }
		void ApplyRecordMercenary(const LevelFiles::ThingRecord& record) {
			if (!IsCreatureRecord(record) || record.bytes.size() <= CreatureRecordFlags || (record.bytes[CreatureRecordFlags] & RecordMercenaryFlag) == 0) return;
			if (const auto fields = ReadCreatureRecord(record)) MakeMercenaryOnTile(*fields);
		}
		bool MakeThingFromRecord(std::uint8_t* world, const LevelFiles::ThingRecord& record) {
			using InitFn = void*(__fastcall*)(std::uint8_t* decoded);
			using ReadFn = void(__fastcall*)(std::uint8_t* decoded, void* unused, const std::uint8_t* bytes);
			using MakeFn = std::int32_t(__cdecl*)(std::uint8_t* decoded, std::uint8_t* world);
			// Larger than the largest kind's, which is 0x54.
			alignas(4) std::array<std::uint8_t, 0x60> decoded{};
			if (record.kind == TrapRecord) {
				GameFunction<InitFn>(Game::Address::TrapInit)(decoded.data());
				GameFunction<ReadFn>(Game::Address::TrapRead)(decoded.data(), nullptr, record.bytes.data());
				return GameFunction<MakeFn>(Game::Address::TrapMake)(decoded.data(), world) != 0;
			}
			if (record.kind == ObjectRecord) {
				GameFunction<InitFn>(Game::Address::ObjectInit)(decoded.data());
				GameFunction<ReadFn>(Game::Address::ObjectRead)(decoded.data(), nullptr, record.bytes.data());
				return GameFunction<MakeFn>(Game::Address::ObjectMake)(decoded.data(), world) != 0;
			}
			if (record.kind == DoorRecord) {
				GameFunction<InitFn>(Game::Address::DoorInit)(decoded.data());
				GameFunction<ReadFn>(Game::Address::DoorRead)(decoded.data(), nullptr, record.bytes.data());
				return GameFunction<MakeFn>(Game::Address::DoorMake)(decoded.data(), world) != 0;
			}
			if (record.kind == NeutralCreatureRecord) {
				GameFunction<InitFn>(Game::Address::NeutralCreatureInit)(decoded.data());
				GameFunction<ReadFn>(Game::Address::NeutralCreatureRead)(decoded.data(), nullptr, record.bytes.data());
				return GameFunction<MakeFn>(Game::Address::NeutralCreatureMake)(decoded.data(), world) != 0;
			}
			if (record.kind == HeroCreatureRecord) {
				GameFunction<InitFn>(Game::Address::HeroCreatureInit)(decoded.data());
				GameFunction<ReadFn>(Game::Address::HeroCreatureRead)(decoded.data(), nullptr, record.bytes.data());
				return GameFunction<MakeFn>(Game::Address::HeroCreatureMake)(decoded.data(), world) != 0;
			}
			GameFunction<InitFn>(Game::Address::KeeperCreatureInit)(decoded.data());
			GameFunction<ReadFn>(Game::Address::KeeperCreatureRead)(decoded.data(), nullptr, record.bytes.data());
			return GameFunction<MakeFn>(Game::Address::KeeperCreatureMake)(decoded.data(), world) != 0;
		}
		bool MakeCreature(std::uint8_t* world, const LevelFiles::ThingRecord& record) {
			if (!MakeThingFromRecord(world, record)) return false;
			ApplyRecordMercenary(record);
			return true;
		}
		bool RemoveCreature(std::uint8_t* world, std::uint16_t tag) {
			if (!IsCreature(tag)) return false;
			auto* creature = ThingOfTag(tag);
			constexpr std::uint32_t LeavesNothing = 0x40;
			constexpr std::uint32_t DeathEffects = 0x20 | 0x80;
			Store<std::uint32_t>(creature + Game::Offset::CreatureFlags, (Load<std::uint32_t>(creature + Game::Offset::CreatureFlags) | LeavesNothing) & ~DeathEffects);
			using RemoveFn = void(__fastcall*)(std::uint8_t* creatures, void* unused, std::uint32_t tag);
			GameFunction<RemoveFn>(Game::Address::CreatureRemove)(world + Game::Offset::WorldCreatures, nullptr, tag);
			return true;
		}
		void RemoveTrapOrDoor(std::uint16_t tag) {
			if (!IsTrap(tag) && !IsDoor(tag)) return;
			auto* thing = ThingOfTag(tag);
			using RemoveFn = void(__fastcall*)(std::uint8_t* thing);
			reinterpret_cast<RemoveFn>(VirtualOf(thing, Game::Offset::ThingRemoveSlot))(thing);
		}
		// The game's sale, for nothing. It only sets the thing's health to 0 for its next tick to remove it, and a held world has no
		// ticks, so it is removed here. Undoing it makes it again from its record.
		void SellTrapOrDoor(std::uint16_t tag) {
			const auto* thing = ThingOfTag(tag);
			const auto* player = PlayerThing(Load<std::uint16_t>(thing + Game::Offset::ThingOwnerTag) & Game::TagMask);
			if (player != nullptr) {
				const NothingForSale free;
				using SellFn = std::int32_t(__fastcall*)(const std::uint8_t* player, void* unused, std::int32_t x, std::int32_t y);
				GameFunction<SellFn>(IsDoor(tag) ? Game::Address::PlayerSellDoor : Game::Address::PlayerSellTrap)(player, nullptr, CreatureTileX(thing), CreatureTileY(thing));
			}
			RemoveTrapOrDoor(tag);
		}

		// ---- Objects: made as level load makes them, from their record, like traps.
		bool objectsUsable = false;
		// Dead bodies and effect generators, made by level load's own makers (Game::Address::DeadBodyMake, EffectGeneratorMake).
		bool deadBodiesUsable = false;
		bool effectGeneratorsUsable = false;
		constexpr std::uint8_t GoldObject = 1;
		constexpr std::uint8_t GoldBagObject = 2;
		constexpr std::uint8_t SpellBookObject = 4;
		bool IsGoldObject(std::uint8_t type) { return type == GoldObject || type == GoldBagObject; }
		// What the panel offers: what levels place, rather than what rooms and the game make for themselves (beds, furniture,
		// pillars, the heart). Gold, a spell book, a chicken, crates, keys, bones, a barrel, hero tables, and the specials.
		constexpr std::array<std::uint8_t, 48> PlaceableObjects{ 1, 2, 4, 9, 17, 5, 36, 37, 38, 39, 40, 41, 42, 43, 44, 52, 53, 54, 55, 56, 57, 92, 138,
			104, 105, 106, 107, 137, 48, 49, 50, 59, 102, 113, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 140 };
		// An action point has no look of its own in the world, so these stand on its corners while editing. They are in the world
		// only, never in the level's list, and go with the level when it is loaded again.
		constexpr std::uint8_t ActionPointMarkerObject = 49; // Skull
		std::map<std::uint8_t, std::vector<std::uint16_t>> actionPointMarkers;
		// When the outlines can be drawn (the tile draw hook is in), they show action points instead of the markers.
		bool outlinesUsable = false;
		bool IsActionPointMarker(std::uint16_t tag) {
			for (const auto& [id, tags] : actionPointMarkers) {
				if (std::find(tags.begin(), tags.end(), tag) != tags.end()) return true;
			}
			return false;
		}
		std::string ObjectName(std::uint8_t type) { return NameFrom(objectNames, type); }
		// ObjectIcons.bmp holds ten pictures, not one an object [verified by eye, docs/game/tab images]: a crate, a gold bag, a gold pile,
		// a spell book, a bed, a gem, a Special's box, a table, a weapon rack and the level indicator. Others borrow or have none.
		struct Icon {
			const wchar_t* sheet;
			std::int32_t cell;
		};
		Icon ObjectIcon(std::uint8_t type) {
			constexpr std::uint8_t ChickenObject = 9;
			constexpr std::int32_t ChickenSpellCell = 10;
			if (type == GoldObject) return { L"ObjectIcons.bmp", 2 };
			if (type == GoldBagObject) return { L"ObjectIcons.bmp", 1 };
			if (type == SpellBookObject) return { L"ObjectIcons.bmp", 3 };
			if (type == ChickenObject) return { L"KeeperSpellIcons.bmp", ChickenSpellCell };
			if (type == 102) return { L"ObjectIcons.bmp", 7 };
			if (type == 113) return { L"ObjectIcons.bmp", 8 };
			if ((type >= 117 && type <= 129) || type == 140) return { L"ObjectIcons.bmp", 6 };
			const auto name = ObjectName(type);
			if (name.find("Crate") != std::string::npos) return { L"ObjectIcons.bmp", 0 };
			return { L"", -1 };
		}
		std::string SpellName(std::size_t id) { return id >= 1 && id <= keeperSpellNames.size() ? keeperSpellNames[id - 1] : "spell " + std::to_string(id); }
		// What a record says it is, with its gold or its spell.
		std::string ObjectRecordName(const LevelFiles::ThingRecord& record) {
			if (record.bytes.size() < 0x18) return "Object";
			const auto type = record.bytes[0x16];
			if (IsGoldObject(type)) return ObjectName(type) + " (" + std::to_string(Load<std::int32_t>(record.bytes.data() + 0x10)) + ")";
			if (type == SpellBookObject) return ObjectName(type) + ": " + SpellName(Load<std::uint32_t>(record.bytes.data() + 0xC));
			return ObjectName(type);
		}
		// The panel's name for an object, with what a right click has chosen for it.
		std::string ObjectItemName(std::uint8_t type) {
			if (IsGoldObject(type)) return ObjectName(type) + " (" + std::to_string(GoldAmounts[goldChoice]) + ")";
			if (type == SpellBookObject) return ObjectName(type) + ": " + SpellName(spellBookChoice + 1);
			return ObjectName(type);
		}
		// +0 x, +4 y, +8 z, +0xC a spell book's spell (0x520BB0 keeps it for objects whose type has flag 8), +0x10 gold (passed on for flag
		// 0x40), +0x16 type, +0x17 player. Stock levels' spell books hold 1, 2, 6, 9 and 11 there, their gold 50 to 5000.
		LevelFiles::ThingRecord MakeObjectRecord(std::int32_t x, std::int32_t y, std::uint8_t type, std::uint8_t owner) {
			LevelFiles::ThingRecord record;
			record.kind = ObjectRecord;
			record.bytes.assign(0x18, 0);
			Store<std::int32_t>(record.bytes.data(), x);
			Store<std::int32_t>(record.bytes.data() + 4, y);
			if (type == SpellBookObject) Store<std::uint32_t>(record.bytes.data() + 0xC, static_cast<std::uint32_t>(spellBookChoice + 1));
			if (IsGoldObject(type)) Store<std::int32_t>(record.bytes.data() + 0x10, GoldAmounts[goldChoice]);
			record.bytes[0x16] = type;
			record.bytes[0x17] = owner;
			return record;
		}
		std::vector<std::uint16_t> ObjectsOnTile(std::int32_t x, std::int32_t y, std::uint8_t type) {
			std::vector<std::uint16_t> found;
			for (std::uint16_t tag = 1; tag <= Game::TagMask; ++tag) {
				if (!IsObject(tag)) continue;
				const auto* thing = ThingOfTag(tag);
				if (CreatureTileX(thing) == x && CreatureTileY(thing) == y && (type == 0 || thing[Game::Offset::ObjectType] == type)) found.push_back(tag);
			}
			return found;
		}
		// An object of the level's list on this tile: one of a type some record on this tile has, and not a marker.
		std::uint16_t ListedObjectOnTile(std::int32_t x, std::int32_t y, std::uint8_t type = 0) {
			if (!document || !document->things) return 0;
			for (const auto& record : document->things->records) {
				if (record.kind != ObjectRecord) continue;
				const auto fields = ReadCreatureRecord(record);
				if (!fields || fields->x != x || fields->y != y || (type != 0 && fields->type != type)) continue;
				for (const auto tag : ObjectsOnTile(x, y, fields->type)) {
					if (!IsActionPointMarker(tag)) return tag;
				}
			}
			return 0;
		}
		std::optional<std::size_t> FindObjectRecord(std::uint16_t tag) {
			if (!document || !document->things || !IsObject(tag)) return std::nullopt;
			const auto* thing = ThingOfTag(tag);
			const auto& records = document->things->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				if (records[i].kind != ObjectRecord) continue;
				const auto fields = ReadCreatureRecord(records[i]);
				if (fields && fields->x == CreatureTileX(thing) && fields->y == CreatureTileY(thing) && fields->type == thing[Game::Offset::ObjectType]) return i;
			}
			return std::nullopt;
		}
		void RemoveObject(std::uint16_t tag) {
			if (!IsObject(tag)) return;
			auto* thing = ThingOfTag(tag);
			using RemoveFn = void(__fastcall*)(std::uint8_t* thing);
			reinterpret_cast<RemoveFn>(VirtualOf(thing, Game::Offset::ThingRemoveSlot))(thing);
		}
		// Made from a record and found again, as the maker does not say which it made: the one on the tile that was not there before.
		std::uint16_t MakeObjectAndFind(std::uint8_t* world, const LevelFiles::ThingRecord& record) {
			const auto fields = ReadCreatureRecord(record);
			if (!fields) return 0;
			const auto before = ObjectsOnTile(fields->x, fields->y, fields->type);
			if (!MakeThingFromRecord(world, record)) return 0;
			for (const auto tag : ObjectsOnTile(fields->x, fields->y, fields->type)) {
				if (std::find(before.begin(), before.end(), tag) == before.end()) return tag;
			}
			return 0;
		}

		// ---- Action points: boxes of tiles that a level's triggers and hero parties refer to by number. Only the level's list holds
		// them while editing; a test or the game makes them from it.
		struct ActionPointBox {
			std::int32_t left, top, right, bottom;
			std::uint8_t id;
			// +0x18, 32 characters, empty in stock levels.
			std::string name = {};
		};
		constexpr std::size_t ActionPointNameAt = 0x18;
		constexpr std::size_t ActionPointNameSize = 32;
		// +0 and +4 one corner, +8 and +0xC the other, both in the box. With +8 at 0, +0 and +4 are its center and +0xC how far it
		// reaches (0x56F790); no stock level uses that. +0x16 its number.
		std::optional<ActionPointBox> ReadActionPoint(const LevelFiles::ThingRecord& record) {
			if (record.kind != ActionPointRecord || record.bytes.size() < 0x18) return std::nullopt;
			const auto* bytes = record.bytes.data();
			const auto a = Load<std::int32_t>(bytes);
			const auto b = Load<std::int32_t>(bytes + 4);
			const auto c = Load<std::int32_t>(bytes + 8);
			const auto d = Load<std::int32_t>(bytes + 0xC);
			std::string name;
			if (record.bytes.size() >= ActionPointNameAt + ActionPointNameSize) {
				const auto* text = reinterpret_cast<const char*>(bytes + ActionPointNameAt);
				name.assign(text, strnlen(text, ActionPointNameSize));
			}
			if (c == 0) return ActionPointBox{ a - d, b - d, a + d, b + d, bytes[0x16], name };
			return ActionPointBox{ (std::min)(a, c), (std::min)(b, d), (std::max)(a, c), (std::max)(b, d), bytes[0x16], name };
		}
		// +0x10 is 0x40000 in most stock ones and +0x14, +0x17 and the name at +0x18 mostly empty, none of it understood yet.
		LevelFiles::ThingRecord MakeActionPointRecord(const ActionPointBox& box) {
			constexpr std::uint32_t UsualSixteen = 0x40000;
			LevelFiles::ThingRecord record;
			record.kind = ActionPointRecord;
			record.bytes.assign(0x38, 0);
			Store<std::int32_t>(record.bytes.data(), box.left);
			Store<std::int32_t>(record.bytes.data() + 4, box.top);
			Store<std::int32_t>(record.bytes.data() + 8, box.right);
			Store<std::int32_t>(record.bytes.data() + 0xC, box.bottom);
			Store<std::uint32_t>(record.bytes.data() + 0x10, UsualSixteen);
			record.bytes[0x16] = box.id;
			return record;
		}
		std::string ActionPointText(const ActionPointBox& box) {
			return "Action point " + std::to_string(box.id) + (box.name.empty() ? "" : " \"" + box.name + "\"") + ", " + std::to_string(box.left) + "," + std::to_string(box.top)
				+ " to " + std::to_string(box.right) + "," + std::to_string(box.bottom);
		}
		// The smallest one this tile is in, for one inside another.
		std::optional<std::size_t> ActionPointAt(std::int32_t x, std::int32_t y) {
			if (!document || !document->things) return std::nullopt;
			std::optional<std::size_t> found;
			std::int64_t foundArea = 0;
			const auto& records = document->things->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				const auto box = ReadActionPoint(records[i]);
				if (!box || x < box->left || x > box->right || y < box->top || y > box->bottom) continue;
				const auto area = static_cast<std::int64_t>(box->right - box->left + 1) * (box->bottom - box->top + 1);
				if (!found || area < foundArea) {
					found = i;
					foundArea = area;
				}
			}
			return found;
		}
		std::optional<std::size_t> FindActionPoint(std::uint8_t id) {
			if (!document || !document->things) return std::nullopt;
			const auto& records = document->things->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				if (const auto box = ReadActionPoint(records[i]); box && box->id == id) return i;
			}
			return std::nullopt;
		}
		void RemoveActionPointMarkers(std::uint8_t id) {
			const auto found = actionPointMarkers.find(id);
			if (found == actionPointMarkers.end()) return;
			for (const auto tag : found->second) RemoveObject(tag);
			actionPointMarkers.erase(found);
		}
		void MakeActionPointMarkers(std::uint8_t* world, const LevelFiles::ThingRecord& record) {
			const auto box = ReadActionPoint(record);
			if (!box || !objectsUsable || outlinesUsable) return;
			RemoveActionPointMarkers(box->id);
			auto& tags = actionPointMarkers[box->id];
			std::vector<std::pair<std::int32_t, std::int32_t>> corners{ { box->left, box->top }, { box->right, box->top }, { box->left, box->bottom }, { box->right, box->bottom } };
			std::sort(corners.begin(), corners.end());
			corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
			for (const auto& [x, y] : corners) {
				if (DocumentTile(x, y) == nullptr) continue;
				if (const auto tag = MakeObjectAndFind(world, MakeObjectRecord(x, y, ActionPointMarkerObject, NeutralOwner)); tag != 0) tags.push_back(tag);
			}
		}

		// ---- Action point outlines. The game draws one box on the map, the hand's, a tile at a time: drawing a tile on the box's
		// border puts the drawer's box piece there with the sides to show (Game::Address::TileDraw). Each tile drawn here gets the
		// same piece again for every action point it borders, so all of them show at once, and the one being dragged out too.
		struct OutlineBox {
			std::int32_t left, top, right, bottom;
		};
		std::vector<OutlineBox> outlineBoxes;
		// Remade after every world tick, which is where action points change, so drawing a tile only reads this.
		// Also the box being dragged out (an action point, a box of terrain, a copy), and where a paste would go.
		void RefreshOutlines(std::uint8_t* world) {
			outlineBoxes.clear();
			if (!outlinesUsable || !inSession || testing || !document || !document->things) return;
			for (const auto& record : document->things->records) {
				if (const auto box = ReadActionPoint(record)) outlineBoxes.push_back({ box->left, box->top, box->right, box->bottom });
			}
			if (selectedCreature != 0 && IsCreature(selectedCreature)) {
				const auto* creature = ThingOfTag(selectedCreature);
				const auto x = CreatureTileX(creature);
				const auto y = CreatureTileY(creature);
				outlineBoxes.push_back({ x, y, x, y });
				if (creatureDragging && (dragToX != x || dragToY != y)) outlineBoxes.push_back({ dragToX, dragToY, dragToX, dragToY });
			}
			if (actionPointDragging) {
				outlineBoxes.push_back({ (std::min)(actionPointStartX, actionPointEndX), (std::min)(actionPointStartY, actionPointEndY),
					(std::max)(actionPointStartX, actionPointEndX), (std::max)(actionPointStartY, actionPointEndY) });
			}
			if (tool == Tool::Terrain && paintMode == PaintMode::Paste && clipboard) {
				const auto* playerInterface = Load<const std::uint8_t*>(Load<const std::uint8_t*>(world + Game::Offset::WorldSession) + Game::Offset::SessionInterface);
				if (playerInterface != nullptr) {
					const auto* underHand = playerInterface + Game::Offset::InterfaceUnderHand;
					const auto x = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileX);
					const auto y = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileY);
					outlineBoxes.push_back({ x, y, x + clipboard->width - 1, y + clipboard->height - 1 });
				}
			}
		}
		// The piece's sides, as the game numbers them, and the bit the game always adds.
		constexpr std::uint32_t SideTop = 1;
		constexpr std::uint32_t SideBottom = 2;
		constexpr std::uint32_t SideLeft = 4;
		constexpr std::uint32_t SideRight = 8;
		constexpr std::uint32_t SidesShown = 0x10;
		constexpr std::int32_t TileSize = 0x1000;
		// The outlines' tint, from config.yml (editor_outline_color, six hex digits in the order the game keeps them, or "game" for
		// the hand's box's own). The byte order and whether the piece is drawn with it at all are [unverified].
		bool outlineTinted = false;
		std::array<std::uint8_t, 4> outlineTint{ 0x80, 0xC0, 0xFF, 0xFF };
		void ReadOutlineColor() {
			const auto text = Config::GetString("editor_outline_color", "80C0FF");
			outlineTinted = false;
			if (text.size() != 6) return;
			for (std::size_t i = 0; i < 3; ++i) {
				const auto digit = [&](char c) -> int {
					if (c >= '0' && c <= '9') return c - '0';
					if (c >= 'a' && c <= 'f') return c - 'a' + 10;
					if (c >= 'A' && c <= 'F') return c - 'A' + 10;
					return -1;
				};
				const auto high = digit(text[i * 2]);
				const auto low = digit(text[i * 2 + 1]);
				if (high < 0 || low < 0) return;
				outlineTint[i] = static_cast<std::uint8_t>(high * 16 + low);
			}
			outlineTinted = true;
		}
		using TileDrawFn = std::int32_t(__fastcall*)(std::uint8_t* drawer, void* unused, std::int32_t x, std::int32_t y, std::int32_t pass);
		TileDrawFn originalTileDraw = nullptr;
		std::int32_t __fastcall TileDrawDetour(std::uint8_t* drawer, void* unused, std::int32_t x, std::int32_t y, std::int32_t pass) {
			const auto result = originalTileDraw(drawer, unused, x, y, pass);
			if (result == 0 || pass != 0 || !inSession || testing || outlineBoxes.empty()) return result;
			std::uint32_t sides = 0;
			for (const auto& box : outlineBoxes) {
				if (x < box.left || x > box.right || y < box.top || y > box.bottom) continue;
				if (x == box.left) sides |= SideLeft;
				if (x == box.right) sides |= SideRight;
				if (y == box.top) sides |= SideTop;
				if (y == box.bottom) sides |= SideBottom;
			}
			if (sides == 0) return result;
			const auto index = Load<std::uint16_t>(drawer + Game::Offset::DrawerBoxPiece);
			if (index == 0) return result;
			auto* piece = reinterpret_cast<std::uint8_t* const*>(Game::Resolve(Game::Address::EngineThingTable))[index];
			auto* renderer = Load<std::uint8_t*>(drawer + Game::Offset::DrawerRenderer);
			if (piece == nullptr || renderer == nullptr) return result;
			// The piece is left as the game had it, so the hand's own box and the game's later pass see no difference.
			const auto savedSides = Load<std::uint32_t>(piece + Game::Offset::BoxPieceSides);
			const auto savedInUse = piece[Game::Offset::BoxPieceInUse];
			const bool wasPlaced = (piece[Game::Offset::BoxPiecePlaced] & 1) != 0;
			std::array<std::int32_t, 3> savedPosition{};
			std::memcpy(savedPosition.data(), piece + Game::Offset::BoxPiecePosition, sizeof(savedPosition));

			using MoveFn = std::int32_t(__fastcall*)(std::uint8_t* thing, void* unused, const std::int32_t* position);
			using DrawThingFn = std::int32_t(__fastcall*)(std::uint8_t* renderer, void* unused, std::int32_t x, std::int32_t y, std::uint8_t* thing, std::int32_t flag);
			const std::array<std::int32_t, 3> position{ x * TileSize + TileSize / 2, y * TileSize + TileSize / 2, 0 };
			GameFunction<MoveFn>(Game::Address::EngineThingMove)(piece, nullptr, position.data());
			Store<std::uint32_t>(piece + Game::Offset::BoxPieceSides, sides | SidesShown);
			piece[Game::Offset::BoxPieceInUse] = static_cast<std::uint8_t>(savedInUse | 1);
			const auto savedTint = Load<std::uint32_t>(piece + Game::Offset::EngineThingTint);
			if (outlineTinted) std::memcpy(piece + Game::Offset::EngineThingTint, outlineTint.data(), outlineTint.size());
			reinterpret_cast<DrawThingFn>(VirtualOf(renderer, Game::Offset::RendererDrawThingSlot))(renderer, nullptr, x, y, piece, 0);
			Store<std::uint32_t>(piece + Game::Offset::EngineThingTint, savedTint);

			Store<std::uint32_t>(piece + Game::Offset::BoxPieceSides, savedSides);
			piece[Game::Offset::BoxPieceInUse] = savedInUse;
			if (wasPlaced) GameFunction<MoveFn>(Game::Address::EngineThingMove)(piece, nullptr, savedPosition.data());
			return result;
		}
		void PushCreatureUndo(CreatureChange change) {
			UndoStep step;
			step.things.push_back(std::move(change));
			PushUndo(std::move(step));
		}
		std::uint8_t TerrainFlagsAt(std::uint8_t* world, const LevelFiles::Tile& tile) {
			const auto* data = reinterpret_cast<TerrainDataFn>(VirtualOf(world, Game::Offset::WorldTerrainData))(world, nullptr, tile.terrain);
			return data[Game::Offset::TerrainFlags];
		}
		bool HasPlayer(std::uint8_t* world, std::uint8_t number) {
			if (number < FirstOwner || number > LastOwner) return false;
			const auto* owner = Load<const std::uint8_t*>(world + Game::Offset::WorldPlayerTable) + (number - 1) * Game::PlayerTableEntrySize;
			return Load<std::uint16_t>(owner + 4) != 0;
		}
		// Why the thing a record makes cannot stand where it says, or nothing if it can. Creatures and objects want open ground;
		// traps and doors ground that can be built on, one to a tile.
		std::string PlacingProblem(std::uint8_t* world, const LevelFiles::ThingRecord& record) {
			const auto fields = ReadCreatureRecord(record);
			if (!fields) return "That cannot be placed";
			const auto* tile = DocumentTile(fields->x, fields->y);
			if (tile == nullptr) return "That is off the map";
			const auto flags = TerrainFlagsAt(world, *tile);
			const bool trapOrDoor = record.kind == TrapRecord || record.kind == DoorRecord;
			if (!trapOrDoor) {
				if ((flags & TerrainSolidFlag) != 0) return record.kind == ObjectRecord ? "Objects need open ground" : "Creatures need open ground";
				return {};
			}
			if ((flags & (TerrainSolidFlag | TerrainNoBuildFlag)) != 0) return "Traps and doors need solid ground";
			if (TrapOrDoorOnTile(fields->x, fields->y) != 0) return "This tile already has a trap or door";
			// A door stands between two walls, left and right or above and below, as the game's own placing wants. Any solid
			// terrain counts, reinforced or not. Barricades stand anywhere, as in the game (door data flag 0x40, read by 0x4A5070).
			if (record.kind == DoorRecord && NameFrom(doorNames, fields->type).find("Barricade") == std::string::npos) {
				const auto wall = [&](std::int32_t wallX, std::int32_t wallY) {
					const auto* next = DocumentTile(wallX, wallY);
					return next != nullptr && (TerrainFlagsAt(world, *next) & TerrainSolidFlag) != 0;
				};
				const auto x = fields->x;
				const auto y = fields->y;
				if (!(wall(x - 1, y) && wall(x + 1, y)) && !(wall(x, y - 1) && wall(x, y + 1))) return "Doors need a wall on both sides";
			}
			return {};
		}
		// The world's thing a record of the level's list made, found by its tile.
		std::uint16_t ThingOfRecord(const LevelFiles::ThingRecord& record) {
			const auto fields = ReadCreatureRecord(record);
			if (!fields) return 0;
			if (record.kind == ObjectRecord) return ListedObjectOnTile(fields->x, fields->y, fields->type);
			if (record.kind == TrapRecord || record.kind == DoorRecord) return TrapOrDoorOnTile(fields->x, fields->y, record.kind);
			return CreatureOnTile(fields->x, fields->y, fields->type, fields->owner, fields->level);
		}
		// The exact hit points kept for this creature record, if any, go (the creature is deleted or its level changed). Undo
		// brings them back with the step this is part of.
		void ForgetHealthEntry(const LevelFiles::ThingRecord& record);
		// After undo or redo put back a list of exact hit points: every creature in either list gets what it has now.
		void PutHealthBack(const std::vector<LevelFiles::CreatureHealth>& was, const std::vector<LevelFiles::CreatureHealth>& now);
		// Takes a listed thing out of the world and the list, as a change of the stroke under way.
		void TakeOutForStroke(std::uint8_t* world, std::size_t place) {
			auto& records = document->things->records;
			const auto record = records[place];
			const auto tag = ThingOfRecord(record);
			if (record.kind == ObjectRecord) RemoveObject(tag);
			else if (record.kind == TrapRecord || record.kind == DoorRecord) {
				if (tag != 0) SellTrapOrDoor(tag);
			}
			else {
				// The creature window open on it closes, as Delete closes it.
				const bool windowOnIt = tag != 0 && EditorDialogs::UnitId() == tag;
				RemoveCreature(world, tag);
				ForgetHealthEntry(record);
				if (windowOnIt) {
					EditorDialogs::CloseCreatureProperties(true);
					closedWindowRecord = record;
				}
				if (tag != 0 && selectedCreature == tag) {
					selectedCreature = 0;
					creatureDragging = false;
				}
			}
			strokeThings.push_back({ CreatureChangeKind::Deleted, place, record });
			records.erase(records.begin() + static_cast<std::ptrdiff_t>(place));
		}
		// What can no longer stand on a tile just painted: anything listed on rock, and traps and doors on water, lava and the
		// like. Things made by level setup rather than listed are left, as nothing could bring them back on undo.
		void ClearThingsForTerrain(std::uint8_t* world, std::int32_t x, std::int32_t y, std::uint8_t terrain) {
			if (!document->things) return;
			LevelFiles::Tile painted{};
			painted.terrain = terrain;
			const auto flags = TerrainFlagsAt(world, painted);
			if ((flags & (TerrainSolidFlag | TerrainNoBuildFlag)) == 0) return;
			const bool solid = (flags & TerrainSolidFlag) != 0;
			auto& records = document->things->records;
			for (std::size_t i = records.size(); i-- > 0;) {
				const auto& record = records[i];
				if (ReadActionPoint(record)) continue;
				const auto fields = ReadCreatureRecord(record);
				if (!fields || fields->x != x || fields->y != y) continue;
				const bool trapOrDoor = record.kind == TrapRecord || record.kind == DoorRecord;
				if (!solid && !trapOrDoor) continue;
				if (record.kind == ObjectRecord ? !objectsUsable : !creaturesUsable) continue;
				TakeOutForStroke(world, i);
			}
		}
		void PlaceListedOnly(std::uint8_t* world, std::int32_t x, std::int32_t y);
		void PlaceCreature(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			const auto* tile = DocumentTile(x, y);
			if (tile == nullptr || !DocumentReady()) return;
			const bool object = tool == Tool::Object;
			if (!document->things || (object ? !objectsUsable : !creaturesUsable)) {
				Say(object ? "Objects cannot be placed in this level, see the log" : "Creatures cannot be placed in this level, see the log");
				return;
			}
			if (!HasPlayer(world, ownerChoice)) {
				Say("This level has no " + OwnerName(ownerChoice));
				return;
			}
			// The variant chosen for it with a right click on the panel.
			const std::size_t creatureItem = creatureChoice - std::size_t{ 1 };
			const bool alternate = tool == Tool::Creature && alternateCreatures[creatureItem];
			const auto unique = uniqueOfCreature.find(creatureChoice);
			const bool asUnique = alternate && unique != uniqueOfCreature.end();
			const bool asMercenary = alternate && !asUnique;
			const std::uint8_t creatureType = asUnique ? unique->second : creatureChoice;
			// Set to go down as a dead body (the creature window's placing settings): as the dead body tool places one.
			if (tool == Tool::Creature && placeDeadTypes.contains(creatureChoice)) {
				const auto chosen = creatureChoice;
				creatureChoice = creatureType;
				tool = Tool::DeadBody;
				PlaceListedOnly(world, x, y);
				tool = Tool::Creature;
				creatureChoice = chosen;
				return;
			}
			auto record = tool == Tool::Trap ? MakeTrapRecord(x, y, trapChoice, ownerChoice) : tool == Tool::Door ? MakeDoorRecord(x, y, doorChoice, ownerChoice)
				: object ? MakeObjectRecord(x, y, objectChoice, ownerChoice) : MakeCreatureRecord(x, y, creatureType, ownerChoice);
			if (asMercenary) record.bytes[CreatureRecordFlags] |= RecordMercenaryFlag;
			if (tool == Tool::Creature) {
				if (const auto level = placeLevels.find(creatureChoice); level != placeLevels.end()) record.bytes[CreatureRecordLevel] = level->second;
				if (creatureType > loadedCreatureTypes) {
					Say(CreatureName(creatureType) + " is a new type: it can be placed once the level is loaded again (save, then Test)");
					return;
				}
			}
			if (tool == Tool::Trap && blueprintItems[trapChoice - std::size_t{ 1 }]) record.bytes[0xC] = 0;
			if (tool == Tool::Door && blueprintItems[doorChoice - std::size_t{ 1 } + DoorItemBase]) record.bytes[0x10] = 2;
			if (const auto problem = PlacingProblem(world, record); !problem.empty()) {
				Say(problem);
				return;
			}
			if (!MakeCreature(world, record)) {
				Say("The level has too many of those");
				return;
			}
			EndStroke(world);
			auto& records = document->things->records;
			PushCreatureUndo({ CreatureChangeKind::Placed, records.size(), record });
			records.push_back(std::move(record));
			unsavedChanges = true;
			const auto name = tool == Tool::Trap ? NameFrom(trapNames, trapChoice) : tool == Tool::Door ? NameFrom(doorNames, doorChoice)
				: object ? ObjectRecordName(records.back()) : (asMercenary ? "Mercenary " : "") + CreatureName(creatureType);
			Say("Placed " + name + " at " + std::to_string(x) + ", " + std::to_string(y));
		}
		// ---- Dead bodies (kind 0xCA, 0x10 bytes) and effect generators (kind 0xCB, 0x24 bytes), made as level load makes them [verified:
		// the things reader 0x570500 and 0x570630 lay them out for the makers 0x5212D0 and 0x521380, which read these places].
		// Dead body: +0 x, +4 y, +8 z, +0xC 16 bit, +0xE the creature's type, +0xF a player (the maker gives it to neutral).
		// Effect generator: +0 x, +4 y, +8 z, +0xC a dword, +0x10 and +0x12 16 bit, +0x14 four 16 bit effects, +0x1C a byte, +0x1D its
		// type. The only stock one (Level17) has 8, 8, effects 3 3 3 3, 10 and type 1, which a new one copies [verified: the survey].
		// Both are taken out of the world through their own remove slot (RemoveListedOnlyThing), on Delete and on undoing their
		// placing, and made again on undoing their deletion.
		constexpr std::uint32_t DeadBodyRecord = 0xCA;
		constexpr std::uint32_t EffectGeneratorRecord = 0xCB;
		bool IsListedOnly(std::uint32_t kind) { return kind == DeadBodyRecord || kind == EffectGeneratorRecord; }
		std::string ListedOnlyText(const LevelFiles::ThingRecord& record) {
			if (record.kind == DeadBodyRecord && record.bytes.size() >= 0x10) return "Dead " + CreatureName(record.bytes[0xE]);
			if (record.kind == EffectGeneratorRecord && record.bytes.size() >= 0x1E) {
				const auto* bytes = record.bytes.data();
				return "Effect generator type " + std::to_string(bytes[0x1D]) + ", effects " + std::to_string(Load<std::uint16_t>(bytes + 0x14)) + " " + std::to_string(Load<std::uint16_t>(bytes + 0x16))
					+ " " + std::to_string(Load<std::uint16_t>(bytes + 0x18)) + " " + std::to_string(Load<std::uint16_t>(bytes + 0x1A));
			}
			return "Thing";
		}
		bool MakeListedOnly(std::uint8_t* world, const LevelFiles::ThingRecord& record) {
			using MakeFn = std::int32_t(__cdecl*)(std::uint8_t* decoded, std::uint8_t* world);
			// What the reader hands the maker: flag dwords (all 1), then the fields where the maker reads them.
			alignas(4) std::array<std::uint8_t, 0x60> decoded{};
			const auto* bytes = record.bytes.data();
			if (record.kind == DeadBodyRecord) {
				if (!deadBodiesUsable || record.bytes.size() < 0x10) return false;
				for (std::size_t at = 0; at < 0x10; at += 4) Store<std::uint32_t>(decoded.data() + at, 1);
				std::memcpy(decoded.data() + 0x10, bytes, 0x10);
				return GameFunction<MakeFn>(Game::Address::DeadBodyMake)(decoded.data(), world) != 0;
			}
			if (record.kind == EffectGeneratorRecord) {
				if (!effectGeneratorsUsable || record.bytes.size() < 0x1E) return false;
				for (std::size_t at = 0; at < 0x28; at += 4) Store<std::uint32_t>(decoded.data() + at, 1);
				std::memcpy(decoded.data() + 0x28, bytes, 0x10);
				std::memcpy(decoded.data() + 0x38, bytes + 0x10, 0xE);
				return GameFunction<MakeFn>(Game::Address::EffectGeneratorMake)(decoded.data(), world) != 0;
			}
			return false;
		}
		void PlaceListedOnly(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			const auto* tile = DocumentTile(x, y);
			if (tile == nullptr || !DocumentReady() || !document->things) return;
			if ((TerrainFlagsAt(world, *tile) & TerrainSolidFlag) != 0) {
				Say("That needs open ground");
				return;
			}
			LevelFiles::ThingRecord record;
			if (tool == Tool::DeadBody && creatureChoice > loadedCreatureTypes) {
				Say(CreatureName(creatureChoice) + " is a new type: it can be placed once the level is loaded again (save, then Test)");
				return;
			}
			if (tool == Tool::DeadBody) {
				record = { DeadBodyRecord, std::vector<std::uint8_t>(0x10, 0) };
				record.bytes[0xE] = creatureChoice;
				record.bytes[0xF] = ownerChoice;
			}
			else {
				record = { EffectGeneratorRecord, std::vector<std::uint8_t>(0x24, 0) };
				constexpr std::array<std::uint8_t, 14> Level17Settings{ 8, 0, 8, 0, 3, 0, 3, 0, 3, 0, 3, 0, 10, 1 };
				std::copy(Level17Settings.begin(), Level17Settings.end(), record.bytes.begin() + 0x10);
			}
			Store<std::int32_t>(record.bytes.data(), x);
			Store<std::int32_t>(record.bytes.data() + 4, y);
			if (!MakeListedOnly(world, record)) {
				Say("It could not be made here, see the log");
				Logger::Warning("Editor: the game's maker refused a " + ListedOnlyText(record) + " at " + std::to_string(x) + ", " + std::to_string(y) + ".");
				return;
			}
			EndStroke(world);
			auto& records = document->things->records;
			PushCreatureUndo({ CreatureChangeKind::Placed, records.size(), record });
			records.push_back(std::move(record));
			unsavedChanges = true;
			Say("Placed " + ListedOnlyText(records.back()) + " at " + std::to_string(x) + ", " + std::to_string(y));
		}
		std::optional<std::size_t> ListedOnlyAt(std::int32_t x, std::int32_t y) {
			if (!document || !document->things) return std::nullopt;
			const auto& records = document->things->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				if (IsListedOnly(records[i].kind) && records[i].bytes.size() >= 8 && Load<std::int32_t>(records[i].bytes.data()) == x && Load<std::int32_t>(records[i].bytes.data() + 4) == y) return i;
			}
			return std::nullopt;
		}
		// The world's dead body or effect generator a record made: one of its kind on its tile, preferring its type (a dead body's
		// +0x128 from record +0xE; an effect generator's +0x44 from record +0x1D). Removed through its vtable's remove slot.
		std::uint16_t ListedOnlyThingOfRecord(const LevelFiles::ThingRecord& record) {
			std::uintptr_t vtable = 0;
			std::size_t thingType = 0;
			std::size_t recordType = 0;
			if (record.kind == DeadBodyRecord && record.bytes.size() >= 0x10) {
				vtable = Game::Resolve(Game::Address::DeadBodyVtable);
				thingType = 0x128;
				recordType = 0xE;
			}
			else if (record.kind == EffectGeneratorRecord && record.bytes.size() >= 0x1E) {
				vtable = Game::Resolve(Game::Address::EffectGeneratorVtable);
				thingType = 0x44;
				recordType = 0x1D;
			}
			else return 0;
			const auto x = Load<std::int32_t>(record.bytes.data());
			const auto y = Load<std::int32_t>(record.bytes.data() + 4);
			std::uint16_t found = 0;
			for (std::uint16_t tag = 1; tag <= Game::TagMask; ++tag) {
				if (!ThingHasVtable(tag, vtable)) continue;
				const auto* thing = ThingOfTag(tag);
				if (CreatureTileX(thing) != x || CreatureTileY(thing) != y) continue;
				found = tag;
				if (thing[thingType] == record.bytes[recordType]) return tag;
			}
			return found;
		}
		bool RemoveListedOnlyThing(const LevelFiles::ThingRecord& record) {
			const auto tag = ListedOnlyThingOfRecord(record);
			if (tag == 0) return false;
			auto* thing = ThingOfTag(tag);
			using RemoveFn = void(__fastcall*)(std::uint8_t* thing);
			reinterpret_cast<RemoveFn>(VirtualOf(thing, Game::Offset::ThingRemoveSlot))(thing);
			return true;
		}
		void DeleteListedOnly(std::size_t place) {
			if (!document || !document->things || place >= document->things->records.size() || !IsListedOnly(document->things->records[place].kind)) return;
			auto& records = document->things->records;
			const auto name = ListedOnlyText(records[place]);
			const bool removed = RemoveListedOnlyThing(records[place]);
			PushCreatureUndo({ CreatureChangeKind::Deleted, place, records[place] });
			records.erase(records.begin() + static_cast<std::ptrdiff_t>(place));
			unsavedChanges = true;
			Say("Deleted " + name + (removed ? "" : " from the level's list; it shows until the level is opened again"));
		}
		// Its name in the level's list if it has a record there, as a Unique creature is its base type in the world.
		std::string CreatureDisplayName(const std::uint8_t* creature) {
			if (const auto place = FindCreatureRecord(creature)) {
				if (const auto fields = ReadCreatureRecord(document->things->records[*place])) return CreatureName(fields->type);
			}
			return CreatureName(creature[Game::Offset::CreatureType]);
		}
		// By making it again from its changed record, which is what the level will do with it. Level load is what knows how to
		// bring a creature up to a level, and this keeps the world and the file saying the same thing.
		void ChangeCreatureLevel(std::uint8_t* world, std::uint16_t tag, std::uint8_t level) {
			if (!IsCreature(tag)) return;
			const auto* creature = ThingOfTag(tag);
			const auto place = FindCreatureRecord(creature);
			if (!place) {
				Say("Its level cannot change, it is not in the level's list");
				return;
			}
			const auto name = CreatureDisplayName(creature);
			auto& record = document->things->records[*place];
			auto before = record;
			auto after = record;
			after.bytes[CreatureRecordLevel] = level;
			if (!RemoveCreature(world, tag)) return;
			if (!MakeCreature(world, after)) {
				MakeCreature(world, before);
				Say("Its level could not be changed");
				return;
			}
			EndStroke(world);
			ForgetHealthEntry(before);
			record = std::move(after);
			PushCreatureUndo({ CreatureChangeKind::Changed, *place, std::move(before) });
			unsavedChanges = true;
			Say(name + " is now level " + std::to_string(level));
		}
		void DeleteCreature(std::uint8_t* world, std::uint16_t tag) {
			if (IsObject(tag)) {
				const auto place = FindObjectRecord(tag);
				if (!place) {
					Say("That object is not in the level's list, so it stays");
					return;
				}
				auto& records = document->things->records;
				const auto name = ObjectRecordName(records[*place]);
				RemoveObject(tag);
				EndStroke(world);
				PushCreatureUndo({ CreatureChangeKind::Deleted, *place, records[*place] });
				records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
				unsavedChanges = true;
				Say("Deleted " + name);
				return;
			}
			if (IsTrap(tag) || IsDoor(tag)) {
				const auto place = FindTrapOrDoorRecord(tag);
				const auto name = TrapOrDoorName(tag);
				SellTrapOrDoor(tag);
				EndStroke(world);
				if (!place) {
					Say(name + " removed, but it is not in the level's list");
					return;
				}
				auto& records = document->things->records;
				PushCreatureUndo({ CreatureChangeKind::Deleted, *place, records[*place] });
				records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
				unsavedChanges = true;
				Say("Deleted " + name);
				return;
			}
			if (!IsCreature(tag)) return;
			const auto* creature = ThingOfTag(tag);
			const auto place = FindCreatureRecord(creature);
			const auto name = CreatureDisplayName(creature);
			const bool windowOnIt = EditorDialogs::UnitId() == tag;
			if (!RemoveCreature(world, tag)) return;
			// The creature window open on it has nothing left to show or change.
			if (windowOnIt) {
				EditorDialogs::CloseCreatureProperties(true);
				// Undo brings it back, and the window with it.
				if (place) closedWindowRecord = document->things->records[*place];
			}
			if (selectedCreature == tag) selectedCreature = 0;
			EndStroke(world);
			if (!place) {
				// Made by level setup rather than listed, so the level would make it again next time.
				Say(name + " removed, but it is not in the level's list");
				return;
			}
			auto& records = document->things->records;
			ForgetHealthEntry(records[*place]);
			PushCreatureUndo({ CreatureChangeKind::Deleted, *place, records[*place] });
			records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
			unsavedChanges = true;
			Say("Deleted " + name);
		}
		void PlaceActionPoint(std::uint8_t* world) {
			if (!DocumentReady() || !document->things) {
				Say("Action points cannot be placed in this level, see the log");
				return;
			}
			ActionPointBox box{ (std::min)(actionPointStartX, actionPointEndX), (std::min)(actionPointStartY, actionPointEndY),
				(std::max)(actionPointStartX, actionPointEndX), (std::max)(actionPointStartY, actionPointEndY), 0 };
			// Off the edge: and a box at x 0 would read as a center and a reach (ReadActionPoint).
			if (box.left < 1 || box.top < 1 || static_cast<std::uint32_t>(box.right) + 1 >= document->map.width || static_cast<std::uint32_t>(box.bottom) + 1 >= document->map.height) {
				Say(MapEdgeMessage);
				return;
			}
			// The lowest number not in use, as triggers refer to them by number. One byte holds it.
			std::array<bool, 256> used{};
			for (const auto& record : document->things->records) {
				if (const auto other = ReadActionPoint(record)) used[other->id] = true;
			}
			for (std::size_t id = 1; id < used.size(); ++id) {
				if (!used[id]) {
					box.id = static_cast<std::uint8_t>(id);
					break;
				}
			}
			if (box.id == 0) {
				Say("The level has 255 action points already");
				return;
			}
			EndStroke(world);
			auto record = MakeActionPointRecord(box);
			auto& records = document->things->records;
			PushCreatureUndo({ CreatureChangeKind::Placed, records.size(), record });
			MakeActionPointMarkers(world, record);
			records.push_back(std::move(record));
			unsavedChanges = true;
			Say("Placed " + ActionPointText(box));
		}
		std::vector<std::wstring> ActionPointUses(std::uint8_t id);
		// An action point that triggers use is only deleted the second time asked, after a warning.
		std::uint8_t deleteWarnedActionPoint = 0;
		void DeleteActionPoint(std::uint8_t* world, std::uint8_t id) {
			const auto place = FindActionPoint(id);
			if (!place) return;
			auto& records = document->things->records;
			const auto box = ReadActionPoint(records[*place]);
			const auto uses = ActionPointUses(id);
			if (!uses.empty() && deleteWarnedActionPoint != id) {
				deleteWarnedActionPoint = id;
				Say(ActionPointText(*box) + " is used by triggers (see its menu); delete it again to delete it anyway");
				return;
			}
			deleteWarnedActionPoint = 0;
			RemoveActionPointMarkers(id);
			EndStroke(world);
			PushCreatureUndo({ CreatureChangeKind::Deleted, *place, records[*place] });
			records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
			unsavedChanges = true;
			Say("Deleted " + ActionPointText(*box) + (uses.empty() ? "" : "; the triggers that used it now name an action point that is not there"));
		}
		// The local player's Dungeon Heart starts with its Arches and rings see-through, for the heart's tick to fade them in as the
		// level opens. Nothing ticks here, so they are put where that fade ends. Other keepers' are never faded.
		void FinishHeartFadeIn() {
			constexpr std::int32_t ObjectsList = Game::Offset::ObjectsList;
			constexpr std::uint8_t ArchesObject = 86;
			constexpr std::uint8_t HeartRingObject = 88;
			constexpr std::uint32_t FadingIn = 1;
			std::uint32_t finished = 0;
			for (std::uint16_t tag = 1; tag <= Game::TagMask; ++tag) {
				if (reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::ThingInUse))[tag] == 0) continue;
				auto* thing = ThingOfTag(tag);
				if (thing == nullptr || Load<std::int32_t>(thing + Game::Offset::ThingList) != ObjectsList) continue;
				const auto type = thing[Game::Offset::ObjectType];
				if (type != ArchesObject && type != HeartRingObject) continue;
				const auto flags = Load<std::uint32_t>(thing + Game::Offset::ThingFadeFlags);
				if ((flags & FadingIn) == 0) continue;
				thing[Game::Offset::ThingAlpha] = 0xFF;
				Store<std::uint32_t>(thing + Game::Offset::ThingFadeFlags, flags & ~FadingIn);
				++finished;
			}
			Logger::Debug("Editor: finished the Dungeon Heart's fade in on " + std::to_string(finished) + " objects.");
		}
		// A room tile sold for nothing, by the same steps as painting over one. What it leaves is read back, and undo paints the room
		// back. The Dungeon Heart and hero gates are things in the level's list as well as tiles, so they stay.
		void SellRoomTile(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			auto* tile = DocumentTile(x, y);
			auto* worldTile = reinterpret_cast<TileAtFn>(VirtualOf(world, Game::Offset::WorldTileAt))(world, nullptr, x, y);
			if (tile == nullptr || worldTile == nullptr || (Load<std::uint16_t>(worldTile + Game::Offset::TileRoom) & Game::TagMask) == 0) return;
			const auto* roomOwner = PlayerThing(Load<std::uint16_t>(worldTile + Game::Offset::TileOwner) & Game::TagMask);
			if (roomOwner == nullptr) return;
			auto* rooms = world + Game::Offset::WorldRooms;
			const auto before = *tile;
			{
				const NothingForSale free;
				GameFunction<RoomsTileFn>(Game::Address::RoomsBeforeRemoveTile)(rooms, nullptr, x, y, x, y);
				GameFunction<PlayerRemoveFn>(Game::Address::PlayerRemoveRoomTile)(roomOwner, nullptr, x, y, 0);
				GameFunction<RoomsTileFn>(Game::Address::RoomsAfterRemoveTile)(rooms, nullptr, x, y, x, y);
			}
			tile->terrain = worldTile[Game::Offset::TileTerrain];
			const auto* owner = PlayerThing(Load<std::uint16_t>(worldTile + Game::Offset::TileOwner) & Game::TagMask);
			tile->owner = owner != nullptr ? owner[Game::Offset::PlayerNumber] : NeutralOwner;
			EndStroke(world);
			stroke.push_back({ x, y, before });
			// What is left of the room, built again with the stroke's end.
			roomSeeds.push_back({ x, y });
			EndStroke(world);
			unsavedChanges = true;
			Say("Room tile deleted at " + std::to_string(x) + ", " + std::to_string(y));
		}
		// Takes one change back, and gives the change that does it again: a placing's is a deletion, a deletion's a placing, and a
		// change's the record as it was before going back. Nothing when it could not be done.
		std::optional<CreatureChange> UndoCreatureChange(std::uint8_t* world, const CreatureChange& change) {
			if (!document || !document->things) return std::nullopt;
			auto& records = document->things->records;
			const auto insertAt = (std::min)(change.index, records.size());
			// Action points and hero parties are only in the list (action points with markers in the world).
			constexpr std::uint32_t PartyKind = 0xC9;
			const auto box = ReadActionPoint(change.record);
			// Dead bodies and effect generators too, as nothing found removes them from the world (see DeadBodyRecord).
			if (box || change.record.kind == PartyKind || IsListedOnly(change.record.kind)) {
				if (change.kind == CreatureChangeKind::Placed) {
					if (box) RemoveActionPointMarkers(box->id);
					// A dead body or effect generator leaves the world too.
					RemoveListedOnlyThing(change.record);
					if (change.index < records.size()) records.erase(records.begin() + static_cast<std::ptrdiff_t>(change.index));
					return CreatureChange{ CreatureChangeKind::Deleted, change.index, change.record };
				}
				if (change.kind == CreatureChangeKind::Deleted) {
					records.insert(records.begin() + static_cast<std::ptrdiff_t>(insertAt), change.record);
					if (box) MakeActionPointMarkers(world, change.record);
					// A dead body or effect generator taken away with its record comes back into the world.
					if (IsListedOnly(change.record.kind)) MakeListedOnly(world, change.record);
					return CreatureChange{ CreatureChangeKind::Placed, insertAt, change.record };
				}
				// A rename: only its record changes, as it stays where it was.
				if (change.index >= records.size()) return std::nullopt;
				auto now = records[change.index];
				records[change.index] = change.record;
				return CreatureChange{ CreatureChangeKind::Changed, change.index, std::move(now) };
			}
			const auto fields = ReadCreatureRecord(change.record);
			if (change.kind == CreatureChangeKind::Placed) {
				if (fields && change.record.kind == ObjectRecord) RemoveObject(ListedObjectOnTile(fields->x, fields->y, fields->type));
				else if (fields && (change.record.kind == TrapRecord || change.record.kind == DoorRecord)) RemoveTrapOrDoor(TrapOrDoorOnTile(fields->x, fields->y, change.record.kind));
				else if (fields) RemoveCreature(world, CreatureOnTile(fields->x, fields->y, fields->type, fields->owner, fields->level));
				if (change.index < records.size()) records.erase(records.begin() + static_cast<std::ptrdiff_t>(change.index));
				return CreatureChange{ CreatureChangeKind::Deleted, change.index, change.record };
			}
			if (change.kind == CreatureChangeKind::Changed) {
				if (!fields || change.index >= records.size()) return std::nullopt;
				// The one made from the record as it is now, which goes for the one this step's record makes.
				auto currentRecord = records[change.index];
				const auto current = ReadCreatureRecord(currentRecord);
				RemoveCreature(world, CreatureOnTile(fields->x, fields->y, fields->type, fields->owner, current ? current->level : std::uint8_t{ 0 }));
				if (!MakeCreature(world, change.record)) {
					Say("The level has too many creatures");
					return std::nullopt;
				}
				records[change.index] = change.record;
				return CreatureChange{ CreatureChangeKind::Changed, change.index, std::move(currentRecord) };
			}
			if (!MakeCreature(world, change.record)) {
				Say("The level has too many of those");
				return std::nullopt;
			}
			records.insert(records.begin() + static_cast<std::ptrdiff_t>(insertAt), change.record);
			return CreatureChange{ CreatureChangeKind::Placed, insertAt, change.record };
		}
		// Goes back through a step, the tiles first so things have their ground again, and gives the step that goes forward again.
		// Each of its lists is in the order done here, so going back through it is the step's own order.
		UndoStep TakeStepBack(std::uint8_t* world, const UndoStep& step) {
			UndoStep back;
			// Back to front, as a stroke can pass over a tile twice.
			for (auto change = step.tiles.rbegin(); change != step.tiles.rend(); ++change) {
				auto* tile = DocumentTile(change->x, change->y);
				if (tile == nullptr) continue;
				const auto now = *tile;
				if (!PaintTile(world, change->x, change->y, change->before.terrain, change->before.owner)) continue;
				*tile = change->before;
				back.tiles.push_back({ change->x, change->y, now });
			}
			for (auto change = step.things.rbegin(); change != step.things.rend(); ++change) {
				if (auto again = UndoCreatureChange(world, *change)) back.things.push_back(std::move(*again));
			}
			// The exact hit points as they were, after the creatures are back (a creature made again has its record's percent).
			if (step.health && document) {
				back.health = std::exchange(document->creatureHealth, *step.health);
				document->creatureHealthChanged = true;
				PutHealthBack(*back.health, document->creatureHealth);
			}
			if (document) healthAtLastStep = document->creatureHealth;
			RebuildTouchedRooms(world);
			if (!step.tiles.empty()) RevealMap(world);
			unsavedChanges = true;
			leaveWarned = false;
			return back;
		}
		std::optional<EditorDialogs::UnitData> UnitDataOf(std::uint16_t tag);
		void PutDraggedBack(std::uint8_t* world);
		void OpenCreatureWindow(std::uint8_t type, std::uint16_t unitTag = 0, bool reopen = false);
		// The record of a placed creature (the creature window's, or the chosen one) before undo or redo, to find it again after.
		std::optional<LevelFiles::ThingRecord> RecordOfCreature(std::optional<std::uint16_t> tag) {
			if (!tag || !IsCreature(*tag) || !document || !document->things) return std::nullopt;
			const auto place = FindCreatureRecord(ThingOfTag(*tag));
			if (!place) return std::nullopt;
			return document->things->records[*place];
		}
		// After undo or redo: that creature as it is now. Its record unchanged (a step of hit points alone, or one about others),
		// it is where it was; changed, it is what its place in the list holds now; taken out and another like it put in (a move),
		// it is that one; else it is gone (0). "back" is the step that would do it again.
		std::uint16_t CreatureAfterStep(const std::optional<LevelFiles::ThingRecord>& record, const UndoStep& back) {
			if (!record || !document || !document->things) return 0;
			auto& records = document->things->records;
			const auto same = [](const LevelFiles::ThingRecord& a, const LevelFiles::ThingRecord& b) { return a.kind == b.kind && a.bytes == b.bytes; };
			std::optional<LevelFiles::ThingRecord> now;
			if (std::any_of(records.begin(), records.end(), [&](const LevelFiles::ThingRecord& r) { return same(r, *record); })) now = *record;
			else {
				const auto was = ReadCreatureRecord(*record);
				bool takenOut = false;
				for (const auto& change : back.things) {
					if (change.kind == CreatureChangeKind::Changed && same(change.record, *record) && change.index < records.size()) now = records[change.index];
					if (change.kind == CreatureChangeKind::Deleted && same(change.record, *record)) takenOut = true;
				}
				if (!now && takenOut && was) {
					for (const auto& change : back.things) {
						const auto fields = ReadCreatureRecord(change.record);
						if (change.kind == CreatureChangeKind::Placed && change.record.kind == record->kind && fields && fields->type == was->type && fields->owner == was->owner) now = change.record;
					}
				}
			}
			const auto tag = now && IsCreatureRecord(*now) ? ThingOfRecord(*now) : std::uint16_t{ 0 };
			return tag != 0 && IsCreature(tag) ? tag : std::uint16_t{ 0 };
		}
		// Takes a step back (undo, or redo from its stack) with the creature window's creature and the chosen creature following
		// it. A drag under way ends first, its creature back on its own tile, as the step works from the list.
		UndoStep StepBackFollowing(std::uint8_t* world, const UndoStep& step) {
			if (creatureDragging) PutDraggedBack(world);
			const auto unitRecord = RecordOfCreature(EditorDialogs::UnitId());
			const auto chosenRecord = selectedCreature != 0 ? RecordOfCreature(selectedCreature) : std::nullopt;
			const bool hadChosen = selectedCreature != 0;
			auto back = TakeStepBack(world, step);
			if (unitRecord) {
				const auto tag = CreatureAfterStep(unitRecord, back);
				if (tag != 0) EditorDialogs::RefreshUnit(UnitDataOf(tag));
				else {
					// Taken away again (redo of a delete): the window closes as the delete closed it, to open again on undo.
					EditorDialogs::CloseCreatureProperties(true);
					closedWindowRecord = unitRecord;
				}
			}
			else if (closedWindowRecord && !EditorDialogs::CreaturePropertiesOpen() && document && document->things) {
				// Back again: the window opens on it as it was closed.
				const auto& records = document->things->records;
				const auto found = std::find_if(records.begin(), records.end(), [&](const LevelFiles::ThingRecord& record) {
					return record.kind == closedWindowRecord->kind && record.bytes == closedWindowRecord->bytes;
				});
				const auto tag = found != records.end() ? ThingOfRecord(*found) : std::uint16_t{ 0 };
				if (tag != 0 && IsCreature(tag)) OpenCreatureWindow(0, tag, true);
			}
			if (hadChosen) {
				// One not in the list (made by level setup) stays chosen if it is still there.
				selectedCreature = chosenRecord ? CreatureAfterStep(chosenRecord, back) : IsCreature(selectedCreature) ? selectedCreature : std::uint16_t{ 0 };
			}
			return back;
		}
		void UndoStroke(std::uint8_t* world) {
			if (undoSteps.empty()) {
				Say("Nothing to undo");
				return;
			}
			const auto step = std::move(undoSteps.back());
			undoSteps.pop_back();
			PushStep(redoSteps, StepBackFollowing(world, step));
			Say("Undone");
		}
		void RedoStroke(std::uint8_t* world) {
			if (redoSteps.empty()) {
				Say("Nothing to redo");
				return;
			}
			const auto step = std::move(redoSteps.back());
			redoSteps.pop_back();
			PushStep(undoSteps, StepBackFollowing(world, step));
			Say("Redone");
		}

		// ---- Box and fill: the terrain chosen over many tiles, as one undo step.
		struct TileBox {
			std::int32_t left, top, right, bottom;
		};
		TileBox DraggedBox() {
			return { (std::min)(actionPointStartX, actionPointEndX), (std::min)(actionPointStartY, actionPointEndY),
				(std::max)(actionPointStartX, actionPointEndX), (std::max)(actionPointStartY, actionPointEndY) };
		}
		std::string BoxText(const TileBox& box) {
			return std::to_string(box.left) + "," + std::to_string(box.top) + " to " + std::to_string(box.right) + "," + std::to_string(box.bottom);
		}
		std::size_t BoxTiles(const TileBox& box) {
			return static_cast<std::size_t>(box.right - box.left + 1) * static_cast<std::size_t>(box.bottom - box.top + 1);
		}
		void PaintBox(std::uint8_t* world, const TileBox& box) {
			if (!DocumentReady()) return;
			if (BoxTiles(box) > MostTilesAtOnce) {
				Say("That box is more than " + std::to_string(MostTilesAtOnce) + " tiles");
				return;
			}
			EndStroke(world);
			for (auto y = box.top; y <= box.bottom; ++y) {
				for (auto x = box.left; x <= box.right; ++x) {
					// The edge is skipped quietly here: a box reaching it still paints the rest.
					if (DocumentTile(x, y) != nullptr && !OnMapEdge(x, y)) PaintTo(world, x, y, terrains[terrainChoice].id, ownerChoice);
				}
			}
			const auto painted = stroke.size();
			EndStroke(world);
			Say("Painted " + std::to_string(painted) + " tiles, " + BoxText(box));
		}
		// Every tile joined to this one, side by side, with the same terrain and owner, inside the map's edge.
		void FloodFill(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			const auto* start = DocumentTile(x, y);
			if (start == nullptr || !DocumentReady() || OnMapEdge(x, y)) return;
			const auto from = *start;
			const auto terrain = terrains[terrainChoice].id;
			if (from.terrain == terrain && from.owner == ownerChoice) {
				Say("That is already " + TerrainName());
				return;
			}
			const auto width = static_cast<std::size_t>(document->map.width);
			std::vector<bool> seen(width * document->map.height, false);
			std::vector<std::pair<std::int32_t, std::int32_t>> found;
			std::vector<std::pair<std::int32_t, std::int32_t>> waiting{ { x, y } };
			seen[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = true;
			while (!waiting.empty()) {
				const auto [atX, atY] = waiting.back();
				waiting.pop_back();
				found.push_back({ atX, atY });
				if (found.size() > MostTilesAtOnce) {
					Say("That area is more than " + std::to_string(MostTilesAtOnce) + " tiles; fill a box instead");
					return;
				}
				for (const auto& [nextX, nextY] : { std::pair{ atX + 1, atY }, std::pair{ atX - 1, atY }, std::pair{ atX, atY + 1 }, std::pair{ atX, atY - 1 } }) {
					const auto* next = DocumentTile(nextX, nextY);
					if (next == nullptr || OnMapEdge(nextX, nextY)) continue;
					const auto index = static_cast<std::size_t>(nextY) * width + static_cast<std::size_t>(nextX);
					if (seen[index] || next->terrain != from.terrain || next->owner != from.owner) continue;
					seen[index] = true;
					waiting.push_back({ nextX, nextY });
				}
			}
			EndStroke(world);
			for (const auto& [atX, atY] : found) PaintTo(world, atX, atY, terrain, ownerChoice);
			const auto painted = stroke.size();
			EndStroke(world);
			Say("Filled " + std::to_string(painted) + " tiles with " + TerrainName());
		}

		// ---- Copy and paste: a box's tiles and the listed things on it. Action points stay, as triggers know them by number.
		// Hearts and hero gates stay too (see IsListedRoomTerrain), both their tiles and their things.
		void CopyBox(const TileBox& box) {
			if (!DocumentReady()) return;
			if (BoxTiles(box) > MostTilesAtOnce) {
				Say("That box is more than " + std::to_string(MostTilesAtOnce) + " tiles");
				return;
			}
			Clipboard copied;
			copied.width = box.right - box.left + 1;
			copied.height = box.bottom - box.top + 1;
			for (auto y = box.top; y <= box.bottom; ++y) {
				for (auto x = box.left; x <= box.right; ++x) {
					const auto* tile = DocumentTile(x, y);
					if (tile == nullptr || OnMapEdge(x, y) || IsListedRoomTerrain(tile->terrain)) copied.tiles.push_back(std::nullopt);
					else copied.tiles.push_back(*tile);
				}
			}
			if (document->things) {
				for (const auto& record : document->things->records) {
					if (record.kind != ObjectRecord && record.kind != TrapRecord && record.kind != DoorRecord && !IsCreatureKind(record.kind)) continue;
					const auto fields = ReadCreatureRecord(record);
					if (!fields || fields->x < box.left || fields->x > box.right || fields->y < box.top || fields->y > box.bottom) continue;
					auto moved = record;
					Store<std::int32_t>(moved.bytes.data(), fields->x - box.left);
					Store<std::int32_t>(moved.bytes.data() + 4, fields->y - box.top);
					copied.things.push_back(std::move(moved));
				}
			}
			const auto things = copied.things.size();
			clipboard = std::move(copied);
			paintMode = PaintMode::Paste;
			Say("Copied " + BoxText(box) + " with " + std::to_string(things) + " things; click where its top left goes");
		}
		void PasteAt(std::uint8_t* world, std::int32_t left, std::int32_t top) {
			if (!clipboard || !DocumentReady()) return;
			EndStroke(world);
			for (std::int32_t y = 0; y < clipboard->height; ++y) {
				for (std::int32_t x = 0; x < clipboard->width; ++x) {
					const auto& tile = clipboard->tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(clipboard->width) + static_cast<std::size_t>(x)];
					if (!tile || DocumentTile(left + x, top + y) == nullptr || OnMapEdge(left + x, top + y)) continue;
					PaintTo(world, left + x, top + y, tile->terrain, tile->owner);
				}
			}
			const auto tiles = stroke.size();
			std::size_t placed = 0;
			std::size_t refused = 0;
			if (document->things) {
				for (const auto& copied : clipboard->things) {
					auto record = copied;
					const auto fields = ReadCreatureRecord(record);
					if (!fields) continue;
					Store<std::int32_t>(record.bytes.data(), left + fields->x);
					Store<std::int32_t>(record.bytes.data() + 4, top + fields->y);
					const bool usable = record.kind == ObjectRecord ? objectsUsable : creaturesUsable;
					if (!usable || DocumentTile(left + fields->x, top + fields->y) == nullptr || !HasPlayer(world, fields->owner) || !PlacingProblem(world, record).empty()
						|| !MakeCreature(world, record)) {
						++refused;
						continue;
					}
					auto& records = document->things->records;
					strokeThings.push_back({ CreatureChangeKind::Placed, records.size(), record });
					records.push_back(std::move(record));
					unsavedChanges = true;
					++placed;
				}
			}
			EndStroke(world);
			Say("Pasted " + std::to_string(tiles) + " tiles and " + std::to_string(placed) + " things" + (refused != 0 ? ", " + std::to_string(refused) + " things did not fit" : ""));
		}

		// ---- Moving a listed thing or an action point to the tile clicked, as one undo step: out of the list, then in again.
		void StopMoving() {
			if (tool == Tool::Move) tool = toolBeforeMove;
			moveTag = 0;
			moveActionPoint = 0;
		}
		void MoveHealthEntry(const CreatureFields& from, const CreatureFields& to);
		void MoveThingTo(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			const auto tag = moveTag;
			const auto actionPoint = moveActionPoint;
			StopMoving();
			// The press that put it down must not go on to paint.
			leftDown = false;
			if (!DocumentReady() || !document->things || DocumentTile(x, y) == nullptr) return;
			auto& records = document->things->records;
			std::optional<std::size_t> place;
			if (actionPoint != 0) place = FindActionPoint(actionPoint);
			else if (IsCreature(tag)) place = FindCreatureRecord(ThingOfTag(tag));
			else if (IsTrap(tag) || IsDoor(tag)) place = FindTrapOrDoorRecord(tag);
			else if (IsObject(tag)) place = FindObjectRecord(tag);
			if (!place) {
				Say("It is not in the level's list any more, so it cannot be moved");
				return;
			}
			const auto before = records[*place];
			auto after = before;
			std::string name;
			if (const auto box = ReadActionPoint(before)) {
				const ActionPointBox moved{ x, y, x + (box->right - box->left), y + (box->bottom - box->top), box->id };
				if (OnMapEdge(moved.left, moved.top) || OnMapEdge(moved.right, moved.bottom)) {
					Say(MapEdgeMessage);
					return;
				}
				// Its corners, the rest of the record kept (a center and reach is written as corners).
				Store<std::int32_t>(after.bytes.data(), moved.left);
				Store<std::int32_t>(after.bytes.data() + 4, moved.top);
				Store<std::int32_t>(after.bytes.data() + 8, moved.right);
				Store<std::int32_t>(after.bytes.data() + 0xC, moved.bottom);
				EndStroke(world);
				RemoveActionPointMarkers(box->id);
				records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
				records.push_back(after);
				MakeActionPointMarkers(world, records.back());
				name = ActionPointText(moved);
			}
			else {
				const auto fields = ReadCreatureRecord(before);
				if (!fields || (fields->x == x && fields->y == y)) return;
				Store<std::int32_t>(after.bytes.data(), x);
				Store<std::int32_t>(after.bytes.data() + 4, y);
				const bool trapOrDoor = before.kind == TrapRecord || before.kind == DoorRecord;
				if (const auto problem = PlacingProblem(world, after); !problem.empty()) {
					Say(problem);
					return;
				}
				if (before.kind == ObjectRecord) {
					name = ObjectRecordName(before);
					RemoveObject(tag);
				}
				else if (trapOrDoor) {
					name = TrapOrDoorName(tag);
					SellTrapOrDoor(tag);
				}
				// A creature keeps its hit points, as its exact ones in the level follow it.
				const bool creature = before.kind != ObjectRecord && !trapOrDoor;
				const bool hasHealth = creature && IsCreatureRecord(before) && IsCreature(tag);
				const auto points = hasHealth ? Load<std::int32_t>(ThingOfTag(tag) + Game::Offset::CreatureHealth) : 0;
				if (creature) {
					name = CreatureDisplayName(ThingOfTag(tag));
					RemoveCreature(world, tag);
				}
				if (!MakeCreature(world, after)) {
					MakeCreature(world, before);
					Say(name + " could not be made there");
					return;
				}
				if (hasHealth) {
					if (const auto made = ThingOfRecord(after); made != 0) Store<std::int32_t>(ThingOfTag(made) + Game::Offset::CreatureHealth, points);
				}
				EndStroke(world);
				// Its exact hit points move with it, in this move's undo step (after anything under way went in its own).
				if (hasHealth) {
					if (const auto to = ReadCreatureRecord(after)) MoveHealthEntry(*fields, *to);
				}
				records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
				records.push_back(after);
			}
			strokeThings.push_back({ CreatureChangeKind::Deleted, *place, before });
			strokeThings.push_back({ CreatureChangeKind::Placed, records.size() - 1, after });
			EndStroke(world);
			unsavedChanges = true;
			Say("Moved " + name + " to " + std::to_string(x) + ", " + std::to_string(y));
		}
		void PointGameAtSavedCopy();
		void SaveDocument() {
			if (!document) {
				Say("nothing loaded to save");
				return;
			}
			std::string error;
			if (LevelFiles::SaveMap(documentLevel, *document, error)) {
				unsavedChanges = false;
				leaveWarned = false;
				statusText = "Saved";
				Logger::Info("Editor: saved to " + document->path.string() + ".");
				PointGameAtSavedCopy();
				// Only levels of our own: a stock level's picture stays the game's.
				if (LevelFiles::IsEditorLevel(documentLevel) && !LevelFiles::WriteThumbnail(document->map, documentLevel, error)) Logger::Warning("Editor: no thumbnail: " + error);
			}
			else {
				Say("Could not save, see the log");
				Logger::Warning("Editor: " + error);
			}
		}
		// The game's restart: the level torn down and loaded again from its files (by the level name), in the same game.
		bool ReloadLevel(std::uint8_t* world) {
			using ReloadFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, const std::uint8_t* action);
			// It reads nothing of its action [verified: 0x513870 uses only the world]; the kind is set for any later reader.
			std::array<std::uint8_t, 0x12> action{};
			action[0xC] = 0x66;
			const bool reloaded = GameFunction<ReloadFn>(Game::Address::ReloadLevelInPlace)(world, nullptr, action.data()) != 0;
			if (!reloaded) Logger::Warning("Editor: the game could not load the level again.");
			return reloaded;
		}
		// A new level of its own in Data\editor\maps, which is then the one being edited and the one the game loads.
		// The level the game loads, by name.
		void SetGameLevelName(const std::wstring& name) {
			auto* level = reinterpret_cast<wchar_t*>(Game::Resolve(Game::Address::GameConfigLevelName));
			std::wcsncpy(level, name.c_str(), Game::LevelNameLength - 1);
			level[Game::LevelNameLength - 1] = 0;
		}
		// A stock level saved for the first time while open has its copy in KeeperForge2\maps only now, but the game's level
		// name still named the stock files (it is pointed at the copy when a level is opened that already has one). Test and
		// Back to editing load the level again by that name, so they showed the stock level: nothing that was saved, while
		// the editor's own list (read from the copy) had it all. The name now goes to the copy as soon as there is one.
		void PointGameAtSavedCopy() {
			if (LevelFiles::IsEditorLevel(documentLevel) || !LevelFiles::HasSavedCopy(documentLevel)) return;
			const auto path = LevelFiles::SavedLevelName(documentLevel);
			if (path.size() >= Game::LevelNameLength) {
				Logger::Warning("Editor: the name of this level's saved copy is too long; Test plays the original.");
				return;
			}
			const auto* level = reinterpret_cast<const wchar_t*>(Game::Resolve(Game::Address::GameConfigLevelName));
			if (path == std::wstring(level, wcsnlen(level, Game::LevelNameLength))) return;
			SetGameLevelName(path);
			Logger::Info("Editor: the level is now loaded from its saved copy.");
		}
		void SaveDocumentAs(const std::string& name) {
			if (!document) return;
			const std::wstring wide(name.begin(), name.end());
			std::string error;
			if (!LevelFiles::SaveLevelAs(LevelFiles::LevelPath(documentLevel), wide, *document, error)) {
				Say("Could not save as " + name + ", see the log");
				Logger::Warning("Editor: " + error);
				return;
			}
			documentLevel = wide;
			SetGameLevelName(wide);
			unsavedChanges = false;
			if (!LevelFiles::WriteThumbnail(document->map, wide, error)) Logger::Warning("Editor: no thumbnail: " + error);
			Say("Saved as " + name);
			Logger::Info("Editor: saved as " + name + " in Data\\editor\\maps.");
		}
		// The Creatures tab's order, the user's: the keeper creatures on the first row, the heroes on the second, then anything
		// else (Jack, types added in the editor) on a row of its own, in the list's order.
		std::vector<EditorPanel::Item> InPanelOrder(std::vector<EditorPanel::Item> items) {
			using namespace Game::Creature;
			static constexpr std::array<std::uint8_t, 16> Keepers{ Imp, Goblin, Warlock, Firefly, Troll, Dark_Elf, Skeleton, Mistress, Salamander, Rogue,
				Bile_Demon, Vampire, Black_Knight, Dark_Angel, Horny, Maiden_Of_The_Nest };
			static constexpr std::array<std::uint8_t, 16> Heroes{ Dwarf, Knight, Thief, Guard, Wizard, Giant, Elven_Archer, Monk, Fairy, Royal_Guard,
				Stone_Knight, Lord_Of_The_Land, King_Reginald, Prince_Tristran, Prince_Felix, Prince_Balder };
			std::vector<EditorPanel::Item> ordered;
			std::vector<bool> taken(items.size(), false);
			const auto takeRow = [&](const auto& types) {
				bool first = true;
				for (const auto type : types) {
					for (std::size_t i = 0; i < items.size(); ++i) {
						// An item's value is its type id less one.
						if (taken[i] || items[i].value + 1 != type) continue;
						taken[i] = true;
						ordered.push_back(items[i]);
						ordered.back().startsRow = first;
						first = false;
						break;
					}
				}
			};
			takeRow(Keepers);
			takeRow(Heroes);
			bool first = true;
			for (std::size_t i = 0; i < items.size(); ++i) {
				if (taken[i]) continue;
				ordered.push_back(items[i]);
				ordered.back().startsRow = first;
				first = false;
			}
			return ordered;
		}
		// The Creatures tab and the editor's creature names, from the level's creature list (its own names and added types), or
		// the shared Creatures.kwd when that could not be read.
		void SetCreatureItems() {
			std::vector<EditorPanel::Item> creatures;
			if (document && document->creatures) {
				const auto& records = document->creatures->records;
				for (std::size_t i = 0; i < records.size(); ++i) {
					const auto* text = reinterpret_cast<const char*>(records[i].data());
					const std::string name(text, strnlen(text, (std::min)(records[i].size(), LevelFiles::CreatureOffset::NameLength)));
					// Past the stock types: a mod's copy of a stock type shows that type's icon.
					std::int32_t cell = CreatureCells.back();
					if (i < CreatureCells.size()) cell = CreatureCells[i];
					else if (const auto from = CreatureRegistry::CopiedFrom(name); from >= 1 && from <= CreatureCells.size()) cell = CreatureCells[from - 1u];
					if (!name.empty() && !PaletteHides(name)) creatures.push_back({ name, L"CreatureIcons.bmp", cell, i });
				}
			}
			else creatures = ListItems("Creatures.kwd", 0xAA, L"CreatureIcons.bmp", CreatureCells);
			creatureNames.clear();
			for (const auto& creature : creatures) {
				if (creature.value >= creatureNames.size()) creatureNames.resize(creature.value + 1);
				creatureNames[creature.value] = creature.name;
			}
			// A Unique creature is a kind of its normal one here, chosen with a right click, rather than an item of its own.
			uniqueOfCreature.clear();
			constexpr std::string_view UniquePrefix = "Unique ";
			for (std::size_t i = 0; i < creatureNames.size(); ++i) {
				if (!creatureNames[i].starts_with(UniquePrefix)) continue;
				// Some names in the file end in a space ("Dark Angel ").
				const auto wanted = creatureNames[i].substr(UniquePrefix.size());
				const auto base = std::find_if(creatureNames.begin(), creatureNames.end(), [&wanted](const std::string& name) {
					auto trimmed = name;
					while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
					return trimmed == wanted;
				});
				if (base != creatureNames.end()) uniqueOfCreature[static_cast<std::uint8_t>(base - creatureNames.begin() + 1)] = static_cast<std::uint8_t>(i + 1);
			}
			creatures.erase(std::remove_if(creatures.begin(), creatures.end(), [](const EditorPanel::Item& item) { return item.name.starts_with("Unique "); }), creatures.end());
			EditorPanel::SetItems(EditorPanel::Tab::Creatures, InPanelOrder(std::move(creatures)));
		}
		void ApplyCreatureHealth(const std::vector<LevelFiles::CreatureHealth>& entries);
		void LoadDocument(const std::wstring& level) {
			// Windows open on the last level would change this one's document.
			EditorDialogs::CloseAll();
			documentLevel = level;
			document.reset();
			documentChecked = false;
			stroke.clear();
			strokeThings.clear();
			undoSteps.clear();
			redoSteps.clear();
			unsavedChanges = false;
			leaveWarned = false;
			leftDown = clickWaiting = undoWanted = redoWanted = saveWanted = deleteAtPointerWanted = false;
			tool = Tool::Terrain;
			moveTag = 0;
			moveActionPoint = 0;
			// What was copied stays, to paste into another level.
			if (paintMode == PaintMode::Paste && !clipboard) paintMode = PaintMode::Brush;
			availabilityMode = false;
			deleteWarnedActionPoint = 0;
			deleteWanted = 0;
			levelChangeTag = 0;
			roomSaleWanted = false;
			blueprintItems.clear();
			alternateCreatures.clear();
			std::string error;
			document = LevelFiles::LoadMap(LevelFiles::LevelPath(level), error);
			healthAtLastStep = document ? document->creatureHealth : std::vector<LevelFiles::CreatureHealth>{};
			if (!document) Logger::Warning("Editor: could not load the level's map, so it cannot be edited: " + error);
			else if (!document->things) Logger::Warning("Editor: could not load the level's things, so creatures cannot be changed: " + error);
			else Logger::Debug("Editor: the level lists " + std::to_string(document->things->records.size()) + " things.");
			error.clear();
			terrains = LevelFiles::ReadTerrainTypes(error);
			if (terrains.empty()) Logger::Warning("Editor: could not load the terrain list, so nothing can be painted: " + error);
			terrains.erase(std::remove_if(terrains.begin(), terrains.end(), [](const LevelFiles::TerrainType& type) { return PaletteHides(type.name); }), terrains.end());
			terrainChoice = 0;
			statusText.clear();
			paletteEntries.clear();
			EditorPanel::Reset();
			SetCreatureItems();
			loadedCreatureTypes = document && document->creatures ? document->creatures->records.size() : creatureNames.size();
			selectedCreature = clickCreature = 0;
			creatureDragging = dragCancelWanted = false;
			dragRecord.reset();
			closedWindowRecord.reset();
			placeLevels.clear();
			placeDeadTypes.clear();
			auto workshop = ListItems("Traps.kwd", 0x82, L"TrapIcons.bmp", TrapCells);
			auto doors = ListItems("Doors.kwd", 0x8C, L"DoorIcons.bmp", DoorCells);
			trapNames.clear();
			for (const auto& trap : workshop) {
				if (trap.value >= trapNames.size()) trapNames.resize(trap.value + 1);
				trapNames[trap.value] = trap.name;
			}
			doorNames.clear();
			for (auto& door : doors) {
				if (door.value >= doorNames.size()) doorNames.resize(door.value + 1);
				doorNames[door.value] = door.name;
				door.value += DoorItemBase;
			}
			// The trap list's Magic Door is the door's own workings. Doors.kwd has the one to place.
			workshop.erase(std::remove_if(workshop.begin(), workshop.end(), [](const EditorPanel::Item& item) { return item.name == "Magic Door"; }), workshop.end());
			workshop.insert(workshop.end(), doors.begin(), doors.end());
			constexpr std::uint32_t PageMark = 0xFF305030u;
			workshop.push_back({ "Objects and action points (click)", L"", -1, ObjectsPageItem, PageMark });
			EditorPanel::SetItems(EditorPanel::Tab::Workshop, std::move(workshop));
			// Objects: a page of their own, with icons from ObjectIcon. Those with none show as a plain cell, named on hover.
			std::string listError;
			objectNames.clear();
			if (const auto objects = LevelFiles::ReadDefinitions("Data\\editor\\Objects.kwd", 0xF0, listError)) objectNames = objects->names;
			else Logger::Warning("Editor: " + listError);
			keeperSpellNames.clear();
			if (const auto spells = LevelFiles::ReadDefinitions("Data\\editor\\KeeperSpells.kwd", 0x96, listError)) keeperSpellNames = spells->names;
			else Logger::Warning("Editor: " + listError);
			// One-Shot Reaper is not a spell a keeper researches.
			if (keeperSpellNames.size() > 12) keeperSpellNames.resize(12);
			effectNames.clear();
			if (const auto effects = LevelFiles::ReadDefinitions("Data\\editor\\Effects.kwd", 0x10E, listError)) effectNames = effects->names;
			else Logger::Warning("Editor: " + listError);
			roomNames.clear();
			if (const auto rooms = LevelFiles::ReadDefinitions("Data\\editor\\Rooms.kwd", 0x78, listError)) roomNames = rooms->names;
			else Logger::Warning("Editor: " + listError);
			creatureSpellNames.clear();
			if (const auto spells = LevelFiles::ReadDefinitions("Data\\editor\\CreatureSpells.kwd", 0xA0, listError)) creatureSpellNames = spells->names;
			else Logger::Warning("Editor: " + listError);
			if (document && !document->creatures) Logger::Warning("Editor: the level's creature list could not be read, so creature types cannot be changed.");
			else if (document) Logger::Debug("Editor: creature types from " + document->creaturesPath.string() + (document->creaturesShared ? " (shared)." : " (the level's own)."));
			std::vector<EditorPanel::Item> objectItems;
			objectItems.push_back({ "Traps and doors (click)", L"", -1, WorkshopPageItem, PageMark });
			objectItems.push_back({ "Action point (drag across tiles)", L"", -1, ActionPointItem, 0xFF2070A0u });
			objectItems.push_back({ "Dead body (of the creature chosen on the Creatures tab)", L"", -1, DeadBodyItem, 0xFF603030u });
			objectItems.push_back({ "Effect generator", L"", -1, EffectGeneratorItem, 0xFF306060u });
			for (const auto type : PlaceableObjects) {
				if (type > objectNames.size() || objectNames[type - std::size_t{ 1 }].empty()) continue;
				const auto icon = ObjectIcon(type);
				objectItems.push_back({ ObjectItemName(type), icon.sheet, icon.cell, static_cast<std::size_t>(type - 1), 0 });
			}
			EditorPanel::SetItems(EditorPanel::Tab::Objects, std::move(objectItems));
			actionPointMarkers.clear();
			actionPointDragging = false;
			actionPointDeleteWanted = 0;
		}
		// A blank level made from the typed name and size, then loaded in place of this one and held to edit, as after a test.
		void MakeNewMap(std::uint8_t* world) {
			const std::wstring wide(newMapName.begin(), newMapName.end());
			const auto size = std::to_string(newMapWidth) + "x" + std::to_string(newMapHeight);
			std::string error;
			if (!LevelFiles::MakeBlankLevel(wide, newMapWidth, newMapHeight, error)) {
				Say("Could not make " + newMapName + ": " + error);
				return;
			}
			Logger::Info("Editor: made the new level " + newMapName + ", " + size + ", in Data\\editor\\maps.");
			const auto previous = documentLevel;
			SetGameLevelName(wide);
			revealPending = revealUsable;
			turnHeld = false;
			droppedLogged.fill(false);
			if (!ReloadLevel(world)) {
				// Still the old level in the world, so back to its name.
				SetGameLevelName(previous);
				Say("Made " + newMapName + " but could not load it; open it from the menu");
				return;
			}
			LoadDocument(wide);
			statusText = "New map " + newMapName + ", " + size;
		}
		// The terrain and room tabs, made again when the owner changes, as some of their pictures show the owner's color.
		struct RoomEntry {
			std::size_t terrain;
			std::int32_t cell;
			// Its room type, as availability names rooms by it.
			std::uint8_t type;
		};
		std::vector<RoomEntry> roomEntries;
		void RefreshPanelIcons() {
			if (!DocumentReady()) return;
			std::vector<EditorPanel::Item> terrainItems;
			for (const auto entry : paletteEntries) {
				terrainItems.push_back({ terrains[entry].name, L"TerrainIcons.bmp", TerrainCell(terrains[entry], ownerChoice), entry });
			}
			std::vector<EditorPanel::Item> roomItems;
			for (const auto& room : roomEntries) roomItems.push_back({ terrains[room.terrain].name, L"RoomIcons.bmp", room.cell, room.terrain });
			EditorPanel::SetItems(EditorPanel::Tab::Terrain, std::move(terrainItems));
			EditorPanel::SetItems(EditorPanel::Tab::Rooms, std::move(roomItems));
		}

		// ---- Availability: the level's Variables hold what each keeper may have. A record of kind 2 is (2, player * 0x10000 +
		// category, type, value) [verified: 0x51CA60 and 0x51CAE0, and the official editor's writer]; player 0 is every keeper.
		// Categories: 1 rooms, 2 creatures, 3 traps, 4 doors, 5 keeper spells, by the player lists each sets [verified]. Values below.
		constexpr std::uint32_t AvailabilityVariable = 2;
		enum class Availability : std::uint16_t { Room = 1, Creature = 2, Trap = 3, Door = 4, Spell = 5 };
		// Whose availability the panel shows: the keeper chosen by the heart, or every keeper for the heroes or neutral.
		std::uint8_t AvailabilityPlayer() {
			constexpr std::uint8_t LastKeeper = Game::FirstKeeperPlayer + 3;
			return ownerChoice >= Game::FirstKeeperPlayer && ownerChoice <= LastKeeper ? ownerChoice : std::uint8_t{ 0 };
		}
		std::string AvailabilityPlayerName() {
			const auto player = AvailabilityPlayer();
			return player == 0 ? std::string("every keeper") : OwnerName(player);
		}
		std::optional<std::size_t> FindAvailability(Availability kind, std::uint8_t player, std::uint32_t type) {
			if (!document || !document->variables) return std::nullopt;
			const auto& records = document->variables->records;
			const auto wanted = static_cast<std::uint32_t>(player) << 16 | static_cast<std::uint32_t>(kind);
			for (std::size_t i = 0; i < records.size(); ++i) {
				const auto& record = records[i];
				if (record.size() < 0x10) continue;
				if (Load<std::uint32_t>(record.data()) == AvailabilityVariable && Load<std::uint32_t>(record.data() + 4) == wanted && Load<std::uint32_t>(record.data() + 8) == type) return i;
			}
			return std::nullopt;
		}
		std::optional<std::uint32_t> AvailabilityOf(Availability kind, std::uint8_t player, std::uint32_t type) {
			const auto place = FindAvailability(kind, player, type);
			if (!place) return std::nullopt;
			return Load<std::uint32_t>(document->variables->records[*place].data() + 0xC);
		}
		// Values as all 67 stock levels use them [verified by a survey of their Variables]: the rooms a keeper starts with
		// (Treasury, Lair, Hatchery, Library, Portal) have 4, the ones researched later 3, and a few levels give 1 to keep one
		// back. Traps, doors and spells the same way. Creatures always have 3. 2 is in three records only, meaning not known.
		constexpr std::uint32_t NotAvailable = 1;
		constexpr std::uint32_t Researchable = 3;
		constexpr std::uint32_t AvailableAtStart = 4;
		constexpr std::uint32_t CreatureComes = 3;
		// Each creature allowed also has a record of kind 1: (1, creature, how many may come, player) [verified: in every stock
		// level the two sets match one for one]. Counts there run from 20 to 60; a new one gets 20.
		constexpr std::uint32_t CreaturePoolVariable = 1;
		constexpr std::uint32_t NewPoolCount = 20;
		std::optional<std::size_t> FindCreaturePool(std::uint8_t player, std::uint32_t type) {
			if (!document || !document->variables) return std::nullopt;
			const auto& records = document->variables->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				const auto* bytes = records[i].data();
				if (records[i].size() >= 0x10 && Load<std::uint32_t>(bytes) == CreaturePoolVariable && Load<std::uint32_t>(bytes + 4) == type && Load<std::uint32_t>(bytes + 0xC) == player) return i;
			}
			return std::nullopt;
		}
		std::string AvailabilityText(Availability kind, std::optional<std::uint32_t> value) {
			if (!value) return "not set";
			if (kind == Availability::Creature) return *value == CreatureComes ? "comes" : "value " + std::to_string(*value);
			switch (*value) {
				case NotAvailable: return "not available";
				case Researchable: return "researchable";
				case AvailableAtStart: return "available";
				default: return "value " + std::to_string(*value);
			}
		}
		std::uint32_t AvailabilityMark(std::optional<std::uint32_t> value) {
			if (!value) return 0;
			switch (*value) {
				case NotAvailable: return 0xFF802020u;
				case Researchable: return 0xFFA08020u;
				case AvailableAtStart: return 0xFF208030u;
				default: return 0xFF505050u;
			}
		}
		void PutVariable(std::uint32_t kind, std::uint32_t second, std::uint32_t third, std::uint32_t fourth) {
			std::vector<std::uint8_t> record(document->variables->recordSize, 0);
			Store<std::uint32_t>(record.data(), kind);
			Store<std::uint32_t>(record.data() + 4, second);
			Store<std::uint32_t>(record.data() + 8, third);
			Store<std::uint32_t>(record.data() + 0xC, fourth);
			document->variables->records.push_back(std::move(record));
		}
		// Not set, then available, researchable, not available, and back to not set. A creature only comes or not, with its pool.
		void CycleAvailability(Availability kind, std::uint32_t type, const std::string& name) {
			if (!document || !document->variables) {
				Say("This level has no variables file, so availability cannot change");
				return;
			}
			const auto player = AvailabilityPlayer();
			// Stock levels give creatures to each keeper, never to all at once, so the pool is only known to work that way.
			if (kind == Availability::Creature && player == 0) {
				Say("Choose a keeper with the heart: creatures are given to one keeper at a time");
				return;
			}
			auto& records = document->variables->records;
			const auto place = FindAvailability(kind, player, type);
			const auto now = place ? std::optional<std::uint32_t>(Load<std::uint32_t>(records[*place].data() + 0xC)) : std::nullopt;
			std::optional<std::uint32_t> next;
			if (kind == Availability::Creature) next = now ? std::nullopt : std::optional<std::uint32_t>(CreatureComes);
			else if (!now) next = AvailableAtStart;
			else if (*now == AvailableAtStart) next = Researchable;
			else if (*now == Researchable) next = NotAvailable;
			if (!next) records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
			else if (place) Store<std::uint32_t>(records[*place].data() + 0xC, *next);
			else PutVariable(AvailabilityVariable, static_cast<std::uint32_t>(player) << 16 | static_cast<std::uint32_t>(kind), type, *next);
			if (kind == Availability::Creature) {
				const auto pool = FindCreaturePool(player, type);
				if (next && !pool) PutVariable(CreaturePoolVariable, type, NewPoolCount, player);
				else if (!next && pool) records.erase(records.begin() + static_cast<std::ptrdiff_t>(*pool));
			}
			unsavedChanges = true;
			leaveWarned = false;
			Say(name + " for " + AvailabilityPlayerName() + ": " + AvailabilityText(kind, next));
		}
		// The panel's items as they are outside availability: names, variants and the terrain tab.
		void RestoreItemLooks() {
			RefreshPanelIcons();
			for (std::size_t value = 0; value < creatureNames.size(); ++value) {
				const auto type = static_cast<std::uint8_t>(value + 1);
				const auto base = CreatureName(type);
				const bool alternate = alternateCreatures[value];
				const bool hasUnique = uniqueOfCreature.contains(type);
				EditorPanel::SetItemLook(EditorPanel::Tab::Creatures, value, alternate ? (hasUnique ? "Unique " : "Mercenary ") + base : base, alternate ? (hasUnique ? 0xFFB08800u : 0xFF903020u) : 0);
			}
			for (std::size_t value = 0; value < trapNames.size(); ++value) {
				const auto base = NameFrom(trapNames, static_cast<std::uint8_t>(value + 1));
				EditorPanel::SetItemLook(EditorPanel::Tab::Workshop, value, blueprintItems[value] ? base + " (Blueprint)" : base, blueprintItems[value] ? 0xFF2050C0u : 0);
			}
			for (std::size_t value = 0; value < doorNames.size(); ++value) {
				const auto base = NameFrom(doorNames, static_cast<std::uint8_t>(value + 1));
				const auto item = value + DoorItemBase;
				EditorPanel::SetItemLook(EditorPanel::Tab::Workshop, item, blueprintItems[item] ? base + " (Blueprint)" : base, blueprintItems[item] ? 0xFF2050C0u : 0);
			}
		}
		// Each item named with its availability for the player chosen, colored by it. The Terrain tab holds the keeper spells.
		void ShowAvailabilityLooks() {
			if (!DocumentReady()) return;
			for (const auto& room : roomEntries) {
				const auto value = AvailabilityOf(Availability::Room, AvailabilityPlayer(), room.type);
				EditorPanel::SetItemLook(EditorPanel::Tab::Rooms, room.terrain, terrains[room.terrain].name + ": " + AvailabilityText(Availability::Room, value), AvailabilityMark(value));
			}
			for (std::size_t value = 0; value < creatureNames.size(); ++value) {
				const auto type = static_cast<std::uint32_t>(value + 1);
				const auto state = AvailabilityOf(Availability::Creature, AvailabilityPlayer(), type);
				EditorPanel::SetItemLook(EditorPanel::Tab::Creatures, value, CreatureName(static_cast<std::uint8_t>(type)) + ": " + AvailabilityText(Availability::Creature, state), state ? 0xFF208030u : 0);
			}
			for (std::size_t value = 0; value < trapNames.size(); ++value) {
				const auto state = AvailabilityOf(Availability::Trap, AvailabilityPlayer(), static_cast<std::uint32_t>(value + 1));
				EditorPanel::SetItemLook(EditorPanel::Tab::Workshop, value, NameFrom(trapNames, static_cast<std::uint8_t>(value + 1)) + ": " + AvailabilityText(Availability::Trap, state), AvailabilityMark(state));
			}
			for (std::size_t value = 0; value < doorNames.size(); ++value) {
				const auto state = AvailabilityOf(Availability::Door, AvailabilityPlayer(), static_cast<std::uint32_t>(value + 1));
				EditorPanel::SetItemLook(EditorPanel::Tab::Workshop, value + DoorItemBase, NameFrom(doorNames, static_cast<std::uint8_t>(value + 1)) + ": " + AvailabilityText(Availability::Door, state), AvailabilityMark(state));
			}
			std::vector<EditorPanel::Item> spells;
			for (std::size_t value = 0; value < keeperSpellNames.size(); ++value) {
				if (keeperSpellNames[value].empty()) continue;
				const auto state = AvailabilityOf(Availability::Spell, AvailabilityPlayer(), static_cast<std::uint32_t>(value + 1));
				spells.push_back({ keeperSpellNames[value] + ": " + AvailabilityText(Availability::Spell, state), L"", -1, value, AvailabilityMark(state) });
			}
			EditorPanel::SetItems(EditorPanel::Tab::Terrain, std::move(spells));
		}
		void SetAvailabilityMode(bool on) {
			availabilityMode = on;
			StopMoving();
			if (on) {
				ShowAvailabilityLooks();
				Say("Availability for " + AvailabilityPlayerName() + ": click an item to change it, the heart changes the keeper; Terrain holds the spells");
			}
			else {
				RestoreItemLooks();
				Say("Availability off; clicks on the panel choose again");
			}
		}
		void RefreshPanelForOwner() {
			if (availabilityMode) ShowAvailabilityLooks();
			else RefreshPanelIcons();
		}
		// A click on the panel while showing availability. False for items that are not about it (the pages' own items).
		bool ClickAvailability(EditorPanel::Tab tab, const EditorPanel::Item& item) {
			const auto value = item.value;
			if (value == ObjectsPageItem || value == WorkshopPageItem || value == ActionPointItem) return false;
			switch (tab) {
			case EditorPanel::Tab::Rooms:
				for (const auto& room : roomEntries) {
					if (room.terrain == value) CycleAvailability(Availability::Room, room.type, terrains[room.terrain].name);
				}
				break;
			case EditorPanel::Tab::Creatures:
				CycleAvailability(Availability::Creature, static_cast<std::uint32_t>(value + 1), CreatureName(static_cast<std::uint8_t>(value + 1)));
				break;
			case EditorPanel::Tab::Workshop:
				if (value >= DoorItemBase) CycleAvailability(Availability::Door, static_cast<std::uint32_t>(value - DoorItemBase + 1), NameFrom(doorNames, static_cast<std::uint8_t>(value - DoorItemBase + 1)));
				else CycleAvailability(Availability::Trap, static_cast<std::uint32_t>(value + 1), NameFrom(trapNames, static_cast<std::uint8_t>(value + 1)));
				break;
			case EditorPanel::Tab::Terrain:
				if (value < keeperSpellNames.size()) CycleAvailability(Availability::Spell, static_cast<std::uint32_t>(value + 1), keeperSpellNames[value]);
				break;
			default:
				return false;
			}
			ShowAvailabilityLooks();
			return true;
		}
		void CheckDocument(std::uint8_t* world) {
			documentChecked = true;
			if (!DocumentReady()) {
				Say("Cannot edit this level, see the log");
				return;
			}
			const auto* map = world + Game::Offset::WorldMap;
			const auto width = Load<std::uint32_t>(map + Game::Offset::MapWidth);
			const auto height = Load<std::uint32_t>(map + Game::Offset::MapHeight);
			if (width != document->map.width || height != document->map.height) {
				Logger::Warning("Editor: the level's file is " + std::to_string(document->map.width) + "x" + std::to_string(document->map.height) + ", the world "
					+ std::to_string(width) + "x" + std::to_string(height) + ", so it cannot be edited.");
				document.reset();
				return;
			}
			// Rooms go on the panel, with their icon by their place in Rooms.kwd, found by name, as a terrain and its room share
			// one. A room's name not found there has no icon.
			std::string roomsError;
			const auto rooms = LevelFiles::ReadDefinitions("Data\\editor\\Rooms.kwd", 0x78, roomsError);
			if (!rooms) Logger::Warning("Editor: " + roomsError);
			roomEntries.clear();
			for (std::size_t i = 0; i < terrains.size(); ++i) {
				const auto* data = reinterpret_cast<TerrainDataFn>(VirtualOf(world, Game::Offset::WorldTerrainData))(world, nullptr, terrains[i].id);
				if ((data[Game::Offset::TerrainFlags] & Game::TerrainIsRoomFlag) == 0) {
					paletteEntries.push_back(i);
					continue;
				}
				std::int32_t cell = RoomQuestionMarkCell;
				if (rooms) {
					const auto found = std::find(rooms->names.begin(), rooms->names.end(), terrains[i].name);
					if (found != rooms->names.end()) cell = (std::min)(static_cast<std::int32_t>(found - rooms->names.begin()), RoomQuestionMarkCell);
				}
				const auto type = GameFunction<RoomTypeFn>(Game::Address::WorldRoomTypeOfTerrain)(world, nullptr, terrains[i].id);
				roomEntries.push_back({ i, cell, type });
			}
			RefreshPanelIcons();
			ShowChoiceOnPanel();
			// Level load does not know this mark, so a saved level's mercenaries are made so here, and checked again a few ticks on.
			if (document->things) {
				std::size_t marked = 0;
				for (const auto& record : document->things->records) {
					if (IsCreatureRecord(record) && record.bytes.size() > CreatureRecordFlags && (record.bytes[CreatureRecordFlags] & RecordMercenaryFlag) != 0) ++marked;
					ApplyRecordMercenary(record);
				}
				Logger::Debug("Editor: the level's list marks " + std::to_string(marked) + " creatures as mercenaries.");
				ApplyCreatureHealth(document->creatureHealth);
				mercenaryRecheckIn = marked != 0 ? MercenaryRecheckAfterTicks : 0;
				std::size_t points = 0;
				for (const auto& record : document->things->records) {
					if (record.kind != ActionPointRecord) continue;
					MakeActionPointMarkers(world, record);
					++points;
				}
				std::size_t markers = 0;
				for (const auto& [id, tags] : actionPointMarkers) markers += tags.size();
				Logger::Debug("Editor: the level has " + std::to_string(points) + " action points, shown by "
					+ (outlinesUsable ? std::string("outlines") : std::to_string(markers) + " markers") + ".");
			}
			Say("Left click paints, right click picks");
			LogChoice();
		}

		// ---- Triggers, read only (docs/game/level-files.md, Triggers). A trigger: +0 to +3 its settings, +4 a value, +8 its id, +0xA the
		// next trigger's id, +0xC a child, a trigger by its id or, below 0, an action by its, +0xE its type. An action: +0 to +7 its
		// settings, +8 its id (below 0), +0xA the next action's, +0xE its type [verified: the records' links over all stock levels].
		// Type names are the official editor's, by their ids, which fit how often each is used in stock levels [reference].
		constexpr std::array<const char*, 0x4C> TriggerTypeNames{ "Nothing", "Flag", "Timer", "Created", "Killed", "Slapped", "Attacked",
			"Imprisoned", "Tortured", "Converted", "Claimed", "Angry", "Afraid", "Steals", "Leaves", "Stunned", "Dying", "Destroyed",
			"Claimed", "Creatures", "Happy Creatures", "Angry Creatures", "Creatures Killed", "Kills Creatures", "Room Slabs", "Rooms",
			"Room Size", "Doors", "Traps", "Keeper Spell", "Gold", "Gold Mined", "Mana", "Destroys", "Time", "Creatures", "Health",
			"Gold Held", "Not Used", "Congregate In", "Claim Part Of", "Claim All Of", "Slab Types", "Created", "Members Killed",
			"Members Captured", "Pay Day", "Killed", "Portal Gem In Hand", "Experience Level", "Enter Action Point", "Enter Action Point",
			"Creatures At Level", "Button Pressed", "Hunger Sated", "Picks Up Portal Gem", "Dungeon Breached", "Enemy Breached",
			"Creature Picked Up", "Creature Dropped", "Creature Slapped", "Creature Sacked", "Tag Part Of", "Sacked",
			"Members Incapacitated", "Picked Up", "Creature Introduced", "Played", "Room Furniture", "Tag All Of",
			"Possessed Creature Enters", "Slaps", "Transition Ends", "Creatures Grouped", "Creatures Dying", "Gems Collected" };
		constexpr std::array<const char*, 0x34> ActionTypeNames{ "Nothing", "Create Creature", "Create Portal Gem", "Send To Action Point",
			"Display Objective", "Display Information", "Set Availability", "Set Flag", "Set Timer", "Flash Button", "Win Game",
			"Lose Game", "Win Subobjective", "Lose Subobjective", "Create Hero Party", "Set Creature Objective", "Flash Action Point",
			"Reveal Action Point", "Set Alliance", "Modify Creature Pool", "Attach Portal Gem", "Force First Person", "Alter Terrain Type",
			"Set Level Time Limit", "Play Speech", "Display Text String", "Zoom To Action Point", "Rotate Around Action Point",
			"Generate Creature", "Make Hungry", "Show Health Flower", "Camera Follow Path", "Collapse Hero Gate", "Alter Speed",
			"Set Music Excitement Level", "Remove From Map", "Set Fight Flag", "Set Portal Status", "Set Widescreen Mode", "Create Door",
			"Create Trap", "Create Effect", "Make Objective", "Zoom To", "Set Creature Moods", "Set System Messages", "Display Slab Owner",
			"Display Next Room Type", "Toggle Effect Generator", "Change Room Owner", "Set Slaps Limit", "Set Timer Speech" };
		constexpr std::size_t TriggerTypeAt = 0xE;
		std::string TriggerTypeName(std::uint8_t type) { return type < TriggerTypeNames.size() ? TriggerTypeNames[type] : "Type " + std::to_string(type); }
		std::string ActionTypeName(std::uint8_t type) { return type < ActionTypeNames.size() ? ActionTypeNames[type] : "Type " + std::to_string(type); }
		// Where an action holds an action point's number, for those that have one. Create Hero Party, Flash and Reveal Action Point
		// [verified in the level's action handler 0x518AD0]; Zoom To and Rotate Around Action Point [verified only by a survey: every
		// stock record has one of its level's action points there]. 0 for none.
		std::uint8_t ActionPointOfAction(const std::vector<std::uint8_t>& bytes) {
			if (bytes.size() <= TriggerTypeAt) return 0;
			switch (bytes[TriggerTypeAt]) {
				case 16: case 17: case 26: case 27: return bytes[0]; // Flash, Reveal, Zoom To, Rotate Around Action Point
				case 14: return bytes[4];                             // Create Hero Party (its party at byte 0)
				default: return 0;
			}
		}
		struct TriggerIndex {
			std::map<std::uint16_t, std::size_t> triggers;
			std::map<std::int16_t, std::size_t> actions;
		};
		TriggerIndex IndexTriggers() {
			TriggerIndex index;
			if (!document || !document->triggers) return index;
			const auto& records = document->triggers->records;
			for (std::size_t i = 0; i < records.size(); ++i) {
				if (records[i].bytes.size() <= TriggerTypeAt) continue;
				if (records[i].kind == LevelFiles::TriggerRecordKind) index.triggers[Load<std::uint16_t>(records[i].bytes.data() + 8)] = i;
				else if (records[i].kind == LevelFiles::ActionRecordKind) index.actions[Load<std::int16_t>(records[i].bytes.data() + 8)] = i;
			}
			return index;
		}
		// Goes through a tree from its first trigger: each trigger, its children one deeper, then the next trigger. Each record once.
		using TriggerVisit = std::function<void(std::size_t depth, const LevelFiles::ThingRecord& record)>;
		void WalkTriggers(const TriggerIndex& index, std::uint16_t first, std::size_t depth, std::set<std::size_t>& seen, const TriggerVisit& visit) {
			constexpr std::size_t DeepestTree = 32;
			if (depth > DeepestTree) return;
			const auto& records = document->triggers->records;
			for (auto id = first; id != 0;) {
				const auto found = index.triggers.find(id);
				if (found == index.triggers.end() || !seen.insert(found->second).second) return;
				const auto& record = records[found->second];
				visit(depth, record);
				const auto child = Load<std::int16_t>(record.bytes.data() + 0xC);
				if (child > 0) WalkTriggers(index, static_cast<std::uint16_t>(child), depth + 1, seen, visit);
				for (auto action = child; action < 0;) {
					const auto act = index.actions.find(action);
					if (act == index.actions.end() || !seen.insert(act->second).second) break;
					visit(depth + 1, records[act->second]);
					action = Load<std::int16_t>(records[act->second].bytes.data() + 0xA);
				}
				id = Load<std::uint16_t>(record.bytes.data() + 0xA);
			}
		}
		std::string TriggerRecordText(const LevelFiles::ThingRecord& record) {
			const auto* bytes = record.bytes.data();
			const auto type = bytes[TriggerTypeAt];
			const auto hexByte = [](std::uint8_t value) {
				constexpr char digits[] = "0123456789abcdef";
				return std::string{ digits[value >> 4], digits[value & 0xF] };
			};
			if (record.kind == LevelFiles::TriggerRecordKind) {
				return "When " + TriggerTypeName(type) + " " + std::to_string(Load<std::int32_t>(bytes + 4)) + " [" + hexByte(bytes[0]) + " " + hexByte(bytes[1]) + " " + hexByte(bytes[2]) + " " + hexByte(bytes[3]) + "]";
			}
			std::string text = "Do " + ActionTypeName(type);
			if (const auto point = ActionPointOfAction(record.bytes); point != 0) text += " at action point " + std::to_string(point);
			text += " [";
			for (std::size_t i = 0; i < 8; ++i) text += (i == 0 ? "" : " ") + hexByte(bytes[i]);
			return text + "]";
		}
		// ---- Hero parties (Things kind 0xC9, 0x22B bytes) [verified: 0x570210 reads them, 0x50FA70 keeps them by number; and a survey of
		// all 220 stock parties]: +0x20 the party's trigger tree, +0x22 its number (0 to 19 in stock levels; Create Hero Party names it),
		// 8 bytes left as they are, then 16 members of 0x20 bytes at +0x2B, each a hero creature's record (type +0x1C, level +0xE, its
		// own tree +0x18), with type 0 for an empty place. The name at +0 is junk in stock levels.
		constexpr std::uint32_t HeroPartyRecord = 0xC9;
		constexpr std::size_t PartyRecordSize = 0x22B;
		constexpr std::size_t PartyRootAt = 0x20;
		constexpr std::size_t PartyNumberAt = 0x22;
		constexpr std::size_t PartyMembersAt = 0x2B;
		constexpr std::size_t PartyMemberSize = 0x20;
		constexpr std::size_t PartyMembers = 16;
		constexpr std::size_t MemberTypeAt = 0x1C;
		bool IsParty(const LevelFiles::ThingRecord& record) { return record.kind == HeroPartyRecord && record.bytes.size() >= PartyRecordSize; }
		std::string PartyText(const LevelFiles::ThingRecord& record) {
			std::string text = "Hero party " + std::to_string(record.bytes[PartyNumberAt]) + ":";
			std::size_t members = 0;
			for (std::size_t m = 0; m < PartyMembers; ++m) {
				const auto* member = record.bytes.data() + PartyMembersAt + m * PartyMemberSize;
				if (member[MemberTypeAt] == 0) continue;
				text += (members++ == 0 ? " " : ", ") + CreatureName(member[MemberTypeAt]) + " " + std::to_string(member[CreatureRecordLevel]);
			}
			return members == 0 ? text + " no members" : text;
		}
		// Each change to a party is one undo step: its record as it was.
		void ChangeParty(std::size_t place, const std::function<bool(std::vector<std::uint8_t>&)>& change, const std::string& said) {
			if (!document || !document->things || place >= document->things->records.size() || !IsParty(document->things->records[place])) return;
			auto& record = document->things->records[place];
			const auto before = record;
			if (!change(record.bytes)) return;
			PushCreatureUndo({ CreatureChangeKind::Changed, place, before });
			unsavedChanges = true;
			Say(said);
		}
		void AddPartyMember(std::size_t place) {
			const auto type = creatureChoice;
			ChangeParty(place, [&](std::vector<std::uint8_t>& bytes) {
				for (std::size_t m = 0; m < PartyMembers; ++m) {
					auto* member = bytes.data() + PartyMembersAt + m * PartyMemberSize;
					if (member[MemberTypeAt] != 0) continue;
					// A hero's record as placing one makes it, at no tile: stock members stand nowhere until the party is made.
					const auto record = MakeCreatureRecord(0, 0, type, Game::GoodPlayer);
					std::copy_n(record.bytes.data(), (std::min)(record.bytes.size(), PartyMemberSize), member);
					return true;
				}
				Say("A party has room for 16");
				return false;
			}, "Added a " + CreatureName(type) + " to the party");
		}
		void RemovePartyMember(std::size_t place) {
			ChangeParty(place, [](std::vector<std::uint8_t>& bytes) {
				for (std::size_t m = PartyMembers; m-- > 0;) {
					auto* member = bytes.data() + PartyMembersAt + m * PartyMemberSize;
					if (member[MemberTypeAt] == 0) continue;
					std::fill_n(member, PartyMemberSize, std::uint8_t{ 0 });
					return true;
				}
				return false;
			}, "Removed the party's last member");
		}
		void StepMemberLevel(std::size_t place, std::size_t m) {
			ChangeParty(place, [m](std::vector<std::uint8_t>& bytes) {
				// Creatures reach level 10 [verified: 0x48B120 stops there].
				constexpr std::uint8_t HighestLevel = 10;
				auto& level = bytes[PartyMembersAt + m * PartyMemberSize + CreatureRecordLevel];
				level = static_cast<std::uint8_t>(level >= HighestLevel ? 1 : level + 1);
				return true;
			}, "Member's level changed");
		}
		// The lowest party number not in use, with the bytes not understood taken from a party the level has.
		void MakeParty() {
			if (!document || !document->things) return;
			auto& records = document->things->records;
			std::array<bool, 256> used{};
			const LevelFiles::ThingRecord* example = nullptr;
			for (const auto& record : records) {
				if (!IsParty(record)) continue;
				used[record.bytes[PartyNumberAt]] = true;
				example = &record;
			}
			std::size_t number = 0;
			while (number < used.size() && used[number]) ++number;
			if (number == used.size()) return;
			LevelFiles::ThingRecord record{ HeroPartyRecord, std::vector<std::uint8_t>(PartyRecordSize, 0) };
			if (example != nullptr) std::copy(example->bytes.begin() + PartyNumberAt + 1, example->bytes.begin() + PartyMembersAt, record.bytes.begin() + PartyNumberAt + 1);
			record.bytes[PartyNumberAt] = static_cast<std::uint8_t>(number);
			PushCreatureUndo({ CreatureChangeKind::Placed, records.size(), record });
			records.push_back(std::move(record));
			unsavedChanges = true;
			Say("Made hero party " + std::to_string(number) + "; add its members, and a Create Hero Party action to bring it in");
		}
		void DeleteParty(std::size_t place) {
			if (!document || !document->things || place >= document->things->records.size() || !IsParty(document->things->records[place])) return;
			auto& records = document->things->records;
			const auto number = records[place].bytes[PartyNumberAt];
			PushCreatureUndo({ CreatureChangeKind::Deleted, place, records[place] });
			records.erase(records.begin() + static_cast<std::ptrdiff_t>(place));
			unsavedChanges = true;
			Say("Deleted hero party " + std::to_string(number) + "; a Create Hero Party action for it now has none to make");
		}

		// Whose trees there are, and where each keeps its first trigger's id [verified: in stock levels every non-zero one is a trigger's
		// id]: the level, each player, each action point, each hero party and its members, each listed creature. Those that can have a
		// tree are listed even with none, but for creatures. What nothing reaches is listed last.
		enum class RootPlace { Level, Player, Thing, None };
		struct TriggerOwner {
			std::string name;
			std::uint16_t root;
			RootPlace place;
			// The record in the players' or things' list, and where in it.
			std::size_t index = 0;
			std::size_t offset = 0;
		};
		bool SameOwner(const TriggerOwner& a, const TriggerOwner& b) { return a.place == b.place && a.index == b.index && a.offset == b.offset && (a.place != RootPlace::None || a.root == b.root); }
		std::vector<TriggerOwner> TriggerOwners() {
			std::vector<TriggerOwner> owners;
			if (!document || !document->triggers) return owners;
			if (document->level) owners.push_back({ "The level", LevelFiles::LevelTriggerRoot(*document->level), RootPlace::Level });
			if (document->players) {
				const auto& records = document->players->records;
				for (std::size_t i = 0; i < records.size(); ++i) {
					// +0xA6 its tree's first trigger, +0xA8 its number [verified].
					if (records[i].size() <= 0xA8) continue;
					owners.push_back({ OwnerName(records[i][0xA8]), Load<std::uint16_t>(records[i].data() + 0xA6), RootPlace::Player, i, 0xA6 });
				}
			}
			if (document->things) {
				const auto& records = document->things->records;
				for (std::size_t i = 0; i < records.size(); ++i) {
					const auto& record = records[i];
					if (const auto box = ReadActionPoint(record)) {
						owners.push_back({ ActionPointText(*box), Load<std::uint16_t>(record.bytes.data() + 0x14), RootPlace::Thing, i, 0x14 });
						continue;
					}
					if (IsParty(record)) {
						owners.push_back({ "Hero party " + std::to_string(record.bytes[PartyNumberAt]), Load<std::uint16_t>(record.bytes.data() + PartyRootAt), RootPlace::Thing, i, PartyRootAt });
						for (std::size_t m = 0; m < PartyMembers; ++m) {
							const auto at = PartyMembersAt + m * PartyMemberSize;
							const auto type = record.bytes[at + MemberTypeAt];
							const auto root = Load<std::uint16_t>(record.bytes.data() + at + 0x18);
							if (type != 0 && root != 0) owners.push_back({ "Party " + std::to_string(record.bytes[PartyNumberAt]) + "'s " + CreatureName(type), root, RootPlace::Thing, i, at + 0x18 });
						}
						continue;
					}
					const auto fields = ReadCreatureRecord(record);
					if (!fields || !IsCreatureKind(record.kind)) continue;
					const std::size_t at = record.kind == NeutralCreatureRecord ? 0x14 : 0x18;
					if (record.bytes.size() < at + 2) continue;
					if (const auto root = Load<std::uint16_t>(record.bytes.data() + at)) {
						owners.push_back({ CreatureName(fields->type) + " at " + std::to_string(fields->x) + "," + std::to_string(fields->y), root, RootPlace::Thing, i, at });
					}
				}
			}
			const auto index = IndexTriggers();
			std::set<std::size_t> seen;
			for (const auto& owner : owners) WalkTriggers(index, owner.root, 0, seen, [](std::size_t, const LevelFiles::ThingRecord&) {});
			std::set<std::uint16_t> linked;
			for (const auto& [id, place] : index.triggers) {
				const auto* bytes = document->triggers->records[place].bytes.data();
				linked.insert(Load<std::uint16_t>(bytes + 0xA));
				if (const auto child = Load<std::int16_t>(bytes + 0xC); child > 0) linked.insert(static_cast<std::uint16_t>(child));
			}
			for (const auto& [id, place] : index.triggers) {
				if (!seen.contains(place) && !linked.contains(id)) owners.push_back({ "Not reached, from trigger " + std::to_string(id), id, RootPlace::None });
			}
			return owners;
		}
		bool SetOwnerRoot(const TriggerOwner& owner, std::uint16_t root) {
			switch (owner.place) {
			case RootPlace::Level:
				if (!document->level || !LevelFiles::SetLevelTriggerRoot(*document->level, root)) return false;
				document->levelChanged = true;
				return true;
			case RootPlace::Player:
				if (!document->players || owner.index >= document->players->records.size()) return false;
				Store<std::uint16_t>(document->players->records[owner.index].data() + owner.offset, root);
				return true;
			case RootPlace::Thing:
				if (!document->things || owner.index >= document->things->records.size() || document->things->records[owner.index].bytes.size() < owner.offset + 2) return false;
				Store<std::uint16_t>(document->things->records[owner.index].bytes.data() + owner.offset, root);
				return true;
			default:
				return false;
			}
		}
		// A tree's rows, each with its record's place in the triggers' list.
		struct TriggerRow {
			std::wstring text;
			std::size_t record;
		};
		std::vector<TriggerRow> TriggerTreeRowsOf(std::uint16_t root) {
			std::vector<TriggerRow> rows;
			const auto index = IndexTriggers();
			std::set<std::size_t> seen;
			const auto* first = document->triggers->records.data();
			WalkTriggers(index, root, 0, seen, [&](std::size_t depth, const LevelFiles::ThingRecord& record) {
				rows.push_back({ std::wstring(depth * 2, L' ') + Widen(TriggerRecordText(record)), static_cast<std::size_t>(&record - first) });
			});
			return rows;
		}
		std::vector<std::wstring> TriggerTreeRows(std::uint16_t root) {
			std::vector<std::wstring> rows;
			for (auto& row : TriggerTreeRowsOf(root)) rows.push_back(std::move(row.text));
			return rows;
		}

		// ---- Changing triggers. Not in undo; they count as unsaved changes and the file is written on Save.
		void TriggersChanged() {
			document->triggersChanged = true;
			unsavedChanges = true;
			leaveWarned = false;
		}
		std::uint16_t TriggerIdOf(const LevelFiles::ThingRecord& record) { return Load<std::uint16_t>(record.bytes.data() + 8); }
		std::int16_t ActionIdOf(const LevelFiles::ThingRecord& record) { return Load<std::int16_t>(record.bytes.data() + 8); }
		// A new trigger: Flag 0 = 0 (byte 2 bit 0, compared with the value), once. A new action: Set Flag 0 to 1 (byte 1: bit 1 the value
		// is a number, bit 3 set it) [verified: 0x518270 and 0x518AD0 read them so].
		LevelFiles::ThingRecord NewTrigger() {
			std::uint16_t id = 0;
			for (const auto& record : document->triggers->records) {
				if (record.kind == LevelFiles::TriggerRecordKind) id = (std::max)(id, TriggerIdOf(record));
			}
			LevelFiles::ThingRecord record{ LevelFiles::TriggerRecordKind, std::vector<std::uint8_t>(0x10, 0) };
			record.bytes[0] = 3;
			record.bytes[2] = 1;
			Store<std::uint16_t>(record.bytes.data() + 8, static_cast<std::uint16_t>(id + 1));
			record.bytes[TriggerTypeAt] = 1;
			record.bytes[0xF] = 1;
			return record;
		}
		LevelFiles::ThingRecord NewAction() {
			std::int16_t id = 0;
			for (const auto& record : document->triggers->records) {
				if (record.kind == LevelFiles::ActionRecordKind) id = (std::min)(id, ActionIdOf(record));
			}
			LevelFiles::ThingRecord record{ LevelFiles::ActionRecordKind, std::vector<std::uint8_t>(0x10, 0) };
			record.bytes[1] = 2 | 8;
			Store<std::int32_t>(record.bytes.data() + 4, 1);
			Store<std::int16_t>(record.bytes.data() + 8, static_cast<std::int16_t>(id - 1));
			record.bytes[TriggerTypeAt] = 7;
			record.bytes[0xF] = 1;
			return record;
		}
		// Where (which list place) a new record went.
		std::size_t AddTriggerRecord(LevelFiles::ThingRecord record) {
			document->triggers->records.push_back(std::move(record));
			TriggersChanged();
			return document->triggers->records.size() - 1;
		}
		std::optional<std::size_t> StartTree(const TriggerOwner& owner) {
			if (owner.root != 0 || owner.place == RootPlace::None) return std::nullopt;
			auto record = NewTrigger();
			const auto id = TriggerIdOf(record);
			if (!SetOwnerRoot(owner, id)) return std::nullopt;
			return AddTriggerRecord(std::move(record));
		}
		// After this record, in its own chain.
		std::size_t AddAfter(std::size_t place) {
			const bool trigger = document->triggers->records[place].kind == LevelFiles::TriggerRecordKind;
			auto record = trigger ? NewTrigger() : NewAction();
			auto& here = document->triggers->records[place];
			Store<std::uint16_t>(record.bytes.data() + 0xA, Load<std::uint16_t>(here.bytes.data() + 0xA));
			Store<std::uint16_t>(here.bytes.data() + 0xA, Load<std::uint16_t>(record.bytes.data() + 8));
			return AddTriggerRecord(std::move(record));
		}
		// Under a trigger: a trigger if it has nothing under it; an action at the end of its actions if it has no triggers under it.
		std::optional<std::size_t> AddUnder(std::size_t place, bool action) {
			const auto child = Load<std::int16_t>(document->triggers->records[place].bytes.data() + 0xC);
			if (!action) {
				if (child != 0) return std::nullopt;
				auto record = NewTrigger();
				Store<std::uint16_t>(document->triggers->records[place].bytes.data() + 0xC, TriggerIdOf(record));
				return AddTriggerRecord(std::move(record));
			}
			if (child > 0) return std::nullopt;
			auto record = NewAction();
			const auto id = ActionIdOf(record);
			if (child == 0) Store<std::int16_t>(document->triggers->records[place].bytes.data() + 0xC, id);
			else {
				const auto index = IndexTriggers();
				for (auto at = child; at < 0;) {
					const auto found = index.actions.find(at);
					if (found == index.actions.end()) break;
					const auto next = Load<std::int16_t>(document->triggers->records[found->second].bytes.data() + 0xA);
					if (next == 0) {
						Store<std::int16_t>(document->triggers->records[found->second].bytes.data() + 0xA, id);
						break;
					}
					at = next;
				}
			}
			return AddTriggerRecord(std::move(record));
		}
		// A record and everything under it: whatever pointed to it points to what came after it.
		void DeleteTriggerRecord(std::size_t place) {
			auto& records = document->triggers->records;
			const auto& gone = records[place];
			const bool trigger = gone.kind == LevelFiles::TriggerRecordKind;
			const auto id = Load<std::int16_t>(gone.bytes.data() + 8);
			const auto next = Load<std::int16_t>(gone.bytes.data() + 0xA);
			std::set<std::size_t> remove{ place };
			if (trigger) {
				const auto index = IndexTriggers();
				const auto child = Load<std::int16_t>(gone.bytes.data() + 0xC);
				if (child > 0) WalkTriggers(index, static_cast<std::uint16_t>(child), 0, remove, [](std::size_t, const LevelFiles::ThingRecord&) {});
				for (auto action = child; action < 0;) {
					const auto found = index.actions.find(action);
					if (found == index.actions.end() || !remove.insert(found->second).second) break;
					action = Load<std::int16_t>(records[found->second].bytes.data() + 0xA);
				}
			}
			for (auto& record : records) {
				if (record.bytes.size() <= TriggerTypeAt) continue;
				if (Load<std::int16_t>(record.bytes.data() + 0xA) == id && (record.kind == LevelFiles::TriggerRecordKind) == trigger) Store<std::int16_t>(record.bytes.data() + 0xA, next);
				if (record.kind == LevelFiles::TriggerRecordKind && Load<std::int16_t>(record.bytes.data() + 0xC) == id) Store<std::int16_t>(record.bytes.data() + 0xC, next);
			}
			if (trigger) {
				for (const auto& owner : TriggerOwners()) {
					if (owner.root == static_cast<std::uint16_t>(id)) SetOwnerRoot(owner, static_cast<std::uint16_t>(next));
				}
			}
			for (auto at = remove.rbegin(); at != remove.rend(); ++at) records.erase(records.begin() + static_cast<std::ptrdiff_t>(*at));
			TriggersChanged();
		}
		// What uses an action point: its own tree, and actions anywhere that name it.
		std::vector<std::wstring> ActionPointUses(std::uint8_t id) {
			std::vector<std::wstring> rows;
			if (!document || !document->triggers) return rows;
			const auto index = IndexTriggers();
			for (const auto& owner : TriggerOwners()) {
				std::set<std::size_t> seen;
				WalkTriggers(index, owner.root, 0, seen, [&](std::size_t, const LevelFiles::ThingRecord& record) {
					if (record.kind == LevelFiles::ActionRecordKind && ActionPointOfAction(record.bytes) == id) rows.push_back(Widen(owner.name + ": " + TriggerRecordText(record)));
				});
			}
			if (const auto place = FindActionPoint(id)) {
				const auto& record = document->things->records[*place];
				if (const auto root = Load<std::uint16_t>(record.bytes.data() + 0x14)) {
					rows.push_back(L"Its own triggers:");
					for (auto& row : TriggerTreeRows(root)) rows.push_back(L"  " + row);
				}
			}
			return rows;
		}

		// ---- The palette: a column down the left of the screen, drawn into the view the options menu uses, which nothing else
		// shows while that menu is closed. Layout units, 2560x1920. It stays above the side panel.
		constexpr std::int32_t PaletteLeft = 0;
		constexpr std::int32_t PaletteRight = 620;
		constexpr std::int32_t PaletteTop = 190;
		constexpr std::int32_t PaletteRowHeight = 56;
		constexpr std::int32_t PaletteRows = 5;
		constexpr std::int32_t PaletteTextIndent = 24;
		// Wider than the box, as a long status line runs past it.
		constexpr std::int32_t PaletteViewLayoutWidth = 1200;
		enum class PaletteAction { None, Undo, Redo, Save, SaveAs, Test, StopTest, NewMap, Brush, Mode, Level };
		enum PaletteColor : std::size_t { ColorNormal, ColorHover, ColorChosen, ColorDim, ColorCount };
		// Blue, green, red, then the two the game wants after them.
		constexpr std::array<std::array<std::uint8_t, 5>, ColorCount> PaletteColors{ {
			{ 0xBF, 0xBF, 0xBF, 0xFF, 0 }, { 0x40, 0xE0, 0xFF, 0xFF, 0 }, { 0x00, 0xA8, 0xFF, 0xFF, 0 }, { 0x90, 0x90, 0x90, 0xFF, 0 } } };
		struct PaletteCell {
			// Layout units until placed, then in view pixels.
			std::int32_t left, top, right, bottom;
			PaletteAction action;
			std::size_t value;
			std::wstring text;
			PaletteColor color;
		};
		std::vector<PaletteCell> paletteCells;
		std::int32_t paletteOriginX = 0;
		std::int32_t paletteOriginY = 0;
		std::wstring paletteDrawn;
		bool paletteShown = false;

		struct ScreenPoint { std::int32_t x, y; };
		ScreenPoint LayoutToScreen(std::uint8_t* playerInterface, std::int32_t x, std::int32_t y) {
			using PointFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* out, const std::int32_t* in);
			const std::int32_t in[2]{ x, y };
			std::int32_t out[2]{};
			GameFunction<PointFn>(Game::Address::GuiPointToScreen)(playerInterface + Game::Offset::InterfaceGuiManager, nullptr, out, in);
			return { out[0], out[1] };
		}
		ScreenPoint MousePosition() {
			using MouseFn = const std::int32_t*(__cdecl*)();
			const auto* mouse = GameFunction<MouseFn>(Game::Address::MouseOnScreen)();
			return { mouse[0], mouse[1] };
		}
		bool MouseOverPalette() {
			if (!paletteShown || paletteCells.empty()) return false;
			const auto mouse = MousePosition();
			const auto x = mouse.x - paletteOriginX;
			const auto y = mouse.y - paletteOriginY;
			return x >= paletteCells.front().left && x < paletteCells.front().right && y >= paletteCells.front().top && y < paletteCells.front().bottom;
		}
		void BuildPalette() {
			paletteCells.clear();
			constexpr std::int32_t width = PaletteRight - PaletteLeft;
			const auto rowTop = [](std::int32_t row) { return PaletteTop + row * PaletteRowHeight; };
			const auto add = [&](std::int32_t left, std::int32_t right, std::int32_t row, PaletteAction action, std::size_t value, std::wstring text, PaletteColor color) {
				paletteCells.push_back({ left, rowTop(row), right, rowTop(row + 1), action, value, std::move(text), color });
			};
			// The first cell is the whole palette: its background, and what the mouse is over when it is not on a row.
			if (testing) {
				paletteCells.push_back({ PaletteLeft, PaletteTop, PaletteRight, rowTop(2), PaletteAction::None, 0, L"", ColorNormal });
				add(PaletteLeft, PaletteRight, 0, PaletteAction::None, 0, L"Testing " + documentLevel, ColorChosen);
				add(PaletteLeft, PaletteRight, 1, PaletteAction::StopTest, 0, L"Back to editing (Ctrl+T)", ColorNormal);
				return;
			}
			paletteCells.push_back({ PaletteLeft, PaletteTop, PaletteRight, rowTop(PaletteRows), PaletteAction::None, 0, L"", ColorNormal });
			if (!DocumentReady()) {
				add(PaletteLeft, PaletteRight, 0, PaletteAction::None, 0, Widen(statusText), ColorDim);
				return;
			}
			add(PaletteLeft, PaletteRight, 0, PaletteAction::None, 0, L"Owner: " + Widen(OwnerName(ownerChoice)), ColorChosen);
			const auto third = width / 3;
			add(PaletteLeft, PaletteLeft + third, 1, PaletteAction::Undo, 0, L"Undo", undoSteps.empty() ? ColorDim : ColorNormal);
			add(PaletteLeft + third, PaletteLeft + 2 * third, 1, PaletteAction::Redo, 0, L"Redo", redoSteps.empty() ? ColorDim : ColorNormal);
			add(PaletteLeft + 2 * third, PaletteRight, 1, PaletteAction::Save, 0, unsavedChanges ? L"Save *" : L"Save", ColorNormal);
			add(PaletteLeft, PaletteLeft + third, 2, PaletteAction::SaveAs, 0, L"Save as", ColorNormal);
			add(PaletteLeft + third, PaletteLeft + 2 * third, 2, PaletteAction::Test, 0, L"Test", ColorNormal);
			add(PaletteLeft + 2 * third, PaletteRight, 2, PaletteAction::NewMap, 0, L"New map", ColorNormal);
			// Terrain only: the brush's size matters only to the brush. The mode is named alone, as the row has three cells.
			const auto brush = std::to_wstring(BrushSizes[brushChoice]);
			add(PaletteLeft, PaletteLeft + third, 3, PaletteAction::Brush, 0, L"Brush " + brush + L"x" + brush, paintMode == PaintMode::Brush ? ColorNormal : ColorDim);
			std::wstring mode;
			switch (paintMode) {
			case PaintMode::Brush: mode = L"Free paint"; break;
			case PaintMode::Box: mode = L"Box"; break;
			case PaintMode::Fill: mode = L"Fill"; break;
			case PaintMode::Copy: mode = L"Copy"; break;
			case PaintMode::Paste: mode = L"Paste"; break;
			}
			add(PaletteLeft + third, PaletteLeft + 2 * third, 3, PaletteAction::Mode, 0, mode, tool == Tool::Terrain ? ColorNormal : ColorDim);
			add(PaletteLeft + 2 * third, PaletteRight, 3, PaletteAction::Level, 0, availabilityMode ? L"Level *" : L"Level", ColorNormal);
			if (naming) {
				std::wstring line;
				switch (prompt) {
				case Prompt::SaveAsName: line = L"Name: " + Widen(nameText) + L"_   Enter saves, Esc stops"; break;
				case Prompt::NewMapName: line = L"New map name: " + Widen(nameText) + L"_   Enter, Esc stops"; break;
				case Prompt::NewMapWidth: line = L"Width in tiles: " + Widen(nameText) + L"_   Enter, Esc stops"; break;
				case Prompt::NewMapHeight: line = L"Height in tiles (width " + std::to_wstring(newMapWidth) + L"): " + Widen(nameText) + L"_   Enter makes it"; break;
				case Prompt::LevelText: {
					// Only the end of a long text fits on the line.
					constexpr std::size_t MostShown = 60;
					const auto shown = nameText.size() > MostShown ? "..." + nameText.substr(nameText.size() - MostShown) : nameText;
					line = std::wstring(LevelTextRows[promptIndex].second) + L": " + Widen(shown) + L"_   Enter keeps it, Esc stops";
					break;
				}
				case Prompt::Gold: line = L"Starting gold: " + Widen(nameText) + L"_   Enter keeps it, Esc stops"; break;
				case Prompt::ActionPointName: line = L"Action point " + std::to_wstring(promptIndex) + L"'s name: " + Widen(nameText) + L"_   Enter keeps it, Esc stops"; break;
				case Prompt::TriggerField: {
					const std::wstring which = promptIndex == FieldValue ? L"Value" : promptIndex == FieldRepeat ? L"Repeat" : L"Byte " + std::to_wstring(promptIndex);
					line = which + L": " + Widen(nameText) + L"_   Enter keeps it, Esc stops";
					break;
				}
				}
				// What went wrong with the last Enter, said while still typing.
				if (!statusText.empty()) line += L"   " + Widen(statusText);
				add(PaletteLeft, PaletteRight, 4, PaletteAction::None, 0, line, ColorHover);
			}
			else add(PaletteLeft, PaletteRight, 4, PaletteAction::None, 0, Widen(statusText), ColorDim);
		}

		// The palette and the creature menu each have a view of their own, cleared here. GuiTextCache holds a clear through
		// 0x42CA70 back and leaves out text that matches the last run, then does the clear when text first differs. That wipes the
		// text it left out, and it only draws it again the next time the view is sent text. The palette redraws only when it
		// changes, so hovering Save or Undo could take the Owner line away until the next change.
		constexpr std::uint32_t TextViewTilesWithoutTheStub = 140;
		bool MakeTextView(std::uint8_t* playerInterface, std::uint8_t* view, std::size_t viewSize, std::int32_t width, std::int32_t height, const char* name) {
			const std::uint32_t tiles = (static_cast<std::uint32_t>(width) + 0x7F) / 128 * ((static_cast<std::uint32_t>(height) + 0x1F) / 32);
			if (tiles > TextViewTilesWithoutTheStub && Memory::Read<std::uint8_t>(Game::Resolve(Game::Address::GuiViewTileWrite)) != 0xE9) {
				Logger::Warning(std::string("Editor: ") + name + " needs Widescreen's larger view table at this resolution.");
				return false;
			}
			std::memset(view, 0, viewSize);
			using MakeFn = std::int32_t(__fastcall*)(std::uint8_t* forInterface, void* unused, std::int32_t width, std::int32_t height, std::int32_t unknown, const char* name, std::uint8_t* forView);
			if (GameFunction<MakeFn>(Game::Address::GuiViewCreate)(playerInterface, nullptr, width, height, 0x20, name, view) == 0) {
				Logger::Warning(std::string("Editor: could not make the view ") + name + ".");
				return false;
			}
			return true;
		}
		void ClearTextView(std::uint8_t* view) {
			std::memset(Load<std::uint8_t*>(view + Game::Offset::ViewPixels), 0,
				static_cast<std::size_t>(Load<std::int32_t>(view + Game::Offset::ViewPitch)) * static_cast<std::size_t>(Load<std::int32_t>(view + Game::Offset::ViewPixelHeight)));
		}
		alignas(4) std::array<std::uint8_t, 0x2A0> paletteView{};
		bool paletteViewMade = false;
		bool paletteViewUnusable = false;
		using DrawViewFn = void(__fastcall*)(std::uint8_t* playerInterface, void* unused, std::uint8_t* view, std::int32_t x, std::int32_t y, std::int32_t flags);
		using UploadViewFn = void(__fastcall*)(std::uint8_t* playerInterface, void* unused, std::uint8_t* view);
		using CopyFontFn = void(__fastcall*)(void* to, void* unused, const void* from);
		// The game's own copy of a font goes on the stack by value, and the function called destroys it.
		struct FontValue { std::uint8_t bytes[0x50]; };
		using ViewTextWideFn = void(__fastcall*)(std::uint8_t* playerInterface, void* unused, std::uint8_t* view, std::int32_t x, std::int32_t y, const wchar_t* text,
			const std::uint8_t* color, std::int32_t alignment, std::int32_t clear, FontValue font, std::int32_t upload);

		// The see-through dark the options menu has behind it, only where no text has drawn.
		void DarkenPaletteBackground(std::uint8_t* view, const PaletteCell& area) {
			const auto bits = Load<std::uint32_t>(view + Game::Offset::ViewBitsPerPixel);
			if (bits != 16 && bits != 32) return;
			auto* pixels = Load<std::uint8_t*>(view + Game::Offset::ViewPixels);
			const auto pitch = Load<std::int32_t>(view + Game::Offset::ViewPitch);
			const auto right = (std::min)(area.right, Load<std::int32_t>(view + Game::Offset::ViewPixelWidth));
			const auto bottom = (std::min)(area.bottom, Load<std::int32_t>(view + Game::Offset::ViewPixelHeight));
			for (auto y = (std::max)(area.top, 0); y < bottom; ++y) {
				auto* line = pixels + static_cast<std::ptrdiff_t>(y) * pitch;
				for (auto x = (std::max)(area.left, 0); x < right; ++x) {
					if (bits == 32) {
						auto* pixel = reinterpret_cast<std::uint32_t*>(line) + x;
						if (*pixel == 0) *pixel = 0x80000000u;
					}
					else {
						auto* pixel = reinterpret_cast<std::uint16_t*>(line) + x;
						if (*pixel == 0) *pixel = 0x8000u;
					}
				}
			}
		}
		void DrawPalette(std::uint8_t* playerInterface) {
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) != 0 || Load<std::int32_t>(playerInterface + Game::Offset::InterfaceObjectiveOpen) != 0) {
				// Those menus use this part of the screen.
				paletteShown = false;
				return;
			}
			const auto origin = LayoutToScreen(playerInterface, PaletteLeft, PaletteTop);
			if (!paletteViewMade && !paletteViewUnusable) {
				const auto corner = LayoutToScreen(playerInterface, PaletteLeft + PaletteViewLayoutWidth, PaletteTop + PaletteRows * PaletteRowHeight);
				paletteViewMade = MakeTextView(playerInterface, paletteView.data(), paletteView.size(), corner.x - origin.x, corner.y - origin.y, "EditorPaletteTextBuffer");
				paletteViewUnusable = !paletteViewMade;
				paletteDrawn.clear();
			}
			if (!paletteViewMade) {
				paletteShown = false;
				return;
			}
			auto* view = paletteView.data();
			paletteOriginX = origin.x;
			paletteOriginY = origin.y;
			BuildPalette();
			for (auto& cell : paletteCells) {
				const auto topLeft = LayoutToScreen(playerInterface, cell.left, cell.top);
				const auto bottomRight = LayoutToScreen(playerInterface, cell.right, cell.bottom);
				cell.left = topLeft.x - origin.x;
				cell.top = topLeft.y - origin.y;
				cell.right = bottomRight.x - origin.x;
				cell.bottom = bottomRight.y - origin.y;
			}
			paletteShown = true;
			const auto mouse = MousePosition();
			const auto mouseX = mouse.x - origin.x;
			const auto mouseY = mouse.y - origin.y;
			std::wstring drawing;
			for (std::size_t i = 1; i < paletteCells.size(); ++i) {
				auto& cell = paletteCells[i];
				if (cell.action != PaletteAction::None && cell.color == ColorNormal && mouseX >= cell.left && mouseX < cell.right && mouseY >= cell.top && mouseY < cell.bottom) cell.color = ColorHover;
				drawing += cell.text;
				drawing += static_cast<wchar_t>(L'0' + cell.color);
			}
			// Sending a view to the screen is slow at high resolutions, so only when something on it has changed.
			if (drawing != paletteDrawn) {
				paletteDrawn = drawing;
				ClearTextView(view);
				const auto indent = LayoutToScreen(playerInterface, PaletteTextIndent, 0).x - LayoutToScreen(playerInterface, 0, 0).x;
				for (std::size_t i = 1; i < paletteCells.size(); ++i) {
					const auto& cell = paletteCells[i];
					if (cell.text.empty()) continue;
					FontValue font;
					GameFunction<CopyFontFn>(Game::Address::TextFontCopy)(&font, nullptr, reinterpret_cast<const void*>(Game::Resolve(Game::Address::ViewTextFont)));
					GameFunction<ViewTextWideFn>(Game::Address::GuiViewTextWide)(playerInterface, nullptr, view, cell.left + indent, cell.top, cell.text.c_str(), PaletteColors[cell.color].data(), 0, 0, font, 0);
				}
				DarkenPaletteBackground(view, paletteCells.front());
				GameFunction<UploadViewFn>(Game::Address::GuiViewUpload)(playerInterface, nullptr, view);
			}
			GameFunction<DrawViewFn>(Game::Address::GuiViewDraw)(playerInterface, nullptr, view, origin.x, origin.y, 8);
		}
		void OpenLevelMenu(std::uint8_t* playerInterface);
		bool ClickPalette(std::uint8_t* playerInterface) {
			if (!MouseOverPalette()) return false;
			const auto mouse = MousePosition();
			const auto x = mouse.x - paletteOriginX;
			const auto y = mouse.y - paletteOriginY;
			for (std::size_t i = 1; i < paletteCells.size(); ++i) {
				const auto& cell = paletteCells[i];
				if (cell.action == PaletteAction::None || x < cell.left || x >= cell.right || y < cell.top || y >= cell.bottom) continue;
				switch (cell.action) {
				case PaletteAction::Undo:
					undoWanted = true;
					break;
				case PaletteAction::Redo:
					redoWanted = true;
					break;
				case PaletteAction::Level:
					OpenLevelMenu(playerInterface);
					break;
				case PaletteAction::Brush:
					brushChoice = (brushChoice + 1) % BrushSizes.size();
					paintMode = PaintMode::Brush;
					tool = Tool::Terrain;
					statusText = "Brush " + std::to_string(BrushSizes[brushChoice]) + "x" + std::to_string(BrushSizes[brushChoice]);
					break;
				case PaletteAction::Mode: {
					// Paste only once something is copied.
					const auto count = static_cast<int>(clipboard ? 5 : 4);
					paintMode = tool != Tool::Terrain ? paintMode : static_cast<PaintMode>((static_cast<int>(paintMode) + 1) % count);
					StopMoving();
					tool = Tool::Terrain;
					switch (paintMode) {
					case PaintMode::Brush: statusText = "Drag to paint with the brush"; break;
					case PaintMode::Box: statusText = "Drag out a box to fill it with " + TerrainName(); break;
					case PaintMode::Fill: statusText = "Click to fill the area of the same tile with " + TerrainName(); break;
					case PaintMode::Copy: statusText = "Drag out a box to copy its tiles and things"; break;
					case PaintMode::Paste: statusText = "Click where the copy's top left goes"; break;
					}
					break;
				}
				case PaletteAction::Save:
					saveWanted = true;
					break;
				case PaletteAction::SaveAs:
					naming = true;
					prompt = Prompt::SaveAsName;
					nameText.clear();
					statusText.clear();
					break;
				case PaletteAction::NewMap:
					if (unsavedChanges && !newMapWarned) {
						newMapWarned = true;
						Say("Changes not saved! Save first, or click New map again to leave them");
						break;
					}
					newMapWarned = false;
					naming = true;
					prompt = Prompt::NewMapName;
					nameText.clear();
					statusText.clear();
					break;
				case PaletteAction::Test:
					testWanted = true;
					break;
				case PaletteAction::StopTest:
					stopTestWanted = true;
					break;
				default:
					break;
				}
				break;
			}
			return true;
		}

		// ---- The menu a right click on a creature opens. A view of its own, so that closing it is only not drawing it: the
		// palette's view keeps what was on it until GuiTextCache sees different text. More options go in OpenCreatureMenu.
		enum class MenuAction { None, ToggleLevels, SetLevel, Move, Delete, PickTile, LevelText, PlayerGold, PlayerComputer, ToggleAvailability,
			OpenTriggers, TriggerOwner, TriggersBack, PagePrevious, PageNext, RenameActionPoint, ActionPointUses,
			EditTrigger, StartTree, ChooseType, SetType, CycleCompare, ToggleCompareWith, TriggerField, AddAfter, AddTriggerUnder, AddActionUnder,
			DeleteTrigger, BackToTree, OpenParties, ShowParty, NewParty, AddMember, RemoveMember, MemberLevel, DeleteParty };
		struct MenuRow {
			std::wstring text;
			MenuAction action;
			std::uint8_t level = 0;
			// Which level text, or which record of the level's players.
			std::size_t index = 0;
		};
		constexpr std::int32_t MenuLayoutWidth = 700;
		constexpr std::int32_t MenuLayoutRowHeight = 56;
		// Creatures reach level 10 [verified: 0x48B120 stops there].
		constexpr std::uint8_t MaxCreatureLevel = 10;
		// The level menu is the longest: its title, five texts, a gold and a computer row for each of up to 7 players, availability.
		constexpr std::size_t MenuMaxRows = 21;
		alignas(4) std::array<std::uint8_t, 0x2A0> menuView{};
		bool menuViewMade = false;
		std::int32_t menuPixelWidth = 0;
		std::int32_t menuRowPixels = 0;
		bool menuOpen = false;
		std::uint16_t menuTag = 0;
		std::int32_t menuX = 0;
		std::int32_t menuY = 0;
		std::vector<MenuRow> menuRows;
		std::int32_t menuDrawnOver = -2;
		std::wstring menuTitle;
		// Where the mouse was when it opened: it moves up from there if the rows would go off the screen.
		std::int32_t menuAskedY = 0;
		std::uint8_t menuLevel = 0;
		bool menuHasRecord = false;
		bool menuLevelsOpen = false;
		// Creatures only: traps and doors have no level.
		bool menuShowsLevel = false;
		// On a room's tile instead of a thing.
		bool menuIsRoom = false;
		bool menuHasDelete = true;
		// On an action point, by its number.
		bool menuIsActionPoint = false;
		std::uint8_t menuActionPointId = 0;
		// Only what is in the level's list can move, as moving is changing its record.
		bool menuCanMove = false;
		// The level's own settings, from the palette's Level.
		bool menuIsLevel = false;
		// On a dead body or effect generator, by its place in the level's list.
		bool menuIsListed = false;
		std::size_t menuListedPlace = 0;
		// The triggers, read only: the list of whose trees there are, or one list of rows (a tree, or what uses an action point),
		// a page at a time.
		bool menuIsTriggers = false;
		// Which page: whose trees there are, one tree (rows that open their record), what uses an action point, one record to change,
		// the types to choose from, the hero parties, one party.
		enum class TriggerPage { Owners, Tree, Uses, Edit, Types, Parties, Party };
		TriggerPage triggerPageKind = TriggerPage::Owners;
		std::wstring triggerTitle;
		std::vector<std::wstring> triggerRows;
		std::optional<TriggerOwner> shownOwner;
		std::size_t editingTrigger = 0;
		std::size_t shownParty = 0;
		std::size_t triggerPage = 0;
		constexpr std::size_t TriggerRowsAPage = 15;
		// The lowest a row may reach, in screen pixels: the top of the bottom bar (layout y 1450).
		constexpr std::int32_t MenuBottomLayoutY = 1450;
		std::int32_t menuBottomLimit = 0;
		std::int32_t menuTileX = 0;
		std::int32_t menuTileY = 0;

		bool MakeMenuView(std::uint8_t* playerInterface) {
			if (menuViewMade) return true;
			menuPixelWidth = LayoutToScreen(playerInterface, MenuLayoutWidth, 0).x - LayoutToScreen(playerInterface, 0, 0).x;
			menuRowPixels = LayoutToScreen(playerInterface, 0, MenuLayoutRowHeight).y - LayoutToScreen(playerInterface, 0, 0).y;
			if (!MakeTextView(playerInterface, menuView.data(), menuView.size(), menuPixelWidth, menuRowPixels * static_cast<std::int32_t>(MenuMaxRows), "EditorMenuTextBuffer")) return false;
			menuViewMade = true;
			menuDrawnOver = -2;
			return true;
		}
		void ReleaseTextViews(std::uint8_t* playerInterface) {
			using FreeFn = void(__fastcall*)(std::uint8_t* forInterface, void* unused, std::uint8_t* forView);
			if (menuViewMade) GameFunction<FreeFn>(Game::Address::GuiViewFree)(playerInterface, nullptr, menuView.data());
			if (paletteViewMade) GameFunction<FreeFn>(Game::Address::GuiViewFree)(playerInterface, nullptr, paletteView.data());
			menuViewMade = false;
			menuOpen = false;
			paletteViewMade = false;
			paletteViewUnusable = false;
			paletteShown = false;
			paletteDrawn.clear();
		}
		// For the log: whether a line of text left anything in the view.
		bool ViewBandHasPixels(const std::uint8_t* view, std::int32_t top, std::int32_t height) {
			const auto* pixels = Load<const std::uint8_t*>(view + Game::Offset::ViewPixels);
			const auto pitch = Load<std::int32_t>(view + Game::Offset::ViewPitch);
			const auto* start = pixels + static_cast<std::ptrdiff_t>(top) * pitch;
			return std::any_of(start, start + static_cast<std::ptrdiff_t>(height) * pitch, [](std::uint8_t byte) { return byte != 0; });
		}
		std::uint32_t menuBlankRowsLogged = 0;
		// The row the mouse is on, below 0 for none. The first row is the creature's name.
		std::int32_t MenuRowAtMouse() {
			const auto mouse = MousePosition();
			if (menuRowPixels <= 0 || mouse.x < menuX || mouse.x >= menuX + menuPixelWidth || mouse.y < menuY) return -1;
			const auto row = (mouse.y - menuY) / menuRowPixels;
			return row < static_cast<std::int32_t>(menuRows.size()) ? row : -1;
		}
		// A player record of the level's Players: +0 starting gold (given to the player at load, 0x4BA420), +4 non-zero for a computer
		// player whose settings follow at +8 (0x51F0B0), +0xA8 the player's number, +0xAD a 32 character name [verified].
		constexpr std::size_t PlayerGoldOffset = 0;
		constexpr std::size_t PlayerComputerOffset = 4;
		constexpr std::size_t PlayerNumberOffset = 0xA8;
		constexpr std::size_t PlayerRecordMinimum = 0xAD;
		// A text cut short for a menu row.
		std::wstring Shortened(const std::wstring& text) {
			constexpr std::size_t MostShown = 28;
			return text.size() <= MostShown ? text : text.substr(0, MostShown) + L"...";
		}
		void BuildLevelMenuRows() {
			menuRows.push_back({ L"Level settings", MenuAction::None });
			if (document && document->level) {
				for (std::size_t i = 0; i < LevelTextRows.size(); ++i) {
					const auto text = LevelFiles::GetLevelText(*document->level, LevelTextRows[i].first);
					menuRows.push_back({ std::wstring(LevelTextRows[i].second) + L": " + (text.empty() ? L"(none)" : Shortened(text)), MenuAction::LevelText, 0, i });
				}
			}
			if (document && document->players) {
				const auto& records = document->players->records;
				for (std::size_t i = 0; i < records.size() && menuRows.size() + 4 < MenuMaxRows; ++i) {
					if (records[i].size() < PlayerRecordMinimum) continue;
					const auto number = records[i][PlayerNumberOffset];
					const auto name = Widen(OwnerName(number));
					menuRows.push_back({ name + L" starts with " + std::to_wstring(Load<std::uint32_t>(records[i].data() + PlayerGoldOffset)) + L" gold", MenuAction::PlayerGold, 0, i });
					if (number >= Game::FirstKeeperPlayer) {
						const bool computer = Load<std::int32_t>(records[i].data() + PlayerComputerOffset) != 0;
						menuRows.push_back({ name + (computer ? L" is a computer player" : L" is not a computer player"), MenuAction::PlayerComputer, 0, i });
					}
				}
			}
			menuRows.push_back({ availabilityMode ? L"Availability on the panel: on" : L"Availability on the panel: off", MenuAction::ToggleAvailability });
			menuRows.push_back({ L"Triggers and hero parties", MenuAction::OpenTriggers });
		}
		// A keeper made a computer player keeps its own settings (+8 on, 0x9E bytes) if it has any; with none, they are copied from
		// another computer player of the level, as all zero settings were never seen in a stock level [unverified what they would do].
		void TogglePlayerComputer(std::size_t index) {
			if (!document || !document->players || index >= document->players->records.size()) return;
			auto& records = document->players->records;
			auto& record = records[index];
			if (record.size() < PlayerRecordMinimum) return;
			constexpr std::size_t SettingsOffset = 8;
			constexpr std::size_t SettingsSize = 0x9E;
			const bool computer = Load<std::int32_t>(record.data() + PlayerComputerOffset) != 0;
			Store<std::int32_t>(record.data() + PlayerComputerOffset, computer ? 0 : 1);
			if (!computer && std::all_of(record.begin() + SettingsOffset, record.begin() + SettingsOffset + SettingsSize, [](std::uint8_t b) { return b == 0; })) {
				for (const auto& other : records) {
					if (&other == &record || other.size() < PlayerRecordMinimum || Load<std::int32_t>(other.data() + PlayerComputerOffset) == 0) continue;
					std::copy(other.begin() + SettingsOffset, other.begin() + SettingsOffset + SettingsSize, record.begin() + SettingsOffset);
					break;
				}
			}
			unsavedChanges = true;
			leaveWarned = false;
			Say(OwnerName(record[PlayerNumberOffset]) + (computer ? " is not a computer player" : " is a computer player"));
		}
		std::wstring RowText(const std::wstring& text) {
			constexpr std::size_t MostShown = 60;
			return text.size() <= MostShown ? text : text.substr(0, MostShown) + L"...";
		}
		constexpr std::array<const wchar_t*, 7> CompareNames{ L"?", L"<", L"<=", L"=", L">", L">=", L"!=" };
		// What a byte of an action is for, as the level's action handler 0x518AD0 reads it [verified], or for Zoom To and Rotate Around
		// Action Point by the survey. Empty where not known.
		std::wstring ActionByteHint(std::uint8_t type, std::size_t at) {
			const auto tile = [at]() -> std::wstring { return at == 4 || at == 5 ? L"x" : at == 6 || at == 7 ? L"y" : L""; };
			switch (type) {
				case 1: return at == 0 ? L"creature" : at == 1 ? L"player" : at == 2 ? L"level" : at == 3 ? L"flags" : tile();
				case 2: return at == 0 ? L"object" : at == 1 ? L"player" : tile();
				case 6: return at == 0 ? L"player" : at == 1 ? L"1 room 2 creature 3 trap 4 door 5 spell" : at == 2 ? L"which" : at == 3 ? L"0 not available, else researchable" : L"";
				case 7: return at == 0 ? L"flag, 128 level score" : at == 1 ? L"2 a number, 8 set, 16 add, else take" : at == 4 ? L"value (or flag)" : L"";
				case 8: return at == 0 ? L"timer" : L"";
				case 14: return at == 0 ? L"hero party" : at == 4 ? L"action point" : L"";
				case 16: return at == 0 ? L"action point" : at == 1 ? L"on" : at == 4 ? L"time" : L"";
				case 17: return at == 0 ? L"action point" : at == 1 ? L"0 reveal, else hide" : L"";
				case 18: return at == 0 || at == 1 ? L"player" : at == 2 ? L"0 allies, 1 not" : L"";
				case 22: return at == 0 ? L"terrain" : at == 1 ? L"player" : tile();
				case 23: return at == 4 ? L"time" : L"";
				case 26: case 27: return at == 0 ? L"action point" : L"";
				case 39: case 40: return at == 0 ? L"type" : at == 1 ? L"player" : tile();
				case 48: return at == 0 ? L"effect generator" : at == 1 ? L"on" : L"";
				case 49: return at == 1 ? L"player" : tile();
				default: return L"";
			}
		}
		void AddEditRows() {
			const auto& record = document->triggers->records[editingTrigger];
			const auto* bytes = record.bytes.data();
			const auto type = bytes[TriggerTypeAt];
			const bool trigger = record.kind == LevelFiles::TriggerRecordKind;
			menuRows.push_back({ L"Type: " + Widen(trigger ? TriggerTypeName(type) : ActionTypeName(type)), MenuAction::ChooseType });
			if (trigger) {
				menuRows.push_back({ L"Compare: " + std::wstring(bytes[0] < CompareNames.size() ? CompareNames[bytes[0]] : L"?"), MenuAction::CycleCompare });
				const std::wstring what = type == 1 ? L"flag, 128 level score" : type == 2 ? L"timer" : L"what";
				menuRows.push_back({ L"Byte 1 (" + what + L"): " + std::to_wstring(bytes[1]), MenuAction::TriggerField, 0, 1 });
				const bool withValue = (bytes[2] & 1) != 0;
				menuRows.push_back({ withValue ? L"Compared with: the value" : L"Compared with: the flag in byte 3", MenuAction::ToggleCompareWith });
				menuRows.push_back({ L"Value: " + std::to_wstring(Load<std::int32_t>(bytes + 4)), MenuAction::TriggerField, 0, FieldValue });
				menuRows.push_back({ L"Byte 3: " + std::to_wstring(bytes[3]), MenuAction::TriggerField, 0, 3 });
			}
			else {
				for (std::size_t at = 0; at < 8; ++at) {
					const auto hint = ActionByteHint(type, at);
					menuRows.push_back({ L"Byte " + std::to_wstring(at) + (hint.empty() ? L"" : L" (" + hint + L")") + L": " + std::to_wstring(bytes[at]), MenuAction::TriggerField, 0, at });
				}
			}
			menuRows.push_back({ L"Repeat: " + std::to_wstring(bytes[0xF]), MenuAction::TriggerField, 0, FieldRepeat });
			menuRows.push_back({ trigger ? L"Add a trigger after this" : L"Add an action after this", MenuAction::AddAfter });
			if (trigger) {
				const auto child = Load<std::int16_t>(bytes + 0xC);
				if (child == 0) menuRows.push_back({ L"Add a trigger under this", MenuAction::AddTriggerUnder });
				if (child <= 0) menuRows.push_back({ L"Add an action under this", MenuAction::AddActionUnder });
			}
			menuRows.push_back({ trigger ? L"Delete this and all under it" : L"Delete this", MenuAction::DeleteTrigger });
			menuRows.push_back({ L"Back to the tree", MenuAction::BackToTree });
		}
		// The owner shown, found again, as roots change when trees start or their first trigger goes.
		std::optional<TriggerOwner> CurrentOwner() {
			if (!shownOwner) return std::nullopt;
			for (const auto& owner : TriggerOwners()) {
				if (SameOwner(owner, *shownOwner)) return owner;
			}
			return std::nullopt;
		}
		void BuildTriggerMenuRows() {
			menuRows.push_back({ triggerTitle, MenuAction::None });
			std::size_t total = 0;
			const auto first = triggerPage * TriggerRowsAPage;
			const auto last = first + TriggerRowsAPage;
			switch (triggerPageKind) {
			case TriggerPage::Owners: {
				const auto owners = TriggerOwners();
				total = owners.size();
				if (owners.empty()) menuRows.push_back({ L"This level has no triggers file", MenuAction::None });
				for (std::size_t i = first; i < owners.size() && i < last; ++i) {
					menuRows.push_back({ RowText(Widen(owners[i].name) + (owners[i].root == 0 ? L" (none)" : L"")), MenuAction::TriggerOwner, 0, i });
				}
				menuRows.push_back({ L"Hero parties", MenuAction::OpenParties });
				break;
			}
			case TriggerPage::Tree: {
				const auto owner = CurrentOwner();
				const auto rows = owner ? TriggerTreeRowsOf(owner->root) : std::vector<TriggerRow>{};
				total = rows.size();
				if (rows.empty()) menuRows.push_back({ L"Nothing yet: add a first trigger", MenuAction::StartTree });
				for (std::size_t i = first; i < rows.size() && i < last; ++i) menuRows.push_back({ RowText(rows[i].text), MenuAction::EditTrigger, 0, rows[i].record });
				menuRows.push_back({ L"Back to the list", MenuAction::TriggersBack });
				break;
			}
			case TriggerPage::Uses:
				total = triggerRows.size();
				if (triggerRows.empty()) menuRows.push_back({ L"Nothing", MenuAction::None });
				for (std::size_t i = first; i < triggerRows.size() && i < last; ++i) menuRows.push_back({ RowText(triggerRows[i]), MenuAction::None });
				menuRows.push_back({ L"Back to the list", MenuAction::TriggersBack });
				break;
			case TriggerPage::Edit:
				if (document && document->triggers && editingTrigger < document->triggers->records.size()) AddEditRows();
				else menuRows.push_back({ L"Back to the tree", MenuAction::BackToTree });
				break;
			case TriggerPage::Types: {
				const bool trigger = document && document->triggers && editingTrigger < document->triggers->records.size() && document->triggers->records[editingTrigger].kind == LevelFiles::TriggerRecordKind;
				total = trigger ? TriggerTypeNames.size() : ActionTypeNames.size();
				for (std::size_t i = (std::max)(first, std::size_t{ 1 }); i < total && i < last; ++i) {
					menuRows.push_back({ std::to_wstring(i) + L" " + Widen(trigger ? TriggerTypeNames[i] : ActionTypeNames[i]), MenuAction::SetType, 0, i });
				}
				menuRows.push_back({ L"Back", MenuAction::EditTrigger, 0, editingTrigger });
				break;
			}
			case TriggerPage::Parties: {
				std::vector<std::size_t> parties;
				for (std::size_t i = 0; document->things && i < document->things->records.size(); ++i) {
					if (IsParty(document->things->records[i])) parties.push_back(i);
				}
				total = parties.size();
				for (std::size_t i = first; i < parties.size() && i < last; ++i) {
					menuRows.push_back({ RowText(Widen(PartyText(document->things->records[parties[i]]))), MenuAction::ShowParty, 0, parties[i] });
				}
				menuRows.push_back({ L"New hero party", MenuAction::NewParty });
				menuRows.push_back({ L"Back to the list", MenuAction::TriggersBack });
				break;
			}
			case TriggerPage::Party: {
				if (!document->things || shownParty >= document->things->records.size() || !IsParty(document->things->records[shownParty])) {
					menuRows.push_back({ L"Back to the parties", MenuAction::OpenParties });
					break;
				}
				const auto& bytes = document->things->records[shownParty].bytes;
				for (std::size_t m = 0; m < PartyMembers; ++m) {
					const auto* member = bytes.data() + PartyMembersAt + m * PartyMemberSize;
					if (member[MemberTypeAt] == 0) continue;
					menuRows.push_back({ Widen(CreatureName(member[MemberTypeAt])) + L", level " + std::to_wstring(member[CreatureRecordLevel]) + L" (click: next level)", MenuAction::MemberLevel, 0, m });
				}
				menuRows.push_back({ L"Add a " + Widen(CreatureName(creatureChoice)) + L" (chosen on the Creatures tab)", MenuAction::AddMember });
				menuRows.push_back({ L"Remove the last member", MenuAction::RemoveMember });
				menuRows.push_back({ L"Delete this party", MenuAction::DeleteParty });
				menuRows.push_back({ L"Back to the parties", MenuAction::OpenParties });
				break;
			}
			}
			if (triggerPage > 0) menuRows.push_back({ L"Previous page", MenuAction::PagePrevious });
			if (last < total) menuRows.push_back({ L"Next page", MenuAction::PageNext });
		}
		void BuildMenuRows() {
			menuRows.clear();
			if (menuIsTriggers) {
				BuildTriggerMenuRows();
				menuY = (std::max)(0, (std::min)(menuAskedY, menuBottomLimit - menuRowPixels * static_cast<std::int32_t>(menuRows.size())));
				menuDrawnOver = -2;
				return;
			}
			if (menuIsLevel) {
				BuildLevelMenuRows();
				menuY = (std::max)(0, (std::min)(menuAskedY, menuBottomLimit - menuRowPixels * static_cast<std::int32_t>(menuRows.size())));
				menuDrawnOver = -2;
				return;
			}
			menuRows.push_back({ menuTitle, MenuAction::None });
			// What is not in the level's list has no record to change, so its level only shows.
			const auto level = L"Level " + std::to_wstring(menuLevel);
			if (!menuShowsLevel) {}
			else if (!menuHasRecord) menuRows.push_back({ level, MenuAction::None });
			else {
				menuRows.push_back({ level + (menuLevelsOpen ? L"  ^" : L"  v"), MenuAction::ToggleLevels });
				if (menuLevelsOpen) {
					for (std::uint8_t value = 1; value <= MaxCreatureLevel; ++value) menuRows.push_back({ std::to_wstring(value), MenuAction::SetLevel, value });
				}
			}
			if (menuIsActionPoint) {
				menuRows.push_back({ L"Rename", MenuAction::RenameActionPoint });
				const auto uses = ActionPointUses(menuActionPointId).size();
				menuRows.push_back({ uses == 0 ? L"No trigger uses it" : L"Triggers using it (" + std::to_wstring(uses) + L")", uses == 0 ? MenuAction::None : MenuAction::ActionPointUses });
			}
			if (menuCanMove) menuRows.push_back({ L"Move", MenuAction::Move });
			if (menuHasDelete) menuRows.push_back({ L"Delete", MenuAction::Delete });
			if (menuIsRoom) menuRows.push_back({ L"Paint with this", MenuAction::PickTile });
			// Above the bottom bar and panel: the game draws those over any view, so rows there were hidden behind them (the
			// user's screenshots: levels 4 and 5 and Delete missing under the bar).
			menuY = (std::max)(0, (std::min)(menuAskedY, menuBottomLimit - menuRowPixels * static_cast<std::int32_t>(menuRows.size())));
			menuDrawnOver = -2;
		}
		void OpenCreatureMenu(std::uint8_t* playerInterface, std::uint16_t tag) {
			if (!MakeMenuView(playerInterface)) {
				statusText = "The creature menu cannot show here, see the log";
				return;
			}
			const auto* thing = ThingOfTag(tag);
			menuShowsLevel = IsCreature(tag);
			menuIsRoom = false;
			menuIsActionPoint = false;
			menuIsLevel = false;
			menuIsTriggers = false;
			menuIsListed = false;
			menuHasDelete = true;
			if (IsObject(tag)) {
				// Only listed objects get a menu (ListedObjectOnTile), so the record is there.
				const auto place = FindObjectRecord(tag);
				const auto fields = place ? ReadCreatureRecord(document->things->records[*place]) : std::optional<CreatureFields>{};
				menuTitle = Widen((place ? ObjectRecordName(document->things->records[*place]) : ObjectName(thing[Game::Offset::ObjectType])) + ", " + OwnerName(fields ? fields->owner : NeutralOwner));
			}
			else if (menuShowsLevel) {
				const bool mercenary = (Load<std::uint32_t>(thing + Game::Offset::CreatureStateFlags) & MercenaryStateFlag) != 0;
				menuTitle = Widen((mercenary ? "Mercenary " : "") + CreatureDisplayName(thing) + ", " + OwnerName(CreatureOwner(thing)));
				menuHasRecord = FindCreatureRecord(thing).has_value();
				menuLevel = thing[Game::Offset::CreatureLevel];
			}
			else menuTitle = Widen(TrapOrDoorName(tag) + ", " + OwnerName(TrapOrDoorOwner(thing)));
			if (IsObject(tag)) menuCanMove = FindObjectRecord(tag).has_value();
			else if (menuShowsLevel) menuCanMove = menuHasRecord;
			else menuCanMove = FindTrapOrDoorRecord(tag).has_value();
			menuLevelsOpen = false;
			menuTag = tag;
			const auto mouse = MousePosition();
			const auto screenWidth = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenWidth));
			menuX = (std::max)(0, (std::min)(mouse.x, screenWidth - menuPixelWidth));
			menuAskedY = mouse.y;
			menuBottomLimit = LayoutToScreen(playerInterface, 0, MenuBottomLayoutY).y;
			BuildMenuRows();
			menuOpen = true;
		}
		void OpenRoomMenu(std::uint8_t* playerInterface, std::int32_t x, std::int32_t y, const std::string& name, std::uint8_t owner) {
			if (!MakeMenuView(playerInterface)) {
				statusText = "The menu cannot show here, see the log";
				return;
			}
			menuShowsLevel = false;
			menuIsRoom = true;
			menuIsActionPoint = false;
			menuIsLevel = false;
			menuIsTriggers = false;
			menuIsListed = false;
			menuCanMove = false;
			// These are things in the level's list as well as tiles.
			menuHasDelete = name.find("Dungeon Heart") == std::string::npos && name.find("Hero Gate") == std::string::npos;
			menuTitle = Widen(name + ", " + OwnerName(owner));
			menuTileX = x;
			menuTileY = y;
			menuLevelsOpen = false;
			const auto mouse = MousePosition();
			const auto screenWidth = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenWidth));
			menuX = (std::max)(0, (std::min)(mouse.x, screenWidth - menuPixelWidth));
			menuAskedY = mouse.y;
			menuBottomLimit = LayoutToScreen(playerInterface, 0, MenuBottomLayoutY).y;
			BuildMenuRows();
			menuOpen = true;
		}
		void OpenActionPointMenu(std::uint8_t* playerInterface, const ActionPointBox& box) {
			if (!MakeMenuView(playerInterface)) {
				statusText = "The menu cannot show here, see the log";
				return;
			}
			menuShowsLevel = false;
			menuIsRoom = false;
			menuIsActionPoint = true;
			menuIsLevel = false;
			menuIsTriggers = false;
			menuIsListed = false;
			menuActionPointId = box.id;
			menuHasDelete = true;
			menuCanMove = true;
			menuTitle = Widen(ActionPointText(box));
			menuLevelsOpen = false;
			const auto mouse = MousePosition();
			const auto screenWidth = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenWidth));
			menuX = (std::max)(0, (std::min)(mouse.x, screenWidth - menuPixelWidth));
			menuAskedY = mouse.y;
			menuBottomLimit = LayoutToScreen(playerInterface, 0, MenuBottomLayoutY).y;
			BuildMenuRows();
			menuOpen = true;
		}
		void OpenListedMenu(std::uint8_t* playerInterface, std::size_t place) {
			if (!MakeMenuView(playerInterface) || !document || !document->things || place >= document->things->records.size()) return;
			menuShowsLevel = false;
			menuIsRoom = false;
			menuIsActionPoint = false;
			menuIsLevel = false;
			menuIsTriggers = false;
			menuIsListed = true;
			menuListedPlace = place;
			menuHasDelete = true;
			menuCanMove = false;
			menuLevelsOpen = false;
			menuTitle = Widen(ListedOnlyText(document->things->records[place]));
			const auto mouse = MousePosition();
			const auto screenWidth = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenWidth));
			menuX = (std::max)(0, (std::min)(mouse.x, screenWidth - menuPixelWidth));
			menuAskedY = mouse.y;
			menuBottomLimit = LayoutToScreen(playerInterface, 0, MenuBottomLayoutY).y;
			BuildMenuRows();
			menuOpen = true;
		}
		// Beside the palette, as it is opened from there.
		void OpenLevelMenu(std::uint8_t* playerInterface) {
			if (!MakeMenuView(playerInterface)) {
				statusText = "The menu cannot show here, see the log";
				return;
			}
			menuShowsLevel = false;
			menuIsRoom = false;
			menuIsActionPoint = false;
			menuIsLevel = true;
			menuIsTriggers = false;
			menuIsListed = false;
			menuHasDelete = false;
			menuCanMove = false;
			menuLevelsOpen = false;
			menuX = LayoutToScreen(playerInterface, PaletteRight, 0).x;
			menuAskedY = LayoutToScreen(playerInterface, 0, PaletteTop).y;
			menuBottomLimit = LayoutToScreen(playerInterface, 0, MenuBottomLayoutY).y;
			BuildMenuRows();
			menuOpen = true;
		}
		void DrawCreatureMenu(std::uint8_t* playerInterface) {
			if (!menuOpen) return;
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) != 0 || !menuViewMade) {
				menuOpen = false;
				return;
			}
			const auto over = MenuRowAtMouse();
			if (over != menuDrawnOver) {
				menuDrawnOver = over;
				ClearTextView(menuView.data());
				const auto indent = LayoutToScreen(playerInterface, PaletteTextIndent, 0).x - LayoutToScreen(playerInterface, 0, 0).x;
				for (std::size_t i = 0; i < menuRows.size(); ++i) {
					const auto& row = menuRows[i];
					const auto current = row.action == MenuAction::SetLevel && row.level == menuLevel;
					const auto color = i == 0 || current ? ColorChosen : row.action == MenuAction::None ? ColorDim : static_cast<std::int32_t>(i) == over ? ColorHover : ColorNormal;
					FontValue font;
					GameFunction<CopyFontFn>(Game::Address::TextFontCopy)(&font, nullptr, reinterpret_cast<const void*>(Game::Resolve(Game::Address::ViewTextFont)));
					// The levels sit further in, by position: text starting with spaces was the first suspect for numbers not showing.
					const auto x = row.action == MenuAction::SetLevel ? indent * 4 : indent;
					const auto y = static_cast<std::int32_t>(i) * menuRowPixels;
					GameFunction<ViewTextWideFn>(Game::Address::GuiViewTextWide)(playerInterface, nullptr, menuView.data(), x, y, row.text.c_str(), PaletteColors[color].data(), 0, 0, font, 0);
					if (menuBlankRowsLogged < 20 && !ViewBandHasPixels(menuView.data(), y, menuRowPixels)) {
						++menuBlankRowsLogged;
						Logger::Warning("Editor: row " + std::to_string(i) + " of the creature menu drew nothing.");
					}
				}
				DarkenPaletteBackground(menuView.data(), { 0, 0, menuPixelWidth, menuRowPixels * static_cast<std::int32_t>(menuRows.size()), PaletteAction::None, 0, L"", ColorNormal });
				GameFunction<UploadViewFn>(Game::Address::GuiViewUpload)(playerInterface, nullptr, menuView.data());
			}
			GameFunction<DrawViewFn>(Game::Address::GuiViewDraw)(playerInterface, nullptr, menuView.data(), menuX, menuY, 8);
		}

		using InterfaceDrawFn = void(__fastcall*)(std::uint8_t* playerInterface);
		InterfaceDrawFn originalInterfaceDraw = nullptr;
		// ---- Windows' pointer instead of the hand. The hand is the game's own software pointer (its pictures come from
		// 0x40ACD0, through SetPointerImage), and the mouse is DirectInput's in exclusive mode, which hides Windows' pointer
		// and stops it moving. In a session the mouse is shared with Windows, the hand has no image, and the game's mouse
		// position is put where Windows' pointer is. See docs/game/level-editor.md.
		constexpr std::uint32_t InputExclusive = 1;
		constexpr std::uint32_t InputShared = 2;
		constexpr std::uint32_t InputForeground = 4;
		bool osPointerUsable = false;
		// Read on the window's thread too.
		std::atomic<bool> osPointerOn = false;
		// Windows' pointer has been shown since the game started, so it may need hiding again.
		std::atomic<bool> osPointerWasUsed = false;
		bool mouseShared = false;
		bool pointerClipped = false;
		std::uint8_t* InputSystemField(std::uintptr_t offset) {
			return Load<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::InputSystem) + offset));
		}
		std::uint8_t* MouseDevice() {
			auto* mouse = InputSystemField(Game::Offset::InputMouseDevice);
			return mouse != nullptr ? mouse + Game::Offset::InputDevicePart : nullptr;
		}
		void SetMouseFlags(std::uint8_t* device, std::uint32_t flags) {
			using SetFlagsFn = std::int32_t*(__fastcall*)(std::uint8_t* device, void* unused, std::int32_t* result, std::uint32_t flags);
			std::int32_t result = 0;
			GameFunction<SetFlagsFn>(Game::Address::InputDeviceSetFlags)(device, nullptr, &result, flags);
			if (result < 0) Logger::Warning("Editor: the mouse's mode could not be changed, code " + std::to_string(result) + ".");
		}
		// WM_SETCURSOR is what the window answers with its pointer. Sent to bring the change in before the mouse moves.
		void AskWindowForPointer(std::uint8_t* device) {
			if (device == nullptr) return;
			const auto window = Load<HWND>(device + Game::Offset::InputDeviceWindow);
			if (window != nullptr) PostMessageA(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
		}
		void PutMouseOnOsPointer(std::uint8_t* mouse) {
			auto* device = MouseDevice();
			if (device == nullptr || mouse == nullptr) return;
			const auto window = Load<HWND>(device + Game::Offset::InputDeviceWindow);
			// With another program in front the pointer is not the game's to follow.
			if (window == nullptr || GetForegroundWindow() != window) return;
			POINT point{};
			if (!GetCursorPos(&point) || !ScreenToClient(window, &point)) return;
			const auto width = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenWidth));
			const auto height = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameScreenHeight));
			if (width <= 0 || height <= 0) return;
			std::int32_t x = point.x;
			std::int32_t y = point.y;
			Renderer::WindowPointToScreen(window, width, height, x, y);
			const auto newX = static_cast<float>(x);
			const auto newY = static_cast<float>(y);
			// The movement is kept up as the game would, for what reads it rather than the position.
			Store<float>(mouse + Game::Offset::MouseMovedX, Load<float>(mouse + Game::Offset::MouseMovedX) + newX - Load<float>(mouse + Game::Offset::MouseFloatX));
			Store<float>(mouse + Game::Offset::MouseMovedY, Load<float>(mouse + Game::Offset::MouseMovedY) + newY - Load<float>(mouse + Game::Offset::MouseFloatY));
			Store<float>(mouse + Game::Offset::MouseFloatX, newX);
			Store<float>(mouse + Game::Offset::MouseFloatY, newY);
			using ClampFn = void(__fastcall*)(std::uint8_t* mouse);
			GameFunction<ClampFn>(Game::Address::MouseClamp)(mouse);
		}
		// Only when the mouse was exclusive, which is full screen: a click off the window (a second monitor) would make
		// the game lose focus and minimize. Windowed, the pointer is free to leave.
		void KeepPointerInWindow(std::uint8_t* device) {
			const auto window = device != nullptr ? Load<HWND>(device + Game::Offset::InputDeviceWindow) : nullptr;
			if (mouseShared && window != nullptr && GetForegroundWindow() == window) {
				RECT client{};
				if (GetClientRect(window, &client)) {
					// Its result says how far the points moved, so 0 is normal for a window at the screen's corner.
					MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
					ClipCursor(&client);
					pointerClipped = true;
				}
			}
			else if (pointerClipped) {
				ClipCursor(nullptr);
				pointerClipped = false;
			}
		}
		void UseOsPointer() {
			auto* device = MouseDevice();
			// Checked each frame: the game sets this again when the screen mode changes.
			if (device != nullptr && (Load<std::uint32_t>(device + Game::Offset::InputDeviceFlags) & InputExclusive) != 0) {
				SetMouseFlags(device, InputShared | InputForeground);
				mouseShared = true;
			}
			if (!osPointerOn.exchange(true)) {
				osPointerWasUsed = true;
				AskWindowForPointer(device);
				Logger::Debug(std::string("Editor: using Windows' mouse pointer (") + (mouseShared ? "full screen" : "windowed") + ").");
			}
			PutMouseOnOsPointer(InputSystemField(Game::Offset::InputMouse));
			KeepPointerInWindow(device);
		}
		void StopOsPointer() {
			if (!osPointerOn.exchange(false)) return;
			if (pointerClipped) {
				ClipCursor(nullptr);
				pointerClipped = false;
			}
			auto* device = MouseDevice();
			if (mouseShared && device != nullptr && (Load<std::uint32_t>(device + Game::Offset::InputDeviceFlags) & InputShared) != 0) SetMouseFlags(device, InputExclusive | InputForeground);
			mouseShared = false;
			AskWindowForPointer(device);
			Logger::Debug("Editor: back to the game's own mouse pointer.");
		}
		bool WindowsPointerShowing() {
			CURSORINFO info{};
			info.cbSize = sizeof(info);
			return GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0;
		}
		// After a session, full screen, the game's pointer is what should show. It was left showing after leaving a level, and
		// with the mouse exclusive again the window hears nothing of the mouse, so it is checked each time the mouse moves.
		void HideLeftoverPointer() {
			auto* device = MouseDevice();
			if (device == nullptr || (Load<std::uint32_t>(device + Game::Offset::InputDeviceFlags) & InputExclusive) == 0) return;
			const auto window = Load<HWND>(device + Game::Offset::InputDeviceWindow);
			if (window == nullptr || GetForegroundWindow() != window) return;
			if (WindowsPointerShowing()) AskWindowForPointer(device);
		}
		using MouseMotionFn = void(__fastcall*)(std::uint8_t* mouse, void* unused, std::uint8_t* event);
		MouseMotionFn originalMouseMotion = nullptr;
		// The game's handler still runs, as it tells the rest of the game the mouse moved, but with its multiplier at
		// zero, so the movement does not go on top of the position taken from Windows.
		void __fastcall MouseMotionDetour(std::uint8_t* mouse, void* unused, std::uint8_t* event) {
			if (!osPointerOn) {
				if (osPointerWasUsed) HideLeftoverPointer();
				originalMouseMotion(mouse, unused, event);
				return;
			}
			PutMouseOnOsPointer(mouse);
			const auto speed = Load<float>(mouse + Game::Offset::MouseSpeed);
			Store<float>(mouse + Game::Offset::MouseSpeed, 0.0f);
			originalMouseMotion(mouse, unused, event);
			Store<float>(mouse + Game::Offset::MouseSpeed, speed);
		}
		using SetPointerImageFn = std::int32_t*(__cdecl*)(std::int32_t* result, void* image, void* sourceRect, void* hotSpot);
		SetPointerImageFn originalSetPointerImage = nullptr;
		std::int32_t* __cdecl SetPointerImageDetour(std::int32_t* result, void* image, void* sourceRect, void* hotSpot) {
			if (inSession && osPointerUsable) return originalSetPointerImage(result, nullptr, nullptr, hotSpot);
			return originalSetPointerImage(result, image, sourceRect, hotSpot);
		}
		using DrawHandIconsFn = void(__fastcall*)(std::uint8_t* playerInterface, void* unused, std::int32_t x, std::int32_t y);
		DrawHandIconsFn originalDrawHandIcons = nullptr;
		void __fastcall DrawHandIconsDetour(std::uint8_t* playerInterface, void* unused, std::int32_t x, std::int32_t y) {
			if (inSession && osPointerUsable) return;
			originalDrawHandIcons(playerInterface, unused, x, y);
		}
		using WindowProcFn = LRESULT(CALLBACK*)(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
		WindowProcFn originalWindowProc = nullptr;
		// The game's window class has no pointer of its own, and the game hides Windows' whenever it comes to the front
		// (0x5BC280). Showing and hiding count per thread, so this is done here, on the window's thread.
		LRESULT CALLBACK WindowProcDetour(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
			// How far the show count was raised, as the game may have it further down than just hidden, and its own calls on
			// focus changes are in pairs that only stay hidden from there.
			static std::int32_t raised = 0;
			if (message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
				if (osPointerOn) {
					// Only when it is hidden: each call counts, and this comes with every mouse movement.
					if (!WindowsPointerShowing()) {
						do { ++raised; } while (ShowCursor(TRUE) < 0);
					}
					SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); // IDC_ARROW: whichever arrow Windows is set to
					return TRUE;
				}
				if (osPointerWasUsed) {
					for (; raised > 0; --raised) ShowCursor(FALSE);
					if (WindowsPointerShowing()) {
						while (ShowCursor(FALSE) >= 0) {}
					}
					SetCursor(nullptr);
				}
			}
			return originalWindowProc(window, message, wParam, lParam);
		}

		void __fastcall InterfaceDrawDetour(std::uint8_t* playerInterface) {
			originalInterfaceDraw(playerInterface);
			if (testing) DrawPalette(playerInterface);
			if (inSession) {
				if (osPointerUsable) UseOsPointer();
				DrawPalette(playerInterface);
				EditorPanel::Draw(playerInterface);
				DrawCreatureMenu(playerInterface);
			}
		}
		// The game's views go, so the panel's does too: its textures go with them.
		using ReleaseViewsFn = void(__fastcall*)(std::uint8_t* playerInterface);
		ReleaseViewsFn originalReleaseViews = nullptr;
		void __fastcall ReleaseViewsDetour(std::uint8_t* playerInterface) {
			EditorPanel::ReleaseView(playerInterface);
			ReleaseTextViews(playerInterface);
			originalReleaseViews(playerInterface);
		}
		// The second tab's button draws over any view, so in a session it is left out and the rock icon in the panel's view shows in
		// its place. It still takes clicks, and its hover text is the Terrain one.
		using PanelButtonRenderFn = std::int32_t(__cdecl*)(const std::uint8_t* button, std::uint8_t* playerInterface);
		PanelButtonRenderFn originalPanelButtonRender = nullptr;
		std::int32_t __cdecl PanelButtonRenderDetour(const std::uint8_t* button, std::uint8_t* playerInterface) {
			constexpr std::int32_t SpellsTabSprite = 6;
			constexpr std::int32_t SpellsTabText = 175;
			if (inSession && Load<std::int32_t>(button + Game::Offset::ButtonSprite) == SpellsTabSprite && Load<std::int32_t>(button + Game::Offset::ButtonTextId) == SpellsTabText
				&& playerInterface[Game::Offset::InterfacePanelOut] != 0) return 1;
			return originalPanelButtonRender(button, playerInterface);
		}
		// The editor hides the game's panels, so their layout has none to share the width between and divided by zero (crash
		// at 0x417366 after testing, when the restart laid the panel out again). In a session there is nothing of theirs to lay out.
		using PanelLayoutFn = void(__fastcall*)(std::uint8_t* playerInterface);
		PanelLayoutFn originalPanelLayout = nullptr;
		void __fastcall PanelLayoutDetour(std::uint8_t* playerInterface) {
			if (inSession) return;
			originalPanelLayout(playerInterface);
		}
		// Nothing is paid here, and a held world would never play the text out.
		using ShowMoneyTextFn = void(__fastcall*)(void* world, void* unused, std::uint32_t first, std::uint32_t second, std::uint32_t third, std::uint32_t fourth);
		ShowMoneyTextFn originalShowMoneyText = nullptr;
		void __fastcall ShowMoneyTextDetour(void* world, void* unused, std::uint32_t first, std::uint32_t second, std::uint32_t third, std::uint32_t fourth) {
			if (inSession) return;
			originalShowMoneyText(world, unused, first, second, third, fourth);
		}
		// What a right click on a tile picks up: its terrain and owner.
		void PickTile(std::int32_t x, std::int32_t y) {
			const auto* tile = DocumentTile(x, y);
			if (tile == nullptr) return;
			for (std::size_t i = 0; i < terrains.size(); ++i) {
				if (terrains[i].id == tile->terrain) terrainChoice = i;
			}
			if (tile->owner >= FirstOwner && tile->owner <= LastOwner) ownerChoice = tile->owner;
			RefreshPanelForOwner();
			ShowChoiceOnPanel();
			LogChoice();
		}
		// A right click on the panel: the item's next variant. Traps and doors go between finished and blueprint, heroes with the
		// textures for it between normal and mercenary, and creatures with a Unique type between normal and Unique.
		constexpr std::uint32_t BlueprintMark = 0xFF2050C0u;
		constexpr std::uint32_t MercenaryMark = 0xFF903020u;
		constexpr std::uint32_t UniqueMark = 0xFFB08800u;
		bool HeroWithMercenaryTextures(std::uint8_t type) {
			using namespace Game::Creature;
			switch (type) {
				case Wizard: case Guard: case Giant: case Thief: case Knight: case Elven_Archer: case Monk: case Fairy: case Royal_Guard:
					return true;
				default:
					return false;
			}
		}
		// Each marked record has a mercenary of its kind on its tile, drawn with the mercenary textures where it has them. Logs what
		// it finds, so a test says which part does not last.
		void EnsureMercenaries(const std::vector<LevelFiles::ThingRecord>& records) {
			std::size_t already = 0;
			std::size_t made = 0;
			std::size_t banksFixed = 0;
			for (const auto& record : records) {
				if (!IsCreatureRecord(record) || record.bytes.size() <= CreatureRecordFlags || (record.bytes[CreatureRecordFlags] & RecordMercenaryFlag) == 0) continue;
				const auto fields = ReadCreatureRecord(record);
				if (!fields) continue;
				std::uint8_t* found = nullptr;
				for (std::uint16_t tag = 1; tag <= Game::TagMask && found == nullptr; ++tag) {
					if (!IsCreature(tag)) continue;
					auto* creature = ThingOfTag(tag);
					if (CreatureTileX(creature) == fields->x && CreatureTileY(creature) == fields->y && creature[Game::Offset::CreatureType] == fields->type
						&& CreatureOwner(creature) == fields->owner && (Load<std::uint32_t>(creature + Game::Offset::CreatureStateFlags) & MercenaryStateFlag) != 0) found = creature;
				}
				if (found == nullptr) {
					MakeMercenaryOnTile(*fields);
					++made;
					continue;
				}
				++already;
				if (HeroWithMercenaryTextures(fields->type) && found[Game::Offset::CreatureTextureBank] != 1) {
					found[Game::Offset::CreatureTextureBank] = 1;
					++banksFixed;
				}
			}
			Logger::Debug("Editor: mercenaries checked: " + std::to_string(already) + " were, " + std::to_string(made) + " made again, " + std::to_string(banksFixed) + " given their textures again.");
		}
		void RecheckMercenaries() {
			if (document && document->things) EnsureMercenaries(document->things->records);
		}
		// ---- A level played for real, not from the editor: the game's level load knows nothing of the mercenary mark, so the
		// level's own things are read and the marked creatures made mercenaries on its first tick, then checked once more. The
		// same files and the same order on every machine, so every player's world does the same.
		bool playLevelPending = false;
		std::uint32_t playRecheckIn = 0;
		std::vector<LevelFiles::ThingRecord> playMercenaries;
		// The level's exact hit points, put on its creatures on its first tick as the mercenary marks are.
		std::vector<LevelFiles::CreatureHealth> playHealth;
		bool HasMercenaryMark(const LevelFiles::ThingRecord& record) {
			return IsCreatureRecord(record) && record.bytes.size() > CreatureRecordFlags && (record.bytes[CreatureRecordFlags] & RecordMercenaryFlag) != 0;
		}
		void ReadPlayMercenaries() {
			playMercenaries.clear();
			if (!creaturesUsable) return;
			const auto* name = reinterpret_cast<const wchar_t*>(Game::Resolve(Game::Address::GameConfigLevelName));
			const std::wstring level(name, wcsnlen(name, Game::LevelNameLength));
			if (level.empty()) return;
			// The path the game builds from the level name (it joins it to Data\editor\Maps), which also reaches a saved copy.
			std::string error;
			const auto loaded = LevelFiles::LoadMap(std::filesystem::path(L"Data\\editor\\Maps") / (level + L".kwd"), error);
			if (!loaded || !loaded->things) return;
			for (const auto& record : loaded->things->records) {
				if (HasMercenaryMark(record)) playMercenaries.push_back(record);
			}
			if (!playMercenaries.empty()) Logger::Debug("Editor: the level marks " + std::to_string(playMercenaries.size()) + " creatures as mercenaries; making them so.");
			playHealth = loaded->creatureHealth;
		}
		// ---- Creature types: Windows' own window (EditorDialogs), opened by a right click on the Creatures tab. It edits a copy of
		// the level's creature list; what it gives back is put in the document after the world tick.
		// What a placed creature of this type is: its plain kind, or its variant (its Unique type, or a mercenary for a hero with
		// the textures for it). Chosen in the creature window's "Places as", shown only for a type that has a variant.
		void SetCreatureVariant(std::uint8_t type, bool variant) {
			if (type == 0) return;
			const bool hasUnique = uniqueOfCreature.contains(type);
			if (!hasUnique && !HeroWithMercenaryTextures(type)) return;
			const std::size_t value = type - std::size_t{ 1 };
			alternateCreatures[value] = variant;
			const auto base = CreatureName(type);
			const std::string kind = hasUnique ? "Unique " : "Mercenary ";
			EditorPanel::SetItemLook(EditorPanel::Tab::Creatures, value, variant ? kind + base : base, variant ? (hasUnique ? UniqueMark : MercenaryMark) : 0);
			Say(variant ? "Places " + kind + base : "Places " + base);
		}
		std::vector<EditorDialogs::NamedId> NamedIds(const std::vector<std::string>& names) {
			std::vector<EditorDialogs::NamedId> named;
			for (std::size_t i = 0; i < names.size(); ++i) named.push_back({ static_cast<std::uint32_t>(i + 1), names[i] });
			return named;
		}
		// The first thing in the level that names this creature type, or empty if nothing does: its things (creatures, dead bodies,
		// hero party members), its variables (a keeper's pool, availability) and Create Creature actions. Ids are places in the list,
		// so a type in use cannot be deleted.
		std::string CreatureTypeUse(std::uint8_t type) {
			if (!document) return {};
			if (document->things) {
				for (const auto& record : document->things->records) {
					if (IsCreatureRecord(record)) {
						if (const auto fields = ReadCreatureRecord(record); fields && fields->type == type) return "a creature placed on the map";
					}
					else if (record.kind == DeadBodyRecord && record.bytes.size() > 0xE && record.bytes[0xE] == type) return "a dead body";
					else if (record.kind == HeroPartyRecord) {
						for (std::size_t m = 0; m < PartyMembers; ++m) {
							const auto at = PartyMembersAt + m * PartyMemberSize + 0x1C;
							if (at < record.bytes.size() && record.bytes[at] == type) return "a hero party";
						}
					}
				}
			}
			if (document->variables) {
				for (const auto& record : document->variables->records) {
					if (record.size() < 0x10) continue;
					const auto kind = Load<std::uint32_t>(record.data());
					if (kind == 1 && Load<std::uint32_t>(record.data() + 4) == type) return "a keeper's creature pool";
					if (kind == 2 && (Load<std::uint32_t>(record.data() + 4) & 0xFFFF) == 2 && Load<std::uint32_t>(record.data() + 8) == type) return "the level's availability";
				}
			}
			if (document->triggers) {
				constexpr std::uint8_t CreateCreatureAction = 1;
				for (const auto& record : document->triggers->records) {
					if (record.kind == LevelFiles::ActionRecordKind && record.bytes.size() > 0xE && record.bytes[0xE] == CreateCreatureAction && record.bytes[0] == type) return "a Create Creature action";
				}
			}
			return {};
		}
		// How many types the shared Creatures.kwd has, with the types mods add: the game's own, which its code knows by id, and the
		// mods', which would come back at the next load. Only types after them can be deleted.
		std::size_t SharedCreatureCount() {
			static std::size_t count = 0;
			if (count == 0) count = CreatureRegistry::SharedTypeCount();
			if (count == 0) {
				std::string error;
				if (const auto table = LevelFiles::ReadDefinitions("Data\\editor\\Creatures.kwd", LevelFiles::CreaturesSection, error)) count = table->names.size();
			}
			return count;
		}
		// A creature record's health percent and type, by kind (0x56F330, 0x56F430, 0x56F540); its gold is 16 bit at +0xC.
		std::size_t CreatureRecordHealth(std::uint32_t kind) { return kind == HeroCreatureRecord ? 0x14 : 0x10; }
		std::size_t CreatureRecordType(std::uint32_t kind) { return kind == HeroCreatureRecord ? 0x1C : kind == NeutralCreatureRecord ? 0x16 : 0x1A; }
		constexpr std::size_t CreatureRecordGold = 0xC;
		// ---- Hit points. The level keeps a whole percent of the creature's most health at its level; KeeperForge2 keeps exact hit
		// points beside it (LevelFiles::CreatureHealth) and puts them on the creature when the level loads.
		// The game's most health percent for a level, from its settings (level 1 is the type's Health as it is).
		std::int32_t LevelHealthPercent(std::uint8_t level) {
			if (level <= 1) return 100;
			const auto settings = Memory::Read<std::uintptr_t>(Game::Resolve(Game::Address::GlobalSettingsPointer));
			if (settings == 0) return 100;
			const auto percent = Memory::Read<std::int16_t>(settings + Game::Offset::SettingsLevelHealthPercent + level * 2u);
			return percent > 0 ? percent : 100;
		}
		// A creature's most hit points at a level: its type's Health (creature record +0xF0C) times that percent.
		std::uint32_t MostHealth(std::uint8_t type, std::uint8_t level) {
			if (!document || !document->creatures || type < 1 || type > document->creatures->records.size()) return 1;
			const auto& record = document->creatures->records[type - std::size_t{ 1 }];
			const std::uint32_t health = record.size() >= 0xF0E ? Load<std::uint16_t>(record.data() + 0xF0C) : 1u;
			return (std::max)(1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(health) * static_cast<std::uint32_t>(LevelHealthPercent(level)) / 100));
		}
		// What the game makes of a whole percent: 1 + trunc(percent * 0.01 * most - 1.0), at least 1 [verified: 0x5208B0].
		std::uint32_t GameHealth(std::int32_t percent, std::uint32_t most) {
			const auto points = 1.0 + std::trunc(percent * 0.01 * most - 1.0);
			return points < 1.0 ? 1u : static_cast<std::uint32_t>(points);
		}
		LevelFiles::CreatureHealth* HealthEntryOf(const CreatureFields& fields) {
			if (!document) return nullptr;
			for (auto& entry : document->creatureHealth) {
				if (entry.x == fields.x && entry.y == fields.y && entry.owner == fields.owner && entry.type == fields.type && entry.level == fields.level) return &entry;
			}
			return nullptr;
		}
		// The record's whole percent, and an exact entry when that percent does not give these points.
		void SetRecordHealth(LevelFiles::ThingRecord& record, const CreatureFields& oldFields, std::uint32_t points) {
			const auto fields = ReadCreatureRecord(record);
			if (!fields || !document) return;
			const auto most = MostHealth(fields->type, fields->level);
			const auto percent = static_cast<std::int32_t>((std::clamp)(std::llround(points * 100.0 / most), 1ll, 100ll));
			Store<std::int32_t>(record.bytes.data() + CreatureRecordHealth(record.kind), percent);
			auto& entries = document->creatureHealth;
			entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const LevelFiles::CreatureHealth& entry) {
				return (entry.x == oldFields.x && entry.y == oldFields.y && entry.owner == oldFields.owner && entry.type == oldFields.type && entry.level == oldFields.level)
					|| (entry.x == fields->x && entry.y == fields->y && entry.owner == fields->owner && entry.type == fields->type && entry.level == fields->level);
			}), entries.end());
			if (GameHealth(percent, most) != points) entries.push_back({ fields->x, fields->y, fields->owner, fields->type, fields->level, points });
			document->creatureHealthChanged = true;
		}
		// An exact entry follows its creature to a new tile (or to its new record).
		void MoveHealthEntry(const CreatureFields& from, const CreatureFields& to) {
			if (auto* entry = HealthEntryOf(from)) {
				entry->x = to.x;
				entry->y = to.y;
				entry->owner = to.owner;
				entry->type = to.type;
				entry->level = to.level;
				document->creatureHealthChanged = true;
			}
		}
		// Puts exact hit points on the creatures they belong to (found on their tile as level load made them).
		void ApplyCreatureHealth(const std::vector<LevelFiles::CreatureHealth>& entries) {
			std::size_t applied = 0;
			for (const auto& entry : entries) {
				// The best match on its tile; its type is not asked to match, as a Unique creature is its base type in the world.
				const auto tag = CreatureOnTile(entry.x, entry.y, entry.type, entry.owner, entry.level);
				if (tag == 0) continue;
				const auto* creature = ThingOfTag(tag);
				if (CreatureOwner(creature) != entry.owner || creature[Game::Offset::CreatureLevel] != entry.level) continue;
				Store<std::int32_t>(ThingOfTag(tag) + Game::Offset::CreatureHealth, static_cast<std::int32_t>(entry.points));
				++applied;
			}
			if (!entries.empty()) Logger::Debug("Editor: exact hit points put on " + std::to_string(applied) + " of " + std::to_string(entries.size()) + " creatures.");
		}
		void ForgetHealthEntry(const LevelFiles::ThingRecord& record) {
			if (!document || !IsCreatureRecord(record)) return;
			const auto fields = ReadCreatureRecord(record);
			if (!fields) return;
			if (const auto* entry = HealthEntryOf(*fields)) {
				document->creatureHealth.erase(document->creatureHealth.begin() + (entry - document->creatureHealth.data()));
				document->creatureHealthChanged = true;
			}
		}
		void PutHealthBack(const std::vector<LevelFiles::CreatureHealth>& was, const std::vector<LevelFiles::CreatureHealth>& now) {
			const auto put = [&](const LevelFiles::CreatureHealth& key) {
				const auto tag = CreatureOnTile(key.x, key.y, key.type, key.owner, key.level);
				if (tag == 0) return;
				auto* creature = ThingOfTag(tag);
				if (CreatureOwner(creature) != key.owner || creature[Game::Offset::CreatureLevel] != key.level) return;
				// Its exact ones if the list has them now, else what its record's percent gives.
				std::uint32_t points = 0;
				for (const auto& entry : now) {
					if (entry.x == key.x && entry.y == key.y && entry.owner == key.owner && entry.type == key.type && entry.level == key.level) points = entry.points;
				}
				if (points == 0) {
					const auto place = FindCreatureRecord(creature);
					if (!place) return;
					const auto& record = document->things->records[*place];
					const auto fields = ReadCreatureRecord(record);
					if (!fields) return;
					points = GameHealth(Load<std::int32_t>(record.bytes.data() + CreatureRecordHealth(record.kind)), MostHealth(fields->type, fields->level));
				}
				Store<std::int32_t>(creature + Game::Offset::CreatureHealth, static_cast<std::int32_t>(points));
			};
			for (const auto& entry : was) put(entry);
			for (const auto& entry : now) put(entry);
		}
		// A placed creature's own fields, from its record in the level's list, for the creature window opened on it.
		std::optional<EditorDialogs::UnitData> UnitDataOf(std::uint16_t tag) {
			if (!IsCreature(tag) || !document || !document->things) return std::nullopt;
			const auto* creature = ThingOfTag(tag);
			const auto place = FindCreatureRecord(creature);
			if (!place) return std::nullopt;
			const auto& record = document->things->records[*place];
			const auto fields = ReadCreatureRecord(record);
			if (!fields) return std::nullopt;
			EditorDialogs::UnitData unit;
			unit.id = tag;
			unit.type = fields->type;
			unit.level = fields->level;
			// Its exact hit points if KeeperForge2 keeps them, else what the level's percent gives.
			if (const auto* entry = HealthEntryOf(*fields)) unit.points = entry->points;
			else unit.points = GameHealth(Load<std::int32_t>(record.bytes.data() + CreatureRecordHealth(record.kind)), MostHealth(fields->type, fields->level));
			unit.gold = Load<std::uint16_t>(record.bytes.data() + CreatureRecordGold);
			unit.where = L"at " + std::to_wstring(fields->x) + L", " + std::to_wstring(fields->y) + L", " + Widen(OwnerName(fields->owner));
			return unit;
		}
		// The window on a type (from the side panel), or, with a tag, on that placed creature and its type.
		void OpenCreatureWindow(std::uint8_t type, std::uint16_t unitTag, bool reopen) {
			closedWindowRecord.reset();
			if (!document || !document->creatures || document->creatures->records.empty()) {
				Say("This level's creature list could not be read, see the log");
				return;
			}
			// The window needs Windows' pointer: with the game's own hand the mouse is held by the game and cannot reach it.
			if (!osPointerOn) {
				Say("Creature properties need Windows' pointer, which is off");
				return;
			}
			EditorDialogs::CreatureDialogData data;
			if (unitTag != 0) {
				data.unit = UnitDataOf(unitTag);
				if (!data.unit) {
					Say("That creature is not in the level's list (level setup made it), so it has nothing of its own to change");
					return;
				}
				type = data.unit->type;
			}
			data.records = document->creatures->records;
			for (std::uint8_t level = 0; level < data.levelHealthPercent.size(); ++level) data.levelHealthPercent[level] = LevelHealthPercent(level);
			for (std::size_t i = 0; i < data.records.size(); ++i) {
				const auto id = static_cast<std::uint8_t>(i + 1);
				const auto level = placeLevels.find(id);
				data.placeLevels.push_back(level != placeLevels.end() ? level->second : std::uint8_t{ 1 });
				data.placeDead.push_back(placeDeadTypes.contains(id));
			}
			data.setPlacingDefaults = [](std::uint8_t id, std::uint8_t level, bool dead) {
				placeLevels[id] = level;
				if (dead) placeDeadTypes.insert(id);
				else placeDeadTypes.erase(id);
				Say("New " + CreatureName(id) + " go down " + (dead ? std::string("as dead bodies") : "at level " + std::to_string(level)));
			};
			data.objects = NamedIds(objectNames);
			data.effects = NamedIds(effectNames);
			data.rooms = NamedIds(roomNames);
			data.spells = NamedIds(creatureSpellNames);
			data.firstDeletable = static_cast<std::uint32_t>((std::max)(SharedCreatureCount(), std::size_t{ 1 }) + 1);
			// Which types have another kind to place as: a Unique type, or a hero with mercenary textures. A type with neither
			// gets no "Places as" list at all.
			for (std::size_t i = 0; i < data.records.size(); ++i) {
				const auto id = static_cast<std::uint8_t>(i + 1);
				using Variant = EditorDialogs::CreatureDialogData::Variant;
				data.variants.push_back(uniqueOfCreature.contains(id) ? Variant::Unique : HeroWithMercenaryTextures(id) ? Variant::Mercenary : Variant::None);
				data.placesVariant.push_back(alternateCreatures.contains(i) && alternateCreatures[i]);
			}
			data.setPlacesVariant = [](std::uint8_t id, bool variant) { SetCreatureVariant(id, variant); };
			data.deleteProblem = [](std::uint8_t deleted) { return CreatureTypeUse(deleted); };
			if ((reopen ? EditorDialogs::ReopenCreatureProperties(std::move(data), type) : EditorDialogs::OpenCreatureProperties(std::move(data), type))) Say(unitTag != 0 ? "Creature properties: this " + CreatureName(type) : "Creature properties: " + CreatureName(type));
			else Say("Could not open the creature properties, see the log");
		}
		void ApplyCreatureListChanges() {
			auto given = EditorDialogs::TakeCreatureList();
			if (!given || !document || !document->creatures) return;
			auto& records = document->creatures->records;
			const auto before = records;
			// Types the window deleted are checked again here, as the level may have used one since: if any is, they all stay.
			if (given->size() < before.size()) {
				for (auto type = given->size() + 1; type <= before.size(); ++type) {
					const auto use = CreatureTypeUse(static_cast<std::uint8_t>(type));
					if (use.empty()) continue;
					Say("Not deleted: " + CreatureName(static_cast<std::uint8_t>(type)) + " is used by " + use);
					given->insert(given->end(), before.begin() + static_cast<std::ptrdiff_t>(given->size()), before.end());
					break;
				}
			}
			std::size_t changed = 0;
			for (std::size_t i = 0; i < (std::max)(given->size(), before.size()); ++i) {
				if (i >= given->size() || i >= before.size() || (*given)[i] != before[i]) ++changed;
			}
			if (changed == 0) return;
			records = std::move(*given);
			document->creaturesChanged = true;
			unsavedChanges = true;
			leaveWarned = false;
			// A deleted type cannot be placed.
			if (creatureChoice > records.size()) {
				creatureChoice = 1;
				if (tool == Tool::Creature || tool == Tool::DeadBody) tool = Tool::Terrain;
			}
			// Names and added types show on the Creatures tab.
			SetCreatureItems();
			if (availabilityMode) ShowAvailabilityLooks();
			else RestoreItemLooks();
			ShowChoiceOnPanel();
			Logger::Debug("Editor: creature list changed: " + std::to_string(changed) + " types, " + std::to_string(records.size()) + " in all.");
			Say(std::to_string(changed) + " creature type(s) changed; save, then Test to play with them");
		}
		// What the window gave back for one placed creature: its level, health, gold and type into its record, made again from it as
		// the level would make it, or turned into a dead body of its type on its tile.
		void ApplyUnitChange(std::uint8_t* world, const EditorDialogs::UnitChange& change) {
			if (!IsCreature(change.id) || !document || !document->things) {
				Say("That creature is not there any more");
				EditorDialogs::SetUnit(std::nullopt);
				return;
			}
			auto* creature = ThingOfTag(change.id);
			const auto place = FindCreatureRecord(creature);
			if (!place) {
				Say("That creature is not in the level's list");
				return;
			}
			auto& records = document->things->records;
			const auto before = records[*place];
			const auto fields = ReadCreatureRecord(before);
			if (!fields) return;
			// Anything still under way is its own undo step, so this change's hit points go with this change's step.
			EndStroke(world);
			auto after = before;
			after.bytes[CreatureRecordLevel] = change.level;
			Store<std::uint16_t>(after.bytes.data() + CreatureRecordGold, change.gold);
			after.bytes[CreatureRecordType(after.kind)] = change.type;
			// Health last, as its percent is of the most at the new level and type.
			const auto healthBefore = document->creatureHealth;
			SetRecordHealth(after, *fields, change.points);
			const auto name = CreatureDisplayName(creature);
			// Only a type the world was made with can be made in it: a creature's own new type shows once the level loads again.
			const bool inWorld = change.type <= loadedCreatureTypes;
			if (change.dead) {
				LevelFiles::ThingRecord body{ DeadBodyRecord, std::vector<std::uint8_t>(0x10, 0) };
				Store<std::int32_t>(body.bytes.data(), fields->x);
				Store<std::int32_t>(body.bytes.data() + 4, fields->y);
				body.bytes[0xE] = change.type;
				body.bytes[0xF] = fields->owner;
				EndStroke(world);
				document->creatureHealth = healthBefore;
				if (!RemoveCreature(world, change.id)) return;
				// A dead body has no hit points: its creature's exact ones go.
				if (const auto* entry = HealthEntryOf(*fields)) {
					document->creatureHealth.erase(document->creatureHealth.begin() + (entry - document->creatureHealth.data()));
					document->creatureHealthChanged = true;
				}
				const bool shown = inWorld && MakeListedOnly(world, body);
				strokeThings.push_back({ CreatureChangeKind::Deleted, *place, before });
				records.erase(records.begin() + static_cast<std::ptrdiff_t>(*place));
				records.push_back(body);
				strokeThings.push_back({ CreatureChangeKind::Placed, records.size() - 1, body });
				EndStroke(world);
				unsavedChanges = true;
				if (selectedCreature == change.id) selectedCreature = 0;
				// The window has no creature left to show, so it closes (as Delete closes it). Types given back with this stay.
				EditorDialogs::CloseCreatureProperties(true);
				// Undo brings the creature back, and the window on it.
				closedWindowRecord = before;
				Say(name + " is a dead body now" + (shown ? "" : "; it shows when the level is loaded again"));
				return;
			}
			if (after.bytes == before.bytes) {
				// Only its exact hit points may have changed (the record's percent is the same): no need to make it again.
				if (inWorld) Store<std::int32_t>(creature + Game::Offset::CreatureHealth, static_cast<std::int32_t>(change.points));
				// An undo step of the hit points alone.
				PushUndo({});
				unsavedChanges = true;
				Say(name + " has " + std::to_string(change.points) + " hit points");
				EditorDialogs::SetUnit(UnitDataOf(change.id));
				return;
			}
			std::uint16_t tag = change.id;
			if (inWorld) {
				if (!RemoveCreature(world, change.id)) {
					document->creatureHealth = healthBefore;
					return;
				}
				if (!MakeCreature(world, after)) {
					MakeCreature(world, before);
					document->creatureHealth = healthBefore;
					Say(name + " could not be changed");
					return;
				}
				EndStroke(world);
				records[*place] = after;
				PushCreatureUndo({ CreatureChangeKind::Changed, *place, before });
				tag = ThingOfRecord(after);
				// Level load's maker gave it the record's whole percent; the exact points go on as the level's load will put them.
				if (tag != 0) Store<std::int32_t>(ThingOfTag(tag) + Game::Offset::CreatureHealth, static_cast<std::int32_t>(change.points));
			}
			else {
				// Its record names a type the world does not have yet, so the world's creature stays as it is until the level loads
				// again. Undo could not find it by its record then, so this one is not in undo.
				EndStroke(world);
				records[*place] = after;
				// Its hit points stay out of undo with it.
				healthAtLastStep = document->creatureHealth;
				Say(name + " has its own type now; it shows once the level is loaded again (not in undo)");
			}
			unsavedChanges = true;
			if (selectedCreature == change.id) selectedCreature = tag;
			EditorDialogs::SetUnit(UnitDataOf(tag));
			if (inWorld) Say(name + " changed: level " + std::to_string(change.level) + ", " + std::to_string(change.points) + " hit points, gold " + std::to_string(change.gold));
		}
		void ApplyCreatureWindowChanges(std::uint8_t* world) {
			// The list first: a creature's own new type comes in it.
			ApplyCreatureListChanges();
			if (const auto unit = EditorDialogs::TakeUnitChange()) ApplyUnitChange(world, *unit);
		}
		void CycleItemVariant() {
			const auto tab = EditorPanel::CurrentTab();
			const auto* item = EditorPanel::ItemUnderMouse();
			if (item == nullptr) return;
			const auto value = item->value;
			if (value == ObjectsPageItem || value == WorkshopPageItem || value == ActionPointItem || value == DeadBodyItem || value == EffectGeneratorItem) return;
			if (tab == EditorPanel::Tab::Objects) {
				// Gold steps through amounts and a spell book through the spells; both kinds of gold share the amount.
				const auto type = static_cast<std::uint8_t>(value + 1);
				if (IsGoldObject(type)) {
					goldChoice = (goldChoice + 1) % GoldAmounts.size();
					EditorPanel::SetItemLook(tab, GoldObject - 1, ObjectItemName(GoldObject), 0);
					EditorPanel::SetItemLook(tab, GoldBagObject - 1, ObjectItemName(GoldBagObject), 0);
				}
				else if (type == SpellBookObject && !keeperSpellNames.empty()) {
					spellBookChoice = (spellBookChoice + 1) % keeperSpellNames.size();
					EditorPanel::SetItemLook(tab, value, ObjectItemName(type), 0);
				}
				else {
					Say(ObjectName(type) + " has nothing to change");
					return;
				}
				Say("Places " + ObjectItemName(type));
				return;
			}
			if (tab == EditorPanel::Tab::Workshop) {
				const bool door = value >= DoorItemBase;
				const auto base = door ? NameFrom(doorNames, static_cast<std::uint8_t>(value - DoorItemBase + 1)) : NameFrom(trapNames, static_cast<std::uint8_t>(value + 1));
				const bool blueprint = !blueprintItems[value];
				blueprintItems[value] = blueprint;
				EditorPanel::SetItemLook(tab, value, blueprint ? base + " (Blueprint)" : base, blueprint ? BlueprintMark : 0);
				Say(blueprint ? base + " places as a blueprint" : base + " places finished");
				return;
			}
			// Creatures choose their kind in their properties window (SetCreatureVariant).
		}
		// While the button is held, the box follows the tile under the mouse; letting go places it.
		void FollowActionPointDrag(std::uint8_t* world) {
			auto* playerInterface = Load<std::uint8_t*>(Load<std::uint8_t*>(world + Game::Offset::WorldSession) + Game::Offset::SessionInterface);
			if (playerInterface != nullptr && !MouseOverPalette() && !EditorPanel::MouseOver()) {
				const auto* underHand = playerInterface + Game::Offset::InterfaceUnderHand;
				actionPointEndX = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileX);
				actionPointEndY = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileY);
			}
			const auto box = DraggedBox();
			if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
				// Not Say: this changes every tick and would fill the log.
				switch (dragPurpose) {
				case DragPurpose::ActionPoint: statusText = "Action point " + BoxText(box) + ", let go to place"; break;
				case DragPurpose::PaintBox: statusText = TerrainName() + " over " + BoxText(box) + ", let go to paint"; break;
				case DragPurpose::Copy: statusText = "Copy " + BoxText(box) + ", let go to copy"; break;
				}
				return;
			}
			actionPointDragging = false;
			switch (dragPurpose) {
			case DragPurpose::ActionPoint: PlaceActionPoint(world); break;
			case DragPurpose::PaintBox: PaintBox(world, box); break;
			case DragPurpose::Copy: CopyBox(box); break;
			}
		}
		void StartDrag(DragPurpose purpose, std::int32_t x, std::int32_t y) {
			dragPurpose = purpose;
			actionPointDragging = true;
			actionPointStartX = actionPointEndX = x;
			actionPointStartY = actionPointEndY = y;
		}
		// ---- The trigger menu's pages.
		void EnterTriggerEdit() {
			triggerPageKind = TriggerPage::Edit;
			triggerPage = 0;
			if (document && document->triggers && editingTrigger < document->triggers->records.size()) triggerTitle = Widen(TriggerRecordText(document->triggers->records[editingTrigger]));
		}
		void EnterTriggerTree() {
			triggerPageKind = TriggerPage::Tree;
			triggerPage = 0;
			const auto owner = CurrentOwner();
			triggerTitle = owner ? Widen(owner->name) : L"Triggers";
		}
		// The value a field holds now, as typed text.
		std::string TriggerFieldText(std::size_t field) {
			const auto* bytes = document->triggers->records[editingTrigger].bytes.data();
			if (field == FieldValue) return std::to_string(Load<std::int32_t>(bytes + 4));
			if (field == FieldRepeat) return std::to_string(bytes[0xF]);
			return std::to_string(bytes[field]);
		}
		// True if the action was the trigger menu's, which then stays open.
		bool HandleTriggerMenu(MenuAction action, std::size_t index) {
			const bool haveTriggers = document && document->triggers;
			auto* record = haveTriggers && editingTrigger < document->triggers->records.size() ? &document->triggers->records[editingTrigger] : nullptr;
			switch (action) {
			case MenuAction::OpenTriggers:
			case MenuAction::TriggersBack:
				triggerPageKind = TriggerPage::Owners;
				triggerPage = 0;
				triggerTitle = L"Triggers: whose (click one)";
				return true;
			case MenuAction::PagePrevious:
				if (triggerPage > 0) --triggerPage;
				return true;
			case MenuAction::PageNext:
				++triggerPage;
				return true;
			case MenuAction::TriggerOwner: {
				const auto owners = TriggerOwners();
				if (index < owners.size()) shownOwner = owners[index];
				EnterTriggerTree();
				return true;
			}
			case MenuAction::ActionPointUses:
				triggerPageKind = TriggerPage::Uses;
				triggerPage = 0;
				triggerTitle = L"Using action point " + std::to_wstring(menuActionPointId);
				triggerRows = ActionPointUses(menuActionPointId);
				return true;
			case MenuAction::StartTree:
				if (const auto owner = CurrentOwner(); owner && haveTriggers) {
					if (const auto place = StartTree(*owner)) {
						editingTrigger = *place;
						EnterTriggerEdit();
						Say("Started " + owner->name + "'s triggers");
					}
				}
				return true;
			case MenuAction::EditTrigger:
				editingTrigger = index;
				EnterTriggerEdit();
				return true;
			case MenuAction::BackToTree:
				EnterTriggerTree();
				return true;
			case MenuAction::ChooseType:
				triggerPageKind = TriggerPage::Types;
				triggerPage = 0;
				triggerTitle = L"Choose its type";
				return true;
			case MenuAction::SetType:
				if (record != nullptr) {
					record->bytes[TriggerTypeAt] = static_cast<std::uint8_t>(index);
					TriggersChanged();
				}
				EnterTriggerEdit();
				return true;
			case MenuAction::CycleCompare:
				if (record != nullptr) {
					record->bytes[0] = static_cast<std::uint8_t>(record->bytes[0] % 6 + 1);
					TriggersChanged();
				}
				EnterTriggerEdit();
				return true;
			case MenuAction::ToggleCompareWith:
				if (record != nullptr) {
					record->bytes[2] ^= 1;
					TriggersChanged();
				}
				EnterTriggerEdit();
				return true;
			case MenuAction::TriggerField:
				if (record != nullptr) {
					naming = true;
					prompt = Prompt::TriggerField;
					promptIndex = index;
					nameText = TriggerFieldText(index);
					statusText.clear();
				}
				return true;
			case MenuAction::AddAfter:
				if (record != nullptr) {
					editingTrigger = AddAfter(editingTrigger);
					EnterTriggerEdit();
				}
				return true;
			case MenuAction::AddTriggerUnder:
			case MenuAction::AddActionUnder:
				if (record != nullptr) {
					if (const auto place = AddUnder(editingTrigger, action == MenuAction::AddActionUnder)) {
						editingTrigger = *place;
						EnterTriggerEdit();
					}
					else Say("A trigger holds either triggers or actions under it, not both");
				}
				return true;
			case MenuAction::DeleteTrigger:
				if (record != nullptr) {
					DeleteTriggerRecord(editingTrigger);
					Say("Deleted it, and all that was under it");
				}
				EnterTriggerTree();
				return true;
			case MenuAction::OpenParties:
				triggerPageKind = TriggerPage::Parties;
				triggerPage = 0;
				triggerTitle = L"Hero parties (click one)";
				return true;
			case MenuAction::ShowParty:
				shownParty = index;
				triggerPageKind = TriggerPage::Party;
				triggerPage = 0;
				if (document && document->things && index < document->things->records.size()) triggerTitle = Widen(PartyText(document->things->records[index]));
				return true;
			case MenuAction::NewParty:
				MakeParty();
				return true;
			case MenuAction::AddMember:
			case MenuAction::RemoveMember:
			case MenuAction::MemberLevel:
				if (action == MenuAction::AddMember) AddPartyMember(shownParty);
				else if (action == MenuAction::RemoveMember) RemovePartyMember(shownParty);
				else StepMemberLevel(shownParty, index);
				if (document && document->things && shownParty < document->things->records.size()) triggerTitle = Widen(PartyText(document->things->records[shownParty]));
				return true;
			case MenuAction::DeleteParty:
				DeleteParty(shownParty);
				triggerPageKind = TriggerPage::Parties;
				triggerPage = 0;
				triggerTitle = L"Hero parties (click one)";
				return true;
			default:
				return false;
			}
		}

		// Clicks on the palette and the panel are taken before the game's own, which would give them to whatever is there.
		using MouseEventFn = std::int32_t(__cdecl*)(std::int32_t button, std::uint32_t pressed, std::int32_t x, std::int32_t y, std::uint8_t* playerInterface);
		MouseEventFn originalMouseEvent = nullptr;
		std::int32_t __cdecl MouseEventDetour(std::int32_t button, std::uint32_t pressed, std::int32_t x, std::int32_t y, std::uint8_t* playerInterface) {
			constexpr std::int32_t LeftButton = 0xF0;
			constexpr std::int32_t RightButton = 0xF1;
			// While testing only the palette's Back to editing is ours.
			if (testing && Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) == 0 && MouseOverPalette()) {
				if (button == LeftButton && pressed != 0) ClickPalette(playerInterface);
				return 1;
			}
			if (!inSession || Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) != 0) return originalMouseEvent(button, pressed, x, y, playerInterface);
			// An open menu takes the next press, wherever it is: on one of its options that is chosen, and any press closes it.
			if (menuOpen && (button == LeftButton || button == RightButton)) {
				if (pressed != 0) {
					const auto rowAt = MenuRowAtMouse();
					const auto action = button == LeftButton && rowAt >= 0 ? menuRows[static_cast<std::size_t>(rowAt)].action : MenuAction::None;
					if (action == MenuAction::ToggleLevels) {
						// Stays open, with the levels shown or hidden under this row.
						menuLevelsOpen = !menuLevelsOpen;
						BuildMenuRows();
						return 1;
					}
					const auto rowIndex = rowAt >= 0 ? menuRows[static_cast<std::size_t>(rowAt)].index : std::size_t{ 0 };
					// These stay open too, showing the change.
					if (action == MenuAction::ToggleAvailability) {
						SetAvailabilityMode(!availabilityMode);
						BuildMenuRows();
						return 1;
					}
					if (action == MenuAction::PlayerComputer) {
						TogglePlayerComputer(rowIndex);
						BuildMenuRows();
						return 1;
					}
					// The triggers and parties: each of these keeps the menu open on its new rows.
					if (HandleTriggerMenu(action, rowIndex)) {
						menuIsTriggers = true;
						menuIsLevel = false;
						menuIsActionPoint = false;
						BuildMenuRows();
						return 1;
					}
					if (action == MenuAction::RenameActionPoint) {
						naming = true;
						prompt = Prompt::ActionPointName;
						promptIndex = menuActionPointId;
						nameText.clear();
						if (const auto place = FindActionPoint(menuActionPointId)) {
							if (const auto box = ReadActionPoint(document->things->records[*place])) nameText = box->name;
						}
						statusText.clear();
					}
					if (action == MenuAction::LevelText && document && document->level) {
						naming = true;
						prompt = Prompt::LevelText;
						promptIndex = rowIndex;
						nameText.clear();
						for (const auto c : LevelFiles::GetLevelText(*document->level, LevelTextRows[rowIndex].first)) nameText += c < 0x100 ? static_cast<char>(c) : '?';
						statusText.clear();
					}
					if (action == MenuAction::PlayerGold && document && document->players && rowIndex < document->players->records.size()) {
						naming = true;
						prompt = Prompt::Gold;
						promptIndex = rowIndex;
						nameText = std::to_string(Load<std::uint32_t>(document->players->records[rowIndex].data() + PlayerGoldOffset));
						statusText.clear();
					}
					if (action == MenuAction::Move) {
						if (tool != Tool::Move) toolBeforeMove = tool;
						tool = Tool::Move;
						moveTag = menuIsActionPoint ? std::uint16_t{ 0 } : menuTag;
						moveActionPoint = menuIsActionPoint ? menuActionPointId : std::uint8_t{ 0 };
						Say("Click the tile to move it to" + std::string(menuIsActionPoint ? " (its top left)" : "") + "; Esc stops");
					}
					if (action == MenuAction::Delete && menuIsListed) DeleteListedOnly(menuListedPlace);
					else if (action == MenuAction::Delete && menuIsActionPoint) actionPointDeleteWanted = menuActionPointId;
					else if (action == MenuAction::Delete && menuIsRoom) {
						roomSaleWanted = true;
						roomSaleX = menuTileX;
						roomSaleY = menuTileY;
					}
					else if (action == MenuAction::Delete) deleteWanted = menuTag;
					if (action == MenuAction::PickTile) PickTile(menuTileX, menuTileY);
					if (action == MenuAction::SetLevel && menuRows[static_cast<std::size_t>(rowAt)].level != menuLevel) {
						levelChangeTag = menuTag;
						levelChangeTo = menuRows[static_cast<std::size_t>(rowAt)].level;
					}
					menuOpen = false;
				}
				return 1;
			}
			// The heart on the top bar chooses the owner. Layout units, around its ring.
			if (button == LeftButton || button == RightButton) {
				const auto heartTopLeft = LayoutToScreen(playerInterface, 8, 8);
				const auto heartBottomRight = LayoutToScreen(playerInterface, 140, 160);
				const auto mouse = MousePosition();
				if (mouse.x >= heartTopLeft.x && mouse.x < heartBottomRight.x && mouse.y >= heartTopLeft.y && mouse.y < heartBottomRight.y) {
					if (pressed != 0) StepOwner(button == LeftButton ? 1 : -1);
					return 1;
				}
			}
			if (MouseOverPalette() || EditorPanel::MouseOver()) {
				if (button == RightButton && pressed != 0 && !MouseOverPalette() && !availabilityMode) {
					// On the Creatures tab a right click opens the creature type's properties, where its kind (Unique, mercenary) is
					// chosen too; on the other tabs it steps the item's variant.
					const auto* item = EditorPanel::ItemUnderMouse();
					if (EditorPanel::CurrentTab() == EditorPanel::Tab::Creatures) {
						if (item != nullptr) OpenCreatureWindow(static_cast<std::uint8_t>(item->value + 1));
					}
					else CycleItemVariant();
				}
				if (button == LeftButton && pressed != 0) {
					if (!ClickPalette(playerInterface)) {
						const auto tab = EditorPanel::CurrentTab();
						if (const auto* item = EditorPanel::Click()) {
							// Choosing anything on the panel stops a move under way.
							StopMoving();
							if (availabilityMode && ClickAvailability(tab, *item)) {}
							else if (tab == EditorPanel::Tab::Rooms || tab == EditorPanel::Tab::Terrain) {
								terrainChoice = item->value;
								tool = Tool::Terrain;
								LogChoice();
								statusText = "Paint with " + item->name;
							}
							else if (item->value == ObjectsPageItem) {
								EditorPanel::SetTab(EditorPanel::Tab::Objects);
								Say("Objects and action points; the first item goes back");
							}
							else if (item->value == WorkshopPageItem) {
								EditorPanel::SetTab(EditorPanel::Tab::Workshop);
								statusText.clear();
							}
							else if (item->value == ActionPointItem) {
								tool = Tool::ActionPoint;
								Say("Press on a tile and drag to another: an action point covers the box between");
							}
							else if (item->value == DeadBodyItem) {
								tool = Tool::DeadBody;
								Say("Left click places a dead " + CreatureName(creatureChoice) + "; choose another on the Creatures tab, then come back here");
							}
							else if (item->value == EffectGeneratorItem) {
								tool = Tool::EffectGenerator;
								Say("Left click places an effect generator, set as the one in Level17; its menu shows its effects");
							}
							else if (tab == EditorPanel::Tab::Objects) {
								objectChoice = static_cast<std::uint8_t>(item->value + 1);
								tool = Tool::Object;
								Say("Left click places " + item->name + (IsGoldObject(objectChoice) || objectChoice == SpellBookObject ? "; right click it here to change" : ""));
							}
							else if (tab == EditorPanel::Tab::Creatures) {
								// Ids run in the file's order, from 1.
								creatureChoice = static_cast<std::uint8_t>(item->value + 1);
								tool = Tool::Creature;
								Say("Left click places " + CreatureName(creatureChoice) + "; right click here for its properties");
							}
							else if (item->value >= DoorItemBase) {
								doorChoice = static_cast<std::uint8_t>(item->value - DoorItemBase + 1);
								tool = Tool::Door;
								Say("Left click places " + item->name);
							}
							else {
								trapChoice = static_cast<std::uint8_t>(item->value + 1);
								tool = Tool::Trap;
								Say("Left click places " + item->name);
							}
						}
					}
				}
				return 1;
			}
			return originalMouseEvent(button, pressed, x, y, playerInterface);
		}

		// What a right click or the Delete key is about, in this order: a creature (pointed at, or standing on the tile; one
		// standing on a trap comes first, delete it to reach what is under it), a trap or door, an object of the level's own, the
		// smallest action point the tile is in, a room's tile. None of them for anything else.
		struct Pointed {
			std::uint16_t tag = 0;
			// Places in the level's list and in roomEntries.
			std::optional<std::size_t> actionPoint;
			std::optional<std::size_t> room;
			// A dead body or effect generator of the level's list.
			std::optional<std::size_t> listed;
		};
		Pointed FindPointed(const std::uint8_t* underHand, std::int32_t x, std::int32_t y) {
			Pointed found;
			const auto* tile = DocumentTile(x, y);
			if (tile == nullptr) return found;
			if (creaturesUsable) {
				constexpr std::int32_t OurCreature = 2;
				constexpr std::int32_t OtherCreature = 3;
				const auto kind = Load<std::int32_t>(underHand + Game::Offset::UnderHandKind);
				std::uint16_t tag = kind == OurCreature || kind == OtherCreature ? Load<std::uint16_t>(underHand + Game::Offset::UnderHandTag) & Game::TagMask : std::uint16_t{ 0 };
				if (!IsCreature(tag)) tag = CreatureOnTile(x, y);
				if (tag == 0) tag = TrapOrDoorOnTile(x, y);
				if (tag != 0) {
					found.tag = tag;
					return found;
				}
			}
			if (objectsUsable) {
				if (const auto tag = ListedObjectOnTile(x, y); tag != 0) {
					found.tag = tag;
					return found;
				}
			}
			if (const auto place = ListedOnlyAt(x, y)) {
				found.listed = place;
				return found;
			}
			if (const auto place = ActionPointAt(x, y)) {
				found.actionPoint = place;
				return found;
			}
			for (std::size_t i = 0; i < roomEntries.size(); ++i) {
				if (terrains[roomEntries[i].terrain].id == tile->terrain) {
					found.room = i;
					return found;
				}
			}
			return found;
		}

		// ---- The chosen creature: a left press on it chooses it and starts a drag; while the button is held it follows the pointer,
		// made again on each tile it can stand on (a tile it cannot is outlined, and it waits on the last good one). Letting go over a
		// good tile moves its record there (one undo step); over one it cannot stand on, it goes back to where it was. Delete deletes
		// it, Esc lets go of it (and, mid drag, puts it back).
		void SelectCreature(std::uint16_t tag) {
			selectedCreature = tag;
			const auto* creature = ThingOfTag(tag);
			dragFromX = dragToX = dragShownX = CreatureTileX(creature);
			dragFromY = dragToY = dragShownY = CreatureTileY(creature);
			dragPoints = Load<std::int32_t>(creature + Game::Offset::CreatureHealth);
			dragRecord.reset();
			if (const auto place = FindCreatureRecord(creature)) dragRecord = document->things->records[*place];
			dragCancelWanted = false;
			dragWindowFollows = EditorDialogs::UnitId() == tag;
			creatureDragging = true;
			// The press must not go on to paint.
			leftDown = false;
			Say("Chose " + CreatureDisplayName(creature) + ": drag to move it, Delete deletes it, right click for its properties");
		}
		LevelFiles::ThingRecord DragRecordAt(std::int32_t x, std::int32_t y) {
			auto record = *dragRecord;
			Store<std::int32_t>(record.bytes.data(), x);
			Store<std::int32_t>(record.bytes.data() + 4, y);
			return record;
		}
		// Makes the dragged creature from its record at this tile (it is not in the world just now), with the hit points it had.
		bool MakeDraggedAt(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			const auto record = DragRecordAt(x, y);
			// The creatures already on the tile, so the new one is told from a like one standing there.
			std::vector<std::uint16_t> there;
			for (std::uint16_t tag = 1; tag <= Game::TagMask; ++tag) {
				if (IsCreature(tag) && CreatureTileX(ThingOfTag(tag)) == x && CreatureTileY(ThingOfTag(tag)) == y) there.push_back(tag);
			}
			if (!MakeCreature(world, record)) return false;
			selectedCreature = 0;
			for (std::uint16_t tag = 1; tag <= Game::TagMask && selectedCreature == 0; ++tag) {
				if (IsCreature(tag) && CreatureTileX(ThingOfTag(tag)) == x && CreatureTileY(ThingOfTag(tag)) == y && std::find(there.begin(), there.end(), tag) == there.end()) selectedCreature = tag;
			}
			if (selectedCreature == 0) selectedCreature = ThingOfRecord(record);
			if (selectedCreature != 0) Store<std::int32_t>(ThingOfTag(selectedCreature) + Game::Offset::CreatureHealth, dragPoints);
			dragShownX = x;
			dragShownY = y;
			return true;
		}
		// Moves the shown creature to this tile, or leaves it where it is shown if it cannot be made there.
		bool ShowDraggedAt(std::uint8_t* world, std::int32_t x, std::int32_t y) {
			if (!dragRecord || (x == dragShownX && y == dragShownY)) return false;
			if (!PlacingProblem(world, DragRecordAt(x, y)).empty()) return false;
			if (!RemoveCreature(world, selectedCreature)) return false;
			if (MakeDraggedAt(world, x, y)) return true;
			MakeDraggedAt(world, dragShownX, dragShownY);
			return false;
		}
		// Back on the tile its record has, and the drag over.
		// The creature window, if it was on the dragged creature, on it again as it is now (its record moved or not).
		void WindowFollowsDrag() {
			if (!std::exchange(dragWindowFollows, false)) return;
			EditorDialogs::RefreshUnit(IsCreature(selectedCreature) ? UnitDataOf(selectedCreature) : std::nullopt);
		}
		void PutDraggedBack(std::uint8_t* world) {
			creatureDragging = false;
			if (dragRecord && (dragShownX != dragFromX || dragShownY != dragFromY) && RemoveCreature(world, selectedCreature)) MakeDraggedAt(world, dragFromX, dragFromY);
			WindowFollowsDrag();
		}
		void FollowCreatureDrag(std::uint8_t* world) {
			if (!IsCreature(selectedCreature)) {
				creatureDragging = false;
				selectedCreature = 0;
				WindowFollowsDrag();
				return;
			}
			if (dragCancelWanted) {
				dragCancelWanted = false;
				PutDraggedBack(world);
				selectedCreature = 0;
				Say("Let go of the creature");
				return;
			}
			if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
				auto* playerInterface = Load<std::uint8_t*>(Load<std::uint8_t*>(world + Game::Offset::WorldSession) + Game::Offset::SessionInterface);
				if (playerInterface != nullptr && !MouseOverPalette() && !EditorPanel::MouseOver()) {
					const auto* underHand = playerInterface + Game::Offset::InterfaceUnderHand;
					dragToX = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileX);
					dragToY = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileY);
				}
				if (dragToX == dragShownX && dragToY == dragShownY) {
					if (dragToX != dragFromX || dragToY != dragFromY) statusText = "Moving to " + std::to_string(dragToX) + ", " + std::to_string(dragToY) + ", let go to put it there";
					return;
				}
				if (!dragRecord) statusText = "It is not in the level's list, so it cannot be moved";
				else if (ShowDraggedAt(world, dragToX, dragToY)) statusText = "Moving to " + std::to_string(dragToX) + ", " + std::to_string(dragToY) + ", let go to put it there";
				else {
					const auto problem = DocumentTile(dragToX, dragToY) == nullptr ? std::string("That is off the map") : PlacingProblem(world, DragRecordAt(dragToX, dragToY));
					statusText = (problem.empty() ? std::string("It cannot be made there") : problem) + ": let go here and it goes back";
				}
				return;
			}
			creatureDragging = false;
			if (!dragRecord || (dragToX == dragFromX && dragToY == dragFromY)) {
				PutDraggedBack(world);
				return;
			}
			const auto name = CreatureDisplayName(ThingOfTag(selectedCreature));
			// Let go over a tile it could not be shown on: back where it was.
			if (dragToX != dragShownX || dragToY != dragShownY) {
				PutDraggedBack(world);
				Say(name + " cannot stand there, so it went back");
				return;
			}
			// Its record, found as it was (the list may have changed under the drag); then moved there, as Move does.
			auto& records = document->things->records;
			const auto found = std::find_if(records.begin(), records.end(), [&](const LevelFiles::ThingRecord& record) { return record.kind == dragRecord->kind && record.bytes == dragRecord->bytes; });
			if (found == records.end()) {
				PutDraggedBack(world);
				Say("It is not in the level's list any more, so it went back");
				return;
			}
			const auto place = static_cast<std::size_t>(found - records.begin());
			const auto before = *found;
			const auto after = DragRecordAt(dragToX, dragToY);
			EndStroke(world);
			records.erase(found);
			records.push_back(after);
			strokeThings.push_back({ CreatureChangeKind::Deleted, place, before });
			strokeThings.push_back({ CreatureChangeKind::Placed, records.size() - 1, after });
			// Its exact hit points move with it, in the same undo step.
			if (const auto from = ReadCreatureRecord(before), to = ReadCreatureRecord(after); from && to) MoveHealthEntry(*from, *to);
			EndStroke(world);
			unsavedChanges = true;
			dragRecord = after;
			dragFromX = dragShownX;
			dragFromY = dragShownY;
			WindowFollowsDrag();
			Say("Moved " + name + " to " + std::to_string(dragToX) + ", " + std::to_string(dragToY));
		}

		// The world's clicks. Clicks on panels and menus never get here. Nothing else of the game's click handling runs in a
		// session, as these are what starts digging, building and picking things up.
		using WorldClickFn = std::int32_t(__fastcall*)(std::uint8_t* playerInterface, void* unused, std::int32_t pressed, const std::uint8_t* underHand);
		WorldClickFn originalWorldLeftClick = nullptr;
		WorldClickFn originalWorldRightClick = nullptr;
		std::int32_t __fastcall WorldLeftClickDetour(std::uint8_t* playerInterface, void* unused, std::int32_t pressed, const std::uint8_t* underHand) {
			if (!inSession) return originalWorldLeftClick(playerInterface, unused, pressed, underHand);
			// The options menu pauses nothing here, so a click beside its buttons still arrives.
			if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) != 0) {
				leftDown = false;
				return 0;
			}
			leftDown = pressed != 0;
			if (pressed != 0) {
				clickWaiting = true;
				clickX = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileX);
				clickY = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileY);
				// A press on a creature chooses it (and may drag it) rather than painting or placing there.
				const auto pointed = FindPointed(underHand, clickX, clickY);
				clickCreature = pointed.tag != 0 && IsCreature(pointed.tag) ? pointed.tag : std::uint16_t{ 0 };
			}
			return 0;
		}
		std::int32_t __fastcall WorldRightClickDetour(std::uint8_t* playerInterface, void* unused, std::int32_t pressed, const std::uint8_t* underHand) {
			if (!inSession) return originalWorldRightClick(playerInterface, unused, pressed, underHand);
			if (pressed == 0 || Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) != 0 || MouseOverPalette()) return 0;
			const auto tileX = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileX);
			const auto tileY = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileY);
			const auto* tile = DocumentTile(tileX, tileY);
			if (tile == nullptr) return 0;
			// Right click puts down nothing being moved.
			StopMoving();
			const auto pointed = FindPointed(underHand, tileX, tileY);
			// A creature opens the creature window on itself; traps, doors and objects keep their menu.
			if (pointed.tag != 0 && IsCreature(pointed.tag)) OpenCreatureWindow(0, pointed.tag);
			else if (pointed.tag != 0) OpenCreatureMenu(playerInterface, pointed.tag);
			else if (pointed.listed) OpenListedMenu(playerInterface, *pointed.listed);
			else if (pointed.actionPoint) OpenActionPointMenu(playerInterface, *ReadActionPoint(document->things->records[*pointed.actionPoint]));
			else if (pointed.room) {
				const auto& room = roomEntries[*pointed.room];
				OpenRoomMenu(playerInterface, tileX, tileY, terrains[room.terrain].name, tile->owner);
			}
			// Anything else is picked up straight away.
			else PickTile(tileX, tileY);
			return 0;
		}
		// The Delete key: what a right click would give a menu for, deleted at once.
		void DeleteAtPointer(std::uint8_t* world) {
			auto* playerInterface = Load<std::uint8_t*>(Load<std::uint8_t*>(world + Game::Offset::WorldSession) + Game::Offset::SessionInterface);
			if (playerInterface == nullptr || MouseOverPalette() || EditorPanel::MouseOver() || !DocumentReady()) return;
			const auto* underHand = playerInterface + Game::Offset::InterfaceUnderHand;
			const auto x = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileX);
			const auto y = Load<std::int32_t>(underHand + Game::Offset::UnderHandTileY);
			const auto* tile = DocumentTile(x, y);
			if (tile == nullptr) return;
			const auto pointed = FindPointed(underHand, x, y);
			if (pointed.tag != 0) DeleteCreature(world, pointed.tag);
			else if (pointed.listed) DeleteListedOnly(*pointed.listed);
			else if (pointed.actionPoint) {
				if (const auto box = ReadActionPoint(document->things->records[*pointed.actionPoint])) DeleteActionPoint(world, box->id);
			}
			else if (pointed.room) {
				if (IsListedRoomTerrain(tile->terrain)) Say(ListedRoomMessage);
				else SellRoomTile(world, x, y);
			}
			else Say("Nothing here to delete");
		}

		bool Hook(std::uintptr_t target, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
	}

	bool LevelEditor::InSession() { return inSession; }

	bool LevelEditor::OnGameComponentStart(bool playing) {
		StopOsPointer();
		EditorDialogs::CloseAll();
		// Leaving a level comes back to the skirmish screen without going through the Editor button, so what is started
		// from there is still a level to edit. Reopening it there used to start an ordinary game on the stock map.
		if (inSession && !playing) armed = true;
		inSession = false;
		revealPending = false;
		heldSession = nullptr;
		turnHeld = false;
		revealAgainIn = 0;
		mercenaryRecheckIn = 0;
		// Leaving a level while testing it leaves the editor too, back to the skirmish screen as from editing.
		if (testing && !playing) armed = true;
		testing = false;
		testWanted = stopTestWanted = saveAsWanted = naming = testMercenariesPending = newMapWanted = newMapWarned = false;
		// A level played for real has its mercenary marks applied on its first tick. A saved game holds them already.
		playLevelPending = playing && Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameConfigHasSaveFile)) == 0;
		playRecheckIn = 0;
		if (!playing || !armed) return false;
		armed = false;
		const auto mode = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode));
		const auto fromSave = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameConfigHasSaveFile)) != 0;
		// Editor opens the skirmish screen. Left that way, any other kind of game is an ordinary one.
		if (mode != Game::PlayModeSkirmish || fromSave) return false;
		inSession = true;
		playLevelPending = false;
		revealPending = revealUsable;
		droppedLogged.fill(false);
		*reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::LocalPlayerNumber)) = Game::FirstKeeperPlayer;
		auto* level = reinterpret_cast<wchar_t*>(Game::Resolve(Game::Address::GameConfigLevelName));
		const std::wstring chosen(level, wcsnlen(level, Game::LevelNameLength));
		std::string name;
		for (const auto c : chosen) name += c < 0x80 ? static_cast<char>(c) : '?';
		// The copy in KeeperForge2\maps if it has one, as that is the one being worked on.
		bool saved = false;
		if (LevelFiles::HasSavedCopy(chosen)) {
			const auto path = LevelFiles::SavedLevelName(chosen);
			if (path.size() < Game::LevelNameLength) {
				std::wcsncpy(level, path.c_str(), Game::LevelNameLength);
				saved = true;
			}
			else {
				Logger::Warning("Editor: the name of level " + name + "'s saved copy is too long, opening the original.");
			}
		}
		Logger::Info("Editor: opening " + std::string(saved ? "the saved copy of " : "") + "level " + name + " with the simulation held.");
		LoadDocument(chosen);
		return true;
	}

	const std::uint8_t* LevelEditor::BeforeWorldTick(std::uint8_t* world, const std::uint8_t* actions) {
		heldSession = Load<std::uint8_t*>(world + Game::Offset::WorldSession);
		savedMenuOpen = Load<std::int32_t>(heldSession + Game::Offset::SessionMenuOpen);

		const std::size_t count = actions[0];
		std::memcpy(keptActions.data(), actions, ActionListHeader);
		std::size_t kept = 0;
		bool leaveHeldThisTick = false;
		for (std::size_t i = 0; i < count; ++i) {
			const auto* action = actions + ActionListHeader + i * ActionSize;
			const auto kind = Load<std::int32_t>(action + Game::Offset::ActionKind);
			// Leaving with changes not saved is held back once, with a warning (every leave in this tick). Leaving again goes.
			const bool leave = kind == ActionLeaveLevel || kind == ActionLeaveLevelOther;
			if (leave && unsavedChanges && (!leaveWarned || leaveHeldThisTick)) {
				if (!leaveWarned) Say("Changes not saved! Save first, or quit again to leave them");
				leaveWarned = true;
				leaveHeldThisTick = true;
				continue;
			}
			if (KeepAction(kind)) {
				std::memcpy(keptActions.data() + ActionListHeader + kept * ActionSize, action, ActionSize);
				++kept;
			}
			else if (kind >= 0 && static_cast<std::size_t>(kind) < droppedLogged.size() && !droppedLogged[static_cast<std::size_t>(kind)]) {
				droppedLogged[static_cast<std::size_t>(kind)] = true;
				Logger::Debug("Editor: ignoring game action " + std::to_string(kind) + ".");
			}
		}
		keptActions[0] = static_cast<std::uint8_t>(kept);

		// With this set the tick carries out the actions and stops before the simulation, as it does with a menu open.
		// It is only set during the tick, as the player interface takes it to mean a menu is open and stops its input.
		Store<std::int32_t>(heldSession + Game::Offset::SessionMenuOpen, 1);
		// The session counts a turn after the tick, so the turn is put back here and drawing always sees the same one.
		if (!turnHeld) {
			turnHeld = true;
			heldTurn = Load<std::uint32_t>(heldSession + Game::Offset::SessionGameTurn);
		}
		Store<std::uint32_t>(heldSession + Game::Offset::SessionGameTurn, heldTurn);
		return keptActions.data();
	}

	void LevelEditor::AfterWorldTick(std::uint8_t* world) {
		if (heldSession != nullptr) Store<std::int32_t>(heldSession + Game::Offset::SessionMenuOpen, savedMenuOpen);
		heldSession = nullptr;
		ApplyCreatureWindowChanges(world);
		if (revealPending) {
			// Level setup treats this level like a campaign one, so nothing else lifts the fog. The first tick is the
			// first place after level setup that we run.
			revealPending = false;
			using RevealFn = void(__fastcall*)(std::uint8_t* map);
			GameFunction<RevealFn>(Game::Address::MapRevealAll)(world + Game::Offset::WorldMap);
			Logger::Debug("Editor: map revealed.");
			FinishHeartFadeIn();
			revealAgainIn = RevealAgainAfterTicks;
		}
		if (revealAgainIn != 0 && --revealAgainIn == 0) RevealMap(world);
		if (mercenaryRecheckIn != 0 && --mercenaryRecheckIn == 0) RecheckMercenaries();
		// The minimap only draws when the turn differs from the one it last drew for, and the turn is held here. So it could stay as
		// it was, black when a session's held turn happened to be the one the last session stopped on. Once a tick is its normal rate.
		*reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::MinimapLastTurn)) = 0xFFFFFFFFu;
		if (!documentChecked) CheckDocument(world);
		if (clickWaiting) {
			clickWaiting = false;
			const auto pressedOn = std::exchange(clickCreature, std::uint16_t{ 0 });
			if (tool != Tool::Move && tool != Tool::ActionPoint && pressedOn != 0 && IsCreature(pressedOn)) SelectCreature(pressedOn);
			else {
				// Anywhere else lets go of the chosen creature, and does what a press there does.
				selectedCreature = 0;
				if (tool == Tool::Move) MoveThingTo(world, clickX, clickY);
				else if (tool == Tool::ActionPoint) StartDrag(DragPurpose::ActionPoint, clickX, clickY);
				else if (tool == Tool::DeadBody || tool == Tool::EffectGenerator) PlaceListedOnly(world, clickX, clickY);
				else if (tool != Tool::Terrain) PlaceCreature(world, clickX, clickY);
				else {
					switch (paintMode) {
					case PaintMode::Brush: PaintChosen(world, clickX, clickY); break;
					case PaintMode::Box: StartDrag(DragPurpose::PaintBox, clickX, clickY); break;
					case PaintMode::Fill: FloodFill(world, clickX, clickY); break;
					case PaintMode::Copy: StartDrag(DragPurpose::Copy, clickX, clickY); break;
					case PaintMode::Paste: PasteAt(world, clickX, clickY); break;
					}
				}
			}
		}
		if (creatureDragging) FollowCreatureDrag(world);
		// The creature window's title marks its creature when it is the chosen (outlined) one.
		EditorDialogs::SetUnitSelected((creatureDragging && dragWindowFollows) || (selectedCreature != 0 && EditorDialogs::UnitId() == selectedCreature));
		if (deleteAtPointerWanted) {
			deleteAtPointerWanted = false;
			EndStroke(world);
			// The chosen creature, if there is one; else what is under the pointer, as before.
			if (selectedCreature != 0 && IsCreature(selectedCreature)) {
				// Mid drag it is back on its own tile first, where its record is.
				if (creatureDragging) PutDraggedBack(world);
				const auto tag = std::exchange(selectedCreature, std::uint16_t{ 0 });
				creatureDragging = false;
				if (IsCreature(tag)) DeleteCreature(world, tag);
			}
			else DeleteAtPointer(world);
		}
		if (actionPointDragging) FollowActionPointDrag(world);
		if (actionPointDeleteWanted != 0) {
			const auto id = actionPointDeleteWanted;
			actionPointDeleteWanted = 0;
			DeleteActionPoint(world, id);
		}
		if (deleteWanted != 0) {
			const auto tag = deleteWanted;
			deleteWanted = 0;
			DeleteCreature(world, tag);
		}
		if (roomSaleWanted) {
			roomSaleWanted = false;
			SellRoomTile(world, roomSaleX, roomSaleY);
		}
		if (levelChangeTag != 0) {
			const auto tag = levelChangeTag;
			levelChangeTag = 0;
			ChangeCreatureLevel(world, tag, levelChangeTo);
		}
		// Windows' own state, as a release over a panel never reaches the world's click. A creature goes down once a click.
		if (tool == Tool::Terrain && paintMode == PaintMode::Brush && leftDown && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
			auto* playerInterface = Load<std::uint8_t*>(Load<std::uint8_t*>(world + Game::Offset::WorldSession) + Game::Offset::SessionInterface);
			if (playerInterface != nullptr && !MouseOverPalette() && !EditorPanel::MouseOver()) {
				const auto* underHand = playerInterface + Game::Offset::InterfaceUnderHand;
				PaintChosen(world, Load<std::int32_t>(underHand + Game::Offset::UnderHandTileX), Load<std::int32_t>(underHand + Game::Offset::UnderHandTileY));
			}
		}
		else {
			leftDown = false;
			EndStroke(world);
		}
		if (undoWanted) {
			undoWanted = false;
			menuOpen = false;
			EndStroke(world);
			UndoStroke(world);
		}
		if (redoWanted) {
			redoWanted = false;
			menuOpen = false;
			EndStroke(world);
			RedoStroke(world);
		}
		if (saveWanted) {
			saveWanted = false;
			SaveDocument();
		}
		if (saveAsWanted) {
			saveAsWanted = false;
			EndStroke(world);
			SaveDocumentAs(nameText);
		}
		if (newMapWanted) {
			newMapWanted = false;
			EndStroke(world);
			menuOpen = false;
			MakeNewMap(world);
			return;
		}
		if (testWanted) {
			testWanted = false;
			// From the files, so it is saved first, and it plays as a normal game of this level would.
			EndStroke(world);
			SaveDocument();
			if (unsavedChanges) return;
			StopOsPointer();
			// Changes given back are saved by now; ones still in the window would not reach a level that is playing.
			EditorDialogs::CloseAll();
			menuOpen = false;
			naming = false;
			inSession = false;
			testing = true;
			Logger::Info("Editor: testing the level.");
			if (!ReloadLevel(world)) {
				testing = false;
				inSession = true;
				turnHeld = false;
				Say("The level could not be loaded to test it");
			}
			else testMercenariesPending = true;
		}
		RefreshOutlines(world);
	}

	bool LevelEditor::Testing() { return testing; }

	void LevelEditor::AfterPlayTick() {
		if (playLevelPending) {
			playLevelPending = false;
			playHealth.clear();
			ReadPlayMercenaries();
			ApplyCreatureHealth(playHealth);
			if (!playMercenaries.empty()) {
				EnsureMercenaries(playMercenaries);
				playRecheckIn = MercenaryRecheckAfterTicks;
			}
		}
		// Once more a few ticks on, as the first try did not last in the editor (docs/game/level-editor.md).
		if (playRecheckIn != 0 && --playRecheckIn == 0) EnsureMercenaries(playMercenaries);
	}

	void LevelEditor::AfterTestTick(std::uint8_t* world) {
		if (testMercenariesPending) {
			// The first tick after the load: nothing has walked off its tile yet.
			testMercenariesPending = false;
			if (document && document->things) {
				for (const auto& record : document->things->records) ApplyRecordMercenary(record);
				RecheckMercenaries();
				ApplyCreatureHealth(document->creatureHealth);
			}
		}
		if (!stopTestWanted) return;
		stopTestWanted = false;
		// Back to editing what was saved: the level is loaded again and held, as when it was opened.
		testing = false;
		inSession = true;
		revealPending = revealUsable;
		heldSession = nullptr;
		turnHeld = false;
		droppedLogged.fill(false);
		Logger::Info("Editor: back to editing the level.");
		if (!ReloadLevel(world)) Logger::Warning("Editor: the level could not be loaded again; leave it and open it from the menu.");
		LoadDocument(documentLevel);
	}

	bool LevelEditor::OnTabClick(std::int32_t button, std::int32_t panel, std::uint8_t* playerInterface) {
		if (!inSession) return false;
		using OpenPanelFn = void(__cdecl*)(std::int32_t, std::int32_t, std::uint8_t*);
		if (playerInterface[Game::Offset::InterfacePanelOut] == 0) GameFunction<OpenPanelFn>(Game::Address::PanelOpenOrClose)(0, 0, playerInterface);
		// Button 0 is level setup asking for Creatures. The editor starts on what it is painting.
		if (button != 0 && panel >= 1 && panel <= 4) EditorPanel::SetTab(static_cast<EditorPanel::Tab>(panel));
		return true;
	}

	bool LevelEditor::OnKey(std::int32_t key, std::int32_t pressed, std::uint32_t modifiers, const std::uint8_t* playerInterface) {
		constexpr std::int32_t KeyT = 0x14;
		constexpr std::int32_t KeyS = 0x1F;
		constexpr std::uint32_t ModifierShift = 1;
		constexpr std::uint32_t ModifierCtrl = 2;
		const bool ctrl = (modifiers & ModifierCtrl) != 0;
		if (testing) {
			if (key != KeyT || !ctrl) return false;
			if (pressed != 0) stopTestWanted = true;
			return true;
		}
		if (!inSession || !DocumentReady()) return false;
		// The options menu takes keys for itself, such as a save's name.
		if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) != 0) return false;
		// Typing a name for Save as takes every key. Letters, digits, space, - and _; Shift for capitals. DirectInput key numbers.
		if (naming) {
			if (pressed == 0) return true;
			constexpr std::int32_t KeyEscape = 0x01;
			constexpr std::int32_t KeyBackspace = 0x0E;
			constexpr std::int32_t KeyEnter = 0x1C;
			constexpr std::int32_t KeySpace = 0x39;
			constexpr std::int32_t KeyMinus = 0x0C;
			const bool shift = (modifiers & ModifierShift) != 0;
			static constexpr std::string_view Row1 = "qwertyuiop";
			static constexpr std::string_view Row2 = "asdfghjkl";
			static constexpr std::string_view Row3 = "zxcvbnm";
			char typed = 0;
			if (key >= 0x02 && key <= 0x0B) typed = key == 0x0B ? '0' : static_cast<char>('1' + key - 0x02);
			else if (key >= 0x10 && key < 0x10 + static_cast<std::int32_t>(Row1.size())) typed = Row1[static_cast<std::size_t>(key - 0x10)];
			else if (key >= 0x1E && key < 0x1E + static_cast<std::int32_t>(Row2.size())) typed = Row2[static_cast<std::size_t>(key - 0x1E)];
			else if (key >= 0x2C && key < 0x2C + static_cast<std::int32_t>(Row3.size())) typed = Row3[static_cast<std::size_t>(key - 0x2C)];
			else if (key == KeySpace) typed = ' ';
			else if (key == KeyMinus) typed = shift ? '_' : '-';
			const bool pointName = prompt == Prompt::ActionPointName;
			const bool levelText = prompt == Prompt::LevelText || pointName;
			// A level's texts are not file names, so they take some punctuation too.
			if (levelText) {
				constexpr std::int32_t KeyComma = 0x33;
				constexpr std::int32_t KeyPeriod = 0x34;
				constexpr std::int32_t KeySlash = 0x35;
				constexpr std::int32_t KeyApostrophe = 0x28;
				constexpr std::int32_t KeyOne = 0x02;
				constexpr std::int32_t KeyTwo = 0x03;
				if (key == KeyComma) typed = ',';
				else if (key == KeyPeriod) typed = '.';
				else if (key == KeySlash) typed = shift ? '?' : '/';
				else if (key == KeyApostrophe) typed = shift ? '"' : '\'';
				else if (key == KeyOne && shift) typed = '!';
				else if (key == KeyTwo && shift) typed = '@';
			}
			if (typed >= 'a' && typed <= 'z' && shift) typed = static_cast<char>(typed - 'a' + 'A');
			const bool side = prompt == Prompt::NewMapWidth || prompt == Prompt::NewMapHeight;
			const bool gold = prompt == Prompt::Gold;
			const bool field = prompt == Prompt::TriggerField;
			// A side typed, or 0 if it is not a usable one (and why is said).
			const auto readSide = [&]() -> std::uint32_t {
				const auto value = nameText.empty() ? 0ull : std::stoull(nameText);
				if (value < LevelFiles::SmallestNewMapSide) {
					Say("A side is at least " + std::to_string(LevelFiles::SmallestNewMapSide) + " tiles");
					return 0;
				}
				return static_cast<std::uint32_t>(value);
			};
			if (key == KeyEscape) {
				naming = false;
				Say(prompt == Prompt::SaveAsName ? "Save as stopped"
					: prompt == Prompt::LevelText || prompt == Prompt::Gold || prompt == Prompt::ActionPointName || prompt == Prompt::TriggerField ? "Left as it was" : "New map stopped");
			}
			else if (key == KeyBackspace) {
				if (!nameText.empty()) nameText.pop_back();
			}
			else if (key == KeyEnter) {
				while (!nameText.empty() && nameText.back() == ' ') nameText.pop_back();
				// A level's text may be emptied; anything else needs something typed.
				if (nameText.empty() && !levelText) return true;
				statusText.clear();
				switch (prompt) {
				case Prompt::TriggerField: {
					naming = false;
					if (!document || !document->triggers || editingTrigger >= document->triggers->records.size() || nameText == "-") break;
					auto& bytes = document->triggers->records[editingTrigger].bytes;
					const auto value = std::stoll(nameText);
					if (promptIndex == FieldValue) Store<std::int32_t>(bytes.data() + 4, static_cast<std::int32_t>(std::clamp(value, -2147483648ll, 2147483647ll)));
					else bytes[promptIndex == FieldRepeat ? 0xF : promptIndex] = static_cast<std::uint8_t>(std::clamp(value, 0ll, 255ll));
					TriggersChanged();
					if (menuOpen && menuIsTriggers) {
						EnterTriggerEdit();
						BuildMenuRows();
					}
					Say("Changed to " + nameText);
					break;
				}
				case Prompt::ActionPointName: {
					naming = false;
					const auto place = FindActionPoint(static_cast<std::uint8_t>(promptIndex));
					auto* records = document && document->things ? &document->things->records : nullptr;
					if (!place || records == nullptr || (*records)[*place].bytes.size() < ActionPointNameAt + ActionPointNameSize) {
						Say("That action point has no room for a name");
						break;
					}
					auto& record = (*records)[*place];
					const auto before = record;
					std::fill(record.bytes.begin() + ActionPointNameAt, record.bytes.begin() + ActionPointNameAt + ActionPointNameSize, std::uint8_t{ 0 });
					std::copy(nameText.begin(), nameText.end(), record.bytes.begin() + ActionPointNameAt);
					PushCreatureUndo({ CreatureChangeKind::Changed, *place, before });
					unsavedChanges = true;
					Say(nameText.empty() ? "Action point " + std::to_string(promptIndex) + " has no name now" : "Action point " + std::to_string(promptIndex) + " is \"" + nameText + "\"");
					break;
				}
				case Prompt::LevelText:
					naming = false;
					if (document && document->level && LevelFiles::SetLevelText(*document->level, LevelTextRows[promptIndex].first, Widen(nameText))) {
						document->levelChanged = true;
						unsavedChanges = true;
						leaveWarned = false;
						// The row names are plain ASCII (LevelTextRows), so each wide character fits in one byte.
						std::string row;
						for (const wchar_t* at = LevelTextRows[promptIndex].second; *at != 0; ++at) row += static_cast<char>(*at);
						Say(row + " changed");
					}
					else Say("That text does not fit in this level's description");
					break;
				case Prompt::Gold:
					naming = false;
					if (document && document->players && promptIndex < document->players->records.size()) {
						auto& record = document->players->records[promptIndex];
						Store<std::uint32_t>(record.data() + PlayerGoldOffset, static_cast<std::uint32_t>(std::stoul(nameText)));
						unsavedChanges = true;
						leaveWarned = false;
						Say(OwnerName(record[PlayerNumberOffset]) + " starts with " + nameText + " gold");
					}
					break;
				case Prompt::SaveAsName:
					naming = false;
					saveAsWanted = true;
					break;
				case Prompt::NewMapName:
					if (std::error_code code; std::filesystem::exists(std::filesystem::path(L"Data\\editor\\maps") / (std::wstring(nameText.begin(), nameText.end()) + L".kwd"), code)) {
						Say("A level called " + nameText + " already exists");
						break;
					}
					newMapName = nameText;
					nameText.clear();
					prompt = Prompt::NewMapWidth;
					break;
				case Prompt::NewMapWidth:
					if (const auto value = readSide(); value != 0) {
						newMapWidth = value;
						nameText.clear();
						prompt = Prompt::NewMapHeight;
					}
					break;
				case Prompt::NewMapHeight:
					if (const auto value = readSide(); value != 0) {
						newMapHeight = value;
						if (!LevelFiles::MapSizeFits(newMapWidth, newMapHeight)) {
							Say(std::to_string(newMapWidth) + "x" + std::to_string(newMapHeight) + " is more tiles than the game can hold");
							break;
						}
						naming = false;
						newMapWanted = true;
						Say("Making " + newMapName + ", " + std::to_string(newMapWidth) + "x" + std::to_string(newMapHeight));
					}
					break;
				}
			}
			else if (side) {
				if (typed >= '0' && typed <= '9' && nameText.size() < MaxSideDigits && !(typed == '0' && nameText.empty())) nameText += typed;
			}
			else if (field) {
				// Up to 11 characters fits any 32 bit number; only the value can be below 0.
				constexpr std::size_t MostDigits = 11;
				const bool minusAllowed = promptIndex == FieldValue && nameText.empty();
				if (((typed >= '0' && typed <= '9') || (typed == '-' && minusAllowed)) && nameText.size() < MostDigits) nameText += typed;
			}
			else if (gold) {
				if (typed >= '0' && typed <= '9' && nameText.size() < MaxGoldDigits && !(typed == '0' && nameText == "0")) {
					if (nameText == "0") nameText.clear();
					nameText += typed;
				}
			}
			else if (pointName) {
				if (typed != 0 && nameText.size() + 1 < ActionPointNameSize) nameText += typed;
			}
			else if (levelText) {
				if (typed != 0 && nameText.size() + 1 < LevelFiles::LevelTextLength(LevelTextRows[promptIndex].first)) nameText += typed;
			}
			else if (typed != 0 && nameText.size() < MaxLevelName && !(typed == ' ' && nameText.empty())) nameText += typed;
			return true;
		}
		constexpr std::int32_t KeyBracketLeft = 0x1A;
		constexpr std::int32_t KeyBracketRight = 0x1B;
		constexpr std::int32_t KeyComma = 0x33;
		constexpr std::int32_t KeyPeriod = 0x34;
		constexpr std::int32_t KeyZ = 0x2C;
		constexpr std::int32_t KeyY = 0x15;
		constexpr std::int32_t KeyF5 = 0x3F;
		// DirectInput's numbers for Delete (on the arrows' block) and Esc. [unverified that the game passes Delete on as 0xD3]
		constexpr std::int32_t KeyDelete = 0xD3;
		constexpr std::int32_t KeyEscape = 0x01;
		const bool busy = tool == Tool::Move || actionPointDragging;
		if (key == KeyEscape && !busy && selectedCreature != 0) {
			// Dragging, it goes back to where it was first (on the next tick, which has the world).
			if (pressed != 0 && creatureDragging) dragCancelWanted = true;
			else if (pressed != 0) {
				selectedCreature = 0;
				creatureDragging = false;
				Say("Let go of the creature");
			}
			return true;
		}
		if (key == KeyEscape && busy) {
			if (pressed == 0) return true;
			if (tool == Tool::Move) Say("Move stopped");
			else Say("Box stopped");
			StopMoving();
			actionPointDragging = false;
			return true;
		}
		if (key != KeyBracketLeft && key != KeyBracketRight && key != KeyComma && key != KeyPeriod && key != KeyF5 && key != KeyDelete
			&& !(ctrl && (key == KeyZ || key == KeyY || key == KeyS || key == KeyT))) return false;
		if (pressed == 0) return true;
		if (key == KeyDelete) {
			deleteAtPointerWanted = true;
			return true;
		}
		if (ctrl && (key == KeyY || (key == KeyZ && (modifiers & ModifierShift) != 0))) {
			redoWanted = true;
			return true;
		}
		if (ctrl && key == KeyS) {
			saveWanted = true;
			return true;
		}
		if (ctrl && key == KeyT) {
			testWanted = true;
			return true;
		}
		switch (key) {
		case KeyBracketLeft:
			terrainChoice = (terrainChoice + terrains.size() - 1) % terrains.size();
			ShowChoiceOnPanel();
			LogChoice();
			break;
		case KeyBracketRight:
			terrainChoice = (terrainChoice + 1) % terrains.size();
			ShowChoiceOnPanel();
			LogChoice();
			break;
		case KeyComma:
			StepOwner(-1);
			break;
		case KeyPeriod:
			StepOwner(1);
			break;
		case KeyZ:
			undoWanted = true;
			break;
		case KeyF5:
			saveWanted = true;
			break;
		default:
			break;
		}
		return true;
	}

	void LevelEditor::Install() {
		if (installed) return;
		const auto windowConfig = Game::Resolve(Game::Address::SinglePlayerWindowConfig);
		const auto buttons = Memory::Read<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons);
		if (Memory::Read<std::uint8_t>(windowConfig) != Game::SinglePlayerWindowId || buttons != Game::Resolve(Game::Address::SinglePlayerButtons)
			|| !BuildButtonList(reinterpret_cast<const std::uint8_t*>(buttons))) {
			Logger::Warning("Editor: unexpected game code, the Editor button is left out.");
			return;
		}
		if (!Hook(Game::Resolve(Game::Address::TextById), { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x0D, 0x50, 0x15, 0x74, 0x00 }, reinterpret_cast<void*>(&TextByIdDetour), reinterpret_cast<void**>(&originalTextById))) {
			Logger::Warning("Editor: could not hook the game's texts, the Editor button is left out.");
			return;
		}
		if (!Hook(Game::Resolve(Game::Address::TimeIntoTurn), { 0x56, 0x8B, 0xF1, 0xE8 }, reinterpret_cast<void*>(&TimeIntoTurnDetour), reinterpret_cast<void**>(&originalTimeIntoTurn))) {
			Logger::Warning("Editor: could not hook the turn time, things may move while a level is held.");
		}
		if (!Hook(Game::Resolve(Game::Address::InterfaceToggleOptions), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xB8, 0x9C, 0x64, 0x00, 0x50 }, reinterpret_cast<void*>(&ToggleOptionsDetour), reinterpret_cast<void**>(&originalToggleOptions))) {
			Logger::Warning("Editor: could not hook the options menu, its buttons may not work while a level is held.");
		}
		constexpr std::uint8_t worldClickStart[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68 };
		const std::vector<std::uint8_t> worldClickBytes(std::begin(worldClickStart), std::end(worldClickStart));
		if (!Hook(Game::Resolve(Game::Address::InterfaceWorldLeftClick), worldClickBytes, reinterpret_cast<void*>(&WorldLeftClickDetour), reinterpret_cast<void**>(&originalWorldLeftClick))
			|| !Hook(Game::Resolve(Game::Address::InterfaceWorldRightClick), worldClickBytes, reinterpret_cast<void*>(&WorldRightClickDetour), reinterpret_cast<void**>(&originalWorldRightClick))) {
			Logger::Warning("Editor: could not hook the world's clicks, so nothing can be painted.");
		}
		if (!Hook(Game::Resolve(Game::Address::InterfaceDraw), { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x46, 0x04 }, reinterpret_cast<void*>(&InterfaceDrawDetour), reinterpret_cast<void**>(&originalInterfaceDraw))) {
			Logger::Warning("Editor: could not hook the interface's drawing, so there is no palette or side panel on screen.");
		}
		if (!Hook(Game::Resolve(Game::Address::InterfaceReleaseViews), { 0x56, 0x8B, 0xF1, 0x57, 0x33, 0xFF, 0x8B, 0x86, 0xBE, 0x12, 0x00, 0x00 }, reinterpret_cast<void*>(&ReleaseViewsDetour), reinterpret_cast<void**>(&originalReleaseViews))
			|| !Hook(Game::Resolve(Game::Address::InterfaceMouseEvent), { 0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x28, 0x57 }, reinterpret_cast<void*>(&MouseEventDetour), reinterpret_cast<void**>(&originalMouseEvent))) {
			Logger::Warning("Editor: could not hook the side panel's tabs and clicks, so its panel will not work.");
		}
		creaturesUsable = Memory::Matches(Game::Resolve(Game::Address::KeeperCreatureMake), { 0x83, 0xEC, 0x24, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x34 })
			&& Memory::Matches(Game::Resolve(Game::Address::NeutralCreatureMake), { 0x83, 0xEC, 0x24, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x34 })
			&& Memory::Matches(Game::Resolve(Game::Address::HeroCreatureMake), { 0x83, 0xEC, 0x20, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x30 })
			&& Memory::Matches(Game::Resolve(Game::Address::CreatureRemove), { 0x8B, 0x44, 0x24, 0x04, 0x56 })
			&& Memory::Matches(Game::Resolve(Game::Address::TrapMake), { 0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x18, 0x56 })
			&& Memory::Matches(Game::Resolve(Game::Address::DoorMake), { 0x6A, 0xFF, 0x68, 0xD8, 0xD5, 0x64, 0x00, 0x64, 0xA1 });
		if (!creaturesUsable) Logger::Warning("Editor: unexpected game code at the things, creatures, traps and doors cannot be placed or deleted.");
		creaturesUsable = creaturesUsable
			&& Memory::Matches(Game::Resolve(Game::Address::PlayerSellDoor), { 0x6A, 0xFF, 0x68, 0xA8, 0xC6, 0x64, 0x00 });
		// push ebx, ebp, esi, edi; mov ebx, ecx; mov ebp, 0x6BC354 (its table of sides) [verified in the assembly at 0x4ECDB0].
		roomsRefreshUsable = Memory::Matches(Game::Resolve(Game::Address::RoomsRefreshAround), { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xD9, 0xBD, 0x54, 0xC3, 0x6B, 0x00 });
		if (!roomsRefreshUsable) Logger::Warning("Editor: unexpected game code at the room refresh, wall props and room objects will not follow painting.");
		// sub esp, 0xC; push esi; mov esi, [esp+0x14]; push edi; mov edi, [esp+0x1C] [verified in the assembly at 0x520BB0].
		objectsUsable = Memory::Matches(Game::Resolve(Game::Address::ObjectMake), { 0x83, 0xEC, 0x0C, 0x56, 0x8B, 0x74, 0x24, 0x14, 0x57, 0x8B, 0x7C, 0x24, 0x1C });
		if (!objectsUsable) Logger::Warning("Editor: unexpected game code at the object maker, objects cannot be placed.");
		// push -1; push 0x64A868; mov eax, fs:[0] [verified in the assembly at 0x43CC30].
		outlinesUsable = Hook(Game::Resolve(Game::Address::TileDraw), { 0x6A, 0xFF, 0x68, 0x68, 0xA8, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&TileDrawDetour), reinterpret_cast<void**>(&originalTileDraw));
		if (!outlinesUsable) Logger::Warning("Editor: could not hook the tile drawing, action points are shown by Skull markers instead of outlines.");
		ReadOutlineColor();
		deadBodiesUsable = Memory::Matches(Game::Resolve(Game::Address::DeadBodyMake), { 0x83, 0xEC, 0x10, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x1C });
		effectGeneratorsUsable = Memory::Matches(Game::Resolve(Game::Address::EffectGeneratorMake), { 0x83, 0xEC, 0x0C, 0x56, 0x8B, 0x74, 0x24, 0x14, 0x57 });
		if (!deadBodiesUsable || !effectGeneratorsUsable) Logger::Warning("Editor: unexpected game code at the dead body or effect generator maker; those cannot be placed.");
		if (!Hook(Game::Resolve(Game::Address::ShowMoneyText), { 0x8B, 0x41, 0x04, 0x8B, 0x48, 0x10, 0x8B, 0x44, 0x24, 0x10 }, reinterpret_cast<void*>(&ShowMoneyTextDetour), reinterpret_cast<void**>(&originalShowMoneyText))) {
			Logger::Warning("Editor: could not hook the money text, selling in the editor may show a floating 0.");
		}
		if (!Hook(Game::Resolve(Game::Address::PanelWindowsLayout), { 0x83, 0xEC, 0x0C, 0xA1, 0x4B, 0xAD, 0x75, 0x00, 0x53, 0x55 }, reinterpret_cast<void*>(&PanelLayoutDetour), reinterpret_cast<void**>(&originalPanelLayout))) {
			Logger::Warning("Editor: could not hook the panel layout; going back to editing after a test may crash.");
		}
		if (!Hook(Game::Resolve(Game::Address::PanelButtonRender), { 0x6A, 0xFF, 0x68, 0x6B, 0x94, 0x64, 0x00, 0x64, 0xA1 }, reinterpret_cast<void*>(&PanelButtonRenderDetour), reinterpret_cast<void**>(&originalPanelButtonRender))) {
			Logger::Warning("Editor: could not hook the tab buttons, the Terrain tab keeps the wand.");
		}
		// All or nothing: the hand stays unless Windows' pointer can take over completely.
		const bool pointerHooks = Hook(Game::Resolve(Game::Address::GameWindowProc), { 0x83, 0xEC, 0x14, 0x53, 0x8B, 0x5C, 0x24, 0x24, 0x55, 0x8B, 0x6C, 0x24, 0x20 }, reinterpret_cast<void*>(&WindowProcDetour), reinterpret_cast<void**>(&originalWindowProc))
			&& Hook(Game::Resolve(Game::Address::MouseMotion), { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1, 0x8B, 0x4F, 0x10 }, reinterpret_cast<void*>(&MouseMotionDetour), reinterpret_cast<void**>(&originalMouseMotion))
			&& Hook(Game::Resolve(Game::Address::SetPointerImage), { 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x4C, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x08 }, reinterpret_cast<void*>(&SetPointerImageDetour), reinterpret_cast<void**>(&originalSetPointerImage))
			&& Hook(Game::Resolve(Game::Address::InterfaceDrawHandIcons), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x0B, 0x91, 0x64, 0x00, 0x50 }, reinterpret_cast<void*>(&DrawHandIconsDetour), reinterpret_cast<void**>(&originalDrawHandIcons));
		osPointerUsable = pointerHooks;
		if (!pointerHooks) Logger::Warning("Editor: unexpected game code at the mouse, the editor keeps the hand.");
		revealUsable = Memory::Matches(Game::Resolve(Game::Address::MapRevealAll), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x28, 0xAD, 0x64, 0x00, 0x50 });
		if (!revealUsable) Logger::Warning("Editor: unexpected map code, levels will keep their fog.");
		Memory::Write<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(buttonList.data()));
		installed = true;
		Logger::Debug("Editor: button added to the Single Player menu.");
		if (Config::GetString("editor_check_maps", "off") == "on") {
			LevelFiles::CheckAllLevels(true);
			levelsChecked = true;
			Config::SetString("editor_check_maps", "off");
		}
	}
}
