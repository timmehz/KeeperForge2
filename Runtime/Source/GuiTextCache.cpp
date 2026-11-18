#include "GuiTextCache.h"
#include "Config.h"
#include "GameAddresses.h"
#include "StringUtils.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		// 0x42CB60, thiscall on the player interface with 0x70 bytes of arguments (ret 0x70):
		// view, x, y, text, color*, alignment flags, clear first, a font object passed by value (0x4C bytes, which
		// the function destroys), upload to the screen.
		constexpr std::size_t ArgumentCount = 0x70 / 4;
		constexpr std::size_t ViewArgument = 0;
		constexpr std::size_t TextArgument = 3;
		constexpr std::size_t ColorArgument = 4;
		constexpr std::size_t ClearArgument = 6;
		constexpr std::size_t UploadArgument = 27;
		constexpr std::size_t FontArgument = 7;
		// The font copy (arguments 7 to 25) holds objects made fresh for each call, so it is not compared.
		constexpr std::size_t FontArgumentsEnd = 26;
		constexpr std::size_t MaxText = 4096;
		constexpr double RefreshSeconds = 5.0;
		// After a view's text changes, it is drawn in full for this long. A run that changes partway only draws from the
		// change on, so text that keeps changing would otherwise show that part and the whole by turns: a flicker.
		constexpr double FullDrawAfterChangeSeconds = 0.5;
		constexpr std::size_t MaxCallsPerRun = 256;
		bool deferUploads = false;

		struct Call {
			void* self = nullptr;
			std::array<std::uint32_t, ArgumentCount> arguments{};
			std::string text;
			// The game reads 5 bytes. Callers keep them on the stack, so anything after them is left over from other calls.
			std::array<std::uint8_t, 5> color{};
			static bool Compared(std::size_t i) { return i != TextArgument && i != ColorArgument && (i < FontArgument || i >= FontArgumentsEnd); }
			bool Matches(const Call& other) const {
				if (self != other.self || text != other.text || color != other.color) { return false; }
				for (std::size_t i = 0; i < ArgumentCount; ++i) {
					if (Compared(i) && arguments[i] != other.arguments[i]) { return false; }
				}
				return true;
			}
		};
		// What a view shows is what its last run of calls (from one "clear first" call to the next) drew.
		struct ViewState {
			// The calls of the last run, and of the run in progress.
			std::vector<Call> shown;
			std::vector<Call> current;
			// Calls in this run are left out while they match the last run.
			bool skipping = false;
			bool skippedAny = false;
			// This run stopped matching after leaving calls out, so the view may be off; draw the next run for real.
			bool redrawNext = false;
			std::int64_t lastDrawn = 0;
			std::int64_t fullDrawUntil = 0;
			// A clear (0x42CA70) that was held back because the run looked like a repeat; done late if it is not.
			bool clearPending = false;
			void* clearSelf = nullptr;
			std::uint32_t clearFlag = 0;
			// Text calls that asked to upload the view; done once, right before the view is next drawn.
			bool uploadPending = false;
			void* uploadSelf = nullptr;
		};
		std::unordered_map<std::uint32_t, ViewState> views;
		LARGE_INTEGER frequency{};
		std::int64_t Now() {
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			return counter.QuadPart;
		}

		Call Describe(void* self, const std::uint32_t* arguments) {
			Call call;
			call.self = self;
			std::memcpy(call.arguments.data(), arguments, sizeof(call.arguments));
			if (const auto* text = reinterpret_cast<const char*>(arguments[TextArgument])) { call.text.assign(text, strnlen(text, MaxText)); }
			if (const auto* color = reinterpret_cast<const std::uint8_t*>(arguments[ColorArgument])) { std::memcpy(call.color.data(), color, call.color.size()); }
			return call;
		}

		using DestroyFontFn = void(__fastcall*)(void* font, void* unused);
		// Clears a whole view: thiscall (view, patterned), ret 8.
		using ClearViewFn = void(__fastcall*)(void* self, void* unused, std::uint32_t view, std::uint32_t patterned);
		ClearViewFn originalClearView = nullptr;
		// Uploads all of a view's tiles: thiscall (view), ret 4.
		using UploadViewFn = void(__fastcall*)(void* self, void* unused, std::uint32_t view);
		void UploadIfPending(ViewState& view, std::uint32_t viewAddress) {
			if (!view.uploadPending) { return; }
			view.uploadPending = false;
			reinterpret_cast<UploadViewFn>(Game::Resolve(Game::Address::GuiViewUpload))(view.uploadSelf, nullptr, viewAddress);
		}

		// A run starts when a view is cleared, either by 0x42CA70 or by a text call asked to clear first. A run that
		// left calls out but ended shorter than the one before leaves the view off (the earlier run's extra text is
		// still in it), so the next run is drawn for real.
		void StartRun(ViewState& view, std::uint32_t viewAddress, std::int64_t now) {
			// Not drawn since its last change; upload before it changes again.
			UploadIfPending(view, viewAddress);
			if (view.skippedAny && view.current.size() != view.shown.size()) { view.redrawNext = true; }
			view.shown = std::move(view.current);
			view.current.clear();
			view.skippedAny = false;
			view.clearPending = false;
			view.skipping = !view.redrawNext && view.lastDrawn != 0 && now >= view.fullDrawUntil
				&& static_cast<double>(now - view.lastDrawn) < RefreshSeconds * static_cast<double>(frequency.QuadPart);
			view.redrawNext = false;
		}

		int Decide(void* self, std::uint32_t* arguments, std::int64_t now);
		// Returns true when the call can be left out because the view already shows its result.
		extern "C" int __cdecl GuiTextCacheSkip(void* self, std::uint32_t* arguments) { return Decide(self, arguments, Now()); }
		// Returns true when the clear can wait, because the run that follows will probably repeat the last one.
		extern "C" int __cdecl GuiTextCacheClear(void* self, std::uint32_t* arguments) {
			auto& view = views[arguments[0]];
			StartRun(view, arguments[0], Now());
			if (!view.skipping || view.shown.empty()) { return 0; }
			view.clearPending = true;
			view.clearSelf = self;
			view.clearFlag = arguments[1];
			// The view keeps its earlier content for now, which counts as leaving something out.
			view.skippedAny = true;
			return 1;
		}
		int Decide(void* self, std::uint32_t* arguments, std::int64_t now) {
			auto& view = views[arguments[ViewArgument]];
			if (arguments[ClearArgument] != 0) { StartRun(view, arguments[ViewArgument], now); }
			// A view that is never cleared through here has no runs to compare; just draw it.
			if (view.current.size() >= MaxCallsPerRun) {
				view.current.clear();
				view.shown.clear();
				view.skipping = false;
				return 0;
			}
			Call call = Describe(self, arguments);
			const std::size_t index = view.current.size();
			const bool same = view.skipping && index < view.shown.size() && view.shown[index].Matches(call);
			view.current.push_back(std::move(call));
			if (same) {
				view.skippedAny = true;
				// The function would have destroyed its copy of the font; do it here instead.
				reinterpret_cast<DestroyFontFn>(Game::Resolve(Game::Address::TextFontDestroy))(&arguments[FontArgument], nullptr);
				return 1;
			}
			// Drawing for real from here on in this run. A held-back clear is done now. If calls were left out before
			// this one, the view is not exactly what this run would draw (the earlier text, or a clear that wiped what
			// was left out), so the next run is drawn for real.
			if (view.clearPending) {
				view.clearPending = false;
				originalClearView(view.clearSelf, nullptr, arguments[ViewArgument], view.clearFlag);
			}
			if (view.skippedAny) {
				view.redrawNext = true;
				view.fullDrawUntil = now + static_cast<std::int64_t>(FullDrawAfterChangeSeconds * static_cast<double>(frequency.QuadPart));
			}
			view.skipping = false;
			view.lastDrawn = now;
			// Each line of a panel asks to upload the whole view; do it once, before the view is drawn.
			if (deferUploads && arguments[UploadArgument] != 0) {
				arguments[UploadArgument] = 0;
				view.uploadPending = true;
				view.uploadSelf = self;
			}
			return 0;
		}

		// Draws a view onto the screen: thiscall (view, x, y, flags), ret 0x10.
		void* originalDrawView = nullptr;
		extern "C" void __cdecl GuiTextCacheBeforeDraw(std::uint32_t* arguments) {
			const auto found = views.find(arguments[0]);
			if (found != views.end()) { UploadIfPending(found->second, arguments[0]); }
		}
		__declspec(naked) void DrawViewStub() {
			__asm {
				lea eax, [esp + 4]
				push ecx
				push eax
				call GuiTextCacheBeforeDraw
				add esp, 4
				pop ecx
				jmp dword ptr [originalDrawView]
			}
		}

		__declspec(naked) void ClearViewStub() {
			__asm {
				lea eax, [esp + 4]
				push ecx
				push eax
				push ecx
				call GuiTextCacheClear
				add esp, 8
				pop ecx
				test eax, eax
				jnz skip
				jmp dword ptr [originalClearView]
			skip:
				ret 8
			}
		}

		void* originalViewText = nullptr;
		__declspec(naked) void ViewTextStub() {
			__asm {
				lea eax, [esp + 4]
				push ecx
				push eax
				push ecx
				call GuiTextCacheSkip
				add esp, 8
				pop ecx
				test eax, eax
				jnz skip
				jmp dword ptr [originalViewText]
			skip:
				ret 0x70
			}
		}
	}

	void GuiTextCache::Install() {
		if (StringUtils::ToLower(Config::GetString("panel_text_cache", "on")) == "off") {
			Logger::Info("Panel text: panel_text_cache is off, panels are redrawn every frame as the game does.");
			return;
		}
		QueryPerformanceFrequency(&frequency);
		const auto target = Game::Resolve(Game::Address::GuiViewText);
		const auto destroy = Game::Resolve(Game::Address::TextFontDestroy);
		if (!Memory::Matches(target, { 0x6A, 0xFF, 0x68, 0x6E, 0xA0, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 })
			|| !Memory::Matches(destroy, { 0x56, 0x8B, 0xF1, 0xC7, 0x06 })) {
			Logger::Warning("Panel text: unexpected game code, panels are redrawn every frame as before.");
			return;
		}
		if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&ViewTextStub), &originalViewText) || !HookManager::EnableHook(target)) {
			HookManager::RemoveHook(target);
			Logger::Warning("Panel text: failed to hook the panel text code, panels are redrawn every frame as before.");
			return;
		}
		// Panels that clear their view first and then draw line by line (the level objective) start their runs here.
		const auto clear = Game::Resolve(Game::Address::GuiViewClear);
		if (!Memory::Matches(clear, { 0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0, 0x74, 0x0D })
			|| !HookManager::CreateHook(clear, reinterpret_cast<void*>(&ClearViewStub), reinterpret_cast<void**>(&originalClearView)) || !HookManager::EnableHook(clear)) {
			HookManager::RemoveHook(clear);
			Logger::Warning("Panel text: could not follow view clears, panels drawn line by line are redrawn every frame as before.");
		}
		// Uploads are only put off when they can be done right before the view is drawn.
		const auto draw = Game::Resolve(Game::Address::GuiViewDraw);
		if (Memory::Matches(draw, { 0x6A, 0xFF, 0x68, 0x8B, 0xA0, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 })
			&& HookManager::CreateHook(draw, reinterpret_cast<void*>(&DrawViewStub), &originalDrawView) && HookManager::EnableHook(draw)) {
			deferUploads = true;
			Logger::Debug("Panel text: repeated panel text is left out, changed panels upload once.");
		} else {
			HookManager::RemoveHook(draw);
			Logger::Warning("Panel text: could not follow view drawing, changed panels upload once per line as before.");
		}
	}
}
