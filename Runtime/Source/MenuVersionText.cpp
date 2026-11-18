#include "MenuVersionText.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "ModManager.h"
#include "WidescreenGui.h"
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>

#ifndef KEEPERFORGE2_VERSION
#define KEEPERFORGE2_VERSION "0.0.0"
#endif

namespace KeeperForge2 {
	namespace {
		// The game's text drawing pieces used by its own version renderer (0x537E10).
		using ButtonRectFn = std::int32_t*(__fastcall*)(void* button, void* unused, std::int32_t* box);
		using BoxToScreenFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* out, const std::int32_t* in);
		using TextDrawerFn = void(__fastcall*)(void* drawer, void* unused);
		using TextAlignFn = std::int32_t*(__fastcall*)(void* drawer, void* unused, std::int32_t* status, std::int32_t align);
		using DrawTextFn = std::int32_t*(__fastcall*)(void* drawer, void* unused, std::int32_t* status, std::int32_t* box, const void* text, void* font, std::int32_t flags);
		using ConvertTextFn = void(__cdecl*)(const wchar_t* text, void* out, std::int32_t size);
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }

		// Draws one line of text left-aligned at the bottom of a screen box, like the game's version text.
		void DrawLine(std::int32_t* box, const wchar_t* text) {
			// The text drawer is a 0x80 byte object on the game's stack; give it room to spare.
			alignas(16) std::uint8_t drawer[0x200]{};
			alignas(4) std::uint8_t converted[0x100]{};
			std::int32_t status = 0;
			GameFunction<TextDrawerFn>(Game::Address::TextDrawerConstruct)(drawer, nullptr);
			GameFunction<TextAlignFn>(Game::Address::TextDrawerAlignHorizontal)(drawer, nullptr, &status, 0);
			GameFunction<TextAlignFn>(Game::Address::TextDrawerAlignVertical)(drawer, nullptr, &status, 2);
			GameFunction<ConvertTextFn>(Game::Address::ConvertText)(text, converted, 0x40);
			GameFunction<DrawTextFn>(Game::Address::TextDrawerDraw)(drawer, nullptr, &status, box, converted, reinterpret_cast<void*>(Game::Resolve(Game::Address::VersionFont)), 0);
			GameFunction<TextDrawerFn>(Game::Address::TextDrawerDestruct)(drawer, nullptr);
		}

		// Replaces the game's version renderer: cdecl (button, menu).
		void __cdecl RenderVersionDetour(void* button, void* menu) {
			std::int32_t layoutBox[4]{};
			std::int32_t box[4]{};
			GameFunction<ButtonRectFn>(Game::Address::GuiButtonRect)(button, nullptr, layoutBox);
			void* manager = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(menu) + Game::Offset::MenuGuiManager);
			GameFunction<BoxToScreenFn>(Game::Address::GuiBoxToScreen)(manager, nullptr, box, layoutBox);
			const auto screenWidth = *reinterpret_cast<const std::int32_t*>(reinterpret_cast<std::uintptr_t>(manager) + Game::Offset::GuiManagerWidth);
			// The menu is drawn centered at 4:3; the version text belongs at the screen's own left edge. The box spans the
			// layout's whole width, so it starts at the screen's left edge once drawing covers the whole screen.
			const auto margin = WidescreenGui::BeginFullScreenMenuDraw();
			box[2] += 2 * margin;
			// A little in from the left edge (curved and overscanned screens cut the edge off).
			box[0] += screenWidth * 15 / 1000;
			// Two lines in the game's box: its version on the upper one, ours under it.
			const std::int32_t middle = (box[1] + box[3]) / 2;
			std::int32_t upper[4] = { box[0], box[1], box[2], middle };
			std::int32_t lower[4] = { box[0], middle, box[2], box[3] };
			wchar_t text[96]{};
			const auto major = *reinterpret_cast<const std::uint32_t*>(Game::Resolve(Game::Address::GameVersionMajor));
			const auto minor = *reinterpret_cast<const std::uint32_t*>(Game::Resolve(Game::Address::GameVersionMinor));
			std::swprintf(text, std::size(text), L"V%lu.%lu", static_cast<unsigned long>(major), static_cast<unsigned long>(minor));
			DrawLine(upper, text);
			const auto mods = ModManager::GetModCount();
			std::swprintf(text, std::size(text), L"KeeperForge2-v%hs (%zu %ls loaded)", KEEPERFORGE2_VERSION, mods, mods == 1 ? L"mod" : L"mods");
			DrawLine(lower, text);
			WidescreenGui::EndFullScreenMenuDraw();
		}
		using RenderVersionFn = void(__cdecl*)(void* button, void* menu);
		RenderVersionFn originalRenderVersion = nullptr;
	}

	void MenuVersionText::Install() {
		const auto target = Game::Resolve(Game::Address::MenuRenderVersion);
		if (!Memory::Matches(target, { 0x6A, 0xFF, 0x68, 0xCB, 0xDC, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 })) {
			Logger::Warning("Menu version text: unexpected game code, left as it is.");
			return;
		}
		if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&RenderVersionDetour), reinterpret_cast<void**>(&originalRenderVersion)) || !HookManager::EnableHook(target)) {
			HookManager::RemoveHook(target);
			Logger::Warning("Menu version text: failed to hook the version text, left as it is.");
		}
	}
}
