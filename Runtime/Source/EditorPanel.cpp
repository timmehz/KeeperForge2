#include "EditorPanel.h"
#include "GameAddresses.h"
#include "Logger.h"
#include "Memory.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <Windows.h>

namespace KeeperForge2::EditorPanel {
	namespace {
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// Layout units, 2560x1920. The grid is along the bottom of the screen, right of the minimap and under the tab buttons,
		// which reach down to about 1587 (seen in a screenshot). The name of an item goes on the rod beside those buttons, so
		// the view starts higher than the grid. What is not drawn on stays see-through, and only the grid takes clicks.
		constexpr std::int32_t PanelLeft = 555;
		// On a 4:3 screen; a wider one adds its spare width (the keeper icon and alarms on the right move by as much).
		constexpr std::int32_t PanelRight4x3 = 2240;
		constexpr std::int32_t ViewTop = 1480;
		constexpr std::int32_t GridTop = 1600;
		constexpr std::int32_t GridBottom = 1900;
		constexpr std::int32_t NameLeft = 1240;
		constexpr std::int32_t CellWidth = 96;
		constexpr std::int32_t CellHeight = 96;
		constexpr std::int32_t Rows = (GridBottom - GridTop) / CellHeight;
		// Set when the view is made, from the screen's shape (MakeView).
		std::int32_t panelRight = PanelRight4x3;
		std::int32_t columns = (PanelRight4x3 - PanelLeft) / CellWidth;
		constexpr std::int32_t IconSide = 64;
		constexpr std::int32_t BorderPixels = 3;
		constexpr std::uint32_t TextureTilesWithoutTheStub = 140;
		// The second tab holds terrain, so a terrain's picture goes over the game's wand. Where the button is, measured on a
		// screenshot with the panel open (window 18 at x 472, plus 272 for this button). It sits inside the view's top.
		constexpr std::int32_t TerrainTabIconLeft = 758;
		constexpr std::int32_t TerrainTabIconTop = 1482;
		constexpr std::int32_t TerrainTabIconSide = 100;
		constexpr std::int32_t TerrainTabIconCell = 3; // Rock, in TerrainIcons.bmp

		std::array<std::vector<Item>, 6> lists;
		std::array<std::int32_t, 6> chosen{ -1, -1, -1, -1, -1, -1 };
		Tab currentTab = Tab::Terrain;

		// The game's view for this: its own objects live inside the player interface, so this one has room for the largest.
		alignas(4) std::array<std::uint8_t, 0x2A0> view{};
		bool viewMade = false;
		bool viewUnusable = false;
		std::int32_t originX = 0;
		std::int32_t originY = 0;
		std::int32_t pixelWidth = 0;
		std::int32_t pixelHeight = 0;
		std::int32_t gridTopPixels = 0;
		std::int32_t hovered = -1;
		std::wstring drawn;
		bool shown = false;

		// A sheet of icons, 32 bits a pixel, top row first.
		struct Sheet {
			std::int32_t width = 0;
			std::int32_t height = 0;
			std::vector<std::uint32_t> pixels;
		};
		std::map<std::wstring, Sheet> sheets;
		const Sheet& SheetNamed(const std::wstring& name) {
			const auto found = sheets.find(name);
			if (found != sheets.end()) return found->second;
			auto& sheet = sheets[name];
			const auto path = L"Data\\editor\\Graphics\\" + name;
			auto* bitmap = static_cast<HBITMAP>(LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
			BITMAP info{};
			if (bitmap == nullptr || GetObjectW(bitmap, sizeof(info), &info) == 0) {
				Logger::Warning("Editor: could not load the icons in Data\\editor\\Graphics, one sheet shows none.");
				if (bitmap != nullptr) DeleteObject(bitmap);
				return sheet;
			}
			BITMAPINFO request{};
			request.bmiHeader.biSize = sizeof(request.bmiHeader);
			request.bmiHeader.biWidth = info.bmWidth;
			request.bmiHeader.biHeight = -info.bmHeight;
			request.bmiHeader.biPlanes = 1;
			request.bmiHeader.biBitCount = 32;
			request.bmiHeader.biCompression = BI_RGB;
			sheet.pixels.resize(static_cast<std::size_t>(info.bmWidth) * info.bmHeight);
			auto* screen = GetDC(nullptr);
			const auto lines = GetDIBits(screen, bitmap, 0, static_cast<UINT>(info.bmHeight), sheet.pixels.data(), &request, DIB_RGB_COLORS);
			ReleaseDC(nullptr, screen);
			DeleteObject(bitmap);
			if (lines != info.bmHeight) {
				sheet.pixels.clear();
				return sheet;
			}
			sheet.width = info.bmWidth;
			sheet.height = info.bmHeight;
			Logger::Debug("Editor: icon sheet of " + std::to_string(sheet.width) + "x" + std::to_string(sheet.height) + " loaded, "
				+ std::to_string(sheet.width / IconSide * (sheet.height / IconSide)) + " cells.");
			return sheet;
		}

		struct Point { std::int32_t x, y; };
		Point LayoutToScreen(std::uint8_t* playerInterface, std::int32_t x, std::int32_t y) {
			using PointFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* out, const std::int32_t* in);
			const std::int32_t in[2]{ x, y };
			std::int32_t out[2]{};
			GameFunction<PointFn>(Game::Address::GuiPointToScreen)(playerInterface + Game::Offset::InterfaceGuiManager, nullptr, out, in);
			return { out[0], out[1] };
		}
		Point MousePosition() {
			using MouseFn = const std::int32_t*(__cdecl*)();
			const auto* mouse = GameFunction<MouseFn>(Game::Address::MouseOnScreen)();
			return { mouse[0], mouse[1] };
		}

