#include "GameFixes.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "LevelEditor.h"
#include "Logger.h"
#include "Memory.h"
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		bool exitListGrown = false;

		bool Hook(std::uintptr_t target, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			if (!Memory::Matches(target, expected)) return false;
			if (!HookManager::CreateHook(target, detour, original) || !HookManager::EnableHook(target)) {
				HookManager::RemoveHook(target);
				return false;
			}
			return true;
		}
		void Report(const char* fix, bool installed) {
			if (!installed) Logger::Warning(std::string("Game fixes: ") + fix + ": unexpected game code, not fixed.");
		}

		// ---- Spare bytes after every block of the game's allocator (0x5B75F0, the C runtime's malloc). The pixel
		// converter (0x5FC460) reads each pixel as 4 bytes, so for a 3-byte-per-pixel picture the last pixel of a row
		// reads one byte past the end of the PNG reader's row buffer (width * 3 bytes from here). When that block
		// ended at the last byte of a memory page, the game crashed (reading at 0x5FCB80) while loading art.
		using GameAllocFn = void*(__cdecl*)(std::uint32_t size);
		GameAllocFn originalGameAlloc = nullptr;
		constexpr std::uint32_t GameAllocSlack = 16;
		void* __cdecl GameAllocDetour(std::uint32_t size) {
			// The game's own: nothing for size 0.
			if (size == 0) return nullptr;
			return originalGameAlloc(size <= 0xFFFFFFFFu - GameAllocSlack ? size + GameAllocSlack : size);
		}

		// ---- Workshop: the item being made is judged by its own build time, not item 0's (an item queued after a
		// quicker one was done too early, after a slower one too late, and the percent shown was off).
		using ManufactureFn = std::int32_t(__fastcall*)(const std::uint8_t* list, void* unused, std::uint32_t work);
		ManufactureFn originalManufactureDone = nullptr;
		ManufactureFn originalManufacturePercent = nullptr;
		constexpr std::uintptr_t ManufactureCount = 0x803;
		constexpr std::uintptr_t ManufactureCurrent = 0x801;
		constexpr std::uintptr_t ManufactureItemSize = 0x10;
		std::uint16_t CurrentBuildTime(const std::uint8_t* list) {
			return *reinterpret_cast<const std::uint16_t*>(list + list[ManufactureCurrent] * ManufactureItemSize);
		}
		std::int32_t __fastcall ManufactureDoneDetour(const std::uint8_t* list, void*, std::uint32_t work) {
			if (list[ManufactureCount] == 0) return 0;
			return static_cast<std::uint16_t>(work) >= CurrentBuildTime(list) ? 1 : 0;
		}
		std::int32_t __fastcall ManufacturePercentDetour(const std::uint8_t* list, void*, std::uint32_t work) {
			if (list[ManufactureCount] == 0) return 0;
			const std::uint16_t buildTime = CurrentBuildTime(list);
			// As the game: 100 * work / build time (a build time of 0 would have crashed it too).
			if (buildTime == 0) return 0;
			return static_cast<std::int32_t>(static_cast<std::uint16_t>(work)) * 100 / buildTime;
		}

		// ---- Screen capture tools: every DirectInput device starts out wanting exclusive and foreground (5, 0x5DCB40), and only a
		// full screen mode change asks for shared keyboard (6, 0x5BC280). An exclusive keyboard keeps keys from other programs
		// while the game is in front, so Print Screen and the hot keys of Lightshot and the like did nothing until it lost focus.
		// The keyboard is now always shared; the game reads it the same way (it already runs so after a mode change).
		using DeviceApplyFn = std::int32_t*(__fastcall*)(std::uint8_t* part, void* unused, std::int32_t* result);
		DeviceApplyFn originalDeviceApply = nullptr;
		constexpr std::uint32_t InputExclusive = 1;
		constexpr std::uint32_t InputShared = 2;
		std::int32_t* __fastcall DeviceApplyDetour(std::uint8_t* part, void* unused, std::int32_t* result) {
			const auto* keyboard = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::InputSystem) + Game::Offset::InputKeyboardDevice);
			if (keyboard != nullptr && part == keyboard + Game::Offset::InputDevicePart) {
				auto& flags = *reinterpret_cast<std::uint32_t*>(part + Game::Offset::InputDeviceFlags);
				if ((flags & InputExclusive) != 0) flags = (flags & ~InputExclusive) | InputShared;
			}
			return originalDeviceApply(part, unused, result);
		}

		// ---- Text input (chat box): the game subclasses its window with SetWindowLongA and then calls the previous
		// window procedure directly. When something else subclassed the window first (Steam, a driver), Windows gives
		// back a handle (0xFFFFxxxx) that only CallWindowProcA understands, and the direct call crashed. Both calls go
		// through CallWindowProcA now, with the same arguments; the game's code continues after the call.
		std::uintptr_t oldProcResume1 = 0;
		std::uintptr_t oldProcResume2 = 0;
		const void* callWindowProc = reinterpret_cast<const void*>(&CallWindowProcA);
		// At 0x43032C: lParam 0 is pushed, EAX is the previous procedure, EBP the window; the message is 0x410 and
		// wParam the low word of [esp + 0x3C].
		__declspec(naked) void OldProcCallStub1() {
			__asm {
				mov ecx, dword ptr [esp + 0x3C]
				and ecx, 0xFFFF
				push ecx
				push 0x410
				push ebp
				push eax
				call dword ptr [callWindowProc]
				jmp dword ptr [oldProcResume1]
			}
		}
		// At 0x4305DD: lParam is pushed, ESI is the previous procedure, EDI the message, EBP the window and wParam is
		// [esp + 0x44].
		__declspec(naked) void OldProcCallStub2() {
			__asm {
				mov ecx, dword ptr [esp + 0x44]
				push ecx
				push edi
				push ebp
				push esi
				call dword ptr [callWindowProc]
				jmp dword ptr [oldProcResume2]
			}
		}

		// ---- Computer player threat: the threat it answers is worked out as 32 bit but kept in a 16 bit slot, so a large
		// army wrapped round to a small or negative number and was answered with a creature or two, or none. Each store
		// now keeps the value within the slot's range (-32768..32767). Registers and flags are kept as they were.
		std::uintptr_t threatResumeA = 0;
		std::uintptr_t threatResumeB = 0;
		std::uintptr_t threatResumeC = 0;
		std::uintptr_t threatResumeD = 0;
		std::uintptr_t threatResumeE = 0;
		__declspec(naked) void ThreatStoreStubA() {
			__asm {
				pushfd
				push edx
				cmp edx, 0x7FFF
				jle a_low
				mov edx, 0x7FFF
			a_low:
				cmp edx, -0x8000
				jge a_store
				mov edx, -0x8000
			a_store:
				mov word ptr [esi + ebp * 8 + 0x5A], dx
				pop edx
				popfd
				jmp dword ptr [threatResumeA]
			}
		}
		__declspec(naked) void ThreatStoreStubB() {
			__asm {
				pushfd
				push eax
				cmp eax, 0x7FFF
				jle b_low
				mov eax, 0x7FFF
			b_low:
				cmp eax, -0x8000
				jge b_store
				mov eax, -0x8000
			b_store:
				mov word ptr [esi + ebp * 8 + 0x5A], ax
				pop eax
				popfd
				jmp dword ptr [threatResumeB]
			}
		}
		__declspec(naked) void ThreatStoreStubC() {
			__asm {
				pushfd
				push edx
				cmp edx, 0x7FFF
				jle c_low
				mov edx, 0x7FFF
			c_low:
				cmp edx, -0x8000
				jge c_store
				mov edx, -0x8000
			c_store:
				mov word ptr [esi + ebx * 8 + 0x5A], dx
				pop edx
				popfd
				jmp dword ptr [threatResumeC]
			}
		}
		__declspec(naked) void ThreatStoreStubD() {
			__asm {
				pushfd
				push eax
				cmp eax, 0x7FFF
				jle d_low
				mov eax, 0x7FFF
			d_low:
				cmp eax, -0x8000
				jge d_store
				mov eax, -0x8000
			d_store:
				mov word ptr [esi + ebx * 8 + 0x5A], ax
				pop eax
				popfd
				jmp dword ptr [threatResumeD]
			}
		}
		__declspec(naked) void ThreatStoreStubE() {
			__asm {
				pushfd
				push eax
				cmp eax, 0x7FFF
				jle e_low
				mov eax, 0x7FFF
			e_low:
				cmp eax, -0x8000
				jge e_store
				mov eax, -0x8000
			e_store:
				mov word ptr [ebp + edi * 8 + 0x5A], ax
				pop eax
				popfd
				jmp dword ptr [threatResumeE]
			}
		}
		struct ThreatStore {
			std::uintptr_t address;
			std::vector<std::uint8_t> bytes;
			const void* stub;
			std::uintptr_t* resume;
		};
		bool InstallThreatClamp() {
			const ThreatStore stores[] = {
				{ Game::Address::ThreatStoreA, { 0x66, 0x89, 0x54, 0xEE, 0x5A }, reinterpret_cast<const void*>(&ThreatStoreStubA), &threatResumeA },
				{ Game::Address::ThreatStoreB, { 0x66, 0x89, 0x44, 0xEE, 0x5A }, reinterpret_cast<const void*>(&ThreatStoreStubB), &threatResumeB },
				{ Game::Address::ThreatStoreC, { 0x66, 0x89, 0x54, 0xDE, 0x5A }, reinterpret_cast<const void*>(&ThreatStoreStubC), &threatResumeC },
				{ Game::Address::ThreatStoreD, { 0x66, 0x89, 0x44, 0xDE, 0x5A }, reinterpret_cast<const void*>(&ThreatStoreStubD), &threatResumeD },
				{ Game::Address::ThreatStoreE, { 0x66, 0x89, 0x44, 0xFD, 0x5A }, reinterpret_cast<const void*>(&ThreatStoreStubE), &threatResumeE },
			};
			// All or none: check every place before changing any.
			for (const auto& store : stores) {
				if (!Memory::Matches(Game::Resolve(store.address), store.bytes)) return false;
			}
			for (const auto& store : stores) {
				const auto address = Game::Resolve(store.address);
				*store.resume = address + store.bytes.size();
				if (!Memory::WriteJump(address, store.stub, store.bytes.size())) return false;
			}
			return true;
		}

		// ---- Backstab (and the North/South advantage): a melee hit gets up to +51% damage by the angle between target and
		// attacker, but the game used the compass angle alone, so an attacker on the +y side always got the most and one on
		// the -y side none, whichever way the target faced. The angle is now taken from the target's facing (+0xF4, turned
		// with the same angle function by 0x4B2C10): 0 when the target faces the attacker, the bonus only from behind.
		std::uintptr_t backstabResume = 0;
		__declspec(naked) void BackstabAngleStub() {
			__asm {
				add esp, 8
				// The target's facing (creature + 0xF4).
				sub ax, word ptr [esi + 0xF4]
				jmp dword ptr [backstabResume]
			}
		}

		// ---- Wooden Bridge burning: every change to a room attaches each of its tiles again, and attaching set the tile's
		// burn level to 0, so selling or losing one tile of a burning bridge put out all the rest. Attaching now leaves a
		// bridge tile's burn level alone; a bridge tile being built gets its burn level set to 0 instead (below).
		std::uintptr_t thingTable = 0;
		std::uintptr_t burnReset = 0;
		std::uintptr_t burnResume = 0;
		__declspec(naked) void AttachBurnStub() {
			__asm {
				// The room's tag is the third argument (as the game reads it just after).
				mov eax, dword ptr [esp + 0x1C]
				and eax, 0xFFFF
				mov ecx, dword ptr [thingTable]
				mov eax, dword ptr [ecx + eax * 4]
				test eax, eax
				jz reset
				// Room type (+ 0x18) 8: Wooden Bridge.
				cmp byte ptr [eax + 0x18], 8
				je done
			reset:
				push 0
				mov ecx, esi
				call dword ptr [burnReset]
			done:
				jmp dword ptr [burnResume]
			}
		}
		using TileBurnFn = void(__fastcall*)(std::uint8_t* tile, void* unused, std::uint32_t level);
		using MapTileAtFn = std::uint8_t*(__fastcall*)(void* world, void* unused, std::int32_t x, std::int32_t y);

		// ---- Room limit: a player may have 96 rooms. Selling a tile splits its room and attaches the other tiles again,
		// allowed only as many new rooms as the player has left; a tile that then fit nowhere was left with no room, which
		// crashed the game later. One tile's removal leaves at most 4 pieces, so the sale is now always allowed 3 more
		// (the room itself counts as one). A player who has gone over 96 that way could then build without limit (the
		// game's "96 - count" wraps), so building is given 0 rooms left instead.
		using AddTileFn = std::int32_t(__fastcall*)(std::uint8_t* rooms, void* unused, std::int32_t x, std::int32_t y, std::uint32_t type,
			std::uint32_t player, std::uint32_t roomsLeft, std::int8_t* made, std::uint32_t orientation);
		using RemoveTileFn = std::int32_t(__fastcall*)(std::uint8_t* rooms, void* unused, std::int32_t x, std::int32_t y, std::uint32_t roomsLeft, std::int8_t* made);
		AddTileFn originalAddTile = nullptr;
		RemoveTileFn originalRemoveTile = nullptr;
		constexpr std::int8_t SaleRoomsLeft = 3;
		bool roomLimitLogged = false;
		std::int32_t __fastcall AddTileDetour(std::uint8_t* rooms, void* unused, std::int32_t x, std::int32_t y, std::uint32_t type,
			std::uint32_t player, std::uint32_t roomsLeft, std::int8_t* made, std::uint32_t orientation) {
			if (static_cast<std::int8_t>(roomsLeft) < 0) roomsLeft &= ~0xFFu;
			if (static_cast<std::uint8_t>(type) == Game::Room::Wooden_Bridge) {
				auto* world = *reinterpret_cast<std::uint8_t**>(rooms + 6);
				if (world != nullptr) {
					const auto tileAt = reinterpret_cast<MapTileAtFn>((*reinterpret_cast<std::uintptr_t**>(world))[Game::Offset::WorldTileAt / sizeof(std::uintptr_t)]);
					auto* tile = tileAt(world, nullptr, x, y);
					if (tile != nullptr) reinterpret_cast<TileBurnFn>(burnReset)(tile, nullptr, 0);
				}
			}
			return originalAddTile(rooms, unused, x, y, type, player, roomsLeft, made, orientation);
		}
		std::int32_t __fastcall RemoveTileDetour(std::uint8_t* rooms, void* unused, std::int32_t x, std::int32_t y, std::uint32_t roomsLeft, std::int8_t* made) {
			if (static_cast<std::int8_t>(roomsLeft) < SaleRoomsLeft) {
				if (!roomLimitLogged) {
					roomLimitLogged = true;
					Logger::Debug("Game fixes: a room tile was sold near the room limit; its room may split past the limit (first time).");
				}
				roomsLeft = (roomsLeft & ~0xFFu) | static_cast<std::uint8_t>(SaleRoomsLeft);
			}
			return originalRemoveTile(rooms, unused, x, y, roomsLeft, made);
		}

		// ---- Dropping from the hand: the interface checks the drop rules on the machine that clicked, and the drop action
		// then runs on every machine some turns later without checking again, by which time the tile may no longer take
		// the thing. The action now asks the same rules (0x40D8F0) at the moment it runs, the same on every machine, and
		// keeps the thing in the hand if they say no.
		using DropAtFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::int32_t* position, std::uint16_t* direction);
		using DropAllowedFn = std::int32_t(__fastcall*)(std::uint8_t* interfaceObject, void* unused, std::uint8_t* thing, std::int32_t x, std::int32_t y);
		DropAtFn originalDropAt = nullptr;
		DropAllowedFn dropAllowed = nullptr;
		constexpr std::uintptr_t InterfacePlayerTag = 0x8;
		constexpr std::uintptr_t InterfaceWorld = 0x53;
		bool dropRefusedLogged = false;
		std::int32_t __fastcall DropAtDetour(std::uint8_t* player, void* unused, std::int32_t* position, std::uint16_t* direction) {
			const auto count = player[Game::Offset::PlayerHandCount];
			auto* world = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (count == 0 || position == nullptr || world == nullptr) return originalDropAt(player, unused, position, direction);
			const auto tag = reinterpret_cast<const std::uint16_t*>(player + Game::Offset::PlayerHand)[count - 1];
			auto* thing = reinterpret_cast<std::uint8_t**>(thingTable)[tag];
			if (thing == nullptr) return originalDropAt(player, unused, position, direction);
			// A stand-in interface with only the two fields the rules read.
			std::uint8_t interfaceObject[0x60]{};
			*reinterpret_cast<std::uint16_t*>(interfaceObject + InterfacePlayerTag) = *reinterpret_cast<const std::uint16_t*>(player + Game::Offset::PlayerTag);
			*reinterpret_cast<std::uint8_t**>(interfaceObject + InterfaceWorld) = world;
			if ((dropAllowed(interfaceObject, nullptr, thing, position[0] >> 12, position[1] >> 12) & 0xFF) == 0) {
				if (!dropRefusedLogged) {
					dropRefusedLogged = true;
					Logger::Debug("Game fixes: a drop from the hand was refused when it ran (the tile no longer takes it; first time).");
				}
				return 0;
			}
			return originalDropAt(player, unused, position, direction);
		}

		// ---- Chat: a received message's text was copied into the chat history with a fixed 0x102 bytes, reading past the
		// end of a shorter message (and out of its memory if it ended at a page's end). The text (16-bit characters, ended
		// by 0) is now copied up to its end, at most 0x102 bytes, and the rest of the line is cleared.
		constexpr std::size_t ChatLineBytes = 0x102;
		std::uintptr_t chatResume = 0;
		// A line is the sender (16 bits, a player's tag, as 0x409EC0 writes it) followed by the text; the sender is always
		// copied, the text up to its end.
		void __cdecl CopyChatText(std::uint8_t* line, const wchar_t* text) {
			std::memset(line, 0, ChatLineBytes);
			if (text == nullptr) return;
			std::size_t characters = 1;
			constexpr std::size_t MostCharacters = ChatLineBytes / sizeof(wchar_t);
			while (characters < MostCharacters && text[characters] != L'\0') ++characters;
			const auto bytes = characters < MostCharacters ? (characters + 1) * sizeof(wchar_t) : ChatLineBytes;
			std::memcpy(line, text, bytes);
		}
		// At 0x409E8F: EBP the message, EBX the interface; ESI and EDI are popped after.
		__declspec(naked) void ChatCopyStub() {
			__asm {
				pushad
				lea eax, [ebp + 4]
				push eax
				lea eax, [ebx + 0x1E37]
				push eax
				call CopyChatText
				add esp, 8
				popad
				jmp dword ptr [chatResume]
			}
		}

		// ---- Dungeon Heart warnings: the Heart's tick raised "the Heart is dying" whenever its health was exactly 50% (and
		// the 10% one at exactly 10%), whether it was being hit or healing, so a recovering Heart passing 50% set it off.
		// A warning is now only allowed on a tick where the Heart's health went down since the last time it was seen.
		// Only game state is used (health, game turn), so every machine decides the same.
		struct HeartSeen {
			std::int32_t health = 0;
			std::uint32_t turn = 0;
		};
		std::unordered_map<std::uintptr_t, HeartSeen> heartsSeen;
		std::int32_t heartWarningAllowed = 0;
		std::uintptr_t heartFifty = 0;
		std::uintptr_t heartTen = 0;
		// Older sightings than this many turns (or from a later turn: another level or a loaded game) do not count.
		constexpr std::uint32_t HeartSightingTurns = 1000;
		std::int32_t __cdecl HeartWarningCheck(std::uintptr_t room, std::int32_t health) {
			auto* world = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (world == nullptr) return 1;
			const auto turn = reinterpret_cast<std::uint32_t(__fastcall*)(void*)>(Game::Resolve(Game::Address::WorldGameTurn))(world);
			bool allowed = false;
			const auto found = heartsSeen.find(room);
			if (found != heartsSeen.end() && turn > found->second.turn && turn - found->second.turn <= HeartSightingTurns)
				allowed = health < found->second.health;
			heartsSeen[room] = { health, turn };
			return allowed ? 1 : 0;
		}
		// At 0x4D9ACB: ESI the Heart room, ECX its health, EAX the percent; EBX is 0 and used after.
		__declspec(naked) void HeartWarningStub() {
			__asm {
				pushad
				push ecx
				push esi
				call HeartWarningCheck
				add esp, 8
				mov dword ptr [heartWarningAllowed], eax
				popad
				cmp dword ptr [heartWarningAllowed], 0
				jnz compare
				// Neither 50 nor 10: no warning.
				mov eax, -1
			compare:
				cmp eax, 0x32
				jnz not_fifty
				jmp dword ptr [heartFifty]
			not_fifty:
				jmp dword ptr [heartTen]
			}
		}

		// ---- Network wait: give the processor up while polling (it spun at full speed).
		using PollFn = std::uint32_t(__fastcall*)(const std::uint8_t* self, void* unused);
		PollFn originalNetworkPoll = nullptr;
		std::uint32_t __fastcall NetworkPollDetour(const std::uint8_t* self, void*) {
			SwitchToThread();
			return *reinterpret_cast<const std::uint32_t*>(self + 0xC) & 2;
		}

		// ---- The game's own debug log, which nothing shows: its lines about going out of step go into ours.
		using GameLogFn = std::int32_t(__cdecl*)(void* window, const char* format, ...);
		GameLogFn originalGameLog = nullptr;
		std::int32_t __cdecl GameLogDetour(void* window, const char* format, ...) {
			char text[256]{};
			va_list arguments;
			va_start(arguments, format);
			std::vsnprintf(text, sizeof(text), format, arguments);
			va_end(arguments);
			if (std::strstr(text, "Sync") != nullptr) {
				std::string line(text);
				while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
				Logger::Warning("Game: " + line);
			}
			return originalGameLog(window, "%s", text);
		}

		// ---- Modifier keys: a key let go while the game could not see the keyboard (alt-tab, Steam's overlay on
		// Shift+Tab) stayed held for it. So the game's answer only counts while Windows agrees.
		using ModifiersFn = std::uint32_t(__cdecl*)();
		ModifiersFn originalModifiers = nullptr;
		bool stuckModifierLogged = false;
		// True during level setup's choice of the first panel, which must not depend on the keys held.
		bool choosingFirstPanel = false;
		std::uint32_t __cdecl ModifiersDetour() {
			if (choosingFirstPanel) return 0;
			const std::uint32_t game = originalModifiers();
			std::uint32_t real = 0;
			if (GetAsyncKeyState(VK_SHIFT) & 0x8000) real |= 1;
			if (GetAsyncKeyState(VK_CONTROL) & 0x8000) real |= 2;
			if (GetAsyncKeyState(VK_MENU) & 0x8000) real |= 4;
			if ((game & ~real) != 0 && !stuckModifierLogged) {
				stuckModifierLogged = true;
				Logger::Debug("Game fixes: the game still saw a modifier key as held after it was let go (first time, value " + std::to_string(game & ~real) + "); ignored.");
			}
			return game & real;
		}

		// ---- Side panel: a pinned panel (Shift + click on its tab) shares the panel's space with every other panel
		// chosen. Each level starts with Creatures alone, whatever was pinned before.
		using PanelTabClickFn = void(__cdecl*)(std::int32_t button, std::int32_t panel, std::uint8_t* interfaceObject);
		PanelTabClickFn originalPanelTabClick = nullptr;
		constexpr std::uintptr_t PinnedPanelCount = 0x5F;
		constexpr std::size_t PanelCount = 4;
		void __cdecl PanelTabClickDetour(std::int32_t button, std::int32_t panel, std::uint8_t* interfaceObject) {
			if (LevelEditor::OnTabClick(button, panel, interfaceObject)) return;
			if (button != 0) return originalPanelTabClick(button, panel, interfaceObject);
			// The count, then one place in the pinned order per panel.
			std::memset(interfaceObject + PinnedPanelCount, 0, PanelCount + 1);
			choosingFirstPanel = true;
			originalPanelTabClick(button, panel, interfaceObject);
			choosingFirstPanel = false;
		}

		// ---- Room objects (Hatchery pens and the like): a room's objects are placed again for the whole room on every change,
		// each on the first tile of the room's chain that fits, the chain being in the order its tiles were built. A Hatchery wants
		// one pen per 6 tiles, on middle tiles at least 1.7 tiles apart, so the order decides how many fit: a 5x5 built in one
		// drag gets 4, one built in strips or grown from a smaller room 2 or 3. The chain is put in row order (top to bottom,
		// left to right, as one drag builds it) before the game places them, so a room gets the same objects however it was
		// built. Every player runs this the same way, so multiplayer stays in step.
		using PlaceObjectsFn = void(__fastcall*)(std::uint8_t* rooms, void* unused, std::uint8_t* room);
		using TileAtFn = std::uint8_t*(__fastcall*)(void* world, void* unused, std::int32_t x, std::int32_t y);
		PlaceObjectsFn originalPlaceObjects = nullptr;
		bool PlacesByChain(std::uint8_t type) {
			switch (type) {
				case 4: case 9: case 10: case 11: case 13: case 14: case 15: case 26: return true;
				default: return false;
			}
		}
		void PutChainInRowOrder(std::uint8_t* rooms, std::uint8_t* room) {
			auto* world = *reinterpret_cast<std::uint8_t**>(rooms + 6);
			if (world == nullptr) return;
			const auto tileAt = reinterpret_cast<TileAtFn>((*reinterpret_cast<std::uintptr_t**>(world))[Game::Offset::WorldTileAt / sizeof(std::uintptr_t)]);
			std::vector<std::pair<std::uint8_t, std::uint8_t>> tiles;
			std::uint8_t x = room[Game::Offset::RoomFirstTile];
			std::uint8_t y = room[Game::Offset::RoomFirstTile + 1];
			// Places are bytes, so a chain longer than this is a loop.
			constexpr std::size_t MostTiles = 256 * 256;
			while ((x != 0 || y != 0) && tiles.size() < MostTiles) {
				const auto* tile = tileAt(world, nullptr, x, y);
				if (tile == nullptr) return;
				tiles.push_back({ x, y });
				x = tile[Game::Offset::TileNextInRoom];
				y = tile[Game::Offset::TileNextInRoom + 1];
			}
			if (tiles.size() < 2 || tiles.size() >= MostTiles) return;
			std::sort(tiles.begin(), tiles.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
			for (std::size_t i = 0; i < tiles.size(); ++i) {
				auto* tile = tileAt(world, nullptr, tiles[i].first, tiles[i].second);
				const bool last = i + 1 == tiles.size();
				tile[Game::Offset::TileNextInRoom] = last ? std::uint8_t{ 0 } : tiles[i + 1].first;
				tile[Game::Offset::TileNextInRoom + 1] = last ? std::uint8_t{ 0 } : tiles[i + 1].second;
			}
			room[Game::Offset::RoomFirstTile] = tiles.front().first;
			room[Game::Offset::RoomFirstTile + 1] = tiles.front().second;
			room[Game::Offset::RoomLastTile] = tiles.back().first;
			room[Game::Offset::RoomLastTile + 1] = tiles.back().second;
		}
		void __fastcall PlaceObjectsDetour(std::uint8_t* rooms, void* unused, std::uint8_t* room) {
			if (rooms != nullptr && room != nullptr && PlacesByChain(room[Game::Offset::RoomType])) PutChainInRowOrder(rooms, room);
			originalPlaceObjects(rooms, unused, room);
		}
	}

	void GameFixes::OnWindowActivated() {
		// Keys let go while another program had the keyboard never reached the game, so they stayed down for it (a key
		// held for alt-tab, Steam's overlay). The game reads its key state again as keys come in, so it is cleared here.
		const auto input = Game::Resolve(Game::Address::InputSystem);
		auto* state = Memory::Read<std::uint8_t*>(input + Game::Offset::InputKeyState);
		if (state == nullptr) return;
		constexpr std::size_t KeyCount = 256;
		std::memset(state + Game::Offset::KeyStateKeys, 0, KeyCount);
	}

	void GameFixes::BeforeGameStarts() {
		// The C runtime's exit function list: 32 entries overflowed (128 now). It is made by the game's start-up code,
		// so only a runtime loaded with the game can change it.
		const auto push = Game::Resolve(Game::Address::ExitListSizePush);
		if (Memory::Matches(push, { 0x68, 0x80, 0x00, 0x00, 0x00 })) {
			Memory::Patch(push, { 0x68, 0x00, 0x02, 0x00, 0x00 });
			exitListGrown = true;
		}
	}

	void GameFixes::Install() {
		if (!exitListGrown) Logger::Debug("Game fixes: the exit function list keeps its size (KeeperForge2 started after the game).");

		// "mov eax, [esp+4]; test eax, eax; jnz +1; ret; push eax; call malloc"
		Report("allocator spare bytes (pixel converter reading past a row)", Hook(Game::Resolve(Game::Address::GameAlloc),
			{ 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x75, 0x01, 0xC3, 0x50, 0xE8 },
			reinterpret_cast<void*>(&GameAllocDetour), reinterpret_cast<void**>(&originalGameAlloc)));

		Report("workshop build time", Hook(Game::Resolve(Game::Address::ManufactureDone), { 0x8A, 0x81, 0x03, 0x08, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x0E, 0x66, 0x8B, 0x44, 0x24, 0x04, 0x66, 0x3B, 0x01 },
			reinterpret_cast<void*>(&ManufactureDoneDetour), reinterpret_cast<void**>(&originalManufactureDone))
			&& Hook(Game::Resolve(Game::Address::ManufacturePercent), { 0x8A, 0x81, 0x03, 0x08, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x1F, 0x8B, 0x44, 0x24, 0x04 },
			reinterpret_cast<void*>(&ManufacturePercentDetour), reinterpret_cast<void**>(&originalManufacturePercent)));

		// Computer player: drop creatures onto its own tiles under threat (jz -> jnz).
		const auto threatJump = Game::Resolve(Game::Address::ThreatDropOwnTileJump);
		const bool threat = Memory::Matches(threatJump - 10, { 0x81, 0xE1, 0xFF, 0x0F, 0x00, 0x00, 0x66, 0x3B, 0x4A, 0x04, 0x0F, 0x84 });
		if (threat) Memory::Patch(threatJump + 1, { 0x85 });
		Report("computer player threat response", threat);
		Report("computer player threat total (16 bit overflow)", InstallThreatClamp());

		// Backstab: "and eax, 0xFFFF; add esp, 8; and eax, 0x7FF" after the angle call.
		const auto backstab = Game::Resolve(Game::Address::BackstabAngle);
		backstabResume = Game::Resolve(Game::Address::BackstabAngleResume);
		Report("backstab angle (target's facing)", Memory::Matches(backstab, { 0x25, 0xFF, 0xFF, 0x00, 0x00, 0x83, 0xC4, 0x08, 0x25, 0xFF, 0x07, 0x00, 0x00 })
			&& Memory::WriteJump(backstab, reinterpret_cast<const void*>(&BackstabAngleStub), 8));

		thingTable = Game::Resolve(Game::Address::ThingTable);
		burnReset = Game::Resolve(Game::Address::TileSetBurnLevel);
		burnResume = Game::Resolve(Game::Address::RoomAttachBurnResume);
		const auto attachBurn = Game::Resolve(Game::Address::RoomAttachBurnReset);
		const auto buildCall = Game::Resolve(Game::Address::BuildRoomTileCall);
		const auto sellCall = Game::Resolve(Game::Address::SellRoomTileCall);
		const auto addTile = Game::Resolve(Game::Address::RoomsAddTile);
		const auto removeTile = Game::Resolve(Game::Address::RoomsRemoveTile);
		originalAddTile = reinterpret_cast<AddTileFn>(addTile);
		originalRemoveTile = reinterpret_cast<RemoveTileFn>(removeTile);
		// "push 0; mov ecx, esi; call 0x452B70", and a "call rel32" at each place.
		const auto relativeCall = [](std::uintptr_t at, std::uintptr_t to) {
			const auto rel = static_cast<std::uint32_t>(to - (at + 5));
			return std::vector<std::uint8_t>{ 0xE8, static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
		};
		const bool callsMatch = Memory::Matches(buildCall, relativeCall(buildCall, addTile)) && Memory::Matches(sellCall, relativeCall(sellCall, removeTile));
		std::vector<std::uint8_t> burnPattern = { 0x6A, 0x00, 0x8B, 0xCE };
		const auto burnCall = relativeCall(attachBurn + 4, burnReset);
		burnPattern.insert(burnPattern.end(), burnCall.begin(), burnCall.end());
		const bool bridge = callsMatch && Memory::Matches(attachBurn, burnPattern)
			&& Memory::WriteJump(attachBurn, reinterpret_cast<const void*>(&AttachBurnStub), 9);
		Report("wooden bridge burning", bridge);
		const bool roomLimit = callsMatch && Memory::WriteCall(buildCall, reinterpret_cast<const void*>(&AddTileDetour))
			&& Memory::WriteCall(sellCall, reinterpret_cast<const void*>(&RemoveTileDetour));
		Report("room limit (selling near the limit)", roomLimit);

		const auto dropCall = Game::Resolve(Game::Address::DropActionDropCall);
		const auto dropAt = Game::Resolve(Game::Address::PlayerDropAt);
		originalDropAt = reinterpret_cast<DropAtFn>(dropAt);
		dropAllowed = reinterpret_cast<DropAllowedFn>(Game::Resolve(Game::Address::InterfaceDropAllowed));
		// The rules start "push ebx; push ebp; push esi; push edi; mov edi, [esp+0x14]; mov esi, ecx".
		Report("drop from hand (rules checked when it runs)", Memory::Matches(dropCall, relativeCall(dropCall, dropAt))
			&& Memory::Matches(reinterpret_cast<std::uintptr_t>(dropAllowed), { 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x8B, 0xF1 })
			&& Memory::WriteCall(dropCall, reinterpret_cast<const void*>(&DropAtDetour)));

		const auto heartCompare = Game::Resolve(Game::Address::HeartWarningCompare);
		heartFifty = Game::Resolve(Game::Address::HeartWarningFifty);
		heartTen = Game::Resolve(Game::Address::HeartWarningTen);
		// "cmp eax, 0x32; jnz +0x12", then at the 10% place "cmp eax, 0x0A; jnz +0x1B".
		Report("Dungeon Heart warnings (only while it is losing health)", Memory::Matches(heartCompare, { 0x83, 0xF8, 0x32, 0x75, 0x12 })
			&& Memory::Matches(heartTen, { 0x83, 0xF8, 0x0A, 0x75, 0x1B })
			&& Memory::WriteJump(heartCompare, reinterpret_cast<const void*>(&HeartWarningStub), 5));

		const auto chatCopy = Game::Resolve(Game::Address::ChatReceivedCopy);
		chatResume = Game::Resolve(Game::Address::ChatReceivedResume);
		Report("chat message read past its end", Memory::Matches(chatCopy, { 0x8D, 0x75, 0x04, 0x8D, 0xBB, 0x37, 0x1E, 0x00, 0x00, 0xB9, 0x40, 0x00, 0x00, 0x00, 0xF3, 0xA5, 0x66, 0xA5 })
			&& Memory::WriteJump(chatCopy, reinterpret_cast<const void*>(&ChatCopyStub), 18));

		Report("network wait", Hook(Game::Resolve(Game::Address::NetworkWaitPoll), { 0x8B, 0x41, 0x0C, 0x83, 0xE0, 0x02, 0xC3 },
			reinterpret_cast<void*>(&NetworkPollDetour), reinterpret_cast<void**>(&originalNetworkPoll)));

		const auto oldProcCall1 = Game::Resolve(Game::Address::TextInputOldProcCall1);
		const auto oldProcCall2 = Game::Resolve(Game::Address::TextInputOldProcCall2);
		oldProcResume1 = Game::Resolve(Game::Address::TextInputOldProcResume1);
		oldProcResume2 = Game::Resolve(Game::Address::TextInputOldProcResume2);
		// "mov ecx, [esp+0x3C]; and ecx, 0xFFFF; push ecx; push 0x410; push ebp; call eax" and
		// "mov ecx, [esp+0x44]; push ecx; push edi; push ebp; call esi".
		const bool textInput = Memory::Matches(oldProcCall1, { 0x8B, 0x4C, 0x24, 0x3C, 0x81, 0xE1, 0xFF, 0xFF, 0x00, 0x00, 0x51, 0x68, 0x10, 0x04, 0x00, 0x00, 0x55, 0xFF, 0xD0 })
			&& Memory::Matches(oldProcCall2, { 0x8B, 0x4C, 0x24, 0x44, 0x51, 0x57, 0x55, 0xFF, 0xD6 })
			&& Memory::WriteJump(oldProcCall1, reinterpret_cast<const void*>(&OldProcCallStub1), 19)
			&& Memory::WriteJump(oldProcCall2, reinterpret_cast<const void*>(&OldProcCallStub2), 9);
		Report("text input window procedure", textInput);

		Report("game log", Hook(Game::Resolve(Game::Address::GameLog), { 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00, 0x83, 0xC9, 0xFF },
			reinterpret_cast<void*>(&GameLogDetour), reinterpret_cast<void**>(&originalGameLog)));
		Report("stuck modifier keys", Hook(Game::Resolve(Game::Address::ModifiersHeld), { 0x56, 0x6A, 0x36, 0xB9, 0x90, 0xCF, 0x79, 0x00 },
			reinterpret_cast<void*>(&ModifiersDetour), reinterpret_cast<void**>(&originalModifiers)));
		Report("side panel at level start", Hook(Game::Resolve(Game::Address::PanelTabClick), { 0x53, 0x8B, 0x5C, 0x24, 0x08 },
			reinterpret_cast<void*>(&PanelTabClickDetour), reinterpret_cast<void**>(&originalPanelTabClick)));
		// push -1; push 0x64CEFB (its exception handler).
		Report("room objects (Hatchery pens)", Hook(Game::Resolve(Game::Address::RoomsPlaceObjects), { 0x6A, 0xFF, 0x68, 0xFB, 0xCE, 0x64, 0x00 },
			reinterpret_cast<void*>(&PlaceObjectsDetour), reinterpret_cast<void**>(&originalPlaceObjects)));

		// push esi; mov esi, ecx; xor eax, eax; push edi; mov edx, [esi+0xC] [verified in the assembly at 0x5DCBE0].
		Report("screen capture keys (shared keyboard)", Hook(Game::Resolve(Game::Address::InputDeviceApply), { 0x56, 0x8B, 0xF1, 0x33, 0xC0, 0x57, 0x8B, 0x56, 0x0C },
			reinterpret_cast<void*>(&DeviceApplyDetour), reinterpret_cast<void**>(&originalDeviceApply)));

		Logger::Debug("Game fixes: installed (see docs/game/game-fixes.md).");
	}
}
