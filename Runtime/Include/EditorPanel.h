#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace KeeperForge2::EditorPanel {
	// The editor's side panel: in place of the game's panels, the four tabs show grids of icons from the bitmaps in
	// Data\editor\Graphics (docs/game/level-editor.md). Only draws and answers clicks; the level editor says what it holds.
	// The numbers are the game's own for its tabs. Its second, Keeper Spells, holds terrain here. Objects is the editor's own, a
	// second page of the Workshop tab, reached from an item at the end of that tab's list.
	enum class Tab : std::int32_t { Rooms = 1, Terrain = 2, Workshop = 3, Creatures = 4, Objects = 5 };
	struct Item {
		std::string name;
		// A bitmap in Data\editor\Graphics, and which 64x64 cell of it, left to right and top to bottom. Below 0 for no icon.
		std::wstring sheet;
		std::int32_t icon = -1;
		// For the level editor: what this item stands for.
		std::size_t value = 0;
		// The color behind its icon when it is a variant, such as a blueprint. 0 for the plain one.
		std::uint32_t mark = 0;
		// It goes at the start of the next row of the grid, leaving the rest of the row it would have gone on empty.
		bool startsRow = false;
	};
	// A new level is being edited: the lists go, as does the game's view it used.
	void Reset();
	void SetItems(Tab tab, std::vector<Item> items);
	void SetTab(Tab tab);
	// The item with this value on this tab is marked as chosen, and the tab shown. If there is none, nothing is chosen.
	void Choose(Tab tab, std::size_t value);
	// Once a frame while a level is edited.
	void Draw(std::uint8_t* playerInterface);
	bool MouseOver();
	// A press where the mouse is. Returns the item it is on, which is then the one chosen on its tab, or null.
	const Item* Click();
	// The item the mouse is on, without choosing it, or null.
	const Item* ItemUnderMouse();
	// Changes how an item reads and looks, as its variant changes.
	void SetItemLook(Tab tab, std::size_t value, std::string name, std::uint32_t mark);
	Tab CurrentTab();
	// The game is releasing its views and their textures, as the screen is changing or the level ending.
	void ReleaseView(std::uint8_t* playerInterface);
}