		std::size_t TabIndex(Tab tab) { return static_cast<std::size_t>(tab); }

		// The game's own panels would draw and take clicks in this place, so while a level is edited none of them shows. Ids
		// 1 to 3 are rooms, spells and the workshop, 5 to 17 the creature panels (docs/game/screen-resolution.md).
		void HideGamePanels(std::uint8_t* playerInterface) {
			using WindowFn = std::uint8_t*(__fastcall*)(void* manager, void* unused, std::int32_t id);
			for (std::int32_t id = 1; id <= 17; ++id) {
				if (id == 4) continue;
				auto* window = GameFunction<WindowFn>(Game::Address::GuiWindowById)(playerInterface + Game::Offset::InterfaceGuiManager, nullptr, id);
				if (window != nullptr) Store<std::int32_t>(window + Game::Offset::WindowShown, 0);
			}
		}

		bool MakeView(std::uint8_t* playerInterface) {
			if (viewMade) return true;
			if (viewUnusable) return false;
			// The in-game layout keeps its 4:3 shape from the left edge (WidescreenGui); a wider screen has spare layout width
			// on the right, which the grid takes, so the panel reaches across the screen as the game's bars do.
			const auto* manager = playerInterface + Game::Offset::InterfaceGuiManager;
			const auto screenWidth = Load<std::int32_t>(manager + Game::Offset::GuiManagerWidth);
			const auto screenHeight = Load<std::int32_t>(manager + Game::Offset::GuiManagerHeight);
			std::int32_t spare = 0;
			if (screenWidth > 0 && screenHeight > 0) spare = (std::max)(0, static_cast<std::int32_t>(static_cast<std::int64_t>(screenWidth) * 1920 / screenHeight) - 2560);
			panelRight = PanelRight4x3 + spare;
			columns = (panelRight - PanelLeft) / CellWidth;
			const auto topLeft = LayoutToScreen(playerInterface, PanelLeft, ViewTop);
			const auto bottomRight = LayoutToScreen(playerInterface, panelRight, GridBottom);
			pixelWidth = bottomRight.x - topLeft.x;
			pixelHeight = bottomRight.y - topLeft.y;
			// More tiles than the game's own table holds need Widescreen's replacement for it, which is a jump put at this place.
			const std::uint32_t tiles = (static_cast<std::uint32_t>(pixelWidth) + 0x7F) / 128 * ((static_cast<std::uint32_t>(pixelHeight) + 0x1F) / 32);
			if (tiles > TextureTilesWithoutTheStub && Memory::Read<std::uint8_t>(Game::Resolve(Game::Address::GuiViewTileWrite)) != 0xE9) {
				viewUnusable = true;
				Logger::Warning("Editor: the side panel needs Widescreen's larger view table at this resolution, so it is not shown.");
				return false;
			}
			view.fill(0);
			using MakeFn = std::int32_t(__fastcall*)(std::uint8_t* forInterface, void* unused, std::int32_t width, std::int32_t height, std::int32_t unknown, const char* name, std::uint8_t* forView);
			if (GameFunction<MakeFn>(Game::Address::GuiViewCreate)(playerInterface, nullptr, pixelWidth, pixelHeight, 0x20, "EditorPanelTextBuffer", view.data()) == 0) {
				viewUnusable = true;
				Logger::Warning("Editor: could not make the side panel's view.");
				return false;
			}
			viewMade = true;
			drawn.clear();
			return true;
		}

