#include "WidescreenGui.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Memory.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <iterator>
#include <vector>
#include "Logger.h"

namespace KeeperForge2 {
	namespace {
		constexpr double LayoutWidth = 2560.0;
		constexpr double LayoutHeight = 1920.0;
		constexpr std::int32_t LayoutRight = 2560;

		// The in-game GUI manager (the one built from the game's window table). Every other manager (the main menu)
		// uses the centered mapping below.
		void* gameManager = nullptr;

		std::int32_t ManagerField(const void* manager, std::uintptr_t offset) { return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<std::uintptr_t>(manager) + offset); }
		std::int32_t ScreenWidth(const void* manager) { return ManagerField(manager, Game::Offset::GuiManagerWidth); }
		std::int32_t ScreenHeight(const void* manager) { return ManagerField(manager, Game::Offset::GuiManagerHeight); }
		bool IsGameManager(const void* manager) { return manager != nullptr && manager == gameManager; }

		// Layout units beyond the 4:3 area on each side when the layout keeps its shape and is centered.
		double SideExtra(std::int32_t width, std::int32_t height) {
			if (width <= 0 || height <= 0) { return 0.0; }
			const double extra = (width / (height / LayoutHeight) - LayoutWidth) / 2.0;
			return extra > 0.5 ? extra : 0.0;
		}

		// ---- In-game: the layout keeps its shape and starts at the screen's left edge. It must be a plain scale with
		// no offset, because the game also passes sizes (not only positions) through the point conversion. The spare
		// width is on the right; windows are moved into it by id (below).
		std::int32_t GameLayoutToScreenX(std::int32_t width, std::int32_t height, std::int32_t x) {
			if (SideExtra(width, height) == 0.0) { return (x * width) / 2560; }
			return static_cast<std::int32_t>(std::lround(x * (height / LayoutHeight)));
		}
		std::int32_t GameScreenToLayoutX(std::int32_t width, std::int32_t height, std::int32_t x) {
			if (SideExtra(width, height) == 0.0) { return (x * 2560) / width; }
			return static_cast<std::int32_t>(std::lround(x / (height / LayoutHeight)));
		}

		// ---- Main menu: the 4:3 layout keeps its shape and is centered; the 3D background fills the rest. Much of the
		// menu is drawn at fixed pixel positions (list rows, bars, map previews, scroll bars) that never pass through
		// these conversions, so the centring is not done here: the conversions are a plain scale, and while the menu
		// draws, the frame it draws into starts at the centered area instead (RenderWindowsDetour). Everything drawn
		// moves together. The mouse is moved the other way to match.
		std::int32_t MenuMarginPixels(std::int32_t width, std::int32_t height) {
			if (SideExtra(width, height) == 0.0) { return 0; }
			return (width - static_cast<std::int32_t>(std::lround(height * LayoutWidth / LayoutHeight))) / 2;
		}
		// True while the menu draws: the manager's stored mouse is then already relative to the centered area.
		bool menuMouseShifted = false;
		// True while the menu's mouse handler runs: the positions it passes on are then already relative to it.
		bool menuEventShifted = false;
		// Which mouse a conversion is given: the manager's stored one, or a position passed in.
		enum class MouseSource { Stored, Passed };
		std::int32_t MenuLayoutToScreenX(std::int32_t width, std::int32_t height, std::int32_t x) {
			if (SideExtra(width, height) == 0.0) { return (x * width) / 2560; }
			return static_cast<std::int32_t>(std::lround(x * (height / LayoutHeight)));
		}
		std::int32_t MenuScreenToLayoutX(std::int32_t width, std::int32_t height, std::int32_t x, MouseSource source) {
			if (SideExtra(width, height) == 0.0) { return (x * 2560) / width; }
			const bool shifted = source == MouseSource::Stored ? menuMouseShifted : menuEventShifted;
			const auto margin = shifted ? 0 : MenuMarginPixels(width, height);
			return static_cast<std::int32_t>(std::lround((x - margin) / (height / LayoutHeight)));
		}

		// Vertical scaling is already uniform on a wide screen, so it stays exactly the game's integer math.
		std::int32_t LayoutToScreenY(std::int32_t height, std::int32_t y) { return (y * height) / 1920; }
		std::int32_t ScreenToLayoutY(std::int32_t height, std::int32_t y) { return (y * 1920) / height; }

		std::int32_t LayoutToScreenX(const void* manager, std::int32_t x) {
			const auto width = ScreenWidth(manager);
			const auto height = ScreenHeight(manager);
			return IsGameManager(manager) ? GameLayoutToScreenX(width, height, x) : MenuLayoutToScreenX(width, height, x);
		}
		std::int32_t ScreenToLayoutX(const void* manager, std::int32_t width, std::int32_t height, std::int32_t x, MouseSource source) {
			return IsGameManager(manager) ? GameScreenToLayoutX(width, height, x) : MenuScreenToLayoutX(width, height, x, source);
		}

