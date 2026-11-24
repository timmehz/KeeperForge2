#include "GameFixes.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "LevelEditor.h"
#include "Logger.h"
#include "Memory.h"
#include "SessionOptions.h"
#include <algorithm>
#include <charconv>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		bool exitListGrown = false;
		// The window close fix (GameFixes::OnWindowClose) once the interface's push was found as expected.
		bool closeFixReady = false;
		constexpr std::size_t ActionBytes = 0x12;

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

		// ---- Missing picture. Ported from Flame: null_surf_fix.
		// The PNG loader returns 0 for a picture that is not there, and its caller reads the size from it at once, so a
		// missing file crashed the game. A shared 64x64 checkerboard (dark gray and purple) stands in; the caller only copies
		// from it.
		struct PictureRecord {
			std::uint32_t redMask, greenMask, blueMask, alphaMask;
			std::uint32_t bitsPerPixel;
			std::uint32_t bytePerPixel;
			void* pixels;
			std::int32_t width, height;
			std::int32_t pitch;
		};
		static_assert(sizeof(PictureRecord) == 40);
		constexpr std::int32_t PlaceholderSide = 64;
		constexpr std::int32_t PlaceholderSquare = 8;
		std::uint32_t placeholderPixels[PlaceholderSide * PlaceholderSide];
		PictureRecord placeholderPicture{};
		using LoadPngFn = PictureRecord*(__cdecl*)(const char* name);
		LoadPngFn originalLoadPng = nullptr;
		void MakePlaceholderPicture() {
			for (std::int32_t y = 0; y < PlaceholderSide; ++y)
				for (std::int32_t x = 0; x < PlaceholderSide; ++x)
					placeholderPixels[y * PlaceholderSide + x] = ((x / PlaceholderSquare + y / PlaceholderSquare) & 1) ? 0xFF800080u : 0xFF202020u;
			placeholderPicture = { 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u, 32, 0, placeholderPixels,
				PlaceholderSide, PlaceholderSide, PlaceholderSide * 4 };
		}
		PictureRecord* __cdecl LoadPngDetour(const char* name) {
			PictureRecord* picture = originalLoadPng(name);
			if (picture != nullptr) return picture;
			Logger::Warning(std::string("Game fixes: the picture \"") + (name != nullptr ? name : "") + "\" is missing; a placeholder is shown.");
			return &placeholderPicture;
		}

		// ---- Title screen. Ported from Flame: skippable_title_screen.
		// The game showed its title screen for 10 seconds in a loop that only read the clock. It is now shown for
		// "title_screen_ms" (default 600, Flame's) and Space, Escape, Enter or a left click ends it at once. The loop
		// sleeps between checks, and after a skip waits (at most 1 s) for the key to be let go, so the click does not
		// also land on the menu.
		std::uint32_t titleScreenMs = 600;
		std::uintptr_t titleScreenWaitDone = 0;
		bool SkipKeyHeld() {
			for (const int key : { VK_SPACE, VK_ESCAPE, VK_RETURN, VK_LBUTTON }) {
				if (GetAsyncKeyState(key) & 0x8000) return true;
			}
			return false;
		}
		void __cdecl TitleScreenWait() {
			const auto end = GetTickCount64() + titleScreenMs;
			while (GetTickCount64() < end) {
				if (SkipKeyHeld()) {
					const auto releaseEnd = GetTickCount64() + 1000;
					while (SkipKeyHeld() && GetTickCount64() < releaseEnd) Sleep(10);
					return;
				}
				Sleep(10);
			}
		}
		__declspec(naked) void TitleScreenWaitStub() {
			__asm {
				pushad
				call TitleScreenWait
				popad
				jmp dword ptr [titleScreenWaitDone]
			}
		}

		// ---- Creatures with nothing to show in the hand. Our own fix (not Flame's).
		// King Reginald and the Stone Knight have no In Hand animation in the stock creature list (only heroes nobody could
		// pick up), so once a keeper owns one (Mercenary Portal) the hand showed nothing. A type with an empty In Hand
		// animation gets its Stunned pose there, as it is read, before the game converts it. Same files give the same
		// result on every machine, and it only changes what is drawn.
		constexpr std::size_t CreatureAnimationBase = 0x20;
		constexpr std::size_t CreatureAnimationSize = 0x54;
		constexpr std::size_t AnimationStunned = 10;
		constexpr std::size_t AnimationInHand = 11;
		using CreatureConvertFn = void(__fastcall*)(void* type, void* unused, std::uint8_t* record);
		CreatureConvertFn originalCreatureConvert = nullptr;
		void __fastcall CreatureConvertDetour(void* type, void* unused, std::uint8_t* record) {
			if (record != nullptr) {
				auto* inHand = record + CreatureAnimationBase + CreatureAnimationSize * AnimationInHand;
				const auto* stunned = record + CreatureAnimationBase + CreatureAnimationSize * AnimationStunned;
				if (inHand[0] == '\0' && stunned[0] != '\0') {
					std::memcpy(inHand, stunned, CreatureAnimationSize);
					char name[33]{};
					std::memcpy(name, record, sizeof(name) - 1);
					Logger::Debug(std::string("Game fixes: the creature type \"") + name + "\" has no In Hand animation; its Stunned pose is used.");
				}
			}
			originalCreatureConvert(type, unused, record);
		}

		// ---- Room tile with no room. Ported from Flame: the room check in CBridge sub_443610.
		// Drawing a tile marked as room terrain read its room without a check, so a room tile left with no room (for
		// example a tile cut off from its room near the room limit) crashed the game when drawn. Such a tile now draws
		// no room meshes.
		using RoomTileMeshesFn = void(__fastcall*)(void* bridge, void* unused, std::int32_t x, std::int32_t y, std::uint8_t* tile,
			void* terrain, void* arrays);
		RoomTileMeshesFn originalRoomTileMeshes = nullptr;
		bool roomlessTileLogged = false;
		void __fastcall RoomTileMeshesDetour(void* bridge, void* unused, std::int32_t x, std::int32_t y, std::uint8_t* tile,
			void* terrain, void* arrays) {
			if (tile != nullptr) {
				const auto index = *reinterpret_cast<const std::uint16_t*>(tile + 2) & 0xFFFu;
				const auto* things = reinterpret_cast<void* const*>(thingTable);
				if (things[index] == nullptr) {
					if (!roomlessTileLogged) {
						roomlessTileLogged = true;
						Logger::Debug("Game fixes: a room tile with no room was drawn without its room meshes at " + std::to_string(x) + ", "
							+ std::to_string(y) + " (first time).");
					}
					return;
				}
			}
			originalRoomTileMeshes(bridge, unused, x, y, tile, terrain, arrays);
		}

		// ---- Long save path. Ported from Flame: buffer_overrun_fix (the save path part).
		// Loading a saved game wrote the saves folder (a full path) and the save's name into 64-byte stack buffers with
		// no limit, so a game installed in a deep folder, or a long save name, wrote past them. The path is now built
		// here into a buffer of our own, and the loader is handed that.
		constexpr std::size_t SavePathSize = 1024;
		char savePath[SavePathSize]{};
		std::uintptr_t saveNameAddress = 0;
		std::uintptr_t savesFolderAddress = 0;
		std::uintptr_t savePathResume = 0;
		void __cdecl BuildSavePath() {
			const auto* name = reinterpret_cast<const wchar_t*>(saveNameAddress);
			const auto* folder = reinterpret_cast<const char*>(savesFolderAddress);
			// As the game: the name in the system's code page.
			char narrow[MAX_PATH]{};
			if (WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr) == 0)
				narrow[sizeof(narrow) - 1] = '\0';
			const int length = std::snprintf(savePath, sizeof(savePath), "%s%s", folder, narrow);
			if (length < 0 || static_cast<std::size_t>(length) >= sizeof(savePath))
				Logger::Warning(std::string("Game fixes: the saved game's path is too long and was cut: ") + savePath);
		}
		// At 0x52555E (ECX the game config); EBX must stay 0 for the code after.
		__declspec(naked) void SavePathStub() {
			__asm {
				pushad
				call BuildSavePath
				popad
				jmp dword ptr [savePathResume]
			}
		}

		// ---- Sound thread stop. Ported from Flame: while_without_syscall_fix (the sound thread part).
		// Stopping the sound list thread waited for it in a loop that never gave the processor up; with the game's
		// threads on one processor that loop could keep the sound thread from ever finishing, hanging the game as it
		// closed. The wait now yields while it waits.
		std::uintptr_t soundStopResume = 0;
		void __cdecl YieldOnce() {
			SwitchToThread();
		}
		// At 0x60D64A, ESI the thread object (+ 0x38 set once the thread has stopped).
		__declspec(naked) void SoundThreadStopStub() {
			__asm {
			wait_loop:
				cmp byte ptr [esi + 0x38], 0
				jnz stopped
				pushad
				call YieldOnce
				popad
				jmp wait_loop
			stopped:
				mov al, byte ptr [esi + 0x38]
				jmp dword ptr [soundStopResume]
			}
		}

		// ---- Host port box. Ported from Flame: max_host_port_number_fix.
		// The TCP/IP menu's port box (button 0x212) gets a limit of 5, which counts the closing 0, so a 5-digit port such
		// as 27015 could not be typed. It gets one more.
		constexpr std::int32_t PortBoxId = 0x212;
		constexpr std::uintptr_t TextInputLimit = 0x280;
		constexpr std::uintptr_t TextInputConfigLimit = 0x1A;
		using TextInputConfigureFn = std::int32_t(__fastcall*)(std::uint8_t* button, void* unused, const std::uint8_t* config);
		TextInputConfigureFn originalTextInputConfigure = nullptr;
		std::int32_t __fastcall TextInputConfigureDetour(std::uint8_t* button, void* unused, const std::uint8_t* config) {
			const auto result = originalTextInputConfigure(button, unused, config);
			if (*reinterpret_cast<const std::int32_t*>(button + Game::Offset::ButtonId) == PortBoxId
				&& *reinterpret_cast<const std::int32_t*>(config + TextInputConfigLimit) != -1)
				++*reinterpret_cast<std::int32_t*>(button + TextInputLimit);
			return result;
		}

		// ---- Why a game cannot be joined. Ported from Flame: display_incompatible_reason.
		// The multiplayer session list greyed out games that could not be joined without saying why. The reason is now
		// added to the session's line after its version ("#reason", as Flame shows it). A game whose map this player does
		// not have is also no longer joinable, as in Flame.
		constexpr std::size_t SessionEntrySize = 0xA4;
		constexpr std::size_t SessionVersionChars = 64;
		std::uintptr_t sessionVersionResume = 0;
		std::uintptr_t sessionJoinableResume = 0;
		std::uintptr_t gameVersionMajorAddress = 0;
		bool sessionMapMissing = false;
		void __cdecl AddSessionReason(std::uint32_t entryOffset, wchar_t* version, void* frontEnd) {
			sessionMapMissing = false;
			const auto* entry = Memory::Read<const std::uint8_t*>(Game::Resolve(Game::Address::SessionArray)) + entryOffset;
			const auto read32 = [entry](std::size_t at) { return *reinterpret_cast<const std::uint32_t*>(entry + at); };
			const std::uint32_t players = read32(0x28) + (entry[0x40] & 0x7Fu);
			const std::uint32_t most = *reinterpret_cast<const std::uint16_t*>(entry + 0x42);
			const std::uint32_t ours = (Memory::Read<std::uint32_t>(Game::Resolve(Game::Address::GameVersionMajor)) << 16)
				| Memory::Read<std::uint32_t>(Game::Resolve(Game::Address::GameVersionMinor));
			const std::uint32_t fileCheck = Memory::Read<std::uint32_t>(Game::Resolve(Game::Address::GameFileCheck));
			wchar_t mapName[128]{};
			const auto lookup = reinterpret_cast<std::int32_t(__fastcall*)(void* frontEnd, void* unused, const std::uint8_t* mapInfo, wchar_t* name)>(
				Game::Resolve(Game::Address::SessionMapLookup));
			sessionMapMissing = lookup(frontEnd, nullptr, entry + 0x40, mapName) == 0;
			// The game's own order, after the map.
			wchar_t reason[48]{};
			if (sessionMapMissing) std::swprintf(reason, std::size(reason), L"#map not found");
			if (players >= most) std::swprintf(reason, std::size(reason), L"#game is full");
			else if (read32(0) != ours) std::swprintf(reason, std::size(reason), L"#version %u.%u", read32(0) >> 16, read32(0) & 0xFFFFu);
			else if (read32(0x48) != fileCheck) std::swprintf(reason, std::size(reason), L"#different game files");
			else if (most <= 1) std::swprintf(reason, std::size(reason), L"#one player map");
			if (reason[0] == L'\0') return;
			const std::size_t used = wcsnlen(version, SessionVersionChars);
			if (used + std::wcslen(reason) < SessionVersionChars) std::wcscat(version, reason);
		}
		// At 0x54D548: ESI the entry's offset in the array, [esp + 0x30] the version text, [esp + 0x14] the front end.
		__declspec(naked) void SessionVersionStub() {
			__asm {
				pushad
				mov eax, dword ptr [esp + 0x34]
				lea ecx, [esp + 0x50]
				push eax
				push ecx
				push esi
				call AddSessionReason
				add esp, 12
				popad
				mov eax, dword ptr [gameVersionMajorAddress]
				mov eax, dword ptr [eax]
				jmp dword ptr [sessionVersionResume]
			}
		}
		// At 0x54D58C, after the joinable byte was written through [esp + 0x10].
		__declspec(naked) void SessionJoinableStub() {
			__asm {
				cmp byte ptr [sessionMapMissing], 0
				je keep
				mov ecx, dword ptr [esp + 0x10]
				mov byte ptr [ecx], 0
			keep:
				test ebp, ebp
				jnz done
				mov ebp, 4
			done:
				jmp dword ptr [sessionJoinableResume]
			}
		}

		// ---- Start-up errors. Ported from Flame: print_game_start_errors and notify_another_instance_is_running.
		// When a step of the game's start-up failed, or another copy was already running, the game closed without a word.
		// Each step's call now goes through a wrapper that logs a failure, and after the start-up a message box says what
		// went wrong.
		enum StartupStep : int { StepCommandLine, StepFonts, StepWindow, StepSpeech, StepNetwork, StepWindowsCheck, StepComponents, StepCount };
		constexpr const char* StartupStepNames[StepCount] = {
			"reading the command line", "loading the fonts and texts", "creating the window", "starting speech",
			"starting the network library", "the Windows version check", "setting up the game's components" };
		std::uintptr_t startupOriginals[StepCount]{};
		int startupFailedStep = -1;
		void StartupStepResult(int step, std::int32_t result) {
			if (result != 0 || startupFailedStep >= 0) return;
			startupFailedStep = step;
			Logger::ErrorAndContinue(std::string("Game start-up failed: ") + StartupStepNames[step] + ".");
		}
		std::int32_t __cdecl StartupCommandLineDetour(std::int32_t argc, const char** argv) {
			const auto result = reinterpret_cast<std::int32_t(__cdecl*)(std::int32_t, const char**)>(startupOriginals[StepCommandLine])(argc, argv);
			StartupStepResult(StepCommandLine, result);
			return result;
		}
		template<int Step> std::int32_t __cdecl StartupPlainDetour() {
			const auto result = reinterpret_cast<std::int32_t(__cdecl*)()>(startupOriginals[Step])();
			StartupStepResult(Step, result);
			return result;
		}
		template<int Step> std::int32_t __fastcall StartupObjectDetour(void* object, void* unused) {
			const auto result = reinterpret_cast<std::int32_t(__fastcall*)(void*, void*)>(startupOriginals[Step])(object, unused);
			StartupStepResult(Step, result);
			return result;
		}
		std::uintptr_t originalGameStartup = 0;
		std::int32_t __cdecl GameStartupDetour(std::int32_t argc, const char** argv) {
			// The game's own test (0x5BAE70 makes this mutex and stops when it was already there).
			bool otherCopy = false;
			if (HANDLE mutex = OpenMutexA(SYNCHRONIZE, FALSE, "DKII MUTEX")) {
				otherCopy = true;
				CloseHandle(mutex);
			}
			const auto result = reinterpret_cast<std::int32_t(__cdecl*)(std::int32_t, const char**)>(originalGameStartup)(argc, argv);
			constexpr UINT BoxStyle = MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND;
			if (otherCopy) {
				Logger::Warning("Game start-up: Dungeon Keeper 2 is already running; this copy closes.");
				MessageBoxA(nullptr, "Dungeon Keeper 2 is already running.", "KeeperForge2", BoxStyle);
			}
			else if (startupFailedStep >= 0) {
				const std::string text = std::string("Dungeon Keeper 2 could not start.\n\nThis step failed: ") + StartupStepNames[startupFailedStep]
					+ ".\n\nThe KeeperForge2 log has more.";
				MessageBoxA(nullptr, text.c_str(), "KeeperForge2", BoxStyle);
			}
			return result;
		}

		// ---- Computer player stuck on one creature. Ported from Flame: blocking_response_to_threat_fix.
		// To answer a threat the computer picks the first creature of its list that passes its tests, then drops it only
		// if the creature is its own (0x48E6A0). When it was not, the next pick took the same creature again, so the
		// computer never sent anyone. Such a creature now gets a penalty of (creatures owned - 1) picks: each later pick
		// skips it and counts the penalty down. Game state only (tags, counts), cleared at each level start, so every
		// machine decides the same.
		constexpr std::uintptr_t KeeperCreatureCount = 0x16;
		std::unordered_map<std::uint16_t, std::int32_t> dropPenalties;
		std::int32_t dropPenaltySkip = 0;
		std::uintptr_t threatPickNext = 0;
		std::uintptr_t threatPicked = 0;
		std::uintptr_t threatPickOwned = 0;
		std::uintptr_t threatPickNotOwned = 0;
		std::uint16_t CreatureTag(const std::uint8_t* creature) { return *reinterpret_cast<const std::uint16_t*>(creature + 4); }
		std::int32_t KeeperCreatures(const std::uint8_t* keeper) { return *reinterpret_cast<const std::int32_t*>(keeper + KeeperCreatureCount); }
		std::int32_t __cdecl DropPenaltySkip(const std::uint8_t* creature, const std::uint8_t* keeper) {
			const auto found = dropPenalties.find(CreatureTag(creature));
			if (found == dropPenalties.end() || found->second <= 0) return 0;
			found->second = std::clamp(found->second - 1, 0, (std::max)(KeeperCreatures(keeper) - 1, 0));
			if (found->second == 0) dropPenalties.erase(found);
			return 1;
		}
		void __cdecl DropPenaltySet(const std::uint8_t* creature, const std::uint8_t* keeper) {
			const std::int32_t penalty = (std::max)(KeeperCreatures(keeper) - 1, 0);
			if (penalty > 0) dropPenalties[CreatureTag(creature)] = penalty;
		}
		void ClearDropPenalties() { dropPenalties.clear(); }
		// At 0x4FE504: EAX the creature's type data, EBX the creature, ESI the computer player.
		__declspec(naked) void ThreatPickStub() {
			__asm {
				test byte ptr [eax + 0x686], 1
				jnz skip_creature
				pushad
				mov ecx, dword ptr [esi + 0xE]
				push ecx
				push ebx
				call DropPenaltySkip
				add esp, 8
				mov dword ptr [dropPenaltySkip], eax
				popad
				cmp dword ptr [dropPenaltySkip], 0
				jne skip_creature
				jmp dword ptr [threatPicked]
			skip_creature:
				jmp dword ptr [threatPickNext]
			}
		}
		// At 0x4FE544: EAX whether the creature (EBX) is the keeper's.
		__declspec(naked) void ThreatPickOwnedStub() {
			__asm {
				test eax, eax
				jz not_owned
				jmp dword ptr [threatPickOwned]
			not_owned:
				pushad
				mov ecx, dword ptr [esi + 0xE]
				push ecx
				push ebx
				call DropPenaltySet
				add esp, 8
				popad
				jmp dword ptr [threatPickNotOwned]
			}
		}

		// ---- Hero party spawn at the creature limit. Ported from Flame: hero_party_spawn_limit_fix.
		// When a member could not be made (the world was full), the spawn returned at once with the members made so far
		// left without their leader links or party count. It now goes on to the game's own finish code with the members
		// it has; the finish code's two uses of the leader are skipped when no leader was made.
		std::uintptr_t partyNext = 0;
		std::uintptr_t partyFinish = 0;
		std::uintptr_t partyEffectResume = 0;
		std::uintptr_t partyEffectSkip = 0;
		std::uintptr_t partyDone = 0;
		bool partyCutLogged = false;
		void __cdecl LogPartyCut(std::uint32_t party) {
			if (partyCutLogged) return;
			partyCutLogged = true;
			Logger::Debug("Game fixes: hero party " + std::to_string(party & 0xFF) + " was spawned short (no room for more creatures; first time).");
		}
		// At 0x51A274, right after the member's spawn call.
		__declspec(naked) void PartySpawnFailStub() {
			__asm {
				test eax, eax
				jz cut
				jmp dword ptr [partyNext]
			cut:
				pushad
				// The party index (first argument): [esp + 0x84] before pushad.
				push dword ptr [esp + 0xA4]
				call LogPartyCut
				add esp, 4
				popad
				jmp dword ptr [partyFinish]
			}
		}
		// At 0x51A66A: the effect at the leader, only when there is one.
		__declspec(naked) void PartyLeaderEffectStub() {
			__asm {
				mov eax, dword ptr [esp + 0x18]
				test eax, eax
				jz skip
				cmp dword ptr [esp + 0x88], 0
				je skip
				jmp dword ptr [partyEffectResume]
			skip:
				jmp dword ptr [partyEffectSkip]
			}
		}
		// At 0x51A73A: the leader's place in the party, only when there is one.
		__declspec(naked) void PartyLeaderPlaceStub() {
			__asm {
				mov eax, dword ptr [esp + 0x88]
				test eax, eax
				jz done
				mov byte ptr [eax + 0x3E1], bl
			done:
				jmp dword ptr [partyDone]
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

		// ---- Chickens hatching outside the Hatchery: an egg about to hatch is thrown off its pen (the pen's facing, give or
		// take a random turn) and the chicken comes out wherever the egg stops rolling, so an egg that bounced out of the
		// room (a small or narrow Hatchery, a pen near its edge) gave a chicken outside it. Now the egg stops at the room's
		// edge: on every turn of its own state (0x18, while it sits and while it rolls) an egg that has left the Hatchery
		// tiles of its owner is put back on the nearest point inside them and loses its speed across (its upward speed is
		// kept, so it still drops), and the game then hatches it there. Its hatching (state 0x20) checks the same once more.
		// Only game state is read and the tiles are searched in a fixed order, so every machine moves it the same way.
		using EggStateFn = std::int32_t(__fastcall*)(std::uint8_t* egg);
		using SetPositionFn = void(__fastcall*)(std::uint8_t* thing, void* unused, std::int32_t* position);
		using FloorHeightFn = std::int32_t(__fastcall*)(std::uint8_t* tile, void* unused, std::int32_t* position);
		using MapSizeFn = std::int32_t(__fastcall*)(void* world);
		EggStateFn originalEggTick = nullptr;
		EggStateFn originalEggHatch = nullptr;
		bool eggStoppedLogged = false;
		bool eggLostLogged = false;
		// How far (in tiles, each way) a stray egg looks for its Hatchery. The throw carries an egg a tile or two.
		constexpr std::int32_t EggSearchTiles = 5;
		constexpr std::int32_t TileShift = 12;
		constexpr std::int32_t TileSize = 1 << TileShift;
		// How far inside the room's edge a stopped egg is kept (an eighth of a tile), so it is plainly on the room's tile.
		constexpr std::int32_t EggEdgeMargin = 0x200;
		bool IsEgg(const std::uint8_t* thing) {
			return thing != nullptr && thingTable != 0
				&& *reinterpret_cast<const std::int32_t*>(thing + Game::Offset::ThingKind) == Game::Offset::ObjectsList
				&& thing[Game::Offset::ObjectType] == Game::ObjectEgg;
		}
		bool IsOwnHatchery(const std::uint8_t* tile, std::uint16_t owner) {
			if (tile == nullptr) return false;
			const auto roomTag = *reinterpret_cast<const std::uint16_t*>(tile + Game::Offset::TileRoom) & Game::TagMask;
			if (roomTag == 0) return false;
			if ((*reinterpret_cast<const std::uint16_t*>(tile + Game::Offset::TileOwner) & Game::TagMask) != owner) return false;
			const auto* room = reinterpret_cast<std::uint8_t* const*>(thingTable)[roomTag];
			return room != nullptr && room[Game::Offset::RoomType] == Game::Room::Hatchery;
		}
		void KeepEggInHatchery(std::uint8_t* egg) {
			auto* world = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (world == nullptr) return;
			const auto* vtable = *reinterpret_cast<std::uintptr_t**>(world);
			const auto tileAt = reinterpret_cast<TileAtFn>(vtable[Game::Offset::WorldTileAt / sizeof(std::uintptr_t)]);
			const auto width = reinterpret_cast<MapSizeFn>(vtable[Game::Offset::WorldMapWidth / sizeof(std::uintptr_t)])(world);
			const auto height = reinterpret_cast<MapSizeFn>(vtable[Game::Offset::WorldMapHeight / sizeof(std::uintptr_t)])(world);
			const auto owner = static_cast<std::uint16_t>(*reinterpret_cast<const std::uint16_t*>(egg + Game::Offset::ThingOwnerTag) & Game::TagMask);
			const auto x = *reinterpret_cast<const std::int32_t*>(egg + Game::Offset::ThingPositionX);
			const auto y = *reinterpret_cast<const std::int32_t*>(egg + Game::Offset::ThingPositionY);
			const auto z = *reinterpret_cast<const std::int32_t*>(egg + Game::Offset::ThingPositionZ);
			const auto tileX = x >> TileShift;
			const auto tileY = y >> TileShift;
			const auto inMap = [&](std::int32_t tx, std::int32_t ty) { return tx >= 0 && ty >= 0 && tx < width && ty < height; };
			if (inMap(tileX, tileY) && IsOwnHatchery(tileAt(world, nullptr, tileX, tileY), owner)) return;

			// The nearest point of the nearest Hatchery tile: the egg's place pulled inside that tile (less the margin), which
			// is on the room's edge for the tiles beside where it left.
			std::uint8_t* bestTile = nullptr;
			std::int32_t bestX = 0;
			std::int32_t bestY = 0;
			std::int64_t bestDistance = 0;
			for (auto ty = tileY - EggSearchTiles; ty <= tileY + EggSearchTiles; ++ty) {
				for (auto tx = tileX - EggSearchTiles; tx <= tileX + EggSearchTiles; ++tx) {
					if (!inMap(tx, ty)) continue;
					auto* tile = tileAt(world, nullptr, tx, ty);
					if (!IsOwnHatchery(tile, owner)) continue;
					const std::int32_t pointX = std::clamp(x, (tx << TileShift) + EggEdgeMargin, (tx << TileShift) + TileSize - 1 - EggEdgeMargin);
					const std::int32_t pointY = std::clamp(y, (ty << TileShift) + EggEdgeMargin, (ty << TileShift) + TileSize - 1 - EggEdgeMargin);
					const std::int64_t dx = pointX - x;
					const std::int64_t dy = pointY - y;
					const auto distance = dx * dx + dy * dy;
					// Strictly nearer only: on a tie the first in row order wins, the same on every machine.
					if (bestTile == nullptr || distance < bestDistance) {
						bestTile = tile;
						bestX = pointX;
						bestY = pointY;
						bestDistance = distance;
					}
				}
			}
			if (bestTile == nullptr) {
				// No Hatchery of its owner nearby (sold while the egg rolled): the game's own way, a chicken where it lies.
				if (!eggLostLogged) {
					eggLostLogged = true;
					Logger::Debug("Game fixes: an egg left its Hatchery with none of its owner's nearby; left where it was (first time).");
				}
				return;
			}
			std::int32_t position[3] = { bestX, bestY, z };
			const auto floor = reinterpret_cast<FloorHeightFn>(Game::Resolve(Game::Address::TileFloorHeight))(bestTile, nullptr, position);
			if (position[2] < floor) position[2] = floor;
			reinterpret_cast<SetPositionFn>(Game::Resolve(Game::Address::ThingSetPosition))(egg, nullptr, position);
			// Stopped across; the game's own check (speed below 0x1000) then hatches it on its next turn.
			*reinterpret_cast<std::int32_t*>(egg + Game::Offset::ThingVelocityX) = 0;
			*reinterpret_cast<std::int32_t*>(egg + Game::Offset::ThingVelocityY) = 0;
			if (!eggStoppedLogged) {
				eggStoppedLogged = true;
				Logger::Debug("Game fixes: an egg rolling out of its Hatchery was stopped at the room's edge (first time).");
			}
		}
		std::int32_t __fastcall EggTickDetour(std::uint8_t* egg) {
			const bool isEgg = IsEgg(egg);
			const auto result = originalEggTick(egg);
			// 0: the game removed the egg (it was not on a pen).
			if (result != 0 && isEgg) KeepEggInHatchery(egg);
			return result;
		}
		std::int32_t __fastcall EggHatchDetour(std::uint8_t* egg) {
			if (IsEgg(egg)) KeepEggInHatchery(egg);
			return originalEggHatch(egg);
		}

		// ---- Chickens walking out of the Hatchery: a chicken's wander state turns it at random and checks the room only
		// when its walk ends, so it walked off the Hatchery's edge. Now a chicken that starts its turn on a Hatchery tile of
		// its owner and would end it off one is put back where it was, stopped across and turned round, so it walks back
		// in. Chickens already outside (dropped there by the hand, or a Hatchery sold) are left alone. Only game state is
		// read, so every machine does the same.
		using ObjectTurnFn = std::int32_t(__fastcall*)(std::uint8_t* object);
		ObjectTurnFn originalObjectTurn = nullptr;
		bool chickenKeptLogged = false;
		// Half of a full turn (0x800).
		constexpr std::uint16_t HalfTurn = 0x400;
		bool OnOwnHatchery(const std::uint8_t* thing) {
			auto* world = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::WorldPointer));
			if (world == nullptr) return false;
			const auto* vtable = *reinterpret_cast<std::uintptr_t**>(world);
			const auto tileAt = reinterpret_cast<TileAtFn>(vtable[Game::Offset::WorldTileAt / sizeof(std::uintptr_t)]);
			const auto width = reinterpret_cast<MapSizeFn>(vtable[Game::Offset::WorldMapWidth / sizeof(std::uintptr_t)])(world);
			const auto height = reinterpret_cast<MapSizeFn>(vtable[Game::Offset::WorldMapHeight / sizeof(std::uintptr_t)])(world);
			const auto tileX = *reinterpret_cast<const std::int32_t*>(thing + Game::Offset::ThingPositionX) >> TileShift;
			const auto tileY = *reinterpret_cast<const std::int32_t*>(thing + Game::Offset::ThingPositionY) >> TileShift;
			if (tileX < 0 || tileY < 0 || tileX >= width || tileY >= height) return false;
			const auto owner = static_cast<std::uint16_t>(*reinterpret_cast<const std::uint16_t*>(thing + Game::Offset::ThingOwnerTag) & Game::TagMask);
			return IsOwnHatchery(tileAt(world, nullptr, tileX, tileY), owner);
		}
		bool IsChicken(const std::uint8_t* thing) {
			return thing != nullptr && thingTable != 0
				&& *reinterpret_cast<const std::int32_t*>(thing + Game::Offset::ThingKind) == Game::Offset::ObjectsList
				&& thing[Game::Offset::ObjectType] == Game::ObjectChicken;
		}
		std::int32_t __fastcall ObjectTurnDetour(std::uint8_t* object) {
			if (!IsChicken(object) || !OnOwnHatchery(object)) return originalObjectTurn(object);
			std::int32_t before[3] = {
				*reinterpret_cast<const std::int32_t*>(object + Game::Offset::ThingPositionX),
				*reinterpret_cast<const std::int32_t*>(object + Game::Offset::ThingPositionY),
				*reinterpret_cast<const std::int32_t*>(object + Game::Offset::ThingPositionZ) };
			const auto result = originalObjectTurn(object);
			// Still the same chicken (its turn may have removed it: eaten, or turned into something else).
			if (!IsChicken(object) || OnOwnHatchery(object)) return result;
			before[2] = *reinterpret_cast<const std::int32_t*>(object + Game::Offset::ThingPositionZ);
			reinterpret_cast<SetPositionFn>(Game::Resolve(Game::Address::ThingSetPosition))(object, nullptr, before);
			*reinterpret_cast<std::int32_t*>(object + Game::Offset::ThingVelocityX) = 0;
			*reinterpret_cast<std::int32_t*>(object + Game::Offset::ThingVelocityY) = 0;
			auto& heading = *reinterpret_cast<std::uint16_t*>(object + Game::Offset::ThingHeading);
			heading = static_cast<std::uint16_t>((heading + HalfTurn) & 0x7FF);
			if (!chickenKeptLogged) {
				chickenKeptLogged = true;
				Logger::Debug("Game fixes: a chicken walking out of its Hatchery was turned back (first time).");
			}
			return result;
		}

		// ---- Videos frozen after switching away: while a video plays, the game swapped it between playing and paused on
		// every focus event, whichever way the focus went. One switch away and back sends three of each (WM_NCACTIVATE,
		// WM_ACTIVATE, WM_ACTIVATEAPP), but not always (minimizing and restoring, clicking back in), and background running
		// hands the held focus-loss ones over only when focus comes back. An odd count left the video paused on its last
		// frame, sound stopped, for good. Now focus coming plays it and focus lost pauses it, only when it is not so
		// already; any other event goes to the game's own handler.
		using VideoFocusFn = void(__cdecl*)(std::int32_t event, std::int32_t, std::int32_t, std::int32_t context);
		VideoFocusFn originalVideoFocus = nullptr;
		constexpr std::int32_t FocusComing = 0;
		constexpr std::int32_t FocusLost = 1;
		constexpr std::uintptr_t VideoPlayerState = 0x9C;
		constexpr std::int32_t VideoPlaying = 3;
		constexpr std::int32_t VideoPaused = 4;
		bool videoFixLogged = false;
		void __cdecl VideoFocusDetour(std::int32_t event, std::int32_t a, std::int32_t b, std::int32_t context) {
			if (event != FocusComing && event != FocusLost) {
				originalVideoFocus(event, a, b, context);
				return;
			}
			auto* player = Memory::Read<std::uint8_t*>(Game::Resolve(Game::Address::VideoPlayer));
			if (player == nullptr) return;
			const auto state = *reinterpret_cast<volatile const std::int32_t*>(player + VideoPlayerState);
			const std::int32_t wanted = event == FocusComing ? VideoPlaying : VideoPaused;
			// Only between playing and paused (not a video that is starting or done).
			if (state == wanted || (state != VideoPlaying && state != VideoPaused)) return;
			reinterpret_cast<std::int32_t(__cdecl*)(std::uint8_t*)>(Game::Resolve(Game::Address::VideoTogglePause))(player);
			if (!videoFixLogged && event == FocusComing) {
				videoFixLogged = true;
				Logger::Debug("Game fixes: a video paused while the game was away plays again (first time).");
			}
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

	// Ported from Flame: fix_close_window. Closing the window during a game set the game's quit flag (its window procedure,
	// 0x5B5070, on WM_CLOSE), so it dropped out mid-level without its own exit. During a game it now sends the action the
	// in-game "exit to Windows" confirmation sends (0x4205B0): kind 0x7D with this player's tag, through the interface.
	bool GameFixes::OnWindowClose() {
		if (!closeFixReady) return false;
		auto* playerInterface = reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::PlayerInterface));
		// The interface's game session (+ 4): set only while a game is running.
		if (Memory::Read<void*>(reinterpret_cast<std::uintptr_t>(playerInterface) + Game::Offset::InterfaceSession) == nullptr) return false;
		std::uint8_t action[ActionBytes]{};
		*reinterpret_cast<std::int32_t*>(action + Game::Offset::ActionKind) = Game::ActionExitToWindows;
		*reinterpret_cast<std::uint16_t*>(action + Game::Offset::ActionPlayer) = *reinterpret_cast<const std::uint16_t*>(playerInterface + Game::Offset::InterfacePlayerTag);
		const auto push = reinterpret_cast<std::int32_t(__fastcall*)(void* playerInterface, void* unused, const std::uint8_t* action)>(
			Game::Resolve(Game::Address::InterfacePushAction));
		push(playerInterface, nullptr, action);
		Logger::Info("Game fixes: the window was closed during a game; the game's own exit was used.");
		return true;
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
		threatPickNext = Game::Resolve(Game::Address::ThreatPickNext);
		threatPicked = Game::Resolve(Game::Address::ThreatPicked);
		threatPickOwned = Game::Resolve(Game::Address::ThreatPickOwned);
		threatPickNotOwned = Game::Resolve(Game::Address::ThreatPickNotOwned);
		const auto threatPickLast = Game::Resolve(Game::Address::ThreatPickLastTest);
		const auto threatPickOwnedTest = Game::Resolve(Game::Address::ThreatPickOwnedTest);
		// "test byte [eax+0x686], 1; jz +0x15" and "test eax, eax; jz 0x4FE914".
		const bool threatPick = Memory::Matches(threatPickLast, { 0xF6, 0x80, 0x86, 0x06, 0x00, 0x00, 0x01, 0x74, 0x15 })
			&& Memory::Matches(threatPickOwnedTest, { 0x85, 0xC0, 0x0F, 0x84, 0xC8, 0x03, 0x00, 0x00 })
			&& Memory::WriteJump(threatPickOwnedTest, reinterpret_cast<const void*>(&ThreatPickOwnedStub), 8)
			&& Memory::WriteJump(threatPickLast, reinterpret_cast<const void*>(&ThreatPickStub), 9);
		if (threatPick) SessionOptions::OnLevelStart(&ClearDropPenalties);
		Report("computer player stuck on one creature", threatPick);

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

		MakePlaceholderPicture();
		// mov eax, [0x75AF6C]; sub esp, 0x400.
		Report("missing picture", Hook(Game::Resolve(Game::Address::LoadPngPicture), { 0xA1, 0x6C, 0xAF, 0x75, 0x00, 0x81, 0xEC, 0x00, 0x04, 0x00, 0x00 },
			reinterpret_cast<void*>(&LoadPngDetour), reinterpret_cast<void**>(&originalLoadPng)));

		// Title screen wait: "call rel32 (0x5B5E90); mov esi, eax; add esi, 0x2710; call; cmp eax, esi; ja; call; cmp; jbe".
		{
			const auto wait = Game::Resolve(Game::Address::TitleScreenWait);
			titleScreenWaitDone = Game::Resolve(Game::Address::TitleScreenWaitDone);
			std::uint32_t milliseconds = 600;
			const auto setting = Config::GetString("title_screen_ms", "600");
			std::from_chars(setting.data(), setting.data() + setting.size(), milliseconds);
			titleScreenMs = (std::min)(milliseconds, 10000u);
			std::vector<std::uint8_t> loop = relativeCall(wait, Game::Resolve(Game::Address::GameTimeMs));
			const std::vector<std::uint8_t> rest = { 0x8B, 0xF0, 0x81, 0xC6, 0x10, 0x27, 0x00, 0x00 };
			loop.insert(loop.end(), rest.begin(), rest.end());
			Report("title screen", Memory::Matches(wait, loop)
				&& Memory::Matches(titleScreenWaitDone - 4, { 0x3B, 0xC6, 0x76, 0xF7 })
				&& Memory::WriteJump(wait, reinterpret_cast<const void*>(&TitleScreenWaitStub), titleScreenWaitDone - wait));
		}

		// sub esp, 0xC; push ebx; mov ebx, [esp+0x14]; push ebp; push esi; mov al, [ebx+0xF4B].
		Report("creatures with nothing in the hand", Hook(Game::Resolve(Game::Address::CreatureRecordConvert), { 0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x14, 0x55, 0x56 },
			reinterpret_cast<void*>(&CreatureConvertDetour), reinterpret_cast<void**>(&originalCreatureConvert)));

		// push ebx; mov ebx, [esp+0x10]; push ebp; push esi; push edi; mov edi, ecx. Uses thingTable (set above).
		Report("room tile with no room", thingTable != 0 && Hook(Game::Resolve(Game::Address::RoomTileMeshes), { 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x8B, 0xF9 },
			reinterpret_cast<void*>(&RoomTileMeshesDetour), reinterpret_cast<void**>(&originalRoomTileMeshes)));

		// Windows version check. Ported from Flame: modern_windows_support. The game refused to start on Windows 6.0 (Vista)
		// or newer unless Windows reported an older version to it; the limit is now 11.0, which no Windows reports (Windows
		// 11 is 10.0), so every current Windows passes. "push 0; push 0; push 6; call 0x557FD0".
		{
			const auto windowsCheck = Game::Resolve(Game::Address::StartupWindowsCheck);
			const auto majorPush = Game::Resolve(Game::Address::WindowsCheckMajorPush);
			const bool windowsCheckFixed = Memory::Matches(windowsCheck, { 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x06, 0xE8 });
			if (windowsCheckFixed) Memory::Patch(majorPush, { 0x6A, 0x0B });
			Report("Windows version check", windowsCheckFixed);
		}

		// Start-up: each step's "call rel32" goes to its wrapper, and the call of the start-up itself to ours. Only takes
		// effect when the runtime is in place before the game's own code (started by the launcher).
		{
			struct StartupCall { std::uintptr_t call; std::uintptr_t target; int step; const void* detour; };
			const StartupCall startupCalls[] = {
				{ Game::Address::StartupCommandLineCall, Game::Address::StartupCommandLine, StepCommandLine, reinterpret_cast<const void*>(&StartupCommandLineDetour) },
				{ Game::Address::StartupFontsCall, Game::Address::StartupFonts, StepFonts, reinterpret_cast<const void*>(&StartupPlainDetour<StepFonts>) },
				{ Game::Address::StartupWindowCall, Game::Address::StartupWindow, StepWindow, reinterpret_cast<const void*>(&StartupObjectDetour<StepWindow>) },
				{ Game::Address::StartupSpeechCall, Game::Address::StartupSpeech, StepSpeech, reinterpret_cast<const void*>(&StartupObjectDetour<StepSpeech>) },
				{ Game::Address::StartupNetworkCall, Game::Address::StartupNetwork, StepNetwork, reinterpret_cast<const void*>(&StartupObjectDetour<StepNetwork>) },
				{ Game::Address::StartupWindowsCheckCall, Game::Address::StartupWindowsCheck, StepWindowsCheck, reinterpret_cast<const void*>(&StartupObjectDetour<StepWindowsCheck>) },
				{ Game::Address::StartupComponentsCall, Game::Address::StartupComponents, StepComponents, reinterpret_cast<const void*>(&StartupPlainDetour<StepComponents>) },
			};
			const auto startupCall = Game::Resolve(Game::Address::GameStartupCall);
			const auto startup = Game::Resolve(Game::Address::GameStartup);
			// Each step's call must be a "call rel32", but it may already lead somewhere else: patch.dll sends the command
			// line call (0x5A5FAC) to its own parser. The wrapper then calls whatever it called.
			const auto currentTarget = [](std::uintptr_t at) { return at + 5 + Memory::Read<std::uint32_t>(at + 1); };
			bool startupMatches = Memory::Matches(startupCall, relativeCall(startupCall, startup));
			for (const auto& step : startupCalls) {
				const auto at = Game::Resolve(step.call);
				startupMatches = startupMatches && Memory::Read<std::uint8_t>(at) == 0xE8;
				if (startupMatches && currentTarget(at) != Game::Resolve(step.target))
					Logger::Debug(std::string("Game fixes: the start-up's call for ") + StartupStepNames[step.step] + " was already redirected (patch.dll); it is wrapped as it is.");
			}
			bool startupFixed = startupMatches;
			if (startupMatches) {
				for (const auto& step : startupCalls) {
					const auto at = Game::Resolve(step.call);
					startupOriginals[step.step] = currentTarget(at);
					startupFixed = Memory::WriteCall(at, step.detour) && startupFixed;
				}
				originalGameStartup = startup;
				startupFixed = Memory::WriteCall(startupCall, reinterpret_cast<const void*>(&GameStartupDetour)) && startupFixed;
			}
			Report("start-up errors", startupFixed);
		}

		sessionVersionResume = Game::Resolve(Game::Address::SessionListVersionResume);
		sessionJoinableResume = Game::Resolve(Game::Address::SessionListJoinableResume);
		gameVersionMajorAddress = Game::Resolve(Game::Address::GameVersionMajor);
		const auto sessionVersionDone = Game::Resolve(Game::Address::SessionListVersionDone);
		const auto sessionJoinableDone = Game::Resolve(Game::Address::SessionListJoinableDone);
		// "mov eax, [0x6BC430]" and "test ebp, ebp; jnz +5; mov ebp, 4".
		Report("why a game cannot be joined", Memory::Matches(sessionVersionDone, { 0xA1, 0x30, 0xC4, 0x6B, 0x00 })
			&& Memory::Matches(sessionJoinableDone, { 0x85, 0xED, 0x75, 0x05, 0xBD, 0x04, 0x00, 0x00, 0x00 })
			&& Memory::WriteJump(sessionJoinableDone, reinterpret_cast<const void*>(&SessionJoinableStub), 9)
			&& Memory::WriteJump(sessionVersionDone, reinterpret_cast<const void*>(&SessionVersionStub), 5));

		// push ebx; mov ebx, [esp+8]; push ebp; xor ebp, ebp; mov eax, [ebx+0x16].
		Report("host port box", Hook(Game::Resolve(Game::Address::TextInputConfigure), { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x33, 0xED, 0x8B, 0x43, 0x16 },
			reinterpret_cast<void*>(&TextInputConfigureDetour), reinterpret_cast<void**>(&originalTextInputConfigure)));

		// The interface's push: "mov eax, [ecx+0xA]; inc eax; mov [ecx+0xA], eax; mov eax, [esp+4]; mov ecx, [ecx+4]".
		closeFixReady = Memory::Matches(Game::Resolve(Game::Address::InterfacePushAction), { 0x8B, 0x41, 0x0A, 0x40, 0x89, 0x41, 0x0A, 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x49, 0x04 });
		Report("closing the window during a game", closeFixReady);

		soundStopResume = Game::Resolve(Game::Address::SoundThreadStopResume);
		const auto soundStopWait = Game::Resolve(Game::Address::SoundThreadStopWait);
		Report("sound thread stop wait", Memory::Matches(soundStopWait, { 0x8A, 0x46, 0x38, 0x84, 0xC0, 0x75, 0x07, 0x8A, 0x46, 0x38, 0x84, 0xC0, 0x74, 0xF9 })
			&& Memory::WriteJump(soundStopWait, reinterpret_cast<const void*>(&SoundThreadStopStub), 14));

		const auto savePathBuild = Game::Resolve(Game::Address::SaveLoadPathBuild);
		const auto savePathUse = Game::Resolve(Game::Address::SaveLoadPathUse);
		saveNameAddress = Game::Resolve(Game::Address::GameConfigSaveName);
		savesFolderAddress = Game::Resolve(Game::Address::SavesFolder);
		savePathResume = Game::Resolve(Game::Address::SaveLoadPathResume);
		// The whole block: call 0x55D550; swprintf the name; convert it (0x5557E0, 0x40 bytes); sprintf "%s%s" of the
		// saves folder (0x758754) and the name into [esp+0x114]. Then "lea eax, [esp+0x114]" before the loader's call.
		const std::vector<std::uint8_t> savePathBlock = {
			0xE8, 0xED, 0x7F, 0x03, 0x00, 0x50, 0x8D, 0x8C, 0x24, 0x58, 0x01, 0x00, 0x00, 0x68, 0xF8, 0xF4, 0x68, 0x00, 0x51,
			0xE8, 0x1A, 0xF9, 0x10, 0x00, 0x83, 0xC4, 0x0C, 0x8D, 0x54, 0x24, 0x54, 0x8D, 0x84, 0x24, 0x54, 0x01, 0x00, 0x00,
			0x6A, 0x40, 0x52, 0x50, 0xE8, 0x53, 0x02, 0x03, 0x00, 0x83, 0xC4, 0x0C, 0x8D, 0x4C, 0x24, 0x54, 0x8D, 0x94, 0x24,
			0x14, 0x01, 0x00, 0x00, 0x51, 0x68, 0x54, 0x87, 0x75, 0x00, 0x68, 0x30, 0x70, 0x69, 0x00, 0x52, 0xE8, 0x04, 0xF8,
			0x10, 0x00, 0x83, 0xC4, 0x10 };
		const auto pathAddress = reinterpret_cast<std::uintptr_t>(savePath);
		bool savePathFixed = Memory::Matches(savePathBuild, savePathBlock)
			&& Memory::Matches(savePathUse, { 0x8D, 0x84, 0x24, 0x14, 0x01, 0x00, 0x00 })
			&& Memory::WriteJump(savePathBuild, reinterpret_cast<const void*>(&SavePathStub), savePathBlock.size());
		// mov eax, savePath; nop; nop.
		if (savePathFixed)
			Memory::Patch(savePathUse, { 0xB8, static_cast<std::uint8_t>(pathAddress), static_cast<std::uint8_t>(pathAddress >> 8),
				static_cast<std::uint8_t>(pathAddress >> 16), static_cast<std::uint8_t>(pathAddress >> 24), 0x90, 0x90 });
		Report("long save path", savePathFixed);

		const auto partyFailTest = Game::Resolve(Game::Address::HeroPartySpawnFailTest);
		const auto partyLeaderEffect = Game::Resolve(Game::Address::HeroPartyLeaderEffect);
		const auto partyLeaderPlace = Game::Resolve(Game::Address::HeroPartyLeaderPlace);
		partyNext = Game::Resolve(Game::Address::HeroPartySpawnNext);
		partyFinish = Game::Resolve(Game::Address::HeroPartyFinish);
		partyEffectResume = Game::Resolve(Game::Address::HeroPartyLeaderEffectResume);
		partyEffectSkip = Game::Resolve(Game::Address::HeroPartyLeaderEffectSkip);
		partyDone = Game::Resolve(Game::Address::HeroPartyDone);
		// "test eax, eax; jz 0x51A756", "mov eax, [esp+0x18]; test eax, eax; jz 0x51A6CF" and
		// "mov eax, [esp+0x88]; mov [eax+0x3E1], bl".
		Report("hero party spawn at the creature limit", Memory::Matches(partyFailTest, { 0x85, 0xC0, 0x0F, 0x84, 0xDA, 0x04, 0x00, 0x00 })
			&& Memory::Matches(partyLeaderEffect, { 0x8B, 0x44, 0x24, 0x18, 0x85, 0xC0, 0x74, 0x5D })
			&& Memory::Matches(partyLeaderPlace, { 0x8B, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x88, 0x98, 0xE1, 0x03, 0x00, 0x00 })
			&& Memory::WriteJump(partyLeaderEffect, reinterpret_cast<const void*>(&PartyLeaderEffectStub), 8)
			&& Memory::WriteJump(partyLeaderPlace, reinterpret_cast<const void*>(&PartyLeaderPlaceStub), 13)
			&& Memory::WriteJump(partyFailTest, reinterpret_cast<const void*>(&PartySpawnFailStub), 8));

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
		// sub esp, 0xC; push ebx; push esi; mov esi, ecx; push edi; mov ecx, [0x6E5050] [verified in the assembly at 0x4A1CF0].
		// Uses thingTable (set above).
		// push ebp; mov ebp, esp; push -1; push 0x64BB88 (its exception handler) [verified in the assembly at 0x4A21C0].
		Report("chickens hatching outside the Hatchery", thingTable != 0
			&& Hook(Game::Resolve(Game::Address::EggTick), { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x88, 0xBB, 0x64, 0x00 },
				reinterpret_cast<void*>(&EggTickDetour), reinterpret_cast<void**>(&originalEggTick))
			&& Hook(Game::Resolve(Game::Address::EggHatch), { 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x0D, 0x50, 0x50, 0x6E, 0x00 },
				reinterpret_cast<void*>(&EggHatchDetour), reinterpret_cast<void**>(&originalEggHatch)));
		// sub esp, 0x10; push ebx; push esi; mov esi, ecx; push edi; mov eax, [esi] [verified in the assembly at 0x49BD20].
		Report("chickens walking out of the Hatchery", thingTable != 0
			&& Hook(Game::Resolve(Game::Address::ObjectTurn), { 0x83, 0xEC, 0x10, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x06 },
				reinterpret_cast<void*>(&ObjectTurnDetour), reinterpret_cast<void**>(&originalObjectTurn)));
		// mov eax, [esp+4]; sub eax, 0 (then its jz, not moved) [verified in the assembly at 0x53C2E0].
		Report("videos frozen after switching away", Hook(Game::Resolve(Game::Address::VideoFocusHandler), { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xE8, 0x00 },
			reinterpret_cast<void*>(&VideoFocusDetour), reinterpret_cast<void**>(&originalVideoFocus)));

		// push esi; mov esi, ecx; xor eax, eax; push edi; mov edx, [esi+0xC] [verified in the assembly at 0x5DCBE0].
		Report("screen capture keys (shared keyboard)", Hook(Game::Resolve(Game::Address::InputDeviceApply), { 0x56, 0x8B, 0xF1, 0x33, 0xC0, 0x57, 0x8B, 0x56, 0x0C },
			reinterpret_cast<void*>(&DeviceApplyDetour), reinterpret_cast<void**>(&originalDeviceApply)));

		Logger::Debug("Game fixes: installed (see docs/game/game-fixes.md).");
	}
}