		std::uint16_t To16Bits(std::uint32_t color) {
			return static_cast<std::uint16_t>(0x8000u | ((color >> 9) & 0x7C00u) | ((color >> 6) & 0x03E0u) | ((color >> 3) & 0x001Fu));
		}
		void PutPixel(std::uint8_t* line, std::int32_t bits, std::int32_t x, std::uint32_t color) {
			if (bits == 32) reinterpret_cast<std::uint32_t*>(line)[x] = color;
			else reinterpret_cast<std::uint16_t*>(line)[x] = To16Bits(color);
		}
		void FillRect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom, std::uint32_t color) {
			const auto bits = Load<std::int32_t>(view.data() + Game::Offset::ViewBitsPerPixel);
			if (bits != 16 && bits != 32) return;
			auto* pixels = Load<std::uint8_t*>(view.data() + Game::Offset::ViewPixels);
			const auto pitch = Load<std::int32_t>(view.data() + Game::Offset::ViewPitch);
			const auto width = (std::min)(right, Load<std::int32_t>(view.data() + Game::Offset::ViewPixelWidth));
			const auto height = (std::min)(bottom, Load<std::int32_t>(view.data() + Game::Offset::ViewPixelHeight));
			for (auto y = (std::max)(top, 0); y < height; ++y) {
				auto* line = pixels + static_cast<std::ptrdiff_t>(y) * pitch;
				for (auto x = (std::max)(left, 0); x < width; ++x) PutPixel(line, bits, x, color);
			}
		}
		// Stretched to the cell's shorter side, as the view's pixels are not the icon's.
		void DrawIcon(const Item& item, std::int32_t left, std::int32_t top, std::int32_t side) {
			if (item.icon < 0) return;
			const auto& sheet = SheetNamed(item.sheet);
			if (sheet.width < IconSide || sheet.height < IconSide) return;
			const auto columns = sheet.width / IconSide;
			const auto column = item.icon % columns;
			const auto row = item.icon / columns;
			if ((row + 1) * IconSide > sheet.height) return;
			const auto bits = Load<std::int32_t>(view.data() + Game::Offset::ViewBitsPerPixel);
			if (bits != 16 && bits != 32) return;
			auto* pixels = Load<std::uint8_t*>(view.data() + Game::Offset::ViewPixels);
			const auto pitch = Load<std::int32_t>(view.data() + Game::Offset::ViewPitch);
			const auto viewWidth = Load<std::int32_t>(view.data() + Game::Offset::ViewPixelWidth);
			const auto viewHeight = Load<std::int32_t>(view.data() + Game::Offset::ViewPixelHeight);
			for (std::int32_t y = 0; y < side; ++y) {
				if (top + y < 0 || top + y >= viewHeight) continue;
				auto* line = pixels + static_cast<std::ptrdiff_t>(top + y) * pitch;
				const auto sourceY = row * IconSide + y * IconSide / side;
				for (std::int32_t x = 0; x < side; ++x) {
					if (left + x < 0 || left + x >= viewWidth) continue;
					const auto sourceX = column * IconSide + x * IconSide / side;
					const auto color = sheet.pixels[static_cast<std::size_t>(sourceY) * sheet.width + static_cast<std::size_t>(sourceX)];
					PutPixel(line, bits, left + x, color | 0xFF000000u);
				}
			}
		}

		struct FontValue { std::uint8_t bytes[0x50]; };
		void DrawName(std::uint8_t* playerInterface, const std::string& name) {
			using CopyFontFn = void(__fastcall*)(void* to, void* unused, const void* from);
			using TextFn = void(__fastcall*)(std::uint8_t* forInterface, void* unused, std::uint8_t* forView, std::int32_t x, std::int32_t y, const wchar_t* text,
				const std::uint8_t* color, std::int32_t alignment, std::int32_t clear, FontValue font, std::int32_t upload);
			static constexpr std::array<std::uint8_t, 5> nameColor{ 0xBF, 0xBF, 0xBF, 0xFF, 0 };
			std::wstring wide;
			for (const auto c : name) wide += static_cast<wchar_t>(static_cast<unsigned char>(c));
			FontValue font;
			GameFunction<CopyFontFn>(Game::Address::TextFontCopy)(&font, nullptr, reinterpret_cast<const void*>(Game::Resolve(Game::Address::ViewTextFont)));
			const auto x = LayoutToScreen(playerInterface, NameLeft, ViewTop).x - originX;
			GameFunction<TextFn>(Game::Address::GuiViewTextWide)(playerInterface, nullptr, view.data(), x, 0, wide.c_str(), nameColor.data(), 0, 0, font, 0);
		}