		// ---- In-game windows. Each top-level window keeps its shape and moves with the side of the screen it
		// belongs to; buttons are placed relative to their window, so they follow. Ids are the game's window ids.
		enum class Anchor { Center, Left, Right, Stretch };
		Anchor AnchorForWindow(std::int32_t id) {
			switch (id) {
				// Bottom-left control panel: item panels (rooms, spells, workshop, creatures and their pages), the panel
				// tabs, the minimap, objectives/notifications, the top-left info box and the multiplayer counters row.
				case 1: case 2: case 3: case 4: case 5: case 6:
				case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17:
				case 18: case 20: case 21: case 39:
				case 45: case 46: case 47: case 48: case 49: case 50:
					return Anchor::Left;
				// Bottom-right: summon horny (keeper icon), alarms, hero portal control.
				case 41: case 43: case 44:
					return Anchor::Right;
				// Top info bar: starts after the top-left box and runs to the right edge.
				case 53:
					return Anchor::Stretch;
				// Options, dialogs, messages and everything else stay where the 4:3 layout puts them.
				default:
					return Anchor::Center;
			}
		}
		// How far to move a window and how much to widen it, in layout units.
		bool WindowAdjustment(const void* window, std::int32_t& shift, std::int32_t& widen) {
			const auto base = reinterpret_cast<std::uintptr_t>(window);
			const auto* manager = *reinterpret_cast<void* const*>(base + Game::Offset::WindowManager);
			const auto parent = *reinterpret_cast<const std::uintptr_t*>(base + Game::Offset::WindowParent);
			// Nested windows are placed inside their parent, which already moved.
			if (!IsGameManager(manager) || parent != 0) { return false; }
			const auto extra = static_cast<std::int32_t>(std::lround(SideExtra(ScreenWidth(manager), ScreenHeight(manager))));
			if (extra == 0) { return false; }
			// The layout sits at the left edge with all the spare width (2 * extra) on the right. Dialogs are not moved:
			// their text buttons are drawn from the button's own coordinates without the window's position (0x428A30),
			// so moving the window would move their icons away from their labels and backgrounds.
			switch (AnchorForWindow(*reinterpret_cast<const std::int32_t*>(base + Game::Offset::WindowId))) {
				case Anchor::Right: shift = 2 * extra; widen = 0; return true;
				case Anchor::Stretch: shift = 0; widen = 2 * extra; return true;
				default: return false;
			}
		}

		// Both are thiscall on a window, fill a { minX, minY, maxX, maxY } layout box and return it: the window's
		// rectangle (used for drawing and hit tests) and its origin for the windows and buttons inside it.
		using WindowBoxFn = std::int32_t*(__fastcall*)(void* window, void* unused, std::int32_t* out);
		WindowBoxFn originalWindowRect = nullptr;
		WindowBoxFn originalWindowOrigin = nullptr;
		void AdjustWindowBox(void* window, std::int32_t* box) {
			std::int32_t shift = 0;
			std::int32_t widen = 0;
			if (!WindowAdjustment(window, shift, widen)) { return; }
			box[0] += shift;
			box[2] += shift + widen;
		}
		std::int32_t* __fastcall WindowRectDetour(void* window, void* unused, std::int32_t* out) {
			std::int32_t* result = originalWindowRect(window, unused, out);
			AdjustWindowBox(window, out);
			return result;
		}
		std::int32_t* __fastcall WindowOriginDetour(void* window, void* unused, std::int32_t* out) {
			std::int32_t* result = originalWindowOrigin(window, unused, out);
			AdjustWindowBox(window, out);
			return result;
		}

		// Remembers which manager is built from the game's window table.
		using CreateElementsFn = int(__fastcall*)(void* manager, void* unused, void* table, void* owner);
		CreateElementsFn originalCreateElements = nullptr;
		int __fastcall CreateElementsDetour(void* manager, void* unused, void* table, void* owner) {
			if (reinterpret_cast<std::uintptr_t>(table) == Game::Resolve(Game::Address::GameWindowTable)) { gameManager = manager; }
			return originalCreateElements(manager, unused, table, owner);
		}

