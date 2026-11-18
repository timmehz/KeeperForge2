#include "WidescreenFonts.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include <cmath>
#include <cstdint>
#include <string>

namespace KeeperForge2 {
	namespace {
		struct FontSize {
			int size;
			std::uint32_t id;
			// False for ids we inferred rather than saw the game use.
			bool confirmed;
		};
		// Sizes 6-24 are the game's own size table (0x553690). 26-72 are the larger FONT_INGAME files that ship with
		// the game; their ids are inferred from the file order and are only used if the game accepts them.
		constexpr FontSize fontSizes[] = {
			{ 6, 0x444, true }, { 8, 0x445, true }, { 10, 0x447, true }, { 12, 0x448, true }, { 14, 0x449, true },
			{ 16, 0x44A, true }, { 18, 0x44B, true }, { 20, 0x44C, true }, { 22, 0x44D, true }, { 24, 0x44E, true },
			{ 26, 0x44F, false }, { 28, 0x450, false }, { 36, 0x451, false }, { 48, 0x452, false }, { 72, 0x453, false },
		};
		// At or below this height the game's own choice is right (it was built for up to 1024x768).
		constexpr std::uint32_t OriginalMaxHeight = 768;

		using FontIdFn = std::uint32_t(__cdecl*)(std::uint32_t size, std::uint32_t screenWidth);
		using FontSetupFn = int(__cdecl*)(std::uint32_t screenWidth);
		FontIdFn originalFontId = nullptr;
		FontSetupFn originalFontSetup = nullptr;
		bool useInferredSizes = true;

		std::uint32_t ScreenHeight() { return *reinterpret_cast<const std::uint32_t*>(Game::Resolve(Game::Address::GameScreenHeight)); }

		// The closest shipped font to a size made for a referenceHeight pixel tall screen, scaled to this height.
		const FontSize& FontForHeight(int size, std::uint32_t height, bool allowInferred, double referenceHeight = 480.0) {
			const double wanted = size * static_cast<double>(height) / referenceHeight;
			const FontSize* best = nullptr;
			for (const auto& font : fontSizes) {
				if (!font.confirmed && !allowInferred) { continue; }
				if (!best || std::abs(font.size - wanted) < std::abs(best->size - wanted)) { best = &font; }
			}
			return *best;
		}
		std::uint32_t __cdecl FontIdDetour(std::uint32_t size, std::uint32_t screenWidth) {
			const auto height = ScreenHeight();
			if (height <= OriginalMaxHeight) { return originalFontId(size, screenWidth); }
			return FontForHeight(static_cast<int>(size), height, useInferredSizes).id;
		}
		int __cdecl FontSetupDetour(std::uint32_t screenWidth) {
			const int result = originalFontSetup(screenWidth);
			if (result || !useInferredSizes || ScreenHeight() <= OriginalMaxHeight) { return result; }
			// One of the inferred font ids was not a font; stay within the confirmed sizes from now on.
			Logger::Warning("Widescreen: the game rejected a font larger than 24, in-game text is limited to size 24.");
			useInferredSizes = false;
			return originalFontSetup(screenWidth);
		}

		// Three in-game fonts are loaded once at startup by fixed id (16, 18 and 24 pixels) into global slots, so they
		// never follow the screen size; the minimap's "N" is one of them. On a tall in-game screen the slots hold
		// larger fonts of the same family, and the startup ones go back for the menu.
		struct FixedFont {
			std::uintptr_t slot;
			int size;
			std::uintptr_t original;
			std::uintptr_t scaled;
			bool resolved;
		};
		FixedFont fixedFonts[] = {
			{ Game::Address::FontSlotInGame16, 16, 0, 0, false },
			{ Game::Address::FontSlotInGame18, 18, 0, 0, false },
			{ Game::Address::FontSlotInGame24, 24, 0, 0, false },
		};
		// thiscall on the language object: (int* status, font** slot, int id), ret 0xC. Fails with status -1, slot 0.
		using ResolveFontFn = std::int32_t*(__fastcall*)(void* language, void* unused, std::int32_t* status, std::uintptr_t* slot, std::uint32_t id);
		std::uintptr_t ResolveFont(std::uint32_t id) {
			const auto resolve = reinterpret_cast<ResolveFontFn>(Game::Resolve(Game::Address::ResolveFont));
			std::int32_t status = -1;
			std::uintptr_t font = 0;
			resolve(reinterpret_cast<void*>(Game::Resolve(Game::Address::LanguageObject)), nullptr, &status, &font, id);
			return status >= 0 ? font : 0;
		}
	}

	void WidescreenFonts::OnScreenMode(std::int32_t height) {
		for (auto& font : fixedFonts) {
			auto& slot = *reinterpret_cast<std::uintptr_t*>(Game::Resolve(font.slot));
			// Remember the startup font (the slot is empty before the language loads).
			if (slot != 0 && slot != font.scaled) { font.original = slot; }
			if (height > static_cast<std::int32_t>(OriginalMaxHeight)) {
				if (!font.resolved) {
					font.resolved = true;
					// The game never adjusted these, so they keep the look they had at its usual 1024x768.
					constexpr double FixedFontReference = 768.0;
					const auto tall = static_cast<std::uint32_t>(height);
					font.scaled = ResolveFont(FontForHeight(font.size, tall, useInferredSizes, FixedFontReference).id);
					// A larger inferred id may not exist; fall back to the confirmed sizes.
					if (!font.scaled) { font.scaled = ResolveFont(FontForHeight(font.size, tall, false, FixedFontReference).id); }
					Logger::Debug("Widescreen: fixed " + std::to_string(font.size) + " px font " + (font.scaled ? "enlarged" : "kept") + " for in-game screens.");
				}
				if (font.scaled) { slot = font.scaled; }
			} else if (font.original) {
				slot = font.original;
			}
		}
	}

	bool WidescreenFonts::Install() {
		const auto idTarget = Game::Resolve(Game::Address::InGameFontId);
		const auto setupTarget = Game::Resolve(Game::Address::InGameFontSetup);
		if (!HookManager::CreateHook(idTarget, reinterpret_cast<void*>(&FontIdDetour), reinterpret_cast<void**>(&originalFontId))) { return false; }
		if (!HookManager::CreateHook(setupTarget, reinterpret_cast<void*>(&FontSetupDetour), reinterpret_cast<void**>(&originalFontSetup))) {
			HookManager::RemoveHook(idTarget);
			return false;
		}
		// The setup hook goes live first so the retry is in place before larger ids can be handed out.
		if (!HookManager::EnableHook(setupTarget) || !HookManager::EnableHook(idTarget)) {
			HookManager::RemoveHook(idTarget);
			HookManager::RemoveHook(setupTarget);
			return false;
		}
		return true;
	}
}