		// Each item's cell in the grid, left to right and down, an item that starts a row moving on to the next one. Only those
		// that fit in the grid's rows.
		std::vector<std::int32_t> Cells(const std::vector<Item>& list) {
			std::vector<std::int32_t> cells;
			std::int32_t cell = 0;
			for (const auto& item : list) {
				if (item.startsRow && cell % columns != 0) cell = (cell / columns + 1) * columns;
				if (cell >= columns * Rows) break;
				cells.push_back(cell++);
			}
			return cells;
		}
		void CellBox(std::uint8_t* playerInterface, std::int32_t index, std::int32_t& left, std::int32_t& top, std::int32_t& right, std::int32_t& bottom) {
			const auto column = index % columns;
			const auto row = index / columns;
			const auto a = LayoutToScreen(playerInterface, PanelLeft + column * CellWidth, GridTop + row * CellHeight);
			const auto b = LayoutToScreen(playerInterface, PanelLeft + (column + 1) * CellWidth, GridTop + (row + 1) * CellHeight);
			left = a.x - originX;
			top = a.y - originY;
			right = b.x - originX;
			bottom = b.y - originY;
		}
	}

	void Reset() {
		for (auto& list : lists) list.clear();
		chosen.fill(-1);
		currentTab = Tab::Terrain;
		hovered = -1;
		drawn.clear();
		shown = false;
		viewUnusable = false;
	}
	void SetItems(Tab tab, std::vector<Item> items) {
		lists[TabIndex(tab)] = std::move(items);
		drawn.clear();
	}
	void SetTab(Tab tab) {
		currentTab = tab;
		hovered = -1;
	}
	Tab CurrentTab() { return currentTab; }
	void Choose(Tab tab, std::size_t value) {
		chosen.fill(-1);
		const auto& list = lists[TabIndex(tab)];
		for (std::size_t i = 0; i < list.size(); ++i) {
			if (list[i].value == value) {
				chosen[TabIndex(tab)] = static_cast<std::int32_t>(i);
				SetTab(tab);
			}
		}
	}

	void Draw(std::uint8_t* playerInterface) {
		HideGamePanels(playerInterface);
		shown = false;
		if (Load<std::int32_t>(playerInterface + Game::Offset::InterfaceOptionsOpen) != 0 || !MakeView(playerInterface)) return;
		const auto origin = LayoutToScreen(playerInterface, PanelLeft, ViewTop);
		originX = origin.x;
		originY = origin.y;
		gridTopPixels = LayoutToScreen(playerInterface, PanelLeft, GridTop).y - originY;
		shown = true;

		const auto& list = lists[TabIndex(currentTab)];
		const auto mouse = MousePosition();
		std::int32_t over = -1;
		const auto cells = Cells(list);
		const auto count = static_cast<std::int32_t>(cells.size());
		for (std::int32_t i = 0; i < count; ++i) {
			std::int32_t left, top, right, bottom;
			CellBox(playerInterface, cells[static_cast<std::size_t>(i)], left, top, right, bottom);
			const auto x = mouse.x - originX;
			const auto y = mouse.y - originY;
			if (x >= left && x < right && y >= top && y < bottom) over = i;
		}
		hovered = over;

		const auto chosenHere = chosen[TabIndex(currentTab)];
		const bool panelOut = Load<std::uint8_t>(playerInterface + Game::Offset::InterfacePanelOut) != 0;
		const auto state = std::to_wstring(TabIndex(currentTab)) + L":" + std::to_wstring(over) + L":" + std::to_wstring(chosenHere) + L":" + std::to_wstring(list.size()) + (panelOut ? L":out" : L":in");
		// Sending a view to the screen is slow at high resolutions, so only when something on it has changed.
		if (state != drawn) {
			drawn = state;
			using UploadFn = void(__fastcall*)(std::uint8_t* forInterface, void* unused, std::uint8_t* forView);
			// Cleared here rather than with the game's 0x42CA70. GuiTextCache holds that back until a text call shows the
			// view has changed, and a tab with nothing chosen draws no text, so the last tab's icons and name stayed.
			std::memset(Load<std::uint8_t*>(view.data() + Game::Offset::ViewPixels), 0,
				static_cast<std::size_t>(Load<std::int32_t>(view.data() + Game::Offset::ViewPitch)) * static_cast<std::size_t>(Load<std::int32_t>(view.data() + Game::Offset::ViewPixelHeight)));
			const auto shownName = over >= 0 ? over : chosenHere;
			if (shownName >= 0 && shownName < count) DrawName(playerInterface, list[static_cast<std::size_t>(shownName)].name);
			for (std::int32_t i = 0; i < count; ++i) {
				std::int32_t left, top, right, bottom;
				CellBox(playerInterface, cells[static_cast<std::size_t>(i)], left, top, right, bottom);
				const auto side = (std::min)(right - left, bottom - top) - 2 * BorderPixels;
				if (i == chosenHere || i == over) FillRect(left, top, left + side + 2 * BorderPixels, top + side + 2 * BorderPixels, i == chosenHere ? 0xFFFFA800u : 0xFFE0E040u);
				else FillRect(left, top, left + side + 2 * BorderPixels, top + side + 2 * BorderPixels, list[static_cast<std::size_t>(i)].mark != 0 ? list[static_cast<std::size_t>(i)].mark : 0xFF202020u);
				DrawIcon(list[static_cast<std::size_t>(i)], left + BorderPixels, top + BorderPixels, side);
			}
			// The game's button draws over this view, so LevelEditor leaves that button undrawn while the panel is out.
			if (panelOut) {
				const auto iconTopLeft = LayoutToScreen(playerInterface, TerrainTabIconLeft, TerrainTabIconTop);
				const auto iconBottomRight = LayoutToScreen(playerInterface, TerrainTabIconLeft + TerrainTabIconSide, TerrainTabIconTop + TerrainTabIconSide);
				Item tabIcon;
				tabIcon.sheet = L"TerrainIcons.bmp";
				tabIcon.icon = TerrainTabIconCell;
				DrawIcon(tabIcon, iconTopLeft.x - originX, iconTopLeft.y - originY, (std::min)(iconBottomRight.x - iconTopLeft.x, iconBottomRight.y - iconTopLeft.y));
			}
			GameFunction<UploadFn>(Game::Address::GuiViewUpload)(playerInterface, nullptr, view.data());
		}
		using DrawFn = void(__fastcall*)(std::uint8_t* forInterface, void* unused, std::uint8_t* forView, std::int32_t x, std::int32_t y, std::int32_t flags);
		GameFunction<DrawFn>(Game::Address::GuiViewDraw)(playerInterface, nullptr, view.data(), originX, originY, 8);
	}

	bool MouseOver() {
		if (!shown) return false;
		const auto mouse = MousePosition();
		const auto x = mouse.x - originX;
		const auto y = mouse.y - originY;
		return x >= 0 && x < pixelWidth && y >= gridTopPixels && y < pixelHeight;
	}
	const Item* Click() {
		if (!shown || hovered < 0) return nullptr;
		const auto& list = lists[TabIndex(currentTab)];
		if (static_cast<std::size_t>(hovered) >= list.size()) return nullptr;
		chosen.fill(-1);
		chosen[TabIndex(currentTab)] = hovered;
		return &list[static_cast<std::size_t>(hovered)];
	}
	const Item* ItemUnderMouse() {
		if (!shown || hovered < 0) return nullptr;
		const auto& list = lists[TabIndex(currentTab)];
		return static_cast<std::size_t>(hovered) < list.size() ? &list[static_cast<std::size_t>(hovered)] : nullptr;
	}
	void SetItemLook(Tab tab, std::size_t value, std::string name, std::uint32_t mark) {
		for (auto& item : lists[TabIndex(tab)]) {
			if (item.value != value) continue;
			item.name = std::move(name);
			item.mark = mark;
			drawn.clear();
			return;
		}
	}
	void ReleaseView(std::uint8_t* playerInterface) {
		if (viewMade) {
			using FreeFn = void(__fastcall*)(std::uint8_t* forInterface, void* unused, std::uint8_t* forView);
			GameFunction<FreeFn>(Game::Address::GuiViewFree)(playerInterface, nullptr, view.data());
		}
		viewMade = false;
		shown = false;
		drawn.clear();
	}
}
