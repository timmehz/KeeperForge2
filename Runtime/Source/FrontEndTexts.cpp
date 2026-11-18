#include "FrontEndTexts.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		using PrerenderFn = std::int32_t(__fastcall*)(void* frontEnd, void* unused, std::int32_t window, std::int32_t slot, std::uint8_t* entries, std::int32_t font);
		using ConvertTextFn = std::int32_t(__cdecl*)(const wchar_t* text, void* out, std::int32_t size);

		// The front end keeps room for 30 pre-rendered texts per window.
		constexpr std::size_t MaxPrerenderedTexts = 30;

		struct Registration {
			std::int32_t window;
			std::vector<FrontEndTexts::Text> texts;
			std::int32_t* firstIndex;
		};
		std::vector<Registration> registrations;
		std::mutex registrationMutex;
		PrerenderFn originalPrerender = nullptr;
		bool installed = false;
		// The list handed to the game; it reads it during the call only.
		std::vector<std::uint8_t> prerenderList;

		void WriteEntry(std::uint8_t* entry, std::int32_t buttonId, const std::wstring& text) {
			Store<std::int32_t>(entry, 0);
			Store<std::int32_t>(entry + 4, buttonId);
			reinterpret_cast<ConvertTextFn>(Game::Resolve(Game::Address::ConvertText))(text.c_str(), entry + 8, static_cast<std::int32_t>(Game::PrerenderEntrySize - 8));
		}

		std::int32_t __fastcall PrerenderDetour(void* frontEnd, void* unused, std::int32_t window, std::int32_t slot, std::uint8_t* entries, std::int32_t font) {
			std::vector<Registration*> ours;
			{
				std::lock_guard lock(registrationMutex);
				for (auto& registration : registrations) {
					if (registration.window == window) ours.push_back(&registration);
				}
			}
			if (ours.empty() || entries == nullptr) return originalPrerender(frontEnd, unused, window, slot, entries, font);
			// The list ends at button id 0.
			std::size_t count = 0;
			while (count < MaxPrerenderedTexts && Load<std::int32_t>(entries + count * Game::PrerenderEntrySize + 4) != 0) ++count;
			std::size_t added = 0;
			for (const auto* registration : ours) added += registration->texts.size();
			prerenderList.assign((count + added + 1) * Game::PrerenderEntrySize, 0);
			std::memcpy(prerenderList.data(), entries, count * Game::PrerenderEntrySize);
			std::size_t next = count;
			for (auto* registration : ours) {
				if (next + registration->texts.size() > MaxPrerenderedTexts) {
					*registration->firstIndex = -1;
					Logger::Warning("Front end: no room for more texts in window " + std::to_string(window) + ", some of our buttons are hidden.");
					continue;
				}
				*registration->firstIndex = static_cast<std::int32_t>(next);
				for (const auto& text : registration->texts) {
					WriteEntry(prerenderList.data() + next * Game::PrerenderEntrySize, text.buttonId, text.text);
					++next;
				}
			}
			// Unused room keeps the end marker (button id 0) after the last text.
			return originalPrerender(frontEnd, unused, window, slot, prerenderList.data(), font);
		}
	}

	bool FrontEndTexts::Install() {
		if (installed) return true;
		const auto prerender = Game::Resolve(Game::Address::GuiPrerenderTexts);
		if (!Memory::Matches(prerender, { 0x6A, 0xFF, 0x68, 0x0B, 0xDC, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 })) {
			Logger::Warning("Front end: unexpected text code, our menu texts are left out.");
			return false;
		}
		if (!HookManager::CreateHook(prerender, reinterpret_cast<void*>(&PrerenderDetour), reinterpret_cast<void**>(&originalPrerender))
			|| !HookManager::EnableHook(prerender)) {
			HookManager::RemoveHook(prerender);
			Logger::Warning("Front end: failed to hook the menu texts, our menu texts are left out.");
			return false;
		}
		installed = true;
		return true;
	}

	void FrontEndTexts::Add(std::int32_t window, std::vector<Text> texts, std::int32_t* firstIndex) {
		*firstIndex = -1;
		if (!installed) return;
		std::lock_guard lock(registrationMutex);
		registrations.push_back({ window, std::move(texts), firstIndex });
	}
}
