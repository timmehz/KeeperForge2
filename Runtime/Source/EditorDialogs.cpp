#include "EditorDialogs.h"
#include "Config.h"
#include "LevelFiles.h"
#include "Logger.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <set>
#include <utility>
#include <Windows.h>
#include <CommCtrl.h>

namespace KeeperForge2::EditorDialogs {
	namespace {
		namespace Offset = LevelFiles::CreatureOffset;
		namespace Flag = LevelFiles::CreatureFlag;
		using LevelFiles::CreatureNumber;

		constexpr wchar_t CreatureClass[] = L"KeeperForge2CreatureProperties";
		constexpr wchar_t PromptClass[] = L"KeeperForge2TextPrompt";
		constexpr double FixedOne = 4096.0;
		// Type ids are a byte.
		constexpr std::size_t MostCreatureTypes = 255;

		// Control ids. IDOK and IDCANCEL are Windows' own, so Enter and Esc reach them through IsDialogMessage.
		enum : int {
			IdNameLabel = 100, IdName, IdAttribute, IdValue, IdRange, IdNote, IdApply,
			IdEvil, IdGood, IdMale, IdFemale,
			IdUniqueName, IdIntroCamera, IdSound, IdTranslation,
			IdRename, IdAddType, IdDeleteType,
			IdPromptLabel, IdPromptText, IdSubNote,
			IdPlacesLabel, IdPlacesAs,
			// The row for new creatures (a type from the side panel) or for the one creature the window is open on.
			IdRowLabel, IdNewLevelLabel, IdNewLevel, IdNewDead,
			IdUnitLevelLabel, IdUnitLevel, IdUnitHealthLabel, IdUnitHealth, IdUnitGoldLabel, IdUnitGold, IdUnitDead, IdUnitOwnStats,
			IdUnitPetals, IdUnitPointsLabel, IdUnitPoints, IdUnitMost,
			IdArtSlot, IdArtType, IdArtName, IdArtNote, IdArtFieldFirst = 620, IdArtLabelFirst = 640, IdArtFlagFirst = 660,
			IdArtFootLeft = 690, IdArtFootRight, IdArtFootLeftLabel, IdArtFootRightLabel,
			IdArtButtonFirst = 560,  // the buttons that open the art window on a resource
			IdSubButtonFirst = 500,  // the buttons that open the windows of SubWindows()
			IdSubControlFirst = 1000, // a sub window's fields
			IdFlagFirst = 200,
			IdChoiceFirst = 300,
			IdAttractionFirst = 400, // kind, target, amount for each, three apart
		};

		// ---- Colors: Windows' own, or dark ones (editor_dark_windows). The game's process has no visual styles, so the controls
		// are the classic ones, which take their colors from their parent (WM_CTLCOLOR...); push buttons are drawn here when dark.
		struct Theme {
			bool dark = false;
			COLORREF back = 0, text = 0, field = 0, fieldText = 0, button = 0, buttonPressed = 0, edge = 0, disabled = 0;
			HBRUSH backBrush = nullptr, fieldBrush = nullptr, buttonBrush = nullptr, buttonPressedBrush = nullptr;
		};
		Theme theme;
		void LoadTheme() {
			const bool dark = Config::GetString("editor_dark_windows", "on") != "off";
			if (theme.backBrush != nullptr && theme.dark == dark) return;
			for (auto* brush : { theme.backBrush, theme.fieldBrush, theme.buttonBrush, theme.buttonPressedBrush }) {
				if (brush != nullptr) DeleteObject(brush);
			}
			theme = Theme{};
			theme.dark = dark;
			if (dark) {
				theme.back = RGB(32, 32, 32);
				theme.text = RGB(230, 230, 230);
				theme.field = RGB(45, 45, 45);
				theme.fieldText = RGB(235, 235, 235);
				theme.button = RGB(58, 58, 58);
				theme.buttonPressed = RGB(80, 80, 80);
				theme.edge = RGB(100, 100, 100);
				theme.disabled = RGB(130, 130, 130);
			}
			else {
				theme.back = GetSysColor(COLOR_BTNFACE);
				theme.text = GetSysColor(COLOR_BTNTEXT);
				theme.field = GetSysColor(COLOR_WINDOW);
				theme.fieldText = GetSysColor(COLOR_WINDOWTEXT);
				theme.button = GetSysColor(COLOR_BTNFACE);
				theme.buttonPressed = GetSysColor(COLOR_BTNSHADOW);
				theme.edge = GetSysColor(COLOR_BTNSHADOW);
				theme.disabled = GetSysColor(COLOR_GRAYTEXT);
			}
			theme.backBrush = CreateSolidBrush(theme.back);
			theme.fieldBrush = CreateSolidBrush(theme.field);
			theme.buttonBrush = CreateSolidBrush(theme.button);
			theme.buttonPressedBrush = CreateSolidBrush(theme.buttonPressed);
		}
		// A dark title bar (Windows 10 1809 and later; attribute 19 before 20H1). Loaded by name, so older Windows simply keeps its own.
		void DarkTitleBar(HWND window) {
			if (!theme.dark) return;
			static auto* set = reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD)>(
				[] { auto* dwm = LoadLibraryW(L"dwmapi.dll"); return dwm != nullptr ? reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr; }());
			if (set == nullptr) return;
			const BOOL on = TRUE;
			if (FAILED(set(window, 20, &on, sizeof(on)))) set(window, 19, &on, sizeof(on));
		}
		// "Copy a picture of this window" on each window's title bar menu: the window as it is on screen (PrintWindow with
		// PW_RENDERFULLCONTENT, 2), put on the clipboard, for when a screen capture tool cannot reach it.
		constexpr UINT CopyPictureCommand = 0x1F00; // below 0xF000, as Windows' own commands are
		void AddCopyPicture(HWND window) {
			if (auto* menu = GetSystemMenu(window, FALSE)) {
				AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				AppendMenuW(menu, MF_STRING, CopyPictureCommand, L"Copy a picture of this window");
			}
		}
		bool CopyPicture(HWND window) {
			RECT area{};
			if (!GetWindowRect(window, &area)) return false;
			const int width = area.right - area.left;
			const int height = area.bottom - area.top;
			auto* screen = GetDC(nullptr);
			auto* memory = CreateCompatibleDC(screen);
			auto* picture = CreateCompatibleBitmap(screen, width, height);
			ReleaseDC(nullptr, screen);
			auto* old = SelectObject(memory, picture);
			const bool printed = PrintWindow(window, memory, 2) != FALSE;
			SelectObject(memory, old);
			DeleteDC(memory);
			bool copied = false;
			if (printed && OpenClipboard(window)) {
				EmptyClipboard();
				// The clipboard owns the picture once it takes it.
				copied = SetClipboardData(CF_BITMAP, picture) != nullptr;
				CloseClipboard();
			}
			if (!copied) DeleteObject(picture);
			MessageBeep(copied ? MB_OK : MB_ICONWARNING);
			Logger::Info(copied ? "Editor windows: a picture of a window was copied to the clipboard." : "Editor windows: could not copy a picture of the window.");
			return copied;
		}
		bool IsCopyPicture(UINT message, WPARAM wParam) { return message == WM_SYSCOMMAND && (wParam & 0xFFF0) == CopyPictureCommand; }

