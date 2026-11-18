#include "CreatureCountInput.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		// Config tables are packed, so their fields are read and written byte-wise.
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// The Creatures tab shows 8 rows at a time: a slider, its number, and a toggle used by the other tabs.
		constexpr std::int32_t Rows = 8;
		constexpr std::int32_t OptionsWindowId = 0x23;
		constexpr std::int32_t FirstSliderId = 0x27E;
		constexpr std::int32_t FirstNumberId = 0x286;
		constexpr std::int32_t FirstToggleId = 0x28E;
		// Our click areas over the numbers.
		constexpr std::int32_t FirstClickId = 9100;
		constexpr std::int32_t ButtonListEnd = -1;
		constexpr std::size_t MaxButtons = 128;
		constexpr std::uint8_t KindClick = 0;
		constexpr std::uint8_t KindSlider = 3;
		constexpr std::uint8_t KindText = 7;
		constexpr std::int32_t MaxCount = 32;
		constexpr std::size_t MaxDigits = 2;
		// While typing, the number blinks (shown, then hidden, this many ms each).
		constexpr ULONGLONG BlinkMs = 400;
		// The options window's ticks stop when it closes: typing ends if none ran for this long.
		constexpr ULONGLONG TickGoneMs = 250;
		constexpr std::uint32_t Enter = '\r';
		constexpr std::uint32_t Escape = 0x1B;
		constexpr std::uint32_t Backspace = 0x08;

		using TickFn = void(__cdecl*)(std::uint8_t* button, std::uint8_t* frontEnd);
		using RenderFn = char(__cdecl*)(std::uint8_t* button, std::uint8_t* frontEnd);
		using ClickFn = void(__cdecl*)(std::int32_t, std::int32_t, std::uint8_t* frontEnd);
		using FindButtonFn = std::uint8_t*(__fastcall*)(std::uint8_t* frontEnd, void* unused, std::int32_t id, std::int32_t window);
		using SetValueFn = std::int32_t(__fastcall*)(std::uint8_t* slider, void* unused, std::int32_t value);
		using TypedFn = std::int32_t(__cdecl*)(std::uint32_t message, std::uint32_t character);

		std::vector<std::uint8_t> buttonList;
		std::array<TickFn, Rows> sliderTicks{};
		TypedFn originalTyped = nullptr;

		// The row being typed into (-1: none), what was typed, and the front end it belongs to.
		std::int32_t typingRow = -1;
		std::string typed;
		std::uint8_t* typingFrontEnd = nullptr;
		ULONGLONG lastTick = 0;
		bool mouseWasDown = false;
		// Shown empty while the number blinks off (an empty text reads the same in any text format).
		std::array<std::uint8_t, 8> noText{};

		std::uint8_t* FindButton(std::uint8_t* frontEnd, std::int32_t id) {
			return reinterpret_cast<FindButtonFn>(Game::Resolve(Game::Address::FindButton))(frontEnd, nullptr, id, OptionsWindowId);
		}
		bool Shown(const std::uint8_t* button) { return button != nullptr && Load<std::int32_t>(button + Game::Offset::ButtonShown) == 1; }
		bool Typing() { return typingRow >= 0 && GetTickCount64() - lastTick < TickGoneMs; }
		void StopTyping() {
			typingRow = -1;
			typed.clear();
		}

		// Moves the row's slider to the typed number. Its own tick then writes the row's count and shows the number,
		// as when the slider is dragged.
		void ApplyTyped() {
			if (typingFrontEnd == nullptr) return;
			auto* slider = FindButton(typingFrontEnd, FirstSliderId + typingRow);
			if (slider == nullptr) return;
			std::int32_t value = typed.empty() ? 0 : std::stoi(typed);
			if (value > MaxCount) value = MaxCount;
			if (value < 0) value = 0;
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(slider);
			reinterpret_cast<SetValueFn>(vtable[Game::Offset::SliderSetValue / sizeof(std::uintptr_t)])(slider, nullptr, value);
		}

		// Every frame, through each row's slider tick (those run on every tab: the game's own ones check whether their
		// number is shown): our click area is shown with its number, and a mouse press anywhere ends typing (a press
		// on a number then starts it again for that row).
		void AfterSliderTick(std::int32_t row, std::uint8_t* frontEnd) {
			auto* number = FindButton(frontEnd, FirstNumberId + row);
			auto* click = FindButton(frontEnd, FirstClickId + row);
			const bool shown = Shown(number);
			if (click != nullptr) Store<std::int32_t>(click + Game::Offset::ButtonShown, shown ? 1 : 0);
			if (row == typingRow && !shown) StopTyping();
			if (row != 0) return;
			lastTick = GetTickCount64();
			const bool down = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
			if (down && !mouseWasDown) StopTyping();
			mouseWasDown = down;
		}
		template<std::int32_t Row> void __cdecl SliderTick(std::uint8_t* button, std::uint8_t* frontEnd) {
			sliderTicks[Row](button, frontEnd);
			AfterSliderTick(Row, frontEnd);
		}
		template<std::int32_t Row> void __cdecl OnNumberClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) {
			typingRow = Row;
			typingFrontEnd = frontEnd;
			typed.clear();
			// This press started typing, so the tick does not end it.
			mouseWasDown = true;
		}
		char __cdecl RenderClickArea(std::uint8_t*, std::uint8_t*) { return 0; }

		// The numbers: while typed into, they blink.
		char __cdecl RenderNumber(std::uint8_t* button, std::uint8_t* frontEnd) {
			const auto render = reinterpret_cast<RenderFn>(Game::Resolve(Game::Address::CreatureCountLabelRender));
			const bool hidden = Typing() && Load<std::int32_t>(button + Game::Offset::ButtonId) == FirstNumberId + typingRow && (GetTickCount64() / BlinkMs) % 2 == 1;
			if (!hidden) return render(button, frontEnd);
			const auto text = Load<std::uint32_t>(button + Game::Offset::ButtonText);
			Store<std::uint32_t>(button + Game::Offset::ButtonText, reinterpret_cast<std::uint32_t>(noText.data()));
			const char result = render(button, frontEnd);
			Store<std::uint32_t>(button + Game::Offset::ButtonText, text);
			return result;
		}

		// Typed characters while a number is typed into all go to it, so only digits can be entered.
		std::int32_t __cdecl TypedDetour(std::uint32_t message, std::uint32_t character) {
			if (message != WM_CHAR || !Typing()) return originalTyped(message, character);
			if (character >= '0' && character <= '9') {
				// A third digit starts a new number.
				if (typed.size() >= MaxDigits) typed.clear();
				typed.push_back(static_cast<char>(character));
				ApplyTyped();
			}
			else if (character == Backspace) {
				if (!typed.empty()) typed.pop_back();
				ApplyTyped();
			}
			else if (character == Enter || character == Escape) StopTyping();
			return 0;
		}

		const std::array<TickFn, Rows> OurTicks = { &SliderTick<0>, &SliderTick<1>, &SliderTick<2>, &SliderTick<3>, &SliderTick<4>, &SliderTick<5>, &SliderTick<6>, &SliderTick<7> };
		const std::array<ClickFn, Rows> OurClicks = { &OnNumberClick<0>, &OnNumberClick<1>, &OnNumberClick<2>, &OnNumberClick<3>, &OnNumberClick<4>, &OnNumberClick<5>,
			&OnNumberClick<6>, &OnNumberClick<7> };

		// Copies the options window's buttons: the sliders' ticks and the numbers' renderer are ours, and a click
		// area over each number is added before the end marker.
		bool BuildButtonList(const std::uint8_t* buttons) {
			std::size_t count = 0;
			std::array<std::size_t, Rows> sliders{};
			std::array<std::size_t, Rows> numbers{};
			std::array<bool, Rows> sliderFound{};
			std::array<bool, Rows> numberFound{};
			const std::uint8_t* clickTemplate = nullptr;
			const auto numberRender = static_cast<std::uint32_t>(Game::Resolve(Game::Address::CreatureCountLabelRender));
			for (; count < MaxButtons; ++count) {
				const auto* entry = buttons + count * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				const auto kind = entry[Game::Offset::ButtonConfigKind];
				if (id == ButtonListEnd) break;
				if (id >= FirstClickId && id < FirstClickId + Rows) return false;
				if (id >= FirstSliderId && id < FirstSliderId + Rows && kind == KindSlider && Load<std::int32_t>(entry + Game::Offset::SliderConfigMaximum) == MaxCount
					&& Load<std::uint32_t>(entry + Game::Offset::ButtonConfigTick) != 0) {
					sliders[id - FirstSliderId] = count;
					sliderFound[id - FirstSliderId] = true;
				}
				if (id >= FirstNumberId && id < FirstNumberId + Rows && kind == KindText && Load<std::uint32_t>(entry + Game::Offset::ButtonConfigRender) == numberRender) {
					numbers[id - FirstNumberId] = count;
					numberFound[id - FirstNumberId] = true;
				}
				// The first row's toggle (used on the other tabs): a plain click button that opens no window.
				if (id == FirstToggleId && kind == KindClick) clickTemplate = entry;
			}
			if (count == MaxButtons || clickTemplate == nullptr) return false;
			for (std::int32_t row = 0; row < Rows; ++row) {
				if (!sliderFound[row] || !numberFound[row]) return false;
			}
			buttonList.assign((count + Rows + 1) * Game::ButtonConfigSize, 0);
			std::memcpy(buttonList.data(), buttons, count * Game::ButtonConfigSize);
			for (std::int32_t row = 0; row < Rows; ++row) {
				auto* slider = buttonList.data() + sliders[row] * Game::ButtonConfigSize;
				sliderTicks[row] = reinterpret_cast<TickFn>(Load<std::uint32_t>(slider + Game::Offset::ButtonConfigTick));
				Store<std::uint32_t>(slider + Game::Offset::ButtonConfigTick, reinterpret_cast<std::uint32_t>(OurTicks[row]));
				auto* number = buttonList.data() + numbers[row] * Game::ButtonConfigSize;
				Store<std::uint32_t>(number + Game::Offset::ButtonConfigRender, reinterpret_cast<std::uint32_t>(&RenderNumber));
				// The click area: a click button with the number's place and size, drawing nothing.
				auto* click = buttonList.data() + (count + row) * Game::ButtonConfigSize;
				std::memcpy(click, clickTemplate, Game::ButtonConfigSize);
				Store<std::int32_t>(click + Game::Offset::ButtonConfigId, FirstClickId + row);
				Store<std::uint32_t>(click + Game::Offset::ButtonConfigClick, reinterpret_cast<std::uint32_t>(OurClicks[row]));
				Store<std::uint32_t>(click + Game::Offset::ButtonConfigRightClick, 0);
				Store<std::int32_t>(click + Game::Offset::ButtonConfigClickArgument, 0);
				Store<std::int32_t>(click + Game::Offset::ButtonConfigClickArgument + 4, 0);
				std::memcpy(click + Game::Offset::ButtonConfigX, number + Game::Offset::ButtonConfigX, 8);
				std::memcpy(click + Game::Offset::ButtonConfigTextWidth, number + Game::Offset::ButtonConfigWidth, 4);
				Store<std::uint32_t>(click + Game::Offset::ButtonConfigTick, 0);
				Store<std::uint32_t>(click + Game::Offset::ButtonConfigRender, reinterpret_cast<std::uint32_t>(&RenderClickArea));
				Store<std::int32_t>(click + Game::Offset::ButtonConfigTextId, 0);
				Store<std::int32_t>(click + Game::Offset::ButtonConfigTextIndex, 0);
			}
			std::memcpy(buttonList.data() + (count + Rows) * Game::ButtonConfigSize, buttons + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			return true;
		}
	}

	void CreatureCountInput::Install() {
		const auto windowConfig = Game::Resolve(Game::Address::LobbyOptionsWindowConfig);
		const auto buttons = Memory::Read<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons);
		const auto typedAt = Game::Resolve(Game::Address::GameTypedCharacter);
		if (Memory::Read<std::uint8_t>(windowConfig) != OptionsWindowId || buttons != Game::Resolve(Game::Address::LobbyOptionsButtons)
			|| !Memory::Matches(typedAt, { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04, 0x50, 0x51, 0xB9 })
			|| !BuildButtonList(reinterpret_cast<const std::uint8_t*>(buttons))) {
			Logger::Warning("Creature counts: unexpected game code, the numbers cannot be typed into.");
			return;
		}
		if (!HookManager::CreateHook(typedAt, reinterpret_cast<void*>(&TypedDetour), reinterpret_cast<void**>(&originalTyped)) || !HookManager::EnableHook(typedAt)) {
			HookManager::RemoveHook(typedAt);
			Logger::Warning("Creature counts: could not hook typed characters, the numbers cannot be typed into.");
			return;
		}
		Memory::Write<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(buttonList.data()));
		Logger::Debug("Creature counts: the numbers on the lobby's Creatures tab can be clicked and typed into (0 to 32).");
	}
}