		// All four are thiscall on the GUI manager and return the output pointer.
		using ConvertFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* out, const std::int32_t* in);
		using MouseFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* out);
		ConvertFn originalBoxToScreen = nullptr;
		ConvertFn originalPointToScreen = nullptr;
		ConvertFn originalPointToLayout = nullptr;
		MouseFn originalMouseToLayout = nullptr;

		// Box is { minX, minY, maxX, maxY }.
		std::int32_t* __fastcall BoxToScreenDetour(void* manager, void*, std::int32_t* out, const std::int32_t* in) {
			std::int32_t box[4] = { in[0], in[1], in[2], in[3] };
			// In-game pieces drawn straight against the 4:3 layout's right edge (the bottom stone, the brass rods, their
			// end caps) follow the screen's right edge: across the whole layout widens to the whole screen, against the
			// right edge moves with it. The left edge already is the screen's left edge. Windows that were moved never
			// end exactly on 2560, so they are not moved twice.
			if (IsGameManager(manager) && box[2] == LayoutRight) {
				const auto spare = 2 * static_cast<std::int32_t>(std::lround(SideExtra(ScreenWidth(manager), ScreenHeight(manager))));
				if (box[0] != 0) { box[0] += spare; }
				box[2] += spare;
			}
			out[1] = LayoutToScreenY(ScreenHeight(manager), box[1]);
			out[3] = LayoutToScreenY(ScreenHeight(manager), box[3]);
			out[0] = LayoutToScreenX(manager, box[0]);
			out[2] = LayoutToScreenX(manager, box[2]);
			return out;
		}
		std::int32_t* __fastcall PointToScreenDetour(void* manager, void*, std::int32_t* out, const std::int32_t* in) {
			std::int32_t point[2] = { in[0], in[1] };
			// The layout's right edge, as a position or as a size, is the screen's right edge, as for boxes above. The
			// possession HUD (0x420D10) draws its bottom rod at x 0 with the size (2560, 88) from this conversion, so it
			// stopped at the 4:3 width (screenshot at 3840x2160: it ended at 2880).
			if (IsGameManager(manager) && point[0] == LayoutRight) {
				point[0] += 2 * static_cast<std::int32_t>(std::lround(SideExtra(ScreenWidth(manager), ScreenHeight(manager))));
			}
			out[0] = LayoutToScreenX(manager, point[0]);
			out[1] = LayoutToScreenY(ScreenHeight(manager), point[1]);
			return out;
		}
		std::int32_t* __fastcall PointToLayoutDetour(void* manager, void*, std::int32_t* out, const std::int32_t* in) {
			const std::int32_t point[2] = { in[0], in[1] };
			out[0] = ScreenToLayoutX(manager, ScreenWidth(manager), ScreenHeight(manager), point[0], MouseSource::Passed);
			out[1] = ScreenToLayoutY(ScreenHeight(manager), point[1]);
			return out;
		}
		std::int32_t* __fastcall MouseToLayoutDetour(void* manager, void*, std::int32_t* out) {
			out[0] = ScreenToLayoutX(manager, ScreenWidth(manager), ScreenHeight(manager), ManagerField(manager, Game::Offset::GuiManagerMouseX), MouseSource::Stored);
			out[1] = ScreenToLayoutY(ScreenHeight(manager), ManagerField(manager, Game::Offset::GuiManagerMouseY));
			return out;
		}

		// Two hit-test functions convert the mouse with their own inline "x*2560/width, y*1920/height" instead of the
		// functions above, using the game object's screen size. Rather than patch that math, they get a mouse
		// position that their formula turns into the layout point our mapping gives.
		std::int32_t GameScreenWidth() { return *reinterpret_cast<const std::int32_t*>(Game::Resolve(Game::Address::GameScreenWidth)); }
		std::int32_t GameScreenHeight() { return *reinterpret_cast<const std::int32_t*>(Game::Resolve(Game::Address::GameScreenHeight)); }
		// Smallest value v with v*size/span (integer division) == layoutValue.
		std::int32_t InvertGameDivision(std::int32_t layoutValue, std::int32_t size, std::int32_t span) {
			if (size <= 0) { return layoutValue; }
			const double exact = static_cast<double>(layoutValue) * size / span;
			return static_cast<std::int32_t>(layoutValue >= 0 ? std::ceil(exact) : std::floor(exact));
		}
		void PrepareMouseForGameMath(const void* manager, std::int32_t& x, std::int32_t& y, MouseSource source) {
			const auto width = GameScreenWidth();
			const auto height = GameScreenHeight();
			x = InvertGameDivision(ScreenToLayoutX(manager, width, height, x, source), width, 2560);
			y = InvertGameDivision(ScreenToLayoutY(height, y), height, 1920);
		}
		using HoverTestFn = int(__fastcall*)(void* manager, void* unused);
		using ClickFn = int(__fastcall*)(void* manager, void* unused, std::int32_t x, std::int32_t y, std::int32_t a, std::int32_t b);
		HoverTestFn originalHoverTest = nullptr;
		ClickFn originalClick = nullptr;
		// Reads the manager's stored mouse position, so swap it in for the call only.
		int __fastcall HoverTestDetour(void* manager, void* unused) {
			auto& mouseX = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(manager) + Game::Offset::GuiManagerMouseX);
			auto& mouseY = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(manager) + Game::Offset::GuiManagerMouseY);
			const auto realX = mouseX;
			const auto realY = mouseY;
			PrepareMouseForGameMath(manager, mouseX, mouseY, MouseSource::Stored);
			const int result = originalHoverTest(manager, unused);
			mouseX = realX;
			mouseY = realY;
			return result;
		}
		int __fastcall ClickDetour(void* manager, void* unused, std::int32_t x, std::int32_t y, std::int32_t a, std::int32_t b) {
			PrepareMouseForGameMath(manager, x, y, MouseSource::Passed);
			return originalClick(manager, unused, x, y, a, b);
		}

		// Hover handlers that compare the front end's stored mouse (manager + 0xAC, read as front end + 0x30FF0) with
		// boxes from the box conversion themselves: list boxes, horizontal sliders, vertical sliders (scroll bars).
		// They run outside the drawing pass, so they get the stored mouse relative to the centered area while they
		// run. All are cdecl (button, front end).
		using ButtonHandlerFn = int(__cdecl*)(void* button, std::uint8_t* frontEnd);
		ButtonHandlerFn originalListHover = nullptr;
		ButtonHandlerFn originalSliderHover = nullptr;
		ButtonHandlerFn originalScrollBarHover = nullptr;
		int CallWithMenuMouse(ButtonHandlerFn original, void* button, std::uint8_t* frontEnd) {
			if (frontEnd == nullptr || menuMouseShifted) { return original(button, frontEnd); }
			const auto manager = reinterpret_cast<std::uintptr_t>(frontEnd) + Game::Offset::MenuGuiManager;
			const auto margin = MenuMarginPixels(ScreenWidth(reinterpret_cast<void*>(manager)), ScreenHeight(reinterpret_cast<void*>(manager)));
			if (margin <= 0) { return original(button, frontEnd); }
			auto& mouseX = *reinterpret_cast<std::int32_t*>(manager + Game::Offset::GuiManagerMouseX);
			const auto realMouseX = mouseX;
			mouseX -= margin;
			menuMouseShifted = true;
			const int result = original(button, frontEnd);
			menuMouseShifted = false;
			mouseX = realMouseX;
			return result;
		}
		int __cdecl ListHoverDetour(void* button, std::uint8_t* frontEnd) { return CallWithMenuMouse(originalListHover, button, frontEnd); }
		int __cdecl SliderHoverDetour(void* button, std::uint8_t* frontEnd) { return CallWithMenuMouse(originalSliderHover, button, frontEnd); }
		int __cdecl ScrollBarHoverDetour(void* button, std::uint8_t* frontEnd) { return CallWithMenuMouse(originalScrollBarHover, button, frontEnd); }

		// The menu's mouse handler: cdecl (int, int, x, y, front end). Besides passing the position on, it tests it
		// against button boxes itself (sliders and the like, in screen pixels from the box conversion), so it gets the
		// position relative to the centered area.
		using MouseEventFn = int(__cdecl*)(std::int32_t a, std::int32_t b, std::int32_t x, std::int32_t y, std::uint8_t* frontEnd);
		MouseEventFn originalMouseEvent = nullptr;
		int __cdecl MouseEventDetour(std::int32_t a, std::int32_t b, std::int32_t x, std::int32_t y, std::uint8_t* frontEnd) {
			if (frontEnd == nullptr || menuEventShifted) { return originalMouseEvent(a, b, x, y, frontEnd); }
			const auto* manager = frontEnd + Game::Offset::MenuGuiManager;
			const auto margin = MenuMarginPixels(ScreenWidth(manager), ScreenHeight(manager));
			if (margin <= 0) { return originalMouseEvent(a, b, x, y, frontEnd); }
			menuEventShifted = true;
			const int result = originalMouseEvent(a, b, x - margin, y, frontEnd);
			menuEventShifted = false;
			return result;
		}

		// Draws a manager's windows: thiscall (owner), ret 4. For the menu, the owner is the front end, which draws
		// into a description of the frame it fills in just before (pixel format, pixels, width, height, pitch).
		// Starting that description at the centered 4:3 area, 4:3 wide, moves every piece of the menu there, whether
		// it is placed through the conversions above or at fixed pixel positions. The pitch stays, so rows still line
		// up. It is filled in again every frame.
		using RenderWindowsFn = int(__fastcall*)(void* manager, void* unused, void* owner);
		using FrontEndDrawFn = int(__fastcall*)(void* frontEnd);
		using SetDrawSurfaceFn = void(__cdecl*)(void* frame);
		RenderWindowsFn originalRenderWindows = nullptr;
		FrontEndDrawFn originalFrontEndDraw = nullptr;
		void SetDrawSurface(void* frame) { reinterpret_cast<SetDrawSurfaceFn>(Game::Resolve(Game::Address::SetDrawSurface))(frame); }

		// The shift is in place from the window renderer until the end of the front end's 2D pass (0x5334C0), which
		// draws one more window (0x2F, at 0x547C20) after the renderer returns.
		struct MenuShift {
			bool active = false;
			std::uint8_t* frame = nullptr;
			std::int32_t margin = 0;
			std::int32_t bytesPerPixel = 0;
			// The frame as the game filled it in, to recognize other descriptions of the same frame.
			std::uintptr_t framePixels = 0;
			std::int32_t frameWidth = 0;
			void* boundSurface = nullptr;
			std::int32_t* mouseX = nullptr;
			std::int32_t realMouseX = 0;
		};
		MenuShift menuShift;
		bool insideFrontEndDraw = false;
		// The front end being drawn, and whether a piece is drawing across the whole screen (see the header).
		std::uint8_t* drawingFrontEnd = nullptr;
		bool fullScreenMenuDraw = false;
		std::uintptr_t& FramePixels(std::uint8_t* frame) { return *reinterpret_cast<std::uintptr_t*>(frame + Game::Offset::FramePixels); }
		std::int32_t& FrameWidth(std::uint8_t* frame) { return *reinterpret_cast<std::int32_t*>(frame + Game::Offset::FrameWidth); }

		void EndMenuShift() {
			if (!menuShift.active) { return; }
			menuShift.active = false;
			menuMouseShifted = false;
			*menuShift.mouseX = menuShift.realMouseX;
			// The frame description is filled in again next frame; only the drawing binding needs putting back.
			if (menuShift.boundSurface != nullptr && menuShift.boundSurface != menuShift.frame) { SetDrawSurface(menuShift.boundSurface); }
		}

		int __fastcall RenderWindowsDetour(void* manager, void* unused, void* owner) {
			const auto ownerBase = reinterpret_cast<std::uintptr_t>(owner);
			if (menuShift.active || IsGameManager(manager) || owner == nullptr || ownerBase + Game::Offset::MenuGuiManager != reinterpret_cast<std::uintptr_t>(manager)) {
				return originalRenderWindows(manager, unused, owner);
			}
			const auto width = ScreenWidth(manager);
			const auto margin = MenuMarginPixels(width, ScreenHeight(manager));
			auto* frame = reinterpret_cast<std::uint8_t*>(ownerBase + Game::Offset::FrontEndFrame);
			const auto bits = *reinterpret_cast<const std::int32_t*>(frame + Game::Offset::FrameBits);
			// Only a whole, unshifted frame.
			if (margin <= 0 || FramePixels(frame) == 0 || bits <= 0 || bits % 8 != 0 || FrameWidth(frame) != width) {
				return originalRenderWindows(manager, unused, owner);
			}
			menuShift.active = true;
			menuShift.frame = frame;
			menuShift.margin = margin;
			menuShift.bytesPerPixel = bits / 8;
			menuShift.framePixels = FramePixels(frame);
			menuShift.frameWidth = FrameWidth(frame);
			FramePixels(frame) += static_cast<std::uintptr_t>(margin * menuShift.bytesPerPixel);
			FrameWidth(frame) -= 2 * margin;
			// Text and other 2D drawing go to the bound draw surface instead (bound to the unshifted frame earlier), so
			// the shifted frame is bound while the menu draws.
			menuShift.boundSurface = *reinterpret_cast<void**>(Game::Resolve(Game::Address::CurrentDrawSurface));
			SetDrawSurface(frame);
			// Renderers compare the stored mouse with what they drew, so it moves into the same space while they run.
			menuShift.mouseX = reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(manager) + Game::Offset::GuiManagerMouseX);
			menuShift.realMouseX = *menuShift.mouseX;
			*menuShift.mouseX -= margin;
			menuMouseShifted = true;
			const int result = originalRenderWindows(manager, unused, owner);
			if (!insideFrontEndDraw) { EndMenuShift(); }
			return result;
		}
		int __fastcall FrontEndDrawDetour(void* frontEnd) {
			insideFrontEndDraw = true;
			drawingFrontEnd = static_cast<std::uint8_t*>(frontEnd);
			const int result = originalFrontEndDraw(frontEnd);
			insideFrontEndDraw = false;
			drawingFrontEnd = nullptr;
			EndMenuShift();
			return result;
		}

		// Some of the menu (slider knobs, map previews, text in the small font) is copied straight onto the back
		// buffer with DirectDraw (cdecl (result, destination, x, y, source, source rect, flags)), past the frame
		// description. Those copies move by the same margin while the menu draws centered.
		using BlitFn = std::int32_t*(__cdecl*)(std::int32_t* result, void* destination, std::int32_t x, std::int32_t y, void* source, void* sourceRect, std::uint32_t flags);
		BlitFn originalBlit = nullptr;
		std::int32_t* __cdecl BlitDetour(std::int32_t* result, void* destination, std::int32_t x, std::int32_t y, void* source, void* sourceRect, std::uint32_t flags) {
			if (menuShift.active && !fullScreenMenuDraw && drawingFrontEnd != nullptr
				&& destination == *reinterpret_cast<void**>(drawingFrontEnd + Game::Offset::FrontEndBackBuffer)) {
				x += menuShift.margin;
			}
			return originalBlit(result, destination, x, y, source, sourceRect, flags);
		}

		// Anything that binds another, unshifted description of the same frame while the menu draws centered gets a
		// shifted copy instead (callers are logged once, to find them).
		using BindFn = void(__cdecl*)(void* frame);
		BindFn originalBind = nullptr;
		alignas(4) std::uint8_t shiftedCopy[Game::FrameDescriptionSize]{};
		void LogCallerOnce(const char* what, const void* caller) {
			static std::vector<const void*> seen;
			if (std::ranges::contains(seen, caller) || seen.size() >= 64) { return; }
			seen.push_back(caller);
			char text[128]{};
			std::snprintf(text, sizeof(text), "Widescreen: centered menu %s from 0x%08X.", what,
				static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(caller) - Memory::GetModuleBase() + Game::ImageBase));
			Logger::Trace(text);
		}
		void __cdecl BindDetour(void* frame) {
			auto* description = static_cast<std::uint8_t*>(frame);
			if (menuShift.active && !fullScreenMenuDraw && description != nullptr && description != menuShift.frame
				&& description != shiftedCopy && FramePixels(description) == menuShift.framePixels && FrameWidth(description) == menuShift.frameWidth) {
				LogCallerOnce("moved a frame binding", _ReturnAddress());
				std::memcpy(shiftedCopy, description, sizeof(shiftedCopy));
				FramePixels(shiftedCopy) += static_cast<std::uintptr_t>(menuShift.margin * menuShift.bytesPerPixel);
				FrameWidth(shiftedCopy) -= 2 * menuShift.margin;
				frame = shiftedCopy;
			}
			originalBind(frame);
		}

		// The icon that flies in with a sparkle trail when a notification (fight, and others) arrives is an animated
		// mesh whose size is a fixed float (object + 0x18, blending to + 0x1C) whatever the screen size; its path is
		// already in screen pixels. Scale both sizes with the screen height (480 lines = original) for the draw, which
		// also updates them, and scale them back after. The update is linear in them, so nothing drifts.
		using FlyingIconDrawFn = void(__fastcall*)(std::uint8_t* icon, void* unused, void* engine, std::int32_t x, std::int32_t y);
		FlyingIconDrawFn originalFlyingIconDraw = nullptr;
		void __fastcall FlyingIconDrawDetour(std::uint8_t* icon, void* unused, void* engine, std::int32_t x, std::int32_t y) {
			const auto height = GameScreenHeight();
			const float scale = height > 0 ? static_cast<float>(height / 480.0) : 1.0f;
			auto* size = reinterpret_cast<float*>(icon + Game::Offset::FlyingIconSize);
			auto* targetSize = reinterpret_cast<float*>(icon + Game::Offset::FlyingIconTargetSize);
			*size *= scale;
			*targetSize *= scale;
			originalFlyingIconDraw(icon, unused, engine, x, y);
			*size /= scale;
			*targetSize /= scale;
		}

		struct Hook {
			std::uintptr_t virtualAddress;
			void* detour;
			void** original;
		};
	}

	// The minimap's "N" is drawn 8x8 pixels with a -3/-6 pixel nudge whatever the screen size; only its position is
	// scaled. Scale the size and nudges with the screen height like the rest of the GUI (480 lines = original).
	void WidescreenGui::OnScreenMode(std::int32_t height) {
		if (height <= 0) { return; }
		const double scale = height / 480.0;
		const auto scaled = [scale](int pixels) { return static_cast<std::int32_t>(std::lround(pixels * scale)); };
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::MinimapNorthSize) + 1, scaled(8));
		// The nudges are signed 8-bit operands.
		Memory::Write<std::int8_t>(Game::Resolve(Game::Address::MinimapNorthNudgeY) + 2, static_cast<std::int8_t>((std::min)(scaled(6), 127)));
		Memory::Write<std::int8_t>(Game::Resolve(Game::Address::MinimapNorthNudgeX) + 2, static_cast<std::int8_t>((std::min)(scaled(3), 127)));

		// The icons of the creatures and objects held in the hand are drawn at their 32 pixel size and laid out on a
		// 32 pixel grid next to the cursor, whatever the screen size. Scale both the same way.
		static constexpr std::int32_t handIconGrid[9][2] = { { 0, 0 }, { 32, 0 }, { 64, 0 }, { 96, 0 }, { 0, 32 }, { 32, 32 }, { 64, 32 }, { 96, 32 }, { 128, 32 } };
		static std::int32_t writtenGrid[9][2]{};
		static bool gridWritten = false;
		auto* grid = reinterpret_cast<std::int32_t(*)[2]>(Game::Resolve(Game::Address::HandIconOffsets));
		// Only a table the game filled in as expected (or the one written here last time).
		if (std::memcmp(grid, handIconGrid, sizeof(handIconGrid)) == 0 || (gridWritten && std::memcmp(grid, writtenGrid, sizeof(writtenGrid)) == 0)) {
			for (std::size_t i = 0; i < std::size(handIconGrid); ++i) {
				writtenGrid[i][0] = scaled(handIconGrid[i][0]);
				writtenGrid[i][1] = scaled(handIconGrid[i][1]);
			}
			std::memcpy(grid, writtenGrid, sizeof(writtenGrid));
			gridWritten = true;
			Memory::Write<std::int32_t>(Game::Resolve(Game::Address::HandIconSize) + 1, scaled(32));
			Memory::Write<std::int32_t>(Game::Resolve(Game::Address::HandMoreIconSize) + 1, scaled(32));
		}
		// The icon of what the cursor will build or cast, drawn next to the cursor the same way, and the number beside it.
		for (const auto site : { Game::Address::CursorIconSize, Game::Address::CursorIconSizeOther, Game::Address::CursorIconSizeThird }) {
			Memory::Write<std::int32_t>(Game::Resolve(site) + 1, scaled(32));
		}
		Memory::Write<std::int8_t>(Game::Resolve(Game::Address::CursorIconTextOffsetX) + 2, static_cast<std::int8_t>((std::min)(scaled(30), 127)));
		Memory::Write<std::int8_t>(Game::Resolve(Game::Address::CursorIconTextOffsetY) + 2, static_cast<std::int8_t>((std::min)(scaled(10), 127)));
		// The Define Keys list's columns are fixed pixels, so the larger fonts ran the action names into the keys.
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::DefineKeysModifierColumn) + 1, scaled(200));
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::DefineKeysKeyColumn) + 1, scaled(300));
		// The bar under a text box, such as the save name's, is in fixed pixels too. Its height and end pieces scale with the
		// rest. Twice an end piece's width has to fit a signed 8-bit operand, so very large screens get slightly narrow ends.
		const auto barEnd = (std::min)(scaled(16), 63);
		const auto barHeight = scaled(32);
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::BarLeftEndWidth) + 4, barEnd);
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::BarLeftEndHeight) + 4, barHeight);
		Memory::Write<std::int8_t>(Game::Resolve(Game::Address::BarMiddleStart) + 2, static_cast<std::int8_t>(barEnd));
		Memory::Write<std::int8_t>(Game::Resolve(Game::Address::BarMiddleShorter) + 2, static_cast<std::int8_t>(2 * barEnd));
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::BarMiddleHeight) + 4, barHeight);
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::BarRightEndWidth) + 4, barEnd);
		Memory::Write<std::int32_t>(Game::Resolve(Game::Address::BarRightEndHeight) + 4, barHeight);
		Memory::Write<std::int8_t>(Game::Resolve(Game::Address::BarRightEndStart) + 2, static_cast<std::int8_t>(-barEnd));
	}

	std::int32_t WidescreenGui::BeginFullScreenMenuDraw() {
		if (!menuShift.active) { return 0; }
		fullScreenMenuDraw = true;
		FramePixels(menuShift.frame) -= static_cast<std::uintptr_t>(menuShift.margin * menuShift.bytesPerPixel);
		FrameWidth(menuShift.frame) += 2 * menuShift.margin;
		SetDrawSurface(menuShift.frame);
		return menuShift.margin;
	}
	void WidescreenGui::EndFullScreenMenuDraw() {
		if (!menuShift.active) { return; }
		fullScreenMenuDraw = false;
		FramePixels(menuShift.frame) += static_cast<std::uintptr_t>(menuShift.margin * menuShift.bytesPerPixel);
		FrameWidth(menuShift.frame) -= 2 * menuShift.margin;
		SetDrawSurface(menuShift.frame);
	}

	bool WidescreenGui::Install() {
		const Hook hooks[] = {
			{ Game::Address::GuiCreateElements, reinterpret_cast<void*>(&CreateElementsDetour), reinterpret_cast<void**>(&originalCreateElements) },
			{ Game::Address::GuiWindowRect, reinterpret_cast<void*>(&WindowRectDetour), reinterpret_cast<void**>(&originalWindowRect) },
			{ Game::Address::GuiWindowOrigin, reinterpret_cast<void*>(&WindowOriginDetour), reinterpret_cast<void**>(&originalWindowOrigin) },
			{ Game::Address::GuiBoxToScreen, reinterpret_cast<void*>(&BoxToScreenDetour), reinterpret_cast<void**>(&originalBoxToScreen) },
			{ Game::Address::GuiPointToScreen, reinterpret_cast<void*>(&PointToScreenDetour), reinterpret_cast<void**>(&originalPointToScreen) },
			{ Game::Address::GuiPointToLayout, reinterpret_cast<void*>(&PointToLayoutDetour), reinterpret_cast<void**>(&originalPointToLayout) },
			{ Game::Address::GuiMouseToLayout, reinterpret_cast<void*>(&MouseToLayoutDetour), reinterpret_cast<void**>(&originalMouseToLayout) },
			{ Game::Address::GuiHoverTest, reinterpret_cast<void*>(&HoverTestDetour), reinterpret_cast<void**>(&originalHoverTest) },
			{ Game::Address::GuiClick, reinterpret_cast<void*>(&ClickDetour), reinterpret_cast<void**>(&originalClick) },
			{ Game::Address::GuiRenderWindows, reinterpret_cast<void*>(&RenderWindowsDetour), reinterpret_cast<void**>(&originalRenderWindows) },
			{ Game::Address::FrontEndDraw2D, reinterpret_cast<void*>(&FrontEndDrawDetour), reinterpret_cast<void**>(&originalFrontEndDraw) },
			{ Game::Address::BlitSurface, reinterpret_cast<void*>(&BlitDetour), reinterpret_cast<void**>(&originalBlit) },
			{ Game::Address::SetDrawSurface, reinterpret_cast<void*>(&BindDetour), reinterpret_cast<void**>(&originalBind) },
			{ Game::Address::FrontEndMouseEvent, reinterpret_cast<void*>(&MouseEventDetour), reinterpret_cast<void**>(&originalMouseEvent) },
			{ Game::Address::MenuListHover, reinterpret_cast<void*>(&ListHoverDetour), reinterpret_cast<void**>(&originalListHover) },
			{ Game::Address::MenuSliderHover, reinterpret_cast<void*>(&SliderHoverDetour), reinterpret_cast<void**>(&originalSliderHover) },
			{ Game::Address::MenuScrollBarHover, reinterpret_cast<void*>(&ScrollBarHoverDetour), reinterpret_cast<void**>(&originalScrollBarHover) },
		};
		// Drawing and the mouse must switch together, or clicks would land in the wrong place; so all or nothing.
		std::size_t created = 0;
		for (; created < std::size(hooks); ++created) {
			if (!HookManager::CreateHook(Game::Resolve(hooks[created].virtualAddress), hooks[created].detour, hooks[created].original)) { break; }
		}
		bool ok = created == std::size(hooks);
		for (std::size_t i = 0; ok && i < std::size(hooks); ++i) { ok = HookManager::EnableHook(Game::Resolve(hooks[i].virtualAddress)); }
		if (!ok) {
			for (std::size_t i = 0; i < created; ++i) { HookManager::RemoveHook(Game::Resolve(hooks[i].virtualAddress)); }
			return false;
		}
		// Independent of the rest: without it the flying notification icon is only drawn too small.
		const auto flyingIconDraw = Game::Resolve(Game::Address::FlyingIconDraw);
		if (!Memory::Matches(flyingIconDraw, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x2B, 0xA1, 0x64, 0x00 })
			|| !HookManager::CreateHook(flyingIconDraw, reinterpret_cast<void*>(&FlyingIconDrawDetour), reinterpret_cast<void**>(&originalFlyingIconDraw))
			|| !HookManager::EnableHook(flyingIconDraw)) {
			HookManager::RemoveHook(flyingIconDraw);
			Logger::Warning("Widescreen: could not hook the flying notification icon, it stays at its original size.");
		}
		return true;
	}
}