		// Takes visual styles off a control if anything gave it some, as themed checkboxes would keep black text on the dark face.
		void Unthemed(HWND control) {
			if (!theme.dark) return;
			static auto* set = reinterpret_cast<HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR)>(
				[] { auto* ux = LoadLibraryW(L"uxtheme.dll"); return ux != nullptr ? reinterpret_cast<void*>(GetProcAddress(ux, "SetWindowTheme")) : nullptr; }());
			if (set != nullptr) set(control, L"", L"");
		}
		bool IsClass(HWND control, const wchar_t* name) {
			wchar_t found[32] = {};
			GetClassNameW(control, found, static_cast<int>(std::size(found)));
			return _wcsicmp(found, name) == 0;
		}
		// The colors every control asks its parent for. Combo boxes and edits get the field's, labels and boxes the window's.
		std::optional<LRESULT> ControlColor(UINT message, WPARAM wParam, LPARAM lParam) {
			if (message != WM_CTLCOLORSTATIC && message != WM_CTLCOLOREDIT && message != WM_CTLCOLORLISTBOX && message != WM_CTLCOLORBTN) return std::nullopt;
			auto* dc = reinterpret_cast<HDC>(wParam);
			auto* control = reinterpret_cast<HWND>(lParam);
			const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || IsClass(control, WC_COMBOBOXW) || IsClass(control, WC_EDITW);
			SetBkColor(dc, field ? theme.field : theme.back);
			SetTextColor(dc, IsWindowEnabled(control) ? (field ? theme.fieldText : theme.text) : theme.disabled);
			return reinterpret_cast<LRESULT>(field ? theme.fieldBrush : theme.backBrush);
		}
		// Push buttons are owner drawn when dark, as a classic button paints its face in Windows' colors whatever its parent says.
		bool DrawButton(const DRAWITEMSTRUCT& item) {
			if (item.CtlType != ODT_BUTTON) return false;
			const bool pressed = (item.itemState & ODS_SELECTED) != 0;
			const bool disabled = (item.itemState & ODS_DISABLED) != 0;
			RECT area = item.rcItem;
			FillRect(item.hDC, &area, pressed ? theme.buttonPressedBrush : theme.buttonBrush);
			auto* pen = CreatePen(PS_SOLID, 1, (item.itemState & ODS_FOCUS) != 0 ? theme.text : theme.edge);
			auto* oldPen = SelectObject(item.hDC, pen);
			auto* oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
			Rectangle(item.hDC, area.left, area.top, area.right, area.bottom);
			SelectObject(item.hDC, oldBrush);
			SelectObject(item.hDC, oldPen);
			DeleteObject(pen);
			wchar_t text[64] = {};
			GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
			SetBkMode(item.hDC, TRANSPARENT);
			SetTextColor(item.hDC, disabled ? theme.disabled : theme.text);
			if (pressed) OffsetRect(&area, 1, 1);
			DrawTextW(item.hDC, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);
			return true;
		}

		// The checkboxes, laid out as the official editor has them: two columns, with the Evil/Good and Male/Female pairs on rows of
		// the first. A box shows its bit set, or clear for an inverted one.
		struct FlagBox {
			const wchar_t* label;
			std::uint32_t bit;
			bool inverted;
			int column;
			int row;
		};
		constexpr std::array<FlagBox, 24> FlagBoxes{ {
			{ L"Need Body For Fight Idle", Flag::NeedBodyForFightIdle, false, 0, 0 },
			{ L"Is Worker", Flag::IsWorker, false, 0, 1 },
			{ L"Can Be Picked Up", Flag::CanBePickedUp, false, 0, 2 },
			{ L"Can Walk On Water", Flag::CanWalkOnWater, false, 0, 3 },
			{ L"Horny", Flag::Horny, false, 0, 5 },
			{ L"Can Fly", Flag::CanFly, false, 0, 6 },
			{ L"Available Via Portal", Flag::AvailableViaPortal, false, 0, 7 },
			{ L"Frees Friends On JailBreak", Flag::FreesFriendsOnJailBreak, false, 0, 8 },
			{ L"Is Unique", Flag::IsUnique, false, 0, 9 },
			{ L"MPD Random Invader", Flag::MpdRandomInvader, false, 0, 11 },
			{ L"Generate Dead Body", Flag::GenerateDeadBody, false, 0, 12 },
			{ L"Train When Idle", Flag::NotTrainWhenIdle, true, 1, 0 },
			{ L"Can Be Slapped", Flag::CanBeSlapped, false, 1, 1 },
			{ L"Only Attackable By Horny", Flag::OnlyAttackableByHorny, false, 1, 2 },
			{ L"Can Walk On Lava", Flag::CanWalkOnLava, false, 1, 3 },
			{ L"Is Immune To Turncoat", Flag::IsImmuneToTurncoat, false, 1, 4 },
			{ L"Is Immune To Chicken", Flag::IsImmuneToChicken, false, 1, 5 },
			{ L"Reveals Adjacent Traps", Flag::RevealsAdjacentTraps, false, 1, 6 },
			{ L"Can Be Hypnotised", Flag::CanBeHypnotised, false, 1, 7 },
			{ L"Can Be Electrocuted", Flag::CanBeElectrocuted, false, 1, 8 },
			{ L"Can Be Resurrected", Flag::CanBeResurrected, false, 1, 9 },
			{ L"Doesn't Get Angry With Enemies", Flag::DoesntGetAngryWithEnemies, false, 1, 10 },
			{ L"Is Fearless", Flag::IsFearless, false, 1, 11 },
			{ L"Always Flee", Flag::AlwaysFlee, false, 1, 12 },
		} };
		constexpr int EvilRow = 4;
		constexpr int MaleRow = 10;

		// The lists on the right. Ids are the record's value; the named lists are the official editor's own.
		enum class Source { Fixed, Objects, Effects, Creatures };
		struct Choice {
			const wchar_t* label;
			std::size_t offset;
			int bytes;
			Source source;
			std::vector<const wchar_t*> names;
		};
		const std::vector<Choice>& Choices() {
			static const std::vector<Choice> choices{
				{ L"Armor Type:", Offset::ArmorType, 1, Source::Fixed, { L"None", L"Flesh", L"Rock", L"Wood", L"Metal 1", L"Metal 2", L"Magic", L"Glass" } },
				{ L"Fight Style:", Offset::FightStyle, 1, Source::Fixed, { L"Non-Fighter", L"Blitzer", L"Blocker", L"Flanker", L"Support" } },
				{ L"Job Class:", Offset::JobClass, 1, Source::Fixed, { L"Artisan", L"Thinker", L"Fighter", L"Scout" } },
				{ L"Death Fall Direction:", Offset::DeathFallDirection, 1, Source::Fixed, { L"North", L"North-East", L"East", L"South-East", L"South", L"South-West", L"West", L"North-West" } },
				{ L"Lair Object:", Offset::LairObject, 1, Source::Objects, {} },
				{ L"Clone Creature:", Offset::CloneCreature, 1, Source::Creatures, {} },
				{ L"Slap Effect:", Offset::SlapEffect, 2, Source::Effects, {} },
				{ L"OLHI Effect:", Offset::OlhiEffect, 2, Source::Effects, {} },
				{ L"Entrance Effect:", Offset::EntranceEffect, 2, Source::Effects, {} },
				{ L"Death Effect:", Offset::DeathEffect, 2, Source::Effects, {} },
			};
			return choices;
		}
		// An attraction's kind. The first may only be a room, as in the official editor.
		constexpr std::array<const wchar_t*, 5> AttractionKinds{ L"<none>", L"Room", L"Gold", L"Mana", L"Creature" };
		constexpr std::uint32_t AttractionRoom = 1;
		constexpr std::uint32_t AttractionCreature = 4;

		// ---- The record's other parts, each in a window of its own as the official editor has them (docs/game/creature-types.md).
		// A window is sections of rows, each row a label and fields under the section's column headers.
		enum class ListId { Spells, Jobs, Resistances, Abilities, Swipes, Gamma };
		enum class FieldKind { Number, Choice, Check, Color };
		struct SubField {
			FieldKind kind = FieldKind::Number;
			std::size_t offset = 0;
			CreatureNumber number = CreatureNumber::U8;
			ListId list = ListId::Spells;
			// Choice: the value's size. Check: the size of the value holding the bit.
			int bytes = 1;
			// Check: the bit. Color: the bit at flagOffset that adds 256.
			std::uint32_t bit = 0;
			std::size_t flagOffset = 0;
		};
		SubField Num(CreatureNumber number, std::size_t offset) { return { FieldKind::Number, offset, number }; }
		SubField Pick(ListId list, std::size_t offset, int bytes) { return { FieldKind::Choice, offset, CreatureNumber::U8, list, bytes }; }
		SubField Tick(std::size_t offset, int bytes, std::uint32_t bit) { return { FieldKind::Check, offset, CreatureNumber::U8, ListId::Spells, bytes, bit }; }
		SubField Color(std::size_t offset, std::size_t flagOffset, std::uint32_t bit) { return { FieldKind::Color, offset, CreatureNumber::U8, ListId::Spells, 1, bit, flagOffset }; }
		struct SubRow {
			std::wstring label;
			std::vector<SubField> fields;
		};
		struct SubSection {
			std::wstring title;
			std::vector<std::wstring> headers;
			std::vector<SubRow> rows;
		};
		struct SubWindowSpec {
			const wchar_t* button;
			std::wstring title;
			std::vector<SubSection> sections;
		};
		const std::vector<SubWindowSpec>& SubWindows() {
			using enum CreatureNumber;
			static const std::vector<SubWindowSpec> windows = [] {
				std::vector<SubWindowSpec> all;
				// Text ids: tooltip, name, the three descriptions, then the introduction (0x46CD70 sets the first five in this order)
				// [reference, editor; the name's place is verified, creatures.md].
				all.push_back({ L"String / Speech IDs...", L"String / Text IDs", { { L"", { L"Text ID" }, {
					{ L"ToolTip String ID:", { Num(U16, 0xF26) } }, { L"Name String ID:", { Num(U16, 0xF24) } },
					{ L"General Description Text ID:", { Num(U16, 0xF38) } }, { L"Strength Text ID:", { Num(U16, 0xF3A) } },
					{ L"Weakness Text ID:", { Num(U16, 0xF3C) } }, { L"Introduction String ID:", { Num(U16, 0xCFF) } } } } } });
				// Job preferences: entries of { 32 bit job, 16 bit mood change, 16 bit mana change, byte chance } 12 bytes apart.
				SubWindowSpec jobs{ L"Job Preferences...", L"Job Preferences", {} };
				const auto weighted = [](const wchar_t* title, std::size_t base, int count) {
					SubSection section{ title, { L"Job", L"Chance (%)", L"Mood Change", L"Mana Change" }, {} };
					for (int i = 0; i < count; ++i) {
						const auto at = base + static_cast<std::size_t>(i) * 12;
						section.rows.push_back({ std::to_wstring(i + 1) + L":", { Pick(ListId::Jobs, at, 4), Num(U8, at + 8), Num(S16, at + 4), Num(S16, at + 6) } });
					}
					return section;
				};
				jobs.sections.push_back(weighted(L"Happy Jobs", 0xE14, 3));
				jobs.sections.push_back(weighted(L"Unhappy Jobs", 0xE38, 2));
				jobs.sections.push_back(weighted(L"Angry Jobs", 0xE50, 3));
				SubSection hated{ L"Hated Jobs", { L"Job" }, {} };
				for (int i = 0; i < 4; ++i) hated.rows.push_back({ std::to_wstring(i + 1) + L":", { Pick(ListId::Jobs, 0xE74 + static_cast<std::size_t>(i) * 4, 4) } });
				jobs.sections.push_back(hated);
				SubSection alternatives{ L"Alternatives", { L"Job", L"Mood Change", L"Mana Change" }, {} };
				for (int i = 0; i < 3; ++i) {
					const auto at = 0xE7C + static_cast<std::size_t>(i) * 8;
					alternatives.rows.push_back({ std::to_wstring(i + 1) + L":", { Pick(ListId::Jobs, at, 4), Num(S16, at + 4), Num(S16, at + 6) } });
				}
				jobs.sections.push_back(alternatives);
				all.push_back(jobs);
				// Attacks: three of 0x18 bytes: offset X, Y, Z (fixed), +0xD play anim, +0x10 shot delay (fixed), +0x16 the spell,
				// +0x17 the level it comes at. Then the melee attack.
				SubWindowSpec attacks{ L"Available Attacks...", L"Available Attacks", {} };
				SubSection spells{ L"Spells", { L"Spell", L"Level", L"Shot Delay (s)", L"Offset X", L"Offset Y", L"Offset Z", L"Play Anim" }, {} };
				for (int i = 0; i < 3; ++i) {
					const auto at = 0xDC4 + static_cast<std::size_t>(i) * 0x18;
					spells.rows.push_back({ L"Spell " + std::to_wstring(i + 1) + L":", { Pick(ListId::Spells, at + 0x16, 1), Num(U8, at + 0x17), Num(Fixed, at + 0x10),
						Num(Fixed, at), Num(Fixed, at + 4), Num(Fixed, at + 8), Tick(at + 0xD, 1, 1) } });
				}
				attacks.sections.push_back(spells);
				attacks.sections.push_back({ L"Melee", { L"Attack (id)", L"Recharge Time (s)", L"Range (tiles)", L"Damage", L"Shot Delay (s)", L"Play Anim" }, {
					{ L"Melee:", { Num(U8, 0xF50), Num(Fixed, 0xEF8), Num(Fixed, 0xEEC), Num(U16, 0xF14), Num(Fixed, 0xCF9), Tick(Offset::Flags, 4, 1u << 29) } } } });
				all.push_back(attacks);
				// Resistances: four of { type, percent }.
				SubWindowSpec resistances{ L"Resistances...", L"Resistances", { { L"", { L"Type", L"Resistance (%)" }, {} } } };
				for (int i = 0; i < 4; ++i) {
					const auto at = 0xE0C + static_cast<std::size_t>(i) * 2;
					resistances.sections[0].rows.push_back({ L"Resistance " + std::to_wstring(i + 1) + L":", { Pick(ListId::Resistances, at, 1), Num(U8, at + 1) } });
				}
				all.push_back(resistances);
				// Light: X, Y, Z, radius (fixed), flags, then red, green, blue, each with a flag bit that adds 256.
				all.push_back({ L"Light...", L"Light", {
					{ L"", { L"X", L"Y", L"Z", L"Radius" }, { { L"Position:", { Num(Fixed, 0xD94), Num(Fixed, 0xD98), Num(Fixed, 0xD9C), Num(Fixed, 0xDA0) } } } },
					{ L"", { L"Red", L"Green", L"Blue" }, { { L"Color (0 - 511):", { Color(0xDA8, 0xDA4, 0x800), Color(0xDA9, 0xDA4, 0x1000), Color(0xDAA, 0xDA4, 0x2000) } } } },
					{ L"", { L"Flicker", L"Pulse", L"Player Colored" }, { { L"Flags:", { Tick(0xDA4, 4, 0x2), Tick(0xDA4, 4, 0x4), Tick(0xDA4, 4, 0x80) } } } } } });
				// Anger texts: nine at 0xD05 in the official editor's order, then General apart.
				SubWindowSpec anger{ L"Anger String IDs...", L"Anger String IDs", { { L"", { L"Text ID" }, {} } } };
				const std::array<const wchar_t*, 9> angers{ L"Lair:", L"Food:", L"Pay:", L"Work:", L"Slap:", L"Held:", L"Lonely:", L"Hatred:", L"Tortured:" };
				for (std::size_t i = 0; i < angers.size(); ++i) anger.sections[0].rows.push_back({ angers[i], { Num(U16, 0xD05 + i * 2) } });
				anger.sections[0].rows.push_back({ L"General:", { Num(U16, 0xCF7) } });
				all.push_back(anger);
				all.push_back({ L"1st Person Properties...", L"1st Person Properties", {
					{ L"Special Abilities", { L"Ability", L"Value" }, {
						{ L"1:", { Pick(ListId::Abilities, 0xF46, 1), Num(U32, 0x14E9) } }, { L"2:", { Pick(ListId::Abilities, 0xF47, 1), Num(U32, 0x14ED) } } } },
					{ L"Swipe IDs", { L"Swipe" }, {
						{ L"Melee 1:", { Pick(ListId::Swipes, 0xF42, 1) } }, { L"Melee 2:", { Pick(ListId::Swipes, 0xF43, 1) } },
						{ L"Special 1:", { Pick(ListId::Swipes, 0x1025, 1) } }, { L"Special 2:", { Pick(ListId::Swipes, 0x1026, 1) } },
						{ L"Spell:", { Pick(ListId::Swipes, 0xF45, 1) } } } },
					{ L"", { L"Gamma Effect" }, { { L"View:", { Pick(ListId::Gamma, 0xD3C, 1) } } } },
					{ L"Movement", { L"Waddle Scale", L"Oscillate Scale", L"Walk Cycle", L"Camera Rolls" }, {
						{ L"", { Num(Fixed, 0xDBC), Num(Fixed, 0xDC0), Num(U8, 0xD45), Tick(Offset::Flags, 4, 1u << 28) } } } } } });
				// Animation offsets: eight of X, Y, Z, the first apart from the other seven.
				SubWindowSpec offsets{ L"Anim Offsets...", L"Animation Offsets", { { L"", { L"X", L"Y", L"Z" }, {} } } };
				const std::array<const wchar_t*, 8> offsetNames{ L"Portal Entrance:", L"Praying:", L"Fall Back / Get Up:", L"Corpse:", L"Offset 5:", L"Offset 6:", L"Offset 7:", L"Offset 8:" };
				for (std::size_t i = 0; i < offsetNames.size(); ++i) {
					const std::size_t at = i == 0 ? 0xE94 : 0x127B + (i - 1) * 12;
					offsets.sections[0].rows.push_back({ offsetNames[i], { Num(Fixed, at), Num(Fixed, at + 4), Num(Fixed, at + 8) } });
				}
				all.push_back(offsets);
				return all;
			}();
			return windows;
		}
		// The named lists, as the official editor has them [reference, editor]; spells are CreatureSpells.kwd by place from 1
		// [verified: Warlock's attacks are its fireball, heal and firebomb].
		std::vector<std::pair<std::uint32_t, std::wstring>> FixedNames(std::initializer_list<const wchar_t*> names, std::uint32_t first = 0) {
			std::vector<std::pair<std::uint32_t, std::wstring>> list;
			for (const auto* name : names) list.emplace_back(first++, name);
			return list;
		}
		// ---- Art resources (docs/game/creature-types.md, Art resources): 0x54 bytes each, at fixed places in the record.
		struct ArtSlot {
			std::wstring name;
			std::size_t offset;
			// An animation's place in the footstep frame pairs at 0x1323, or below 0 for a resource that is not an animation.
			int animation = -1;
		};
		const std::vector<ArtSlot>& ArtSlots() {
			static const std::vector<ArtSlot> slots = [] {
				// Animations 1 to 47 (0 holds no resource), named from what stock creatures keep in each [verified: their file
				// names]. Those no stock creature uses are marked so.
				static constexpr std::array<const wchar_t*, 48> names{ L"", L"Walk", L"Run", L"Being Dragged", L"Recoil Forwards", L"Melee Attack",
					L"Cast Spell", L"Die", L"Happy", L"Angry", L"Stunned", L"In Hand", L"Sleeping", L"Eating", L"Researching", L"(unused 15)",
					L"(unused 16)", L"Tortured", L"(unused 18)", L"Drinking", L"Idle 1", L"Recoil Backwards", L"Manufacturing", L"Praying", L"Fall Back",
					L"Electrocuted Pose", L"Electrocuted", L"Get Up", L"Dance", L"Drunk 2", L"Entrance", L"Idle 2", L"Special 1", L"Special 2", L"Drunk",
					L"Special 3", L"(unused 36)", L"Drunken Idle", L"Back Off", L"Pose Frame", L"Stealth Walk", L"Death Pose", L"Melee 2", L"Extra 2",
					L"Extra 3", L"Extra 4", L"Extra 5", L"Extra 6" };
				static constexpr std::array<std::size_t, 5> apart{ 0xFD1, 0x12CF, 0x13E3, 0x1437, 0x148B }; // animations 37 to 41
				std::vector<ArtSlot> all;
				for (int i = 1; i < 48; ++i) {
					const std::size_t offset = i < 37 ? 0x20 + static_cast<std::size_t>(i) * 0x54 : i < 42 ? apart[static_cast<std::size_t>(i - 37)] : 0x1083 + static_cast<std::size_t>(i - 42) * 0x54;
					all.push_back({ L"Animation " + std::to_wstring(i) + L": " + names[static_cast<std::size_t>(i)], offset, i });
				}
				all.push_back({ L"GUI Icon", 0xC44 });
				all.push_back({ L"Portrait", 0xD40 });
				all.push_back({ L"Creature Icon (side panel)", 0xC98 });
				all.push_back({ L"Unique Resource", 0x14F1 });
				all.push_back({ L"1st Person Filter", 0xF77 });
				all.push_back({ L"1st Person Melee", 0x1027 });
				return all;
			}();
			return slots;
		}
		// The main window's buttons for them, and the slot each opens on.
		const std::vector<std::pair<const wchar_t*, std::size_t>>& ArtButtons() {
			static const std::vector<std::pair<const wchar_t*, std::size_t>> buttons{
				{ L"Anim Resources...", 0 }, { L"GUI Icon...", 47 }, { L"Portrait Resource...", 48 }, { L"Creature Icon...", 49 },
				{ L"Unique Resource...", 50 }, { L"1st Person Resources...", 51 } };
			return buttons;
		}

		const std::vector<std::pair<std::uint32_t, std::wstring>>& JobNames() {
			static const std::vector<std::pair<std::uint32_t, std::wstring>> jobs{
				{ 0, L"<none>" }, { 1, L"Sleep" }, { 2, L"Eat" }, { 3, L"Research" }, { 4, L"Train" }, { 5, L"Manufacture" }, { 6, L"Guard" },
				{ 7, L"Torture" }, { 8, L"Pray" }, { 9, L"Drink" }, { 10, L"Leave" }, { 11, L"Destroy Rooms" }, { 12, L"Destroy Walls" },
				{ 13, L"Steal Gold" }, { 14, L"Steal Spells" }, { 15, L"Sulk" }, { 16, L"Rebel" }, { 17, L"Steal Manufacture Crates" },
				{ 18, L"Kill Creatures" }, { 19, L"Kill Player" }, { 20, L"Tunnelling" }, { 22, L"Wait" }, { 23, L"Send To Action Point" },
				{ 24, L"Explore" }, { 25, L"Steal Enemy Gold" }, { 26, L"Combat Pit Spectate" }, { 27, L"Jail Break" }, { 28, L"Tolling" } };
			return jobs;
		}

		struct State {
			HWND window = nullptr;
			HFONT font = nullptr;
			HHOOK hook = nullptr;
			double scale = 1.0;
			// The working copy, and the list as last given back (or as opened), so only real changes go back.
			CreatureDialogData data;
			std::vector<std::vector<std::uint8_t>> given;
			std::uint8_t type = 1;
			std::size_t attribute = 0;
			// Controls the player changed since the record was last shown or read: only these are written to it, so a record whose
			// fields were not touched keeps every byte, whatever the fields round to.
			std::set<int> dirty;
			bool filling = false;
			int cursorRaised = 0;
			// The placed creature the window is open on, if any, and the type of its own it was given (0 for none yet).
			std::optional<UnitData> unit;
			std::uint8_t unitOwnType = 0;
			// That creature is the editor's chosen one (outlined in the world): the title says so.
			bool unitSelected = false;
			// Taller than the screen's work area: the window is cut to fit and scrolls (its vertical scroll bar, the mouse wheel,
			// and Tab or a refused field bringing the focused control into view). In pixels: all it holds, and how far down it is.
			bool scrolls = false;
			int contentHeight = 0;
			int scrollY = 0;
		};
		State state;
		std::optional<std::vector<std::vector<std::uint8_t>>> pending;
		std::optional<UnitChange> pendingUnit;

		// A small window asking for one line of text (a name), owned by the creature window, which waits while it is open.
		struct PromptState {
			HWND window = nullptr;
			// Given the text on OK: true closes the prompt, false keeps it open (the text was refused, and the note says why).
			std::function<bool(const std::wstring&)> done;
			// Which it is (Rename or Add Type, as SmallWindow), so a closing can keep it and open it again.
			int kind = 0;
		};
		PromptState prompt;

		std::wstring Widen(const std::string& text) {
			std::wstring wide;
			for (const auto c : text) wide += static_cast<wchar_t>(static_cast<unsigned char>(c));
			return wide;
		}
		std::wstring Trimmed(std::wstring text) {
			while (!text.empty() && iswspace(text.back())) text.pop_back();
			std::size_t start = 0;
			while (start < text.size() && iswspace(text[start])) ++start;
			return text.substr(start);
		}

		std::vector<std::uint8_t>& Record() { return state.data.records[state.type - 1u]; }
		std::uint32_t Read(const std::vector<std::uint8_t>& record, std::size_t offset, int bytes) {
			std::uint32_t value = 0;
			if (offset + static_cast<std::size_t>(bytes) <= record.size()) std::memcpy(&value, record.data() + offset, static_cast<std::size_t>(bytes));
			return value;
		}
		void Write(std::vector<std::uint8_t>& record, std::size_t offset, int bytes, std::uint32_t value) {
			if (offset + static_cast<std::size_t>(bytes) <= record.size()) std::memcpy(record.data() + offset, &value, static_cast<std::size_t>(bytes));
		}
		std::string RecordText(const std::vector<std::uint8_t>& record, std::size_t offset, std::size_t length) {
			if (offset + length > record.size()) return {};
			const auto* text = reinterpret_cast<const char*>(record.data() + offset);
			return std::string(text, strnlen(text, length));
		}
		// A text field of the record, zero filled after the text.
		void WriteRecordText(std::vector<std::uint8_t>& record, std::size_t offset, std::size_t length, const std::string& text) {
			if (offset + length > record.size()) return;
			std::fill_n(record.begin() + static_cast<std::ptrdiff_t>(offset), length, std::uint8_t{ 0 });
			std::memcpy(record.data() + offset, text.data(), (std::min)(text.size(), length - 1));
		}
		std::wstring CreatureName(std::size_t index) {
			const auto name = RecordText(state.data.records[index], Offset::Name, Offset::NameLength);
			return name.empty() ? L"(" + std::to_wstring(index + 1) + L")" : Widen(name);
		}
		// Plain printable characters only, as the game's files hold single bytes.
		std::optional<std::string> PlainText(const std::wstring& text) {
			std::string narrow;
			for (const auto c : text) {
				if (c < 0x20 || c > 0x7E) return std::nullopt;
				narrow += static_cast<char>(c);
			}
			return narrow;
		}

		int NumberBytes(CreatureNumber kind) {
			switch (kind) {
				case CreatureNumber::U8: return 1;
				case CreatureNumber::U16: case CreatureNumber::S16: return 2;
				default: return 4;
			}
		}
		std::wstring NumberText(const std::vector<std::uint8_t>& record, std::size_t offset, CreatureNumber kind) {
			const auto raw = Read(record, offset, NumberBytes(kind));
			switch (kind) {
				case CreatureNumber::S16: return std::to_wstring(static_cast<std::int16_t>(raw));
				case CreatureNumber::Fixed: {
					// Four places are enough to come back to the same 1/4096th.
					wchar_t text[64];
					std::swprintf(text, std::size(text), L"%.4f", static_cast<std::int32_t>(raw) / FixedOne);
					std::wstring value = text;
					while (value.back() == L'0') value.pop_back();
					if (value.back() == L'.') value.pop_back();
					return value;
				}
				default: return std::to_wstring(raw);
			}
		}
		std::wstring RangeText(CreatureNumber kind) {
			switch (kind) {
				case CreatureNumber::U8: return L"[ Range: 0 - 255 ]";
				case CreatureNumber::U16: return L"[ Range: 0 - 65535 ]";
				case CreatureNumber::S16: return L"[ Range: -32768 - 32767 ]";
				case CreatureNumber::U32: return L"[ Range: 0 - 4294967295 ]";
				case CreatureNumber::Fixed: return L"[ Decimal, in 1/4096ths ]";
			}
			return {};
		}
		std::optional<long long> ParseWhole(const std::wstring& text, long long lowest, long long highest) {
			const auto trimmed = Trimmed(text);
			if (trimmed.empty()) return std::nullopt;
			wchar_t* end = nullptr;
			const auto value = std::wcstoll(trimmed.c_str(), &end, 10);
			if (end == nullptr || *end != 0 || value < lowest || value > highest) return std::nullopt;
			return value;
		}
		// The raw value for a number as typed, or none if it is not one or out of range.
		std::optional<std::uint32_t> ParseNumber(const std::wstring& text, CreatureNumber kind) {
			switch (kind) {
				case CreatureNumber::U8: if (const auto v = ParseWhole(text, 0, 0xFF)) return static_cast<std::uint32_t>(*v); break;
				case CreatureNumber::U16: if (const auto v = ParseWhole(text, 0, 0xFFFF)) return static_cast<std::uint32_t>(*v); break;
				case CreatureNumber::S16: if (const auto v = ParseWhole(text, -0x8000, 0x7FFF)) return static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(*v))); break;
				case CreatureNumber::U32: if (const auto v = ParseWhole(text, 0, 0xFFFFFFFFll)) return static_cast<std::uint32_t>(*v); break;
				case CreatureNumber::Fixed: {
					const auto trimmed = Trimmed(text);
					if (trimmed.empty()) break;
					wchar_t* end = nullptr;
					const auto value = std::wcstod(trimmed.c_str(), &end);
					if (end == nullptr || *end != 0 || !std::isfinite(value)) break;
					const auto scaled = std::llround(value * FixedOne);
					if (scaled < INT32_MIN || scaled > INT32_MAX) break;
					return static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled));
				}
			}
			return std::nullopt;
		}

		// ---- Controls.
		int Px(int value) { return static_cast<int>(std::lround(value * state.scale)); }
		HWND Make(HWND parent, const wchar_t* windowClass, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD extended = 0) {
			auto* control = CreateWindowExW(extended, windowClass, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(width), Px(height), parent,
				reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
			if (control != nullptr) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), FALSE);
			return control;
		}
		HWND Make(const wchar_t* windowClass, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD extended = 0) {
			return Make(state.window, windowClass, text, style, x, y, width, height, id, extended);
		}
		// ---- Scrolling the creature window, when it is cut to fit the screen. Its controls move with ScrollWindowEx, so they keep
		// their places relative to each other; what the window paints itself is drawn scrollY higher.
		int ClientPageHeight() {
			RECT client{};
			GetClientRect(state.window, &client);
			return client.bottom;
		}
		void ScrollTo(int position) {
			if (!state.scrolls || state.window == nullptr) return;
			position = (std::clamp)(position, 0, (std::max)(0, state.contentHeight - ClientPageHeight()));
			if (position == state.scrollY) return;
			const int by = state.scrollY - position;
			state.scrollY = position;
			ScrollWindowEx(state.window, 0, by, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
			SCROLLINFO info{ sizeof(info), SIF_POS };
			info.nPos = position;
			SetScrollInfo(state.window, SB_VERT, &info, TRUE);
		}
		void SetUpScrolling() {
			if (!state.scrolls) return;
			SCROLLINFO info{ sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };
			info.nMin = 0;
			info.nMax = state.contentHeight - 1;
			info.nPage = static_cast<UINT>(ClientPageHeight());
			info.nPos = 0;
			state.scrollY = 0;
			SetScrollInfo(state.window, SB_VERT, &info, TRUE);
		}
		// Scrolls just enough to show this control of the creature window (the focused one, after Tab or a refusal).
		void ScrollIntoView(HWND control) {
			if (!state.scrolls || control == nullptr || (control != state.window && !IsChild(state.window, control))) return;
			RECT place{};
			GetWindowRect(control, &place);
			MapWindowPoints(nullptr, state.window, reinterpret_cast<POINT*>(&place), 2);
			const int page = ClientPageHeight();
			if (place.top < 0) ScrollTo(state.scrollY + place.top - Px(8));
			else if (place.bottom > page) ScrollTo(state.scrollY + place.bottom - page + Px(8));
		}
		void ScrollMessage(WPARAM wParam) {
			SCROLLINFO info{ sizeof(info), SIF_ALL };
			GetScrollInfo(state.window, SB_VERT, &info);
			int position = state.scrollY;
			switch (LOWORD(wParam)) {
				case SB_LINEUP: position -= Px(32); break;
				case SB_LINEDOWN: position += Px(32); break;
				case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
				case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
				case SB_THUMBTRACK:
				case SB_THUMBPOSITION: position = info.nTrackPos; break;
				case SB_TOP: position = 0; break;
				case SB_BOTTOM: position = state.contentHeight; break;
				default: return;
			}
			ScrollTo(position);
		}
		// A push button: owner drawn when dark. The default one is told apart by DM_GETDEFID, so it needs no style of its own then.
		HWND PushButton(HWND parent, const wchar_t* text, int x, int y, int width, int id, bool isDefault = false) {
			const DWORD style = theme.dark ? BS_OWNERDRAW : (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
			return Make(parent, WC_BUTTONW, text, style | WS_TABSTOP, x, y, width, 26, id);
		}
		// Checkboxes, radio buttons and the group box, with their theme taken off when dark.
		HWND ClassicButton(const wchar_t* text, DWORD style, int x, int y, int width, int height, int id) {
			auto* button = Make(WC_BUTTONW, text, style, x, y, width, height, id);
			if (button != nullptr) Unthemed(button);
			return button;
		}
		HWND Item(int id) { return GetDlgItem(state.window, id); }
		void Label(const wchar_t* text, int x, int y, int width, int id = -1, DWORD align = SS_RIGHT) { Make(L"STATIC", text, align, x, y + 3, width, 18, id); }
		HWND ComboBox(int x, int y, int width, int id) { return Make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, width, 320, id); }
		HWND EditBox(HWND parent, int x, int y, int width, int id, int limit = 0) {
			auto* edit = Make(parent, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, x, y, width, 22, id, WS_EX_CLIENTEDGE);
			if (edit != nullptr && limit > 0) SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(limit), 0);
			return edit;
		}
		HWND EditBox(int x, int y, int width, int id, int limit = 0) { return EditBox(state.window, x, y, width, id, limit); }
		void AddItem(HWND combo, const std::wstring& text, std::uint32_t data) {
			const auto at = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
			if (at >= 0) SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(at), static_cast<LPARAM>(data));
		}
		// Chooses the item holding this value, adding one for a value the list does not name, so it is kept as it is.
		void SelectData(HWND combo, std::uint32_t value) {
			const auto count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
			for (LRESULT i = 0; i < count; ++i) {
				if (static_cast<std::uint32_t>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0)) == value) {
					SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
					return;
				}
			}
			AddItem(combo, L"(" + std::to_wstring(value) + L")", value);
			SelectData(combo, value);
		}
		std::optional<std::uint32_t> SelectedData(HWND combo) {
			const auto at = SendMessageW(combo, CB_GETCURSEL, 0, 0);
			if (at < 0) return std::nullopt;
			return static_cast<std::uint32_t>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(at), 0));
		}
		std::wstring Text(HWND control) {
			const auto length = GetWindowTextLengthW(control);
			std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
			GetWindowTextW(control, text.data(), length + 1);
			text.resize(static_cast<std::size_t>(length));
			return text;
		}
		std::wstring Text(int id) { return Text(Item(id)); }
		void Note(const std::wstring& text) { SetWindowTextW(Item(IdNote), text.c_str()); }
		void FillNamed(HWND combo, const std::vector<NamedId>& names) {
			AddItem(combo, L"<none>", 0);
			for (const auto& named : names) AddItem(combo, named.name.empty() ? L"(" + std::to_wstring(named.id) + L")" : Widen(named.name), named.id);
		}
		void FillCreatures(HWND combo) {
			for (std::size_t i = 0; i < state.data.records.size(); ++i) AddItem(combo, CreatureName(i), static_cast<std::uint32_t>(i + 1));
		}
		int CloneChoiceId() {
			const auto& choices = Choices();
			for (std::size_t i = 0; i < choices.size(); ++i) {
				if (choices[i].source == Source::Creatures) return IdChoiceFirst + static_cast<int>(i);
			}
			return -1;
		}
		// The lists that name creature types, again: after a rename, or a type added or deleted.
		void RefillCreatureLists() {
			auto* names = Item(IdName);
			SendMessageW(names, CB_RESETCONTENT, 0, 0);
			FillCreatures(names);
			auto* clone = Item(CloneChoiceId());
			SendMessageW(clone, CB_RESETCONTENT, 0, 0);
			AddItem(clone, L"<none>", 0);
			FillCreatures(clone);
		}
		// An attraction's target list follows its kind: rooms or creatures, else nothing.
		void FillAttractionTargets(std::size_t which, std::uint32_t kind) {
			auto* target = Item(IdAttractionFirst + static_cast<int>(which) * 3 + 1);
			SendMessageW(target, CB_RESETCONTENT, 0, 0);
			if (kind == AttractionRoom) for (const auto& room : state.data.rooms) AddItem(target, room.name.empty() ? L"(" + std::to_wstring(room.id) + L")" : Widen(room.name), room.id);
			else if (kind == AttractionCreature) FillCreatures(target);
			// Hidden rather than disabled: a disabled classic combo box or edit paints itself in Windows' gray, whatever colors
			// its parent gives, which showed as a light block in dark mode.
			ShowWindow(target, kind == AttractionRoom || kind == AttractionCreature ? SW_SHOW : SW_HIDE);
			ShowWindow(Item(IdAttractionFirst + static_cast<int>(which) * 3 + 2), kind != 0 ? SW_SHOW : SW_HIDE);
		}

		// ---- Health: a percent kept in hundredths (0.01%), and hit points. Six petals on the game's health flower.
		constexpr std::uint32_t FullHundredths = 10000;
		constexpr std::uint32_t Petals = 6;
		std::wstring PercentText(std::uint32_t hundredths) {
			auto text = std::to_wstring(hundredths / 100);
			if (const auto rest = hundredths % 100; rest != 0) {
				text += L"." + std::to_wstring(rest / 10);
				if (rest % 10 != 0) text += std::to_wstring(rest % 10);
			}
			return text + L"%";
		}
		// Petals left: each is a sixth of the flower, and any part of one still shows it.
		std::wstring PetalsText(std::uint32_t hundredths) {
			const auto left = (hundredths * Petals + FullHundredths - 1) / FullHundredths;
			return L"(" + std::to_wstring(left) + (left == 1 ? L" petal left)" : L" petals left)");
		}
		// A typed percent, to two places: the third place rounds the second up at 6 to 9 and leaves it at 0 to 5 (the user's rule),
		// and any after it are not looked at. "36.846" is 36.84%, "36.847" 36.85%. Text after the number ("%", "(3 petals left)") is
		// left out. From 0.01% to 100%.
		std::optional<std::uint32_t> ParsePercent(const std::wstring& text) {
			const auto trimmed = Trimmed(text);
			std::size_t at = 0;
			std::uint64_t whole = 0;
			bool digits = false;
			while (at < trimmed.size() && iswdigit(trimmed[at]) && whole <= 1000) {
				whole = whole * 10 + static_cast<std::uint64_t>(trimmed[at++] - L'0');
				digits = true;
			}
			std::uint32_t fraction = 0;
			if (at < trimmed.size() && (trimmed[at] == L'.' || trimmed[at] == L',')) {
				++at;
				for (int place = 0; at < trimmed.size() && iswdigit(trimmed[at]); ++place, ++at) {
					const auto digit = static_cast<std::uint32_t>(trimmed[at] - L'0');
					digits = true;
					if (place == 0) fraction += digit * 10;
					else if (place == 1) fraction += digit;
					else if (place == 2 && digit >= 6) fraction += 1;
				}
			}
			if (!digits) return std::nullopt;
			const auto rest = Trimmed(trimmed.substr(at));
			if (!rest.empty() && rest[0] != L'%' && rest[0] != L'(') return std::nullopt;
			const auto hundredths = whole * 100 + fraction;
			if (hundredths < 1 || hundredths > FullHundredths) return std::nullopt;
			return static_cast<std::uint32_t>(hundredths);
		}
		// The creature's most hit points at a level: its type's Health (record +0xF0C), times the game's percent for the level.
		std::uint32_t MostPoints(std::uint8_t type, std::uint8_t level) {
			if (type < 1 || type > state.data.records.size()) return 1;
			const auto health = Read(state.data.records[type - 1u], 0xF0C, 2);
			const auto percent = level <= 1 || level >= state.data.levelHealthPercent.size() || state.data.levelHealthPercent[level] <= 0 ? 100 : state.data.levelHealthPercent[level];
			return (std::max)(1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(health) * static_cast<std::uint32_t>(percent) / 100));
		}
		// Hit points for a percent, as the game works them out when it makes the creature (at least 1).
		std::uint32_t PointsFor(std::uint32_t hundredths, std::uint32_t most) {
			return (std::max)(1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(hundredths) * most / FullHundredths));
		}
		// A percent for hit points, to two places by the same rule.
		std::uint32_t HundredthsFor(std::uint32_t points, std::uint32_t most) {
			const auto thousandths = static_cast<std::uint64_t>(points) * FullHundredths * 10 / (std::max)(1u, most);
			auto hundredths = thousandths / 10 + (thousandths % 10 >= 6 ? 1 : 0);
			return static_cast<std::uint32_t>((std::clamp)(hundredths, std::uint64_t{ 1 }, std::uint64_t{ FullHundredths }));
		}

		void Build() {
			Label(L"[ ID 1 ] Name:", 12, 12, 120, IdNameLabel);
			auto* names = ComboBox(136, 12, 280, IdName);
			FillCreatures(names);
			PushButton(state.window, L"Rename...", 424, 11, 96, IdRename);
			// What a creature of this type is placed as, shown only when it has another kind (ShowRecord).
			Label(L"Places as:", 530, 12, 80, IdPlacesLabel);
			ComboBox(614, 12, 174, IdPlacesAs);
			Label(L"Attribute:", 12, 44, 120);
			auto* attributes = ComboBox(136, 44, 320, IdAttribute);
			for (const auto& attribute : LevelFiles::CreatureAttributes()) AddItem(attributes, Widen(attribute.name), 0);
			Label(L"Value:", 462, 44, 54);
			EditBox(520, 44, 100, IdValue, 24);
			Label(L"", 628, 44, 170, IdRange, SS_LEFT);

			constexpr int FlagsTop = 84;
			constexpr int RowStep = 24;
			for (std::size_t i = 0; i < FlagBoxes.size(); ++i) {
				const auto& box = FlagBoxes[i];
				ClassicButton(box.label, BS_AUTOCHECKBOX | WS_TABSTOP, box.column == 0 ? 12 : 240, FlagsTop + box.row * RowStep, box.column == 0 ? 220 : 236, 20, IdFlagFirst + static_cast<int>(i));
			}
			ClassicButton(L"Is Evil", BS_RADIOBUTTON | WS_TABSTOP, 12, FlagsTop + EvilRow * RowStep, 100, 20, IdEvil);
			ClassicButton(L"Is Good", BS_RADIOBUTTON | WS_TABSTOP, 116, FlagsTop + EvilRow * RowStep, 110, 20, IdGood);
			ClassicButton(L"Is Male", BS_RADIOBUTTON | WS_TABSTOP, 12, FlagsTop + MaleRow * RowStep, 100, 20, IdMale);
			ClassicButton(L"Is Female", BS_RADIOBUTTON | WS_TABSTOP, 116, FlagsTop + MaleRow * RowStep, 110, 20, IdFemale);

			const auto& choices = Choices();
			for (std::size_t i = 0; i < choices.size(); ++i) {
				const auto& choice = choices[i];
				const int y = 80 + static_cast<int>(i) * 30;
				Label(choice.label, 480, y, 130);
				auto* combo = ComboBox(614, y, 174, IdChoiceFirst + static_cast<int>(i));
				switch (choice.source) {
					case Source::Fixed: for (std::size_t n = 0; n < choice.names.size(); ++n) AddItem(combo, choice.names[n], static_cast<std::uint32_t>(n)); break;
					case Source::Objects: FillNamed(combo, state.data.objects); break;
					case Source::Effects: FillNamed(combo, state.data.effects); break;
					case Source::Creatures: AddItem(combo, L"<none>", 0); FillCreatures(combo); break;
				}
			}

			// The Attractions frame is drawn in WM_PAINT: a classic group box paints only its border, and with the window clipping
			// its children nothing filled the inside, which stayed white.
			for (std::size_t which = 0; which < Offset::AttractionCount; ++which) {
				const int y = 428 + static_cast<int>(which) * 32;
				const int first = IdAttractionFirst + static_cast<int>(which) * 3;
				Label(which == 0 ? L"1st:" : L"2nd:", 24, y, 36);
				auto* kinds = ComboBox(64, y, 110, first);
				const std::size_t kindCount = which == 0 ? 2 : AttractionKinds.size();
				for (std::size_t k = 0; k < kindCount; ++k) AddItem(kinds, AttractionKinds[k], static_cast<std::uint32_t>(k));
				ComboBox(182, y, 250, first + 1);
				Label(L"Amount:", 440, y, 60);
				EditBox(504, y, 90, first + 2, 10);
			}

			Label(L"Unique Name Text ID:", 12, 508, 150);
			EditBox(166, 508, 80, IdUniqueName, 5);
			Label(L"Intro Camera Path Index:", 260, 508, 170);
			EditBox(434, 508, 60, IdIntroCamera, 3);
			Label(L"Sound Category:", 12, 540, 150);
			EditBox(166, 540, 220, IdSound, static_cast<int>(Offset::SoundCategoryLength - 1));
			Label(L"Translation Sound Category:", 396, 540, 180);
			EditBox(580, 540, 208, IdTranslation, static_cast<int>(Offset::SoundCategoryLength - 1));

			// The official editor's buttons for the record's other parts, in rows of four: the parts' windows, then the art
			// resources, each opening the art window on its own resource.
			// New creatures of the type (from the side panel), or the one creature the window is open on; only one set shows.
			Label(L"", 12, 576, 150, IdRowLabel, SS_LEFT);
			Label(L"Level:", 166, 576, 44, IdNewLevelLabel);
			auto* newLevel = ComboBox(214, 576, 70, IdNewLevel);
			Make(WC_BUTTONW, L"Placed as a dead body", BS_AUTOCHECKBOX | WS_TABSTOP, 300, 578, 220, 20, IdNewDead);
			Label(L"Level:", 120, 576, 44, IdUnitLevelLabel);
			auto* unitLevel = ComboBox(168, 576, 64, IdUnitLevel);
			for (std::uint32_t level = 1; level <= 10; ++level) {
				AddItem(newLevel, std::to_wstring(level), level);
				AddItem(unitLevel, std::to_wstring(level), level);
			}
			Label(L"Gold:", 240, 576, 40, IdUnitGoldLabel);
			EditBox(284, 576, 80, IdUnitGold, 5);
			Make(WC_BUTTONW, L"Dead", BS_AUTOCHECKBOX | WS_TABSTOP, 380, 578, 70, 20, IdUnitDead);
			PushButton(state.window, L"Give It Its Own Stats...", 562, 573, 226, IdUnitOwnStats);
			// Health as a percent of its most at its level (with the petals of the health flower left, six in all) and as hit points,
			// each following the other.
			Label(L"Health:", 12, 608, 104, IdUnitHealthLabel, SS_LEFT);
			auto* percent = Make(WC_COMBOBOXW, L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 120, 608, 170, 240, IdUnitHealth);
			for (const auto hundredths : { 10000, 8333, 6666, 5000, 3333, 1666 }) AddItem(percent, PercentText(static_cast<std::uint32_t>(hundredths)) + L" " + PetalsText(static_cast<std::uint32_t>(hundredths)), static_cast<std::uint32_t>(hundredths));
			Label(L"", 296, 608, 130, IdUnitPetals, SS_LEFT);
			Label(L"Points:", 430, 608, 52, IdUnitPointsLabel);
			EditBox(486, 608, 90, IdUnitPoints, 10);
			Label(L"", 582, 608, 206, IdUnitMost, SS_LEFT);
			for (const int id : { IdNewDead, IdUnitDead }) Unthemed(Item(id));

			const auto& subWindows = SubWindows();
			const auto& artButtons = ArtButtons();
			for (std::size_t i = 0; i < subWindows.size() + artButtons.size(); ++i) {
				const int column = static_cast<int>(i % 4);
				const int row = static_cast<int>(i / 4);
				const bool art = i >= subWindows.size();
				PushButton(state.window, art ? artButtons[i - subWindows.size()].first : subWindows[i].button, 12 + column * 196, 644 + row * 32, 188,
					art ? IdArtButtonFirst + static_cast<int>(i - subWindows.size()) : IdSubButtonFirst + static_cast<int>(i));
			}

			Make(L"STATIC", L"Saved with the level. The game uses them the next time the level loads (Test, or open it again).", SS_LEFT | SS_ENDELLIPSIS, 12, 776, 776, 20, IdNote);
			PushButton(state.window, L"Add Type...", 12, 804, 104, IdAddType);
			PushButton(state.window, L"Delete Type", 122, 804, 104, IdDeleteType);
			PushButton(state.window, L"OK", 548, 804, 76, IDOK, true);
			PushButton(state.window, L"Apply", 630, 804, 76, IdApply);
			PushButton(state.window, L"Cancel", 712, 804, 76, IDCANCEL);
		}
		constexpr int ClientWidth = 800;
		constexpr int ClientHeight = 842;
		// A frame with a title, as a group box, in the theme's colors.
		void PaintFrame(HDC dc, int x, int y, int width, int height, const wchar_t* title) {
			auto* oldFont = SelectObject(dc, state.font);
			SIZE text{};
			GetTextExtentPoint32W(dc, title, static_cast<int>(wcslen(title)), &text);
			const int top = Px(y) + text.cy / 2;
			auto* pen = CreatePen(PS_SOLID, 1, theme.edge);
			auto* oldPen = SelectObject(dc, pen);
			auto* oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
			Rectangle(dc, Px(x), top, Px(x + width), Px(y + height));
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
			SetBkMode(dc, OPAQUE);
			SetBkColor(dc, theme.back);
			SetTextColor(dc, theme.text);
			TextOutW(dc, Px(x) + Px(8), Px(y), (L" " + std::wstring(title) + L" ").c_str(), static_cast<int>(wcslen(title) + 2));
			SelectObject(dc, oldFont);
		}

		// ---- Record to controls and back.
		void ShowAttribute() {
			const auto& attribute = LevelFiles::CreatureAttributes()[state.attribute];
			SetWindowTextW(Item(IdValue), NumberText(Record(), attribute.offset, attribute.kind).c_str());
			SetWindowTextW(Item(IdRange), RangeText(attribute.kind).c_str());
			state.dirty.erase(IdValue);
		}
		// Only the last type can go, as ids are places in the list, and only one after the game's own.
		bool LastAddedType() { return state.type == state.data.records.size() && state.type >= state.data.firstDeletable; }
		// The title for one placed creature: its name, where it is and its type, marked when it is the chosen (outlined) one.
		void ShowUnitTitle() {
			if (state.window == nullptr || !state.unit) return;
			SetWindowTextW(state.window, (L"Creature " + CreatureName(state.unit->type - 1u) + L" " + state.unit->where + (state.unitSelected ? L" [Outlined]" : L"")
				+ L" - its type: " + CreatureName(state.type - 1u)).c_str());
		}
		void ShowRecord() {
			state.filling = true;
			const auto& record = Record();
			const auto name = CreatureName(state.type - 1u);
			SetWindowTextW(state.window, (L"Modify Creature Properties - " + name).c_str());
			SetWindowTextW(Item(IdNameLabel), (L"[ ID " + std::to_wstring(state.type) + L" ] Name:").c_str());
			SendMessageW(Item(IdName), CB_SETCURSEL, state.type - 1u, 0);
			SendMessageW(Item(IdAttribute), CB_SETCURSEL, state.attribute, 0);
			ShowAttribute();
			const auto flags = Read(record, Offset::Flags, 4);
			for (std::size_t i = 0; i < FlagBoxes.size(); ++i) {
				const bool set = (flags & FlagBoxes[i].bit) != 0;
				CheckDlgButton(state.window, IdFlagFirst + static_cast<int>(i), set != FlagBoxes[i].inverted ? BST_CHECKED : BST_UNCHECKED);
			}
			const bool evil = (flags & Flag::IsEvil) != 0;
			CheckDlgButton(state.window, IdEvil, evil ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(state.window, IdGood, evil ? BST_UNCHECKED : BST_CHECKED);
			const bool male = (flags & Flag::IsMale) != 0;
			CheckDlgButton(state.window, IdMale, male ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(state.window, IdFemale, male ? BST_UNCHECKED : BST_CHECKED);
			const auto& choices = Choices();
			for (std::size_t i = 0; i < choices.size(); ++i) SelectData(Item(IdChoiceFirst + static_cast<int>(i)), Read(record, choices[i].offset, choices[i].bytes));
			for (std::size_t which = 0; which < Offset::AttractionCount; ++which) {
				const auto at = Offset::Attractions + which * Offset::AttractionSize;
				const auto kind = Read(record, at, 4);
				const auto value = Read(record, at + 4, 4);
				const int first = IdAttractionFirst + static_cast<int>(which) * 3;
				SelectData(Item(first), kind);
				FillAttractionTargets(which, kind);
				// A room or creature is the low byte, with an amount above it; gold and mana are the amount alone (the official editor).
				const bool targeted = kind == AttractionRoom || kind == AttractionCreature;
				if (targeted) SelectData(Item(first + 1), value & 0xFF);
				SetWindowTextW(Item(first + 2), std::to_wstring(targeted ? value >> 16 : value).c_str());
			}
			SetWindowTextW(Item(IdUniqueName), std::to_wstring(Read(record, Offset::UniqueNameTextId, 2)).c_str());
			SetWindowTextW(Item(IdIntroCamera), std::to_wstring(Read(record, Offset::IntroCameraPath, 1)).c_str());
			SetWindowTextW(Item(IdSound), Widen(RecordText(record, Offset::SoundCategory, Offset::SoundCategoryLength)).c_str());
			SetWindowTextW(Item(IdTranslation), Widen(RecordText(record, Offset::TranslationSoundCategory, Offset::SoundCategoryLength)).c_str());
			EnableWindow(Item(IdDeleteType), LastAddedType());
			EnableWindow(Item(IdAddType), state.data.records.size() < MostCreatureTypes);
			// "Places as" only for a type with another kind; one without has nothing to choose, so the list is not there at all.
			const auto index = state.type - 1u;
			const auto variant = index < state.data.variants.size() ? state.data.variants[index] : CreatureDialogData::Variant::None;
			const bool hasVariant = variant != CreatureDialogData::Variant::None;
			ShowWindow(Item(IdPlacesLabel), hasVariant ? SW_SHOW : SW_HIDE);
			ShowWindow(Item(IdPlacesAs), hasVariant ? SW_SHOW : SW_HIDE);
			if (hasVariant) {
				auto* places = Item(IdPlacesAs);
				SendMessageW(places, CB_RESETCONTENT, 0, 0);
				AddItem(places, L"Normal", 0);
				AddItem(places, variant == CreatureDialogData::Variant::Unique ? L"Unique" : L"Mercenary", 1);
				SendMessageW(places, CB_SETCURSEL, index < state.data.placesVariant.size() && state.data.placesVariant[index] ? 1 : 0, 0);
			}
			// The placing settings for new creatures of the type shown (side panel), or the one creature's own row.
			const bool unit = state.unit.has_value();
			for (const int id : { IdNewLevelLabel, IdNewLevel, IdNewDead }) ShowWindow(Item(id), unit ? SW_HIDE : SW_SHOW);
			for (const int id : { IdUnitLevelLabel, IdUnitLevel, IdUnitHealthLabel, IdUnitHealth, IdUnitGoldLabel, IdUnitGold, IdUnitDead, IdUnitOwnStats,
				IdUnitPetals, IdUnitPointsLabel, IdUnitPoints, IdUnitMost }) ShowWindow(Item(id), unit ? SW_SHOW : SW_HIDE);
			if (unit) {
				ShowWindow(Item(IdPlacesLabel), SW_HIDE);
				ShowWindow(Item(IdPlacesAs), SW_HIDE);
				SetWindowTextW(Item(IdRowLabel), L"This creature:");
				ShowUnitTitle();
				// A type of its own is only made from a type shared with others.
				EnableWindow(Item(IdUnitOwnStats), state.unitOwnType == 0 && state.unit->type < state.data.firstDeletable);
			}
			else {
				SetWindowTextW(Item(IdRowLabel), L"New ones placed:");
				const std::uint8_t level = index < state.data.placeLevels.size() ? state.data.placeLevels[index] : std::uint8_t{ 1 };
				SelectData(Item(IdNewLevel), (std::max)(std::uint8_t{ 1 }, level));
				CheckDlgButton(state.window, IdNewDead, index < state.data.placeDead.size() && state.data.placeDead[index] ? BST_CHECKED : BST_UNCHECKED);
			}
			state.dirty.clear();
			state.filling = false;
		}
		// The one creature's own fields, as the editor gave them.
		// The type whose Health the creature's most hit points come from: its own new one once it has one.
		std::uint8_t UnitType() { return state.unitOwnType != 0 ? state.unitOwnType : state.unit ? state.unit->type : state.type; }
		std::uint8_t UnitLevel() { return static_cast<std::uint8_t>(SelectedData(Item(IdUnitLevel)).value_or(state.unit ? state.unit->level : 1)); }
		// The percent box, its petals, the points box and "of N" for these hit points; what is typed in one fills the others.
		void ShowHealth(std::uint32_t points, bool percentBox, bool pointsBox) {
			const auto most = MostPoints(UnitType(), UnitLevel());
			const auto hundredths = HundredthsFor(points, most);
			const bool wasFilling = state.filling;
			state.filling = true;
			if (percentBox) SetWindowTextW(Item(IdUnitHealth), PercentText(hundredths).c_str());
			if (pointsBox) SetWindowTextW(Item(IdUnitPoints), std::to_wstring(points).c_str());
			SetWindowTextW(Item(IdUnitPetals), PetalsText(hundredths).c_str());
			SetWindowTextW(Item(IdUnitMost), (L"of " + std::to_wstring(most) + L" at level " + std::to_wstring(UnitLevel())).c_str());
			state.filling = wasFilling;
		}
		void ShowUnit() {
			if (!state.unit) return;
			state.filling = true;
			SelectData(Item(IdUnitLevel), (std::max)(std::uint8_t{ 1 }, state.unit->level));
			SetWindowTextW(Item(IdUnitGold), std::to_wstring(state.unit->gold).c_str());
			CheckDlgButton(state.window, IdUnitDead, BST_UNCHECKED);
			ShowHealth(state.unit->points, true, true);
			state.filling = false;
		}
		// Typed or chosen in the percent box: the points follow (the text of a chosen item, as the box's own text changes after).
		void PercentChanged(bool chosen) {
			auto* box = Item(IdUnitHealth);
			std::wstring text;
			if (chosen) {
				const auto at = SendMessageW(box, CB_GETCURSEL, 0, 0);
				if (at < 0) return;
				text.resize(static_cast<std::size_t>((std::max)(LRESULT{ 0 }, SendMessageW(box, CB_GETLBTEXTLEN, static_cast<WPARAM>(at), 0))) + 1);
				SendMessageW(box, CB_GETLBTEXT, static_cast<WPARAM>(at), reinterpret_cast<LPARAM>(text.data()));
				text.resize(wcslen(text.c_str()));
			}
			else text = Text(box);
			const auto hundredths = ParsePercent(text);
			if (!hundredths) {
				SetWindowTextW(Item(IdUnitPetals), L"(not a percent)");
				return;
			}
			ShowHealth(PointsFor(*hundredths, MostPoints(UnitType(), UnitLevel())), false, true);
			// The petals for what was typed, which a whole hit point may round away from.
			SetWindowTextW(Item(IdUnitPetals), PetalsText(*hundredths).c_str());
		}
		void PointsChanged() {
			const auto points = ParseWhole(Text(IdUnitPoints), 1, 0x7FFFFFFF);
			if (!points) {
				SetWindowTextW(Item(IdUnitPetals), L"(not a number)");
				return;
			}
			ShowHealth(static_cast<std::uint32_t>(*points), true, false);
		}
		// A new level changes the most: the percent stays, the points follow it.
		void UnitLevelChanged() {
			const auto hundredths = ParsePercent(Text(IdUnitHealth));
			if (!hundredths) return;
			ShowHealth(PointsFor(*hundredths, MostPoints(UnitType(), UnitLevel())), true, true);
		}
		bool Refuse(int id, const wchar_t* why) {
			MessageBeep(MB_ICONWARNING);
			Note(why);
			SetFocus(Item(id));
			ScrollIntoView(Item(id));
			SendMessageW(Item(id), EM_SETSEL, 0, -1);
			return false;
		}
		bool ReadValue() {
			if (!state.dirty.contains(IdValue)) return true;
			const auto& attribute = LevelFiles::CreatureAttributes()[state.attribute];
			const auto raw = ParseNumber(Text(IdValue), attribute.kind);
			if (!raw) return Refuse(IdValue, L"That value does not fit this attribute: see its range.");
			Write(Record(), attribute.offset, NumberBytes(attribute.kind), *raw);
			state.dirty.erase(IdValue);
			return true;
		}
		bool WriteText(int id, std::size_t offset) {
			if (!state.dirty.contains(id)) return true;
			const auto text = PlainText(Text(id));
			if (!text) return Refuse(id, L"Sound categories are plain letters, digits and _.");
			WriteRecordText(Record(), offset, Offset::SoundCategoryLength, *text);
			return true;
		}
		// The changed controls into the working record. False, with the field shown, if one does not hold a fitting value.
		bool ReadRecord() {
			if (!ReadValue()) return false;
			auto& record = Record();
			if (state.dirty.contains(IdUniqueName)) {
				const auto value = ParseWhole(Text(IdUniqueName), 0, 0xFFFF);
				if (!value) return Refuse(IdUniqueName, L"Unique Name Text ID is a whole number from 0 to 65535.");
				Write(record, Offset::UniqueNameTextId, 2, static_cast<std::uint32_t>(*value));
			}
			if (state.dirty.contains(IdIntroCamera)) {
				const auto value = ParseWhole(Text(IdIntroCamera), 0, 0xFF);
				if (!value) return Refuse(IdIntroCamera, L"Intro Camera Path Index is a whole number from 0 to 255.");
				Write(record, Offset::IntroCameraPath, 1, static_cast<std::uint32_t>(*value));
			}
			for (std::size_t which = 0; which < Offset::AttractionCount; ++which) {
				const int first = IdAttractionFirst + static_cast<int>(which) * 3;
				if (!state.dirty.contains(first) && !state.dirty.contains(first + 1) && !state.dirty.contains(first + 2)) continue;
				const auto kind = SelectedData(Item(first)).value_or(0);
				const auto amount = ParseWhole(Text(first + 2), 0, kind == AttractionRoom || kind == AttractionCreature ? 0xFFFF : 0xFFFFFFFFll);
				if (kind != 0 && !amount) return Refuse(first + 2, L"An attraction's amount is a whole number (up to 65535 for a room or creature).");
				std::uint32_t value = 0;
				if (kind == AttractionRoom || kind == AttractionCreature) value = (SelectedData(Item(first + 1)).value_or(0) & 0xFF) | static_cast<std::uint32_t>(*amount) << 16;
				else if (kind != 0) value = static_cast<std::uint32_t>(*amount);
				const auto at = Offset::Attractions + which * Offset::AttractionSize;
				Write(record, at, 4, kind);
				Write(record, at + 4, 4, value);
			}
			if (!WriteText(IdSound, Offset::SoundCategory) || !WriteText(IdTranslation, Offset::TranslationSoundCategory)) return false;
			// Only the bits whose boxes were clicked; the rest, the ones not shown among them, stay as they are.
			auto flags = Read(record, Offset::Flags, 4);
			for (std::size_t i = 0; i < FlagBoxes.size(); ++i) {
				if (!state.dirty.contains(IdFlagFirst + static_cast<int>(i))) continue;
				const bool checked = IsDlgButtonChecked(state.window, IdFlagFirst + static_cast<int>(i)) == BST_CHECKED;
				if (checked != FlagBoxes[i].inverted) flags |= FlagBoxes[i].bit;
				else flags &= ~FlagBoxes[i].bit;
			}
			if (state.dirty.contains(IdEvil) || state.dirty.contains(IdGood)) {
				if (IsDlgButtonChecked(state.window, IdEvil) == BST_CHECKED) flags |= Flag::IsEvil;
				else flags &= ~Flag::IsEvil;
			}
			if (state.dirty.contains(IdMale) || state.dirty.contains(IdFemale)) {
				if (IsDlgButtonChecked(state.window, IdMale) == BST_CHECKED) flags |= Flag::IsMale;
				else flags &= ~Flag::IsMale;
			}
			Write(record, Offset::Flags, 4, flags);
			const auto& choices = Choices();
			for (std::size_t i = 0; i < choices.size(); ++i) {
				const int id = IdChoiceFirst + static_cast<int>(i);
				if (!state.dirty.contains(id)) continue;
				if (const auto value = SelectedData(Item(id))) Write(record, choices[i].offset, choices[i].bytes, *value);
			}
			state.dirty.clear();
			return true;
		}
		// The one creature's fields, into what goes back for it. False, with the field shown, if one does not fit.
		bool ReadUnit() {
			if (!state.unit) return true;
			// The points box holds the hit points (the percent box fills it); at most the creature's most at its level, unless it is
			// what the level already had.
			auto points = ParseWhole(Text(IdUnitPoints), 1, 0x7FFFFFFF);
			if (points && *points != state.unit->points && static_cast<std::uint32_t>(*points) > MostPoints(UnitType(), UnitLevel())) points.reset();
			if (!points) return Refuse(IdUnitPoints, L"Hit points are a whole number from 1 to its most at its level.");
			const auto gold = ParseWhole(Text(IdUnitGold), 0, 0xFFFF);
			if (!gold) return Refuse(IdUnitGold, L"Gold is a whole number from 0 to 65535.");
			UnitChange change;
			change.id = state.unit->id;
			change.type = state.unitOwnType != 0 ? state.unitOwnType : state.unit->type;
			change.level = static_cast<std::uint8_t>(SelectedData(Item(IdUnitLevel)).value_or(state.unit->level));
			change.points = static_cast<std::uint32_t>(*points);
			change.gold = static_cast<std::uint16_t>(*gold);
			change.dead = IsDlgButtonChecked(state.window, IdUnitDead) == BST_CHECKED;
			if (change.type != state.unit->type || change.level != state.unit->level || change.points != state.unit->points || change.gold != state.unit->gold || change.dead) {
				pendingUnit = change;
				// Given back once: the editor tells the window of the creature made again (SetUnit).
				state.unit->type = change.type;
				state.unit->level = change.level;
				state.unit->points = change.points;
				state.unit->gold = change.gold;
				state.unitOwnType = 0;
				CheckDlgButton(state.window, IdUnitDead, BST_UNCHECKED);
			}
			return true;
		}
		// The whole list goes back if anything in it differs from what was last given back.
		void GiveBack() {
			const auto& records = state.data.records;
			std::size_t count = 0;
			for (std::size_t i = 0; i < (std::max)(records.size(), state.given.size()); ++i) {
				if (i >= records.size() || i >= state.given.size() || records[i] != state.given[i]) ++count;
			}
			if (count != 0) {
				pending = records;
				state.given = records;
			}
			if (count == 0 && !pendingUnit) Note(L"Nothing changed.");
			else if (count == 0) Note(L"Given to that creature.");
			else Note(std::to_wstring(count) + L" creature type(s) given to the level. Save it to keep them; Test to try them.");
		}
		void Destroy() {
			if (state.window != nullptr) DestroyWindow(state.window);
		}

		// ---- What the window held when taking its creature away closed it (CloseCreatureProperties with keepEdits), for the
		// window opened on that creature again: its working copy of the list, where it was, and the controls changed and not yet
		// read into the copy (the ones in dirty, and the creature's own row), as they read.
		struct KeptControl {
			int id = 0;
			std::wstring text;
			LRESULT selection = CB_ERR;
			UINT check = BST_UNCHECKED;
		};
		// The smaller window that was open over it, if any: which one, where it was, and its own changes not yet given to the
		// creature window (its changed controls; for the art window also its copy of the record, with other resources' changes).
		enum class SmallWindow { None, Rename, AddType, Part, Art };
		struct KeptSmallWindow {
			SmallWindow kind = SmallWindow::None;
			// The part's place in SubWindows(), or the art window's resource slot.
			std::size_t which = 0;
			std::set<int> dirty;
			std::vector<KeptControl> controls;
			std::vector<std::uint8_t> artRecord;
			bool artChanged = false;
			RECT place{};
			bool placed = false;
		};
		struct KeptEdits {
			std::vector<std::vector<std::uint8_t>> records;
			std::vector<std::vector<std::uint8_t>> given;
			std::uint8_t type = 1;
			std::size_t attribute = 0;
			std::uint8_t unitOwnType = 0;
			std::set<int> dirty;
			std::vector<KeptControl> controls;
			RECT place{};
			bool placed = false;
			KeptSmallWindow smallWindow;
		};
		std::optional<KeptEdits> kept;
		constexpr std::array UnitRowIds{ IdUnitLevel, IdUnitGold, IdUnitHealth, IdUnitPoints, IdUnitPetals, IdUnitMost };
		std::wstring ClassOf(HWND control) {
			wchar_t name[32]{};
			GetClassNameW(control, name, 32);
			return name;
		}
		// A control of this window (the creature window or a smaller one) as it reads.
		KeptControl Capture(HWND window, int id) {
			KeptControl control{ id };
			auto* item = GetDlgItem(window, id);
			if (item == nullptr) return control;
			control.text = Text(item);
			const auto kind = ClassOf(item);
			if (lstrcmpiW(kind.c_str(), WC_COMBOBOXW) == 0) control.selection = SendMessageW(item, CB_GETCURSEL, 0, 0);
			else if (lstrcmpiW(kind.c_str(), WC_BUTTONW) == 0) control.check = IsDlgButtonChecked(window, id);
			return control;
		}
		KeptControl Capture(int id) { return Capture(state.window, id); }
		// Puts a kept control back as it read: a check, a list's choice, or text (an edit, an editable combo box, a label).
		void RestoreIn(HWND window, const KeptControl& control) {
			auto* item = GetDlgItem(window, control.id);
			if (item == nullptr) return;
			const auto kind = ClassOf(item);
			if (lstrcmpiW(kind.c_str(), WC_BUTTONW) == 0) CheckDlgButton(window, control.id, control.check);
			else if (lstrcmpiW(kind.c_str(), WC_COMBOBOXW) == 0 && (GetWindowLongW(item, GWL_STYLE) & 3) == CBS_DROPDOWNLIST) SendMessageW(item, CB_SETCURSEL, static_cast<WPARAM>(control.selection), 0);
			else SetWindowTextW(item, control.text.c_str());
		}
		// The smaller window open now, kept (defined with them below).
		KeptSmallWindow KeepSmall();
		void Keep() {
			if (state.window == nullptr) return;
			KeptEdits edits;
			edits.records = state.data.records;
			edits.given = state.given;
			edits.type = state.type;
			edits.attribute = state.attribute;
			edits.unitOwnType = state.unitOwnType;
			edits.dirty = state.dirty;
			for (const int id : state.dirty) edits.controls.push_back(Capture(id));
			if (state.unit) {
				for (const int id : UnitRowIds) edits.controls.push_back(Capture(id));
			}
			edits.placed = GetWindowRect(state.window, &edits.place) != FALSE;
			edits.smallWindow = KeepSmall();
			kept = std::move(edits);
		}
		// Puts a kept control back as it read. An attraction's kind fills its target list first, as choosing it does.
		void Restore(const KeptControl& control) {
			RestoreIn(state.window, control);
			const int first = control.id - IdAttractionFirst;
			if (first >= 0 && first < static_cast<int>(Offset::AttractionCount) * 3 && first % 3 == 0) {
				FillAttractionTargets(static_cast<std::size_t>(first) / 3, SelectedData(Item(control.id)).value_or(0));
				SendMessageW(Item(control.id + 1), CB_SETCURSEL, 0, 0);
			}
		}

		// ---- The name prompt.
		void ClosePrompt() {
			if (prompt.window != nullptr) DestroyWindow(prompt.window);
		}
		LRESULT CALLBACK PromptWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
			if (const auto color = ControlColor(message, wParam, lParam)) return *color;
			if (IsCopyPicture(message, wParam)) {
				CopyPicture(window);
				return 0;
			}
			switch (message) {
				case WM_COMMAND:
					if (LOWORD(wParam) == IDOK && HIWORD(wParam) == BN_CLICKED) {
						const auto text = Text(GetDlgItem(window, IdPromptText));
						// Kept: the callback may open nothing, but closing first would lose the text on a refusal.
						if (prompt.done && prompt.done(text)) ClosePrompt();
						else MessageBeep(MB_ICONWARNING);
						return 0;
					}
					if (LOWORD(wParam) == IDCANCEL && HIWORD(wParam) == BN_CLICKED) ClosePrompt();
					return 0;
				case WM_DRAWITEM:
					if (DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
					break;
				case DM_GETDEFID:
					return MAKELRESULT(IDOK, DC_HASDEFID);
				case WM_ERASEBKGND: {
					RECT area{};
					GetClientRect(window, &area);
					FillRect(reinterpret_cast<HDC>(wParam), &area, theme.backBrush);
					return TRUE;
				}
				case WM_CLOSE:
					ClosePrompt();
					return 0;
				case WM_DESTROY:
					prompt.window = nullptr;
					prompt.done = nullptr;
					prompt.kind = 0;
					if (state.window != nullptr) {
						EnableWindow(state.window, TRUE);
						SetActiveWindow(state.window);
					}
					return 0;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}
		HINSTANCE OurModule();
		void OpenPrompt(const std::wstring& title, const std::wstring& label, const std::wstring& initial, int limit, std::function<bool(const std::wstring&)> done) {
			ClosePrompt();
			constexpr DWORD Style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
			constexpr DWORD Extended = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
			RECT frame{ 0, 0, Px(380), Px(104) };
			AdjustWindowRectEx(&frame, Style, FALSE, Extended);
			RECT owner{};
			GetWindowRect(state.window, &owner);
			const int width = frame.right - frame.left;
			const int height = frame.bottom - frame.top;
			const int x = owner.left + (owner.right - owner.left - width) / 2;
			const int y = owner.top + (owner.bottom - owner.top - height) / 2;
			prompt.window = CreateWindowExW(Extended, PromptClass, title.c_str(), Style, x, y, width, height, state.window, nullptr, OurModule(), nullptr);
			if (prompt.window == nullptr) {
				Note(L"Could not open the name window.");
				return;
			}
			prompt.done = std::move(done);
			DarkTitleBar(prompt.window); AddCopyPicture(prompt.window);
			Make(prompt.window, L"STATIC", label.c_str(), SS_LEFT, 12, 12, 356, 18, IdPromptLabel);
			auto* edit = EditBox(prompt.window, 12, 36, 356, IdPromptText, limit);
			SetWindowTextW(edit, initial.c_str());
			PushButton(prompt.window, L"OK", 210, 68, 76, IDOK, true);
			PushButton(prompt.window, L"Cancel", 292, 68, 76, IDCANCEL);
			EnableWindow(state.window, FALSE);
			ShowWindow(prompt.window, SW_SHOWNORMAL);
			SetFocus(edit);
			SendMessageW(edit, EM_SETSEL, 0, -1);
		}
		// A creature type's name: plain characters, at least one, and room for the end in its 32.
		std::optional<std::string> TypeName(const std::wstring& text) {
			const auto plain = PlainText(Trimmed(text));
			if (!plain || plain->empty() || plain->size() >= Offset::NameLength) return std::nullopt;
			return plain;
		}

		// ---- A type of its own for the one creature: a copy of its type at the end of the list, which its record then names. Asked
		// first, as the game knows some types by their ids.
		void OwnStats() {
			if (!state.unit || state.unitOwnType != 0 || !ReadRecord()) return;
			if (state.data.records.size() >= MostCreatureTypes) {
				Note(L"No room for another type: ids go up to 255.");
				return;
			}
			const auto source = static_cast<std::size_t>(state.unit->type - 1u);
			if (source >= state.data.records.size()) return;
			const auto name = CreatureName(source);
			const auto question = L"Give this creature its own copy of the " + name + L" type?\n\n"
				L"Its stats can then be changed without changing any other " + name + L". The copy is a new creature type, so game rules that "
				L"look for " + name + L" by its id will not count it: the Mercenary Portal and the creature pools, availability, triggers that "
				L"count " + name + L"s, and the special cases some creatures have (Imps digging, the King, Lord and Stone Knight).\n\n"
				L"It shows in the world after the level is loaded again (Test, or open it again).";
			if (MessageBoxW(state.window, question.c_str(), L"Own Stats", MB_YESNO | MB_ICONWARNING) != IDYES) return;
			auto copy = state.data.records[source];
			const auto id = static_cast<std::uint8_t>(state.data.records.size() + 1);
			auto copyName = RecordText(copy, Offset::Name, Offset::NameLength);
			while (!copyName.empty() && copyName.back() == ' ') copyName.pop_back();
			copyName = (copyName + " (" + std::to_string(id) + ")").substr(0, Offset::NameLength - 1);
			WriteRecordText(copy, Offset::Name, Offset::NameLength, copyName);
			Write(copy, Offset::TypeId, 1, id);
			state.data.records.push_back(std::move(copy));
			state.unitOwnType = id;
			state.type = id;
			RefillCreatureLists();
			ShowRecord();
			Note(L"This creature has its own type now (id " + std::to_wstring(id) + L"). Change its stats, then Apply or OK.");
		}

		// ---- Rename, Add Type, Delete Type.
		void Rename() {
			if (!ReadRecord()) return;
			OpenPrompt(L"Rename Creature Type", L"New name for " + CreatureName(state.type - 1u) + L":", CreatureName(state.type - 1u), static_cast<int>(Offset::NameLength - 1),
				[](const std::wstring& text) {
					const auto name = TypeName(text);
					if (!name) return false;
					WriteRecordText(Record(), Offset::Name, Offset::NameLength, *name);
					RefillCreatureLists();
					ShowRecord();
					Note(L"Renamed. Apply or OK gives it to the level.");
					return true;
				});
			if (prompt.window != nullptr) prompt.kind = static_cast<int>(SmallWindow::Rename);
		}
		void AddType() {
			if (!ReadRecord()) return;
			if (state.data.records.size() >= MostCreatureTypes) {
				Note(L"No room for another type: ids go up to 255.");
				return;
			}
			const auto source = CreatureName(state.type - 1u);
			OpenPrompt(L"Add Creature Type", L"Name of the new type (it starts as a copy of " + source + L"):", (L"New " + source).substr(0, Offset::NameLength - 1),
				static_cast<int>(Offset::NameLength - 1), [](const std::wstring& text) {
					const auto name = TypeName(text);
					if (!name || state.data.records.size() >= MostCreatureTypes) return false;
					auto copy = Record();
					const auto id = static_cast<std::uint8_t>(state.data.records.size() + 1);
					WriteRecordText(copy, Offset::Name, Offset::NameLength, *name);
					Write(copy, Offset::TypeId, 1, id);
					state.data.records.push_back(std::move(copy));
					state.type = id;
					RefillCreatureLists();
					ShowRecord();
					Note(L"Added as id " + std::to_wstring(id) + L". Apply or OK gives it to the level.");
					return true;
				});
			if (prompt.window != nullptr) prompt.kind = static_cast<int>(SmallWindow::AddType);
		}
		void DeleteType() {
			if (!ReadRecord()) return;
			if (!LastAddedType()) {
				Note(L"Only the last type can be deleted, and only one added after the game's own: ids are places in the list.");
				return;
			}
			const auto type = state.type;
			// Another type of the list naming it (as its clone, or what attracts it).
			for (std::size_t i = 0; i + 1 < state.data.records.size(); ++i) {
				const auto& record = state.data.records[i];
				bool named = Read(record, Offset::CloneCreature, 1) == type;
				for (std::size_t which = 0; which < Offset::AttractionCount; ++which) {
					const auto at = Offset::Attractions + which * Offset::AttractionSize;
					named = named || (Read(record, at, 4) == AttractionCreature && (Read(record, at + 4, 4) & 0xFF) == type);
				}
				if (named) {
					Note(L"Not deleted: " + CreatureName(i) + L" names it (Clone Creature or an attraction).");
					return;
				}
			}
			if (state.data.deleteProblem) {
				if (const auto use = state.data.deleteProblem(type); !use.empty()) {
					Note(L"Not deleted: the level uses it in " + Widen(use) + L".");
					return;
				}
			}
			const auto question = L"Delete creature type " + CreatureName(type - 1u) + L" (id " + std::to_wstring(type) + L")?";
			if (MessageBoxW(state.window, question.c_str(), L"Delete Creature Type", MB_YESNO | MB_ICONWARNING) != IDYES) return;
			state.data.records.pop_back();
			state.type = static_cast<std::uint8_t>(state.data.records.size());
			RefillCreatureLists();
			ShowRecord();
			Note(L"Deleted. Apply or OK gives that to the level.");
		}

		// ---- The record's other parts (SubWindows): a window over the creature window, which waits while it is open. It works on
		// the working record: OK writes what was changed into it, Cancel drops it. Apply or OK on the creature window then gives it
		// to the level, as with the rest.
		struct SubState {
			HWND window = nullptr;
			const SubWindowSpec* spec = nullptr;
			std::vector<std::pair<int, SubField>> fields;
			std::set<int> dirty;
			bool filling = false;
		};
		SubState sub;
		constexpr wchar_t SubClass[] = L"KeeperForge2CreatureSubWindow";
		constexpr int SubLabelWidth = 170;
		constexpr int SubRowStep = 28;
		int FieldWidth(const SubField& field) {
			switch (field.kind) {
				case FieldKind::Choice: return 190;
				case FieldKind::Check: return 84;
				default: return 92;
			}
		}
		std::vector<std::pair<std::uint32_t, std::wstring>> ListNames(ListId list) {
			switch (list) {
				case ListId::Spells: {
					std::vector<std::pair<std::uint32_t, std::wstring>> names{ { 0, L"<none>" } };
					for (const auto& spell : state.data.spells) names.emplace_back(spell.id, spell.name.empty() ? L"(" + std::to_wstring(spell.id) + L")" : Widen(spell.name));
					return names;
				}
				case ListId::Jobs: return JobNames();
				case ListId::Resistances:
					return { { 0, L"<none>" }, { 1, L"Melee Blade" }, { 2, L"Melee Blunt" }, { 3, L"Melee Body" }, { 4, L"Fire" }, { 5, L"Movement" }, { 6, L"Lightning" },
						{ 7, L"Gas" }, { 8, L"Projectile" }, { 9, L"Enhance Other" }, { 10, L"Enhance Self" }, { 11, L"Close Combat" }, { 12, L"Generation" },
						{ 13, L"Physical Trap" }, { 14, L"Non-Lethal Trap" }, { 15, L"Melee Scythe" } };
				case ListId::Abilities:
					return FixedNames({ L"None", L"Pick Up Workers", L"Sniper Mode", L"Turn To Bat", L"Fly", L"Hypnotise", L"Disguise / Steal", L"Pray", L"Pick Locks",
						L"Fear Attack", L"Ghost Possession" });
				case ListId::Swipes:
					return FixedNames({ L"Null", L"Bite", L"Claw Slash", L"Club Smash", L"Dark FGBS Slash", L"King FGBS Slash", L"Left Punch", L"Right Punch",
						L"Left Scythe Slash", L"Right Scythe Slash", L"Mistress Slash", L"Small Slash", L"Staff Strike", L"Sword Slash", L"Light Arrow", L"Dark Arrow",
						L"Left Bile Smash", L"Right Bile Smash", L"Hypnotise", L"Left Small Punch", L"Right Small Punch", L"Left King FGBS Slash", L"Right King FGBS Slash" });
				case ListId::Gamma:
					return FixedNames({ L"Normal", L"Vampire [ Red ]", L"Dark Elf [ Purple ]", L"Skeleton [ Black & White ]", L"Salamander [ Infra Red ]",
						L"Dark Angel [ Bright Blue ]", L"Whiteout", L"Blackout", L"Redout", L"Death View [ Half Red ]", L"Test 1", L"Test 2", L"Test 3", L"Test 4",
						L"Freeze View [ Only Blue ]", L"Maiden", L"Ghost" });
			}
			return {};
		}
		std::uint32_t ColorValue(const std::vector<std::uint8_t>& record, const SubField& field) {
			return Read(record, field.offset, 1) + ((Read(record, field.flagOffset, 4) & field.bit) != 0 ? 256u : 0u);
		}
		void FillSub() {
			sub.filling = true;
			const auto& record = Record();
			for (const auto& [id, field] : sub.fields) {
				auto* control = GetDlgItem(sub.window, id);
				switch (field.kind) {
					case FieldKind::Number: SetWindowTextW(control, NumberText(record, field.offset, field.number).c_str()); break;
					case FieldKind::Choice: SelectData(control, Read(record, field.offset, field.bytes)); break;
					case FieldKind::Check: SendMessageW(control, BM_SETCHECK, (Read(record, field.offset, field.bytes) & field.bit) != 0 ? BST_CHECKED : BST_UNCHECKED, 0); break;
					case FieldKind::Color: SetWindowTextW(control, std::to_wstring(ColorValue(record, field)).c_str()); break;
				}
			}
			sub.dirty.clear();
			sub.filling = false;
		}
		bool RefuseSub(int id, const wchar_t* why) {
			MessageBeep(MB_ICONWARNING);
			SetWindowTextW(GetDlgItem(sub.window, IdSubNote), why);
			SetFocus(GetDlgItem(sub.window, id));
			SendMessageW(GetDlgItem(sub.window, id), EM_SETSEL, 0, -1);
			return false;
		}
		// The changed fields into the working record, all or none: a copy is changed, and only kept if every field fits.
		bool ReadSub() {
			auto record = Record();
			for (const auto& [id, field] : sub.fields) {
				if (!sub.dirty.contains(id)) continue;
				auto* control = GetDlgItem(sub.window, id);
				switch (field.kind) {
					case FieldKind::Number: {
						const auto raw = ParseNumber(Text(control), field.number);
						if (!raw) return RefuseSub(id, (L"That does not fit: " + RangeText(field.number)).c_str());
						Write(record, field.offset, NumberBytes(field.number), *raw);
						break;
					}
					case FieldKind::Choice:
						if (const auto value = SelectedData(control)) Write(record, field.offset, field.bytes, *value);
						break;
					case FieldKind::Check: {
						auto value = Read(record, field.offset, field.bytes);
						if (SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED) value |= field.bit;
						else value &= ~field.bit;
						Write(record, field.offset, field.bytes, value);
						break;
					}
					case FieldKind::Color: {
						const auto value = ParseWhole(Text(control), 0, 511);
						if (!value) return RefuseSub(id, L"A color is a whole number from 0 to 511.");
						Write(record, field.offset, 1, static_cast<std::uint32_t>(*value) & 0xFF);
						auto flags = Read(record, field.flagOffset, 4);
						if (*value >= 256) flags |= field.bit;
						else flags &= ~field.bit;
						Write(record, field.flagOffset, 4, flags);
						break;
					}
				}
			}
			Record() = std::move(record);
			return true;
		}
		void CloseSub() {
			if (sub.window != nullptr) DestroyWindow(sub.window);
		}
		LRESULT CALLBACK SubWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
			if (const auto color = ControlColor(message, wParam, lParam)) return *color;
			if (IsCopyPicture(message, wParam)) {
				CopyPicture(window);
				return 0;
			}
			switch (message) {
				case WM_COMMAND: {
					const int id = LOWORD(wParam);
					const int code = HIWORD(wParam);
					if (sub.filling) return 0;
					if (id == IDOK && code == BN_CLICKED) {
						if (ReadSub()) {
							const bool changed = !sub.dirty.empty();
							CloseSub();
							ShowRecord();
							if (changed) Note(L"Changed. Apply or OK gives it to the level.");
						}
						return 0;
					}
					if (id == IDCANCEL && code == BN_CLICKED) {
						CloseSub();
						return 0;
					}
					if (id >= IdSubControlFirst && (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED)) sub.dirty.insert(id);
					return 0;
				}
				case WM_DRAWITEM:
					if (DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
					break;
				case DM_GETDEFID:
					return MAKELRESULT(IDOK, DC_HASDEFID);
				case WM_ERASEBKGND: {
					RECT area{};
					GetClientRect(window, &area);
					FillRect(reinterpret_cast<HDC>(wParam), &area, theme.backBrush);
					return TRUE;
				}
				case WM_CLOSE:
					CloseSub();
					return 0;
				case WM_DESTROY:
					sub = SubState{};
					if (state.window != nullptr) {
						EnableWindow(state.window, TRUE);
						SetActiveWindow(state.window);
					}
					return 0;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}
		void OpenSubWindow(std::size_t which) {
			const auto& windows = SubWindows();
			if (which >= windows.size() || !ReadRecord()) return;
			CloseSub();
			const auto& spec = windows[which];
			// The widest section sets the window's width; each section is a title (if it has one), its headers, then its rows.
			int contentWidth = 320;
			int contentHeight = 0;
			for (const auto& section : spec.sections) {
				int width = SubLabelWidth;
				if (!section.rows.empty()) for (const auto& field : section.rows.front().fields) width += FieldWidth(field) + 6;
				contentWidth = (std::max)(contentWidth, width);
				contentHeight += (section.title.empty() ? 0 : 22) + 20 + static_cast<int>(section.rows.size()) * SubRowStep + 10;
			}
			const int clientWidth = contentWidth + 24;
			const int clientHeight = 12 + contentHeight + 24 + 36;
			constexpr DWORD Style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
			constexpr DWORD Extended = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
			RECT frame{ 0, 0, Px(clientWidth), Px(clientHeight) };
			AdjustWindowRectEx(&frame, Style, FALSE, Extended);
			RECT owner{};
			GetWindowRect(state.window, &owner);
			const int width = frame.right - frame.left;
			const int height = frame.bottom - frame.top;
			const auto title = spec.title + L" - " + CreatureName(state.type - 1u);
			sub.window = CreateWindowExW(Extended, SubClass, title.c_str(), Style, owner.left + (owner.right - owner.left - width) / 2,
				owner.top + (std::max)(0L, (owner.bottom - owner.top - height) / 2), width, height, state.window, nullptr, OurModule(), nullptr);
			if (sub.window == nullptr) {
				Note(L"Could not open that window.");
				return;
			}
			sub.spec = &spec;
			DarkTitleBar(sub.window); AddCopyPicture(sub.window);
			int y = 12;
			int next = IdSubControlFirst;
			for (const auto& section : spec.sections) {
				if (!section.title.empty()) {
					Make(sub.window, L"STATIC", section.title.c_str(), SS_LEFT, 12, y, contentWidth, 18, -1);
					y += 22;
				}
				int x = 12 + SubLabelWidth;
				const auto& columns = section.rows.empty() ? std::vector<SubField>{} : section.rows.front().fields;
				for (std::size_t c = 0; c < section.headers.size() && c < columns.size(); ++c) {
					Make(sub.window, L"STATIC", section.headers[c].c_str(), SS_LEFT, x, y, FieldWidth(columns[c]), 18, -1);
					x += FieldWidth(columns[c]) + 6;
				}
				y += 20;
				for (const auto& row : section.rows) {
					if (!row.label.empty()) Make(sub.window, L"STATIC", row.label.c_str(), SS_RIGHT, 12, y + 3, SubLabelWidth - 8, 18, -1);
					x = 12 + SubLabelWidth;
					for (const auto& field : row.fields) {
						const int id = next++;
						switch (field.kind) {
							case FieldKind::Number:
							case FieldKind::Color:
								EditBox(sub.window, x, y, FieldWidth(field), id, 24);
								break;
							case FieldKind::Choice: {
								auto* combo = Make(sub.window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, FieldWidth(field), 320, id);
								for (const auto& [value, name] : ListNames(field.list)) AddItem(combo, name, value);
								break;
							}
							case FieldKind::Check:
								if (auto* box = Make(sub.window, WC_BUTTONW, L"", BS_AUTOCHECKBOX | WS_TABSTOP, x + 4, y + 2, 20, 20, id)) Unthemed(box);
								break;
						}
						sub.fields.emplace_back(id, field);
						x += FieldWidth(field) + 6;
					}
					y += SubRowStep;
				}
				y += 10;
			}
			Make(sub.window, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 12, y, contentWidth, 18, IdSubNote);
			y += 24;
			PushButton(sub.window, L"OK", clientWidth - 12 - 76 - 6 - 76, y, 76, IDOK, true);
			PushButton(sub.window, L"Cancel", clientWidth - 12 - 76, y, 76, IDCANCEL);
			FillSub();
			EnableWindow(state.window, FALSE);
			ShowWindow(sub.window, SW_SHOWNORMAL);
			if (!sub.fields.empty()) SetFocus(GetDlgItem(sub.window, sub.fields.front().first));
		}

		// ---- The art window: one art resource at a time, chosen from the list at its top. It works on a copy of the working
		// record, which OK puts back. A resource is 0x54 bytes: name (64 characters), +0x40 flags, +0x44, +0x48, +0x4C values
		// whose meaning goes with the type, +0x50 the type, +0x51 and +0x52 two bytes [reference, editor: its art window].
		constexpr std::size_t ArtName = 0;
		constexpr std::size_t ArtNameLength = 64;
		constexpr std::size_t ArtFlags = 0x40;
		constexpr std::size_t ArtType = 0x50;
		constexpr std::size_t ArtSize = 0x54;
		constexpr std::uint32_t ArtPresent = 1;
		// Each animation's left and right footstep frames, 16 bit each [reference, editor: its art window sets them].
		constexpr std::size_t FootstepFrames = 0x1323;
		enum class ArtNumber { U8, U32, Fixed, Fixed16 };
		struct ArtField {
			const wchar_t* label;
			std::size_t offset;
			ArtNumber number;
		};
		// The fields each type has, as the official editor labels them.
		std::vector<ArtField> ArtFields(std::uint8_t type) {
			switch (type) {
				case 1: case 2: case 3: return { { L"Width", 0x44, ArtNumber::Fixed }, { L"Height", 0x48, ArtNumber::Fixed }, { L"Frames", 0x4C, ArtNumber::U32 } };
				case 5: return { { L"Scale", 0x44, ArtNumber::Fixed }, { L"Frames", 0x48, ArtNumber::U32 } };
				case 6: return { { L"Frames", 0x44, ArtNumber::U32 }, { L"FPS", 0x48, ArtNumber::U32 }, { L"Start Dist", 0x4C, ArtNumber::Fixed16 },
					{ L"End Dist", 0x4E, ArtNumber::Fixed16 }, { L"Start AF", 0x51, ArtNumber::U8 }, { L"End AF", 0x52, ArtNumber::U8 } };
				case 7: return { { L"ID", 0x44, ArtNumber::U32 } };
				default: return {};
			}
		}
		struct ArtFlag {
			const wchar_t* label;
			std::uint32_t bit;
			// The types it goes with, as bits by type number.
			std::uint32_t types;
		};
		constexpr std::uint32_t Sprites = (1u << 1) | (1u << 2) | (1u << 3);
		constexpr std::uint32_t Meshes = (1u << 5) | (1u << 6);
		constexpr std::uint32_t AnyType = 0x1FE;
		constexpr std::array<ArtFlag, 12> ArtFlagList{ {
			{ L"Player Colored", 0x2, (1u << 2) | (1u << 3) }, { L"Animating Texture", 0x4, Sprites | (1u << 5) }, { L"Has Start Animation", 0x8, 1u << 6 },
			{ L"Has End Animation", 0x10, 1u << 6 }, { L"Random Start Frame", 0x20, Sprites }, { L"Origin At Bottom", 0x40, Sprites },
			{ L"Doesn't Loop", 0x80, Sprites }, { L"Flat", 0x100, Sprites }, { L"Doesn't Use Progressive Mesh", 0x200, Meshes },
			{ L"Use Animating Texture For Selection", 0x10000, AnyType }, { L"Pre-Load", 0x20000, AnyType }, { L"Blood", 0x40000, AnyType } } };
		const std::array<const wchar_t*, 9> ArtTypes{ L"Null", L"Sprite", L"Alpha", L"Additive Alpha", L"Terrain Mesh", L"Mesh", L"Animating Mesh",
			L"Procedural Mesh", L"Mesh Collection" };

		struct ArtState {
			HWND window = nullptr;
			std::vector<std::uint8_t> record;
			std::size_t slot = 0;
			std::uint8_t shownType = 0;
			std::set<int> dirty;
			bool filling = false;
			bool changed = false;
		};
		ArtState art;
		constexpr wchar_t ArtClass[] = L"KeeperForge2CreatureArtWindow";
		constexpr int ArtFieldsTop = 84;
		constexpr int ArtFlagsTop = 172;
		HWND ArtItem(int id) { return GetDlgItem(art.window, id); }
		std::wstring ArtNumberText(std::size_t at, ArtNumber number) {
			switch (number) {
				case ArtNumber::U8: return std::to_wstring(Read(art.record, at, 1));
				case ArtNumber::U32: return std::to_wstring(Read(art.record, at, 4));
				case ArtNumber::Fixed: return NumberText(art.record, at, CreatureNumber::Fixed);
				case ArtNumber::Fixed16: {
					wchar_t text[32];
					std::swprintf(text, std::size(text), L"%.4f", Read(art.record, at, 2) / FixedOne);
					std::wstring value = text;
					while (value.back() == L'0') value.pop_back();
					if (value.back() == L'.') value.pop_back();
					return value;
				}
			}
			return {};
		}
		std::optional<std::pair<std::uint32_t, int>> ParseArtNumber(const std::wstring& text, ArtNumber number) {
			switch (number) {
				case ArtNumber::U8: if (const auto v = ParseWhole(text, 0, 0xFF)) return std::pair{ static_cast<std::uint32_t>(*v), 1 }; break;
				case ArtNumber::U32: if (const auto v = ParseWhole(text, 0, 0xFFFFFFFFll)) return std::pair{ static_cast<std::uint32_t>(*v), 4 }; break;
				case ArtNumber::Fixed: if (const auto v = ParseNumber(text, CreatureNumber::Fixed)) return std::pair{ *v, 4 }; break;
				case ArtNumber::Fixed16: {
					wchar_t* end = nullptr;
					const auto trimmed = Trimmed(text);
					const auto value = std::wcstod(trimmed.c_str(), &end);
					if (trimmed.empty() || end == nullptr || *end != 0 || !std::isfinite(value)) break;
					const auto scaled = std::llround(value * FixedOne);
					if (scaled < 0 || scaled > 0xFFFF) break;
					return std::pair{ static_cast<std::uint32_t>(scaled), 2 };
				}
			}
			return std::nullopt;
		}
		// The fields, labels and boxes for a type: only those the type has are shown.
		void LayOutArtType(std::uint8_t type) {
			const auto fields = ArtFields(type);
			for (int i = 0; i < 6; ++i) {
				const bool shown = static_cast<std::size_t>(i) < fields.size();
				ShowWindow(ArtItem(IdArtLabelFirst + i), shown ? SW_SHOW : SW_HIDE);
				ShowWindow(ArtItem(IdArtFieldFirst + i), shown ? SW_SHOW : SW_HIDE);
				if (shown) SetWindowTextW(ArtItem(IdArtLabelFirst + i), (std::wstring(fields[static_cast<std::size_t>(i)].label) + L":").c_str());
			}
			for (std::size_t i = 0; i < ArtFlagList.size(); ++i) ShowWindow(ArtItem(IdArtFlagFirst + static_cast<int>(i)), (ArtFlagList[i].types & (1u << type)) != 0 ? SW_SHOW : SW_HIDE);
			const bool animation = ArtSlots()[art.slot].animation >= 0 && type != 0;
			for (const int id : { IdArtFootLeft, IdArtFootRight, IdArtFootLeftLabel, IdArtFootRightLabel }) ShowWindow(ArtItem(id), animation ? SW_SHOW : SW_HIDE);
			InvalidateRect(art.window, nullptr, TRUE);
		}
		// The slot's resource into the controls, as type `type` reads it.
		void ShowArtValues(std::uint8_t type) {
			art.filling = true;
			const auto base = ArtSlots()[art.slot].offset;
			const auto fields = ArtFields(type);
			for (std::size_t i = 0; i < fields.size(); ++i) SetWindowTextW(ArtItem(IdArtFieldFirst + static_cast<int>(i)), ArtNumberText(base + fields[i].offset, fields[i].number).c_str());
			const auto flags = Read(art.record, base + ArtFlags, 4);
			for (std::size_t i = 0; i < ArtFlagList.size(); ++i) CheckDlgButton(art.window, IdArtFlagFirst + static_cast<int>(i), (flags & ArtFlagList[i].bit) != 0 ? BST_CHECKED : BST_UNCHECKED);
			art.filling = false;
		}
		void ShowArtSlot() {
			art.filling = true;
			const auto& slot = ArtSlots()[art.slot];
			SendMessageW(ArtItem(IdArtSlot), CB_SETCURSEL, art.slot, 0);
			const auto type = static_cast<std::uint8_t>(Read(art.record, slot.offset + ArtType, 1));
			art.shownType = type;
			SelectData(ArtItem(IdArtType), type);
			SetWindowTextW(ArtItem(IdArtName), Widen(RecordText(art.record, slot.offset + ArtName, ArtNameLength)).c_str());
			if (slot.animation >= 0) {
				const auto at = FootstepFrames + static_cast<std::size_t>(slot.animation) * 4;
				SetWindowTextW(ArtItem(IdArtFootLeft), std::to_wstring(Read(art.record, at, 2)).c_str());
				SetWindowTextW(ArtItem(IdArtFootRight), std::to_wstring(Read(art.record, at + 2, 2)).c_str());
			}
			LayOutArtType(type);
			art.filling = false;
			ShowArtValues(type);
			art.dirty.clear();
		}
		bool RefuseArt(int id, const wchar_t* why) {
			MessageBeep(MB_ICONWARNING);
			SetWindowTextW(ArtItem(IdArtNote), why);
			SetFocus(ArtItem(id));
			SendMessageW(ArtItem(id), EM_SETSEL, 0, -1);
			return false;
		}
		// The changed controls into the copy. A changed type writes every field the new type has, so none keeps another type's
		// meaning; Null clears the resource, as the official editor does.
		bool ReadArtSlot() {
			if (art.dirty.empty()) return true;
			const auto& slot = ArtSlots()[art.slot];
			auto record = art.record;
			const auto type = static_cast<std::uint8_t>(SelectedData(ArtItem(IdArtType)).value_or(art.shownType));
			const bool typeChanged = art.dirty.contains(IdArtType) && type != Read(record, slot.offset + ArtType, 1);
			if (type == 0) {
				if (typeChanged) std::fill_n(record.begin() + static_cast<std::ptrdiff_t>(slot.offset), ArtSize, std::uint8_t{ 0 });
			}
			else {
				if (art.dirty.contains(IdArtName)) {
					const auto name = PlainText(Text(ArtItem(IdArtName)));
					if (!name) return RefuseArt(IdArtName, L"A resource name is plain letters, digits and signs.");
					WriteRecordText(record, slot.offset + ArtName, ArtNameLength, *name);
				}
				const auto fields = ArtFields(type);
				for (std::size_t i = 0; i < fields.size(); ++i) {
					const int id = IdArtFieldFirst + static_cast<int>(i);
					if (!typeChanged && !art.dirty.contains(id)) continue;
					const auto value = ParseArtNumber(Text(ArtItem(id)), fields[i].number);
					if (!value) return RefuseArt(id, L"That value does not fit this field.");
					Write(record, slot.offset + fields[i].offset, value->second, value->first);
				}
				auto flags = Read(record, slot.offset + ArtFlags, 4) | ArtPresent;
				for (std::size_t i = 0; i < ArtFlagList.size(); ++i) {
					if (!art.dirty.contains(IdArtFlagFirst + static_cast<int>(i))) continue;
					if (IsDlgButtonChecked(art.window, IdArtFlagFirst + static_cast<int>(i)) == BST_CHECKED) flags |= ArtFlagList[i].bit;
					else flags &= ~ArtFlagList[i].bit;
				}
				Write(record, slot.offset + ArtFlags, 4, flags);
				Write(record, slot.offset + ArtType, 1, type);
				if (slot.animation >= 0) {
					const auto at = FootstepFrames + static_cast<std::size_t>(slot.animation) * 4;
					for (const auto [id, place] : { std::pair{ IdArtFootLeft, at }, std::pair{ IdArtFootRight, at + 2 } }) {
						if (!art.dirty.contains(id)) continue;
						const auto value = ParseWhole(Text(ArtItem(id)), 0, 0xFFFF);
						if (!value) return RefuseArt(id, L"A footstep frame is a whole number from 0 to 65535.");
						Write(record, place, 2, static_cast<std::uint32_t>(*value));
					}
				}
			}
			art.changed = art.changed || record != art.record;
			art.record = std::move(record);
			art.dirty.clear();
			return true;
		}
		void CloseArt() {
			if (art.window != nullptr) DestroyWindow(art.window);
		}
		LRESULT CALLBACK ArtWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
			if (const auto color = ControlColor(message, wParam, lParam)) return *color;
			if (IsCopyPicture(message, wParam)) {
				CopyPicture(window);
				return 0;
			}
			switch (message) {
				case WM_COMMAND: {
					const int id = LOWORD(wParam);
					const int code = HIWORD(wParam);
					if (art.filling) return 0;
					if (id == IDOK && code == BN_CLICKED) {
						if (!ReadArtSlot()) return 0;
						const bool changed = art.changed;
						if (changed) Record() = art.record;
						CloseArt();
						ShowRecord();
						if (changed) Note(L"Art changed. Apply or OK gives it to the level.");
						return 0;
					}
					if (id == IDCANCEL && code == BN_CLICKED) {
						CloseArt();
						return 0;
					}
					if (id == IdArtSlot && code == CBN_SELCHANGE) {
						const auto at = SendMessageW(ArtItem(IdArtSlot), CB_GETCURSEL, 0, 0);
						if (at < 0 || !ReadArtSlot()) {
							SendMessageW(ArtItem(IdArtSlot), CB_SETCURSEL, art.slot, 0);
							return 0;
						}
						art.slot = static_cast<std::size_t>(at);
						ShowArtSlot();
						return 0;
					}
					if (id == IdArtType && code == CBN_SELCHANGE) {
						// The new type's fields, showing what the resource's bytes are as that type.
						const auto type = static_cast<std::uint8_t>(SelectedData(ArtItem(IdArtType)).value_or(0));
						LayOutArtType(type);
						ShowArtValues(type);
						art.dirty.insert(id);
						return 0;
					}
					if (code == EN_CHANGE || code == BN_CLICKED) art.dirty.insert(id);
					return 0;
				}
				case WM_DRAWITEM:
					if (DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
					break;
				case DM_GETDEFID:
					return MAKELRESULT(IDOK, DC_HASDEFID);
				case WM_ERASEBKGND: {
					RECT area{};
					GetClientRect(window, &area);
					FillRect(reinterpret_cast<HDC>(wParam), &area, theme.backBrush);
					return TRUE;
				}
				case WM_CLOSE:
					CloseArt();
					return 0;
				case WM_DESTROY:
					art = ArtState{};
					if (state.window != nullptr) {
						EnableWindow(state.window, TRUE);
						SetActiveWindow(state.window);
					}
					return 0;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}
		void OpenArtWindow(std::size_t slot) {
			if (slot >= ArtSlots().size() || !ReadRecord()) return;
			CloseArt();
			constexpr int Width = 620;
			constexpr int Height = 362;
			constexpr DWORD Style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
			constexpr DWORD Extended = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
			RECT frame{ 0, 0, Px(Width), Px(Height) };
			AdjustWindowRectEx(&frame, Style, FALSE, Extended);
			RECT owner{};
			GetWindowRect(state.window, &owner);
			const int width = frame.right - frame.left;
			const int height = frame.bottom - frame.top;
			const auto title = L"Art Resources - " + CreatureName(state.type - 1u);
			art.window = CreateWindowExW(Extended, ArtClass, title.c_str(), Style, owner.left + (owner.right - owner.left - width) / 2,
				owner.top + (std::max)(0L, (owner.bottom - owner.top - height) / 2), width, height, state.window, nullptr, OurModule(), nullptr);
			if (art.window == nullptr) {
				Note(L"Could not open the art window.");
				return;
			}
			art.record = Record();
			art.slot = slot;
			DarkTitleBar(art.window);
			AddCopyPicture(art.window);
			const auto& slots = ArtSlots();
			Make(art.window, L"STATIC", L"Resource:", SS_RIGHT, 12, 15, 90, 18, -1);
			auto* slotList = Make(art.window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 106, 12, 330, 400, IdArtSlot);
			for (std::size_t i = 0; i < slots.size(); ++i) AddItem(slotList, slots[i].name, static_cast<std::uint32_t>(i));
			Make(art.window, L"STATIC", L"Type:", SS_RIGHT, 12, 47, 90, 18, -1);
			auto* types = Make(art.window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 106, 44, 180, 300, IdArtType);
			for (std::size_t i = 0; i < ArtTypes.size(); ++i) AddItem(types, ArtTypes[i], static_cast<std::uint32_t>(i));
			Make(art.window, L"STATIC", L"Name:", SS_RIGHT, 292, 47, 50, 18, -1);
			EditBox(art.window, 346, 44, 262, IdArtName, static_cast<int>(ArtNameLength - 1));
			for (int i = 0; i < 6; ++i) {
				const int x = 12 + (i % 3) * 200;
				const int y = ArtFieldsTop + (i / 3) * 32;
				Make(art.window, L"STATIC", L"", SS_RIGHT, x, y + 3, 90, 18, IdArtLabelFirst + i);
				EditBox(art.window, x + 94, y, 96, IdArtFieldFirst + i, 16);
			}
			for (std::size_t i = 0; i < ArtFlagList.size(); ++i) {
				const int x = 12 + static_cast<int>(i % 3) * 200;
				const int y = ArtFlagsTop + static_cast<int>(i / 3) * 24;
				if (auto* box = Make(art.window, WC_BUTTONW, ArtFlagList[i].label, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, 196, 20, IdArtFlagFirst + static_cast<int>(i))) Unthemed(box);
			}
			Make(art.window, L"STATIC", L"Left Footstep Frame:", SS_RIGHT, 12, 275, 150, 18, IdArtFootLeftLabel);
			EditBox(art.window, 166, 272, 80, IdArtFootLeft, 5);
			Make(art.window, L"STATIC", L"Right Footstep Frame:", SS_RIGHT, 256, 275, 150, 18, IdArtFootRightLabel);
			EditBox(art.window, 410, 272, 80, IdArtFootRight, 5);
			Make(art.window, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 12, 300, 596, 18, IdArtNote);
			PushButton(art.window, L"OK", Width - 12 - 76 - 6 - 76, 326, 76, IDOK, true);
			PushButton(art.window, L"Cancel", Width - 12 - 76, 326, 76, IDCANCEL);
			ShowArtSlot();
			EnableWindow(state.window, FALSE);
			ShowWindow(art.window, SW_SHOWNORMAL);
			SetFocus(slotList);
		}

		// ---- The window.
		// ---- The smaller window kept by a closing, and put back over the creature window opened again.
		KeptSmallWindow KeepSmall() {
			KeptSmallWindow smallWindow;
			HWND window = nullptr;
			if (prompt.window != nullptr && prompt.kind != 0) {
				window = prompt.window;
				smallWindow.kind = static_cast<SmallWindow>(prompt.kind);
				smallWindow.controls.push_back(Capture(window, IdPromptText));
			}
			else if (sub.window != nullptr && sub.spec != nullptr) {
				window = sub.window;
				smallWindow.kind = SmallWindow::Part;
				smallWindow.which = static_cast<std::size_t>(sub.spec - SubWindows().data());
				smallWindow.dirty = sub.dirty;
				for (const int id : sub.dirty) smallWindow.controls.push_back(Capture(window, id));
			}
			else if (art.window != nullptr) {
				window = art.window;
				smallWindow.kind = SmallWindow::Art;
				smallWindow.which = art.slot;
				smallWindow.dirty = art.dirty;
				smallWindow.artRecord = art.record;
				smallWindow.artChanged = art.changed;
				// The type first, as putting it back lays out the fields the others go in.
				if (art.dirty.contains(IdArtType)) smallWindow.controls.push_back(Capture(window, IdArtType));
				for (const int id : art.dirty) {
					if (id != IdArtType) smallWindow.controls.push_back(Capture(window, id));
				}
			}
			if (window != nullptr) smallWindow.placed = GetWindowRect(window, &smallWindow.place) != FALSE;
			return smallWindow;
		}
		void RestoreSmall(const KeptSmallWindow& smallWindow) {
			HWND window = nullptr;
			switch (smallWindow.kind) {
				case SmallWindow::None: return;
				case SmallWindow::Rename:
				case SmallWindow::AddType:
					if (smallWindow.kind == SmallWindow::Rename) Rename();
					else AddType();
					window = prompt.window;
					if (window == nullptr) return;
					for (const auto& control : smallWindow.controls) RestoreIn(window, control);
					SendMessageW(GetDlgItem(window, IdPromptText), EM_SETSEL, 0, -1);
					break;
				case SmallWindow::Part:
					if (smallWindow.which >= SubWindows().size()) return;
					OpenSubWindow(smallWindow.which);
					window = sub.window;
					if (window == nullptr) return;
					sub.filling = true;
					for (const auto& control : smallWindow.controls) RestoreIn(window, control);
					sub.filling = false;
					sub.dirty = smallWindow.dirty;
					break;
				case SmallWindow::Art: {
					if (smallWindow.which >= ArtSlots().size()) return;
					OpenArtWindow(smallWindow.which);
					window = art.window;
					if (window == nullptr) return;
					// Its copy with the changes made to other resources, then this resource's fields as they read.
					if (smallWindow.artRecord.size() == art.record.size()) {
						art.record = smallWindow.artRecord;
						art.changed = smallWindow.artChanged;
						ShowArtSlot();
					}
					art.filling = true;
					for (const auto& control : smallWindow.controls) {
						RestoreIn(window, control);
						if (control.id == IdArtType) {
							const auto type = static_cast<std::uint8_t>(SelectedData(ArtItem(IdArtType)).value_or(0));
							LayOutArtType(type);
							ShowArtValues(type);
							art.filling = true;
						}
					}
					art.filling = false;
					art.dirty = smallWindow.dirty;
					break;
				}
			}
			if (smallWindow.placed) SetWindowPos(window, nullptr, smallWindow.place.left, smallWindow.place.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		LRESULT CALLBACK CreatureWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
			if (const auto color = ControlColor(message, wParam, lParam)) return *color;
			if (IsCopyPicture(message, wParam)) {
				CopyPicture(window);
				return 0;
			}
			switch (message) {
				case WM_COMMAND: {
					const int id = LOWORD(wParam);
					const int code = HIWORD(wParam);
					if (state.filling) return 0;
					if (code == BN_CLICKED) {
						switch (id) {
							case IDOK:
								if (ReadRecord() && ReadUnit()) {
									GiveBack();
									Destroy();
								}
								return 0;
							case IdApply:
								if (ReadRecord() && ReadUnit()) GiveBack();
								return 0;
							case IDCANCEL:
								Destroy();
								return 0;
							case IdRename: Rename(); return 0;
							case IdUnitOwnStats: OwnStats(); return 0;
							case IdAddType: AddType(); return 0;
							case IdDeleteType: DeleteType(); return 0;
						}
						if (id >= IdArtButtonFirst && id < IdArtButtonFirst + static_cast<int>(ArtButtons().size())) {
							OpenArtWindow(ArtButtons()[static_cast<std::size_t>(id - IdArtButtonFirst)].second);
							return 0;
						}
						if (id >= IdSubButtonFirst && id < IdSubButtonFirst + static_cast<int>(SubWindows().size())) {
							OpenSubWindow(static_cast<std::size_t>(id - IdSubButtonFirst));
							return 0;
						}
					}
					if (id == IdName && code == CBN_SELCHANGE) {
						const auto at = SendMessageW(Item(IdName), CB_GETCURSEL, 0, 0);
						if (at < 0) return 0;
						if (!ReadRecord()) {
							SendMessageW(Item(IdName), CB_SETCURSEL, state.type - 1u, 0);
							return 0;
						}
						state.type = static_cast<std::uint8_t>(at + 1);
						ShowRecord();
						return 0;
					}
					if (id == IdUnitHealth && (code == CBN_EDITCHANGE || code == CBN_SELCHANGE)) {
						PercentChanged(code == CBN_SELCHANGE);
						return 0;
					}
					if (id == IdUnitPoints && code == EN_CHANGE) {
						PointsChanged();
						return 0;
					}
					if (id == IdUnitLevel && code == CBN_SELCHANGE) {
						UnitLevelChanged();
						return 0;
					}
					if ((id == IdNewLevel && code == CBN_SELCHANGE) || (id == IdNewDead && code == BN_CLICKED)) {
						// The editor's placing settings for this type: they take effect at once, as "Places as" does.
						const auto level = static_cast<std::uint8_t>(SelectedData(Item(IdNewLevel)).value_or(1));
						const bool dead = IsDlgButtonChecked(window, IdNewDead) == BST_CHECKED;
						const auto index = state.type - 1u;
						if (index >= state.data.placeLevels.size()) state.data.placeLevels.resize(index + 1, 1);
						if (index >= state.data.placeDead.size()) state.data.placeDead.resize(index + 1, false);
						state.data.placeLevels[index] = level;
						state.data.placeDead[index] = dead;
						if (state.data.setPlacingDefaults) state.data.setPlacingDefaults(state.type, level, dead);
						return 0;
					}
					if (id == IdPlacesAs && code == CBN_SELCHANGE) {
						// The editor's placing setting, not the level's data: it takes effect at once.
						const bool variant = SelectedData(Item(IdPlacesAs)).value_or(0) != 0;
						const auto index = state.type - 1u;
						if (index < state.data.placesVariant.size()) state.data.placesVariant[index] = variant;
						if (state.data.setPlacesVariant) state.data.setPlacesVariant(state.type, variant);
						return 0;
					}
					if (id == IdAttribute && code == CBN_SELCHANGE) {
						const auto at = SendMessageW(Item(IdAttribute), CB_GETCURSEL, 0, 0);
						if (at < 0) return 0;
						if (!ReadValue()) {
							SendMessageW(Item(IdAttribute), CB_SETCURSEL, state.attribute, 0);
							return 0;
						}
						state.attribute = static_cast<std::size_t>(at);
						state.filling = true;
						ShowAttribute();
						state.filling = false;
						return 0;
					}
					if ((id == IdEvil || id == IdGood) && code == BN_CLICKED) {
						CheckDlgButton(window, IdEvil, id == IdEvil ? BST_CHECKED : BST_UNCHECKED);
						CheckDlgButton(window, IdGood, id == IdGood ? BST_CHECKED : BST_UNCHECKED);
						state.dirty.insert(id);
						return 0;
					}
					if ((id == IdMale || id == IdFemale) && code == BN_CLICKED) {
						CheckDlgButton(window, IdMale, id == IdMale ? BST_CHECKED : BST_UNCHECKED);
						CheckDlgButton(window, IdFemale, id == IdFemale ? BST_CHECKED : BST_UNCHECKED);
						state.dirty.insert(id);
						return 0;
					}
					const bool attractionKind = id >= IdAttractionFirst && id < IdAttractionFirst + static_cast<int>(Offset::AttractionCount) * 3 && (id - IdAttractionFirst) % 3 == 0;
					if (attractionKind && code == CBN_SELCHANGE) {
						state.filling = true;
						FillAttractionTargets(static_cast<std::size_t>(id - IdAttractionFirst) / 3, SelectedData(Item(id)).value_or(0));
						SendMessageW(Item(id + 1), CB_SETCURSEL, 0, 0);
						state.filling = false;
						state.dirty.insert(id);
						return 0;
					}
					if (code == EN_CHANGE || code == CBN_SELCHANGE || (code == BN_CLICKED && id >= IdFlagFirst && id < IdFlagFirst + static_cast<int>(FlagBoxes.size()))) state.dirty.insert(id);
					return 0;
				}
				case WM_DRAWITEM:
					if (DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
					break;
				case DM_GETDEFID:
					return MAKELRESULT(IDOK, DC_HASDEFID);
				case WM_VSCROLL:
					ScrollMessage(wParam);
					return 0;
				case WM_MOUSEWHEEL:
					if (!state.scrolls) break;
					ScrollTo(state.scrollY - GET_WHEEL_DELTA_WPARAM(wParam) * Px(32) * 3 / WHEEL_DELTA);
					return 0;
				case WM_ERASEBKGND: {
					RECT area{};
					GetClientRect(window, &area);
					FillRect(reinterpret_cast<HDC>(wParam), &area, theme.backBrush);
					return TRUE;
				}
				case WM_PAINT: {
					PAINTSTRUCT paint{};
					auto* dc = BeginPaint(window, &paint);
					// Scrolled down, what the window draws itself goes up with its controls.
					SetViewportOrgEx(dc, 0, -state.scrollY, nullptr);
					PaintFrame(dc, 12, 404, 776, 92, L"Attractions");
					EndPaint(window, &paint);
					return 0;
				}
				case WM_SETCURSOR: {
					// The game hides Windows' pointer on its thread, which is this window's too. Raised here, put back on closing.
					CURSORINFO info{ sizeof(info) };
					if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) == 0) {
						do { ++state.cursorRaised; } while (ShowCursor(TRUE) < 0);
					}
					break;
				}
				case WM_CLOSE:
					Destroy();
					return 0;
				case WM_DESTROY:
					if (state.hook != nullptr) UnhookWindowsHookEx(state.hook);
					state.hook = nullptr;
					for (; state.cursorRaised > 0; --state.cursorRaised) ShowCursor(FALSE);
					state.window = nullptr;
					return 0;
				case WM_NCDESTROY:
					if (state.font != nullptr) DeleteObject(state.font);
					state.font = nullptr;
					break;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}
		// The game's message loop has no IsDialogMessage, so Tab, Enter and Esc are given to our windows here, before the game's
		// loop sees the message.
		LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
			if (code == HC_ACTION && wParam == PM_REMOVE) {
				auto* message = reinterpret_cast<MSG*>(lParam);
				if (message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST) {
					for (auto* window : { prompt.window, sub.window, art.window, state.window }) {
						if (window == nullptr || (message->hwnd != window && !IsChild(window, message->hwnd))) continue;
						if (IsDialogMessageW(window, message)) {
							message->message = WM_NULL;
							message->wParam = 0;
							message->lParam = 0;
							// Tab may have moved to a control scrolled out of sight.
							if (window == state.window) ScrollIntoView(GetFocus());
						}
						break;
					}
				}
			}
			return CallNextHookEx(nullptr, code, wParam, lParam);
		}

		HINSTANCE OurModule() {
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&CreatureWindowProc), &module);
			return module;
		}
		bool RegisterWindowClass(const wchar_t* name, WNDPROC procedure) {
			WNDCLASSEXW windowClass{ sizeof(windowClass) };
			windowClass.lpfnWndProc = procedure;
			windowClass.hInstance = OurModule();
			windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
			// None: WM_ERASEBKGND paints it in the theme's color.
			windowClass.hbrBackground = nullptr;
			windowClass.lpszClassName = name;
			return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
		}
		bool RegisterOnce() {
			static bool registered = false;
			if (registered) return true;
			INITCOMMONCONTROLSEX common{ sizeof(common), ICC_STANDARD_CLASSES };
			InitCommonControlsEx(&common);
			registered = RegisterWindowClass(CreatureClass, &CreatureWindowProc) && RegisterWindowClass(PromptClass, &PromptWindowProc)
				&& RegisterWindowClass(SubClass, &SubWindowProc) && RegisterWindowClass(ArtClass, &ArtWindowProc);
			if (!registered) Logger::Warning("Editor windows: could not register the window classes (" + std::to_string(GetLastError()) + ").");
			return registered;
		}
		HFONT MakeFont() {
			NONCLIENTMETRICSW metrics{ sizeof(metrics) };
			if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return CreateFontIndirectW(&metrics.lfMessageFont);
			return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		}
	}

	bool OpenCreatureProperties(CreatureDialogData data, std::uint8_t type) {
		// Opened on anything by the player: what a closing kept is not for this.
		kept.reset();
		if (state.window != nullptr) {
			// While the name window or a part's window is up, it is the one to finish first.
			if (prompt.window == nullptr && sub.window == nullptr && art.window == nullptr && type >= 1 && type <= state.data.records.size() && ReadRecord()) {
				// Now on this creature (or a type from the side panel): a creature's fields not yet applied are left behind.
				state.unit = data.unit;
				state.unitOwnType = 0;
				state.type = type;
				ShowRecord();
				ShowUnit();
			}
			auto* front = prompt.window != nullptr ? prompt.window : sub.window != nullptr ? sub.window : art.window != nullptr ? art.window : state.window;
			ShowWindow(front, SW_SHOWNORMAL);
			SetForegroundWindow(front);
			return true;
		}
		if (data.records.empty() || type < 1 || type > data.records.size() || !RegisterOnce()) return false;
		LoadTheme();
		state = State{};
		state.data = std::move(data);
		state.given = state.data.records;
		state.type = type;
		state.unit = state.data.unit;
		state.font = MakeFont();
		// Laid out for a 15 pixel font, the usual at 96 dpi, and grown with the player's.
		if (auto* screen = GetDC(nullptr)) {
			auto* old = SelectObject(screen, state.font);
			TEXTMETRICW metrics{};
			if (GetTextMetricsW(screen, &metrics) && metrics.tmHeight > 0) state.scale = (std::max)(1.0, metrics.tmHeight / 15.0);
			SelectObject(screen, old);
			ReleaseDC(nullptr, screen);
		}
		DWORD Style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
		constexpr DWORD Extended = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
		RECT frame{ 0, 0, Px(ClientWidth), Px(ClientHeight) };
		AdjustWindowRectEx(&frame, Style, FALSE, Extended);
		int width = frame.right - frame.left;
		int height = frame.bottom - frame.top;
		// Over the game's window (the active one on this thread, as the click that opens this came to it), which keeps it in front.
		auto* owner = GetActiveWindow();
		RECT area{};
		if (owner == nullptr || !GetWindowRect(owner, &area)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
		// The work area (the screen less the taskbar) of the monitor the game is on: a window taller than it is cut to fit and
		// scrolls, so its bottom buttons can always be reached.
		RECT work{};
		MONITORINFO monitor{ sizeof(monitor) };
		if (GetMonitorInfoW(MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST), &monitor)) work = monitor.rcWork;
		else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		state.contentHeight = Px(ClientHeight);
		if (height > work.bottom - work.top) {
			state.scrolls = true;
			Style |= WS_VSCROLL;
			height = work.bottom - work.top;
			width += GetSystemMetrics(SM_CXVSCROLL);
		}
		int x = area.left + (std::max)(0L, (area.right - area.left - width) / 2);
		int y = area.top + (std::max)(0L, (area.bottom - area.top - height) / 2);
		// Kept inside the work area.
		x = (std::clamp)(x, static_cast<int>(work.left), (std::max)(static_cast<int>(work.left), static_cast<int>(work.right) - width));
		y = (std::clamp)(y, static_cast<int>(work.top), (std::max)(static_cast<int>(work.top), static_cast<int>(work.bottom) - height));
		state.window = CreateWindowExW(Extended, CreatureClass, L"Modify Creature Properties", Style, x, y, width, height, owner, nullptr, OurModule(), nullptr);
		if (state.window == nullptr) {
			Logger::Warning("Editor windows: could not make the creature window (" + std::to_string(GetLastError()) + ").");
			if (state.font != nullptr) DeleteObject(state.font);
			state = State{};
			return false;
		}
		DarkTitleBar(state.window); AddCopyPicture(state.window);
		Build();
		SetUpScrolling();
		ShowRecord();
		ShowUnit();
		state.hook = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, GetCurrentThreadId());
		if (state.hook == nullptr) Logger::Warning("Editor windows: no keyboard hook, so Tab and Enter will not move about the creature window.");
		ShowWindow(state.window, SW_SHOWNORMAL);
		SetForegroundWindow(state.window);
		SetFocus(Item(IdName));
		Logger::Debug("Editor windows: creature properties opened on " + std::to_string(type) + (theme.dark ? ", dark." : "."));
		return true;
	}

	bool ReopenCreatureProperties(CreatureDialogData data, std::uint8_t type) {
		// Only onto the list it was working from: if the level's list changed since, its copy is out of date.
		if (!kept || state.window != nullptr || kept->given != data.records) return OpenCreatureProperties(std::move(data), type);
		auto edits = std::move(*kept);
		if (!OpenCreatureProperties(std::move(data), type)) return false;
		state.data.records = std::move(edits.records);
		state.given = std::move(edits.given);
		if (edits.type >= 1 && edits.type <= state.data.records.size()) state.type = edits.type;
		if (edits.attribute < LevelFiles::CreatureAttributes().size()) state.attribute = edits.attribute;
		state.unitOwnType = edits.unitOwnType;
		if (edits.placed) SetWindowPos(state.window, nullptr, edits.place.left, edits.place.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		ShowRecord();
		ShowUnit();
		// The changed controls as they read, then marked changed again so OK or Apply reads them.
		state.filling = true;
		for (const auto& control : edits.controls) Restore(control);
		state.filling = false;
		state.dirty = std::move(edits.dirty);
		Note(L"Opened again with the changes it had when it closed.");
		// The smaller window it had open, over it again with its own changes.
		RestoreSmall(edits.smallWindow);
		Logger::Debug("Editor windows: creature properties opened again with " + std::to_string(state.dirty.size()) + " changed field(s) kept.");
		return true;
	}

	std::optional<std::vector<std::vector<std::uint8_t>>> TakeCreatureList() { return std::exchange(pending, std::nullopt); }

	std::optional<UnitChange> TakeUnitChange() { return std::exchange(pendingUnit, std::nullopt); }

	void SetUnit(std::optional<UnitData> unit) {
		if (state.window == nullptr) return;
		state.unit = std::move(unit);
		state.unitOwnType = 0;
		if (state.unit && state.unit->type >= 1 && state.unit->type <= state.data.records.size()) state.type = state.unit->type;
		ShowRecord();
		ShowUnit();
	}

	std::optional<std::uint16_t> UnitId() {
		if (state.window == nullptr || !state.unit) return std::nullopt;
		return state.unit->id;
	}

	void RefreshUnit(std::optional<UnitData> unit) {
		if (state.window == nullptr || !state.unit) return;
		if (!unit) {
			SetUnit(std::nullopt);
			return;
		}
		// Only the creature's own row: the type's fields keep whatever is being changed there.
		state.unit = std::move(unit);
		ShowUnit();
	}

	void SetUnitSelected(bool selected) {
		if (state.unitSelected == selected) return;
		state.unitSelected = selected;
		ShowUnitTitle();
	}

	bool CreaturePropertiesOpen() { return state.window != nullptr; }

	void CloseCreatureProperties(bool keepEdits) {
		if (state.window == nullptr) return;
		// Kept first, while the smaller window it may have open is still there to read.
		if (keepEdits) Keep();
		else kept.reset();
		ClosePrompt();
		CloseSub();
		CloseArt();
		// A change to the creature not yet taken is for the creature that is going; a list already given back stays to be taken.
		pendingUnit.reset();
		Destroy();
	}

	void ForgetKeptEdits() { kept.reset(); }

	void CloseAll() {
		kept.reset();
		ClosePrompt();
		CloseSub();
		CloseArt();
		pendingUnit.reset();
		Destroy();
		pending.reset();
	}
}
