#include "CreatorName.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "Notices.h"
#include "PlayerRanks.h"
#include "SteamMultiplayer.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		// Who is the creator, an admin or a friend: the Steam account lists are in PlayerRanks.cpp.
		using Rank = PlayerRanks::Rank;
		// A game name is cut to 16 characters (a lobby slot's 0x22 bytes).
		constexpr std::size_t GameNameCharacters = 16;

		enum class Kind { None, Creator, Admin, Friend };

		// The game names of the creator and their friends who are in this machine's Steam lobby (this machine included),
		// refreshed every 2 s from Steam.
		struct Names {
			std::wstring creator;
			std::vector<std::wstring> admins;
			std::vector<std::wstring> friends;
		};
		Names names;
		ULONGLONG lastRefresh = 0;
		constexpr ULONGLONG RefreshMilliseconds = 2000;
		bool loggedCreator = false;
		bool loggedFriend = false;
		bool loggedAdmin = false;

		std::wstring Wide(const std::string& utf8) {
			if (utf8.empty()) return {};
			const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
			std::wstring wide(static_cast<std::size_t>((std::max)(length, 0)), L'\0');
			if (length > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
			if (wide.size() > GameNameCharacters) wide.resize(GameNameCharacters);
			return wide;
		}
		const Names& CurrentNames() {
			const auto now = GetTickCount64();
			if (now - lastRefresh < RefreshMilliseconds && lastRefresh != 0) return names;
			lastRefresh = now;
			Names found;
			const auto consider = [&found](std::uint64_t id, const std::string& name) {
				if (id == 0 || name.empty()) return;
				switch (PlayerRanks::RankOf(id)) {
					case Rank::Creator: found.creator = Wide(name); break;
					case Rank::Admin: found.admins.push_back(Wide(name)); break;
					case Rank::Friend: found.friends.push_back(Wide(name)); break;
					default: break;
				}
			};
			consider(SteamMultiplayer::MySteamId(), SteamMultiplayer::MyName());
			for (const auto& member : SteamMultiplayer::OtherMembers()) consider(member.steamId, member.name);
			names = std::move(found);
			if (!names.creator.empty() && !loggedCreator) {
				loggedCreator = true;
				Logger::Debug("Creator name: KeeperForge2's creator is in the Steam lobby; their name is drawn in rainbow colors.");
			}
			if (!names.admins.empty() && !loggedAdmin) {
				loggedAdmin = true;
				Logger::Debug("Creator name: a KeeperForge2 admin is in the Steam lobby; their name is drawn in dark red.");
			}
			if (!names.friends.empty() && !loggedFriend) {
				loggedFriend = true;
				Logger::Debug("Creator name: a friend of KeeperForge2's creator is in the Steam lobby; their name is drawn in gold.");
			}
			return names;
		}
		// Whose name the text is (whole: the text is only the name) or starts with (highest rank first).
		Kind KindOfText(const wchar_t* text, bool whole) {
			if (text == nullptr) return Kind::None;
			const auto& current = CurrentNames();
			const auto matches = [text, whole](const std::wstring& name) {
				if (name.empty()) return false;
				return whole ? std::wcscmp(text, name.c_str()) == 0 : std::wcsncmp(text, name.c_str(), name.size()) == 0;
			};
			if (matches(current.creator)) return Kind::Creator;
			for (const auto& name : current.admins) if (matches(name)) return Kind::Admin;
			for (const auto& name : current.friends) if (matches(name)) return Kind::Friend;
			return Kind::None;
		}

		// The colors, as the game's text drawing takes them: { blue, green, red, alpha, palette index (8-bit modes only) }.
		// The rainbow steps every 0.75 s and starts again after violet; gold does not change.
		constexpr std::size_t ColorSize = 5;
		constexpr ULONGLONG StepMilliseconds = 750;
		struct Rgb { std::uint8_t r, g, b; };
		constexpr std::array<Rgb, 7> Rainbow = { {
			{ 255, 0, 0 }, { 255, 127, 0 }, { 255, 255, 0 }, { 0, 255, 0 }, { 0, 128, 255 }, { 75, 0, 130 }, { 148, 0, 211 },
		} };
		constexpr std::array<std::uint8_t, ColorSize> GoldColor = { 0, 215, 255, 0xFF, 0 };
		// Admins: dark red, #8B0000.
		constexpr std::array<std::uint8_t, ColorSize> AdminColor = { 0x00, 0x00, 0x8B, 0xFF, 0 };
		// The notify command's alerts: red. (Its system notices are white, NoticeColor below.)
		constexpr std::array<std::uint8_t, ColorSize> AlertColor = { 0x00, 0x00, 0xFF, 0xFF, 0 };
		constexpr std::array<std::uint8_t, ColorSize> WhiteColor = { 0xFF, 0xFF, 0xFF, 0xFF, 0 };
		std::array<std::uint8_t, ColorSize> rainbowColor{};
		std::size_t Step() { return static_cast<std::size_t>(GetTickCount64() / StepMilliseconds); }
		const std::uint8_t* RainbowColor() {
			const auto& rgb = Rainbow[Step() % Rainbow.size()];
			rainbowColor = { rgb.b, rgb.g, rgb.r, 0xFF, 0 };
			return rainbowColor.data();
		}
		// nullptr for no change.
		const std::uint8_t* ColorOf(Kind kind) {
			if (kind == Kind::Creator) return RainbowColor();
			if (kind == Kind::Admin) return AdminColor.data();
			if (kind == Kind::Friend) return GoldColor.data();
			return nullptr;
		}

		// ---- The in-game player list (0x40A0F0, drawn each frame while it is shown): each row's color is pushed at
		// 0x40A1FB ("add eax, 0x14; push eax; mov eax, [esp + 0x7C]", 8 bytes) with the row's player in ESI.
		constexpr std::uintptr_t PlayerName = 0xEEA;
		std::uintptr_t listResume = 0;
		const std::uint8_t* __cdecl PlayerListColor(const std::uint8_t* color, const std::uint8_t* player) {
			if (player == nullptr) return color;
			const auto* special = ColorOf(KindOfText(reinterpret_cast<const wchar_t*>(player + PlayerName), false));
			return special != nullptr ? special : color;
		}
		__declspec(naked) void PlayerListStub() {
			__asm {
				add eax, 0x14
				push edx
				push esi
				push eax
				call PlayerListColor
				add esp, 8
				pop edx
				push eax
				mov eax, dword ptr [esp + 0x7C]
				jmp dword ptr [listResume]
			}
		}

		// ---- The multiplayer lobby's player list (0x5445F0, the menu's drawing): each slot's row takes its color from the
		// player colors, then the call at 0x544C1D (0x62C3C0, thiscall, arguments: out, color) sets it before the row is
		// drawn. The slot's number is in the caller's [esp + 0x10]. Slots are 0x3F bytes at 0x73F5C8: a human player's name
		// (wide, 16 characters and the end) at + 0, the kind in the low 3 bits of + 0x3A (0 empty, 1 computer).
		constexpr std::uintptr_t LobbySlotSize = 0x3F;
		constexpr std::uintptr_t LobbySlotKind = 0x3A;
		constexpr std::size_t LobbySlotNameBytes = (GameNameCharacters + 1) * sizeof(wchar_t);
		constexpr int LobbySlotCount = 4;
		std::uintptr_t lobbySlots = 0;
		std::uintptr_t textColorSet = 0;
		std::uint8_t* SlotRecord(int slot) { return reinterpret_cast<std::uint8_t*>(lobbySlots + static_cast<std::uintptr_t>(slot) * LobbySlotSize); }
		// A human slot's name ("" for an empty or computer slot).
		std::wstring SlotName(int slot) {
			const auto* record = SlotRecord(slot);
			const auto kind = record[LobbySlotKind] & 7;
			if (kind == 0 || kind == 1) return {};
			std::array<wchar_t, GameNameCharacters + 1> name{};
			std::memcpy(name.data(), record, GameNameCharacters * sizeof(wchar_t));
			return name.data();
		}
		void __cdecl LobbySlotColor(int slot, std::uint8_t* color) {
			if (slot < 0 || slot >= LobbySlotCount || color == nullptr || lobbySlots == 0) return;
			const auto name = SlotName(slot);
			if (name.empty()) return;
			if (const auto* special = ColorOf(KindOfText(name.c_str(), true))) std::memcpy(color, special, ColorSize);
		}
		__declspec(naked) void LobbyColorStub() {
			__asm {
				pushad
				mov eax, dword ptr [esp + 0x28]
				push eax
				mov eax, dword ptr [esp + 0x40]
				push eax
				call LobbySlotColor
				add esp, 8
				popad
				jmp dword ptr [textColorSet]
			}
		}

		// ---- The lobby's chat notices. The game's lobby chat handler (0x542BF0, cdecl (sender, message, front end); set as
		// the network's chat callback at 0x7581C0, its front end at 0x7581C4, while the multiplayer lobby is up) adds the
		// line "<sender's slot><slot name>: <text>" to the chat (front end + 0x30CAB). The message is the length of a
		// number as one digit, that number (the sender's screen, which must equal 0x759C35), then the text; the sender must
		// be below the player count (front end + 0x5AAC, a byte). A notice is added through it as slot 0 with that slot's
		// name swapped for "SYSTEM" for the call. Only on this machine: nothing is sent.
		constexpr std::wstring_view SystemName = L"SYSTEM";
		constexpr std::uintptr_t FrontEndPlayerCount = 0x5AAC;
		const std::array<std::wstring, 6> NoticeTexts = {
			L"Creator of KeeperForge2 has joined the lobby!",
			L"Creator of KeeperForge2 has left the lobby!",
			L"Friend of Creator of KeeperForge2 has joined the lobby!",
			L"Friend of Creator of KeeperForge2 has left the lobby!",
			L"Admin of KeeperForge2 has joined the lobby!",
			L"Admin of KeeperForge2 has left the lobby!",
		};
		enum Notice { CreatorJoined, CreatorLeft, FriendJoined, FriendLeft, AdminJoined, AdminLeft };
		std::deque<Notice> pendingNotices;
		using LobbyChatReceiveFn = void(__cdecl*)(std::uint32_t sender, const wchar_t* message, void* frontEnd);
		std::uintptr_t lobbyChatReceive = 0;
		std::uintptr_t chatCallback = 0;
		std::uintptr_t screenState = 0;
		// A line as if slot 0 named `name` said `text` (only on this machine).
		bool AddLobbyLine(std::wstring_view name, const std::wstring& text) {
			if (*reinterpret_cast<const std::uintptr_t*>(chatCallback) != lobbyChatReceive) return false;
			auto* frontEnd = *reinterpret_cast<std::uint8_t* const*>(chatCallback + 4);
			if (frontEnd == nullptr || frontEnd[FrontEndPlayerCount] == 0) return false;
			std::uint16_t screen = 0;
			std::memcpy(&screen, reinterpret_cast<const void*>(screenState), sizeof(screen));
			const auto number = std::to_wstring(screen);
			const std::wstring message = std::wstring(1, static_cast<wchar_t>(L'0' + number.size())) + number + text;
			auto* slotName = SlotRecord(0);
			std::array<std::uint8_t, LobbySlotNameBytes> kept{};
			std::memcpy(kept.data(), slotName, kept.size());
			std::memset(slotName, 0, kept.size());
			std::memcpy(slotName, name.data(), (std::min)(name.size(), GameNameCharacters) * sizeof(wchar_t));
			reinterpret_cast<LobbyChatReceiveFn>(lobbyChatReceive)(0, message.c_str(), frontEnd);
			std::memcpy(slotName, kept.data(), kept.size());
			return true;
		}
		bool AddNotice(Notice notice) { return AddLobbyLine(SystemName, NoticeTexts[notice]); }
		// Who of them is in the lobby's human slots, compared with the last look. The lobby chat is drawn each frame while
		// the lobby is shown; after a pause (the lobby was closed) everyone is new again.
		bool creatorHere = false;
		std::vector<std::wstring> friendsHere;
		std::vector<std::wstring> adminsHere;
		ULONGLONG lastLook = 0;
		constexpr ULONGLONG LobbyClosedMilliseconds = 3000;
		void WatchLobby() {
			const auto now = GetTickCount64();
			if (now - lastLook > LobbyClosedMilliseconds) {
				creatorHere = false;
				friendsHere.clear();
				adminsHere.clear();
				pendingNotices.clear();
			}
			lastLook = now;
			std::vector<std::wstring> inSlots;
			for (int slot = 0; slot < LobbySlotCount; ++slot) if (auto name = SlotName(slot); !name.empty()) inSlots.push_back(std::move(name));
			const auto inLobby = [&inSlots](const std::wstring& name) { return !name.empty() && std::ranges::find(inSlots, name) != inSlots.end(); };
			const auto& current = CurrentNames();
			const bool creatorNow = inLobby(current.creator);
			if (creatorNow != creatorHere) pendingNotices.push_back(creatorNow ? CreatorJoined : CreatorLeft);
			creatorHere = creatorNow;
			// Admins, then friends: who came and who went since the last look.
			const auto compare = [&inLobby](const std::vector<std::wstring>& known, std::vector<std::wstring>& here, Notice joined, Notice left) {
				std::vector<std::wstring> now;
				for (const auto& name : known) if (inLobby(name) && std::ranges::find(now, name) == now.end()) now.push_back(name);
				for (const auto& name : now) if (std::ranges::find(here, name) == here.end()) pendingNotices.push_back(joined);
				for (const auto& name : here) if (std::ranges::find(now, name) == now.end()) pendingNotices.push_back(left);
				here = std::move(now);
			};
			compare(current.admins, adminsHere, AdminJoined, AdminLeft);
			compare(current.friends, friendsHere, FriendJoined, FriendLeft);
			while (!pendingNotices.empty() && AddNotice(pendingNotices.front())) pendingNotices.pop_front();
			// Notices from Admins and the Creator (the notify command), as "SYSTEM" or "ALERT".
			while (const auto notice = Notices::Next()) {
				if (!AddLobbyLine(notice->label, notice->text)) break;
				Notices::Pop();
			}
		}
		// The lobby chat's drawing (0x544FA0, cdecl, two arguments): the lobby is looked at before each draw, outside the
		// game's own walk through the chat lines.
		using LobbyChatDrawFn = int(__cdecl*)(void* button, void* frontEnd);
		LobbyChatDrawFn originalLobbyChatDraw = nullptr;
		int __cdecl LobbyChatDrawDetour(void* button, void* frontEnd) {
			// Also how Steam multiplayer knows the lobby is on screen (its invite key).
			SteamMultiplayer::OnLobbyDrawn();
			WatchLobby();
			return originalLobbyChatDraw(button, frontEnd);
		}

		// The lobby's chat (button 0x1CD, drawn by 0x544FA0): each line is the sender's slot as a digit, then the text, up to
		// the next line's 0xFFFF. Its color (the sender's player color, made lighter) is set by the call at 0x5451D1
		// (0x62C3C0, as above), with the line's text (after the digit) in ESI. Our notices are white; other lines from the
		// creator's or a friend's slot get their color (rainbow, gold).
		bool LineIs(const wchar_t* text, std::wstring_view expected) {
			if (std::wcsncmp(text, expected.data(), expected.size()) != 0) return false;
			const wchar_t after = text[expected.size()];
			return after == 0 || after == static_cast<wchar_t>(0xFFFF);
		}
		Kind NoticeKind(const wchar_t* text) {
			const std::wstring prefix = std::wstring(SystemName) + L": ";
			if (std::wcsncmp(text, prefix.c_str(), prefix.size()) != 0) return Kind::None;
			for (std::size_t i = 0; i < NoticeTexts.size(); ++i) {
				if (!LineIs(text + prefix.size(), NoticeTexts[i])) continue;
				const auto notice = static_cast<Notice>(i);
				if (notice == CreatorJoined || notice == CreatorLeft) return Kind::Creator;
				if (notice == AdminJoined || notice == AdminLeft) return Kind::Admin;
				return Kind::Friend;
			}
			return Kind::None;
		}
		// Our notices are white, the easiest to read on the lobby's background.
		constexpr std::array<std::uint8_t, ColorSize> NoticeColor = { 255, 255, 255, 0xFF, 0 };
		void __cdecl LobbyChatColor(const wchar_t* text, std::uint8_t* color) {
			if (text == nullptr || color == nullptr) return;
			if (const auto alert = Notices::ShownLine(text)) {
				std::memcpy(color, *alert ? AlertColor.data() : NoticeColor.data(), ColorSize);
				return;
			}
			if (NoticeKind(text) != Kind::None) {
				std::memcpy(color, NoticeColor.data(), ColorSize);
				return;
			}
			const wchar_t digit = text[-1];
			if (digit >= L'0' && digit <= L'9') LobbySlotColor(digit - L'0', color);
		}
		__declspec(naked) void LobbyChatColorStub() {
			__asm {
				pushad
				mov eax, dword ptr [esp + 0x28]
				push eax
				push esi
				call LobbyChatColor
				add esp, 8
				popad
				jmp dword ptr [textColorSet]
			}
		}

		// Notices in the in-game chat: added with the game's local chat line call (0x409EC0, thiscall (interface, sender,
		// text) ret 8, which copies 0x102 bytes of text) and its special sender 0x8181 (no name; colored below).
		constexpr std::uint16_t InGameNoticeSender = 0x8181;
		constexpr std::size_t InGameChatCharacters = 0x102 / sizeof(wchar_t);
		using ChatAddLineFn = void(__fastcall*)(void* playerInterface, void* unused, std::uint32_t sender, const wchar_t* text);
		void SayInGame(void* playerInterface, const std::wstring& text) {
			std::array<wchar_t, InGameChatCharacters> line{};
			std::memcpy(line.data(), text.data(), (std::min)(text.size(), line.size() - 1) * sizeof(wchar_t));
			reinterpret_cast<ChatAddLineFn>(Game::Resolve(Game::Address::ChatAddLocalLine))(playerInterface, nullptr, InGameNoticeSender, line.data());
		}

		// ---- Chat lines (0x409F30): the color for each line is chosen by its sender, then at 0x40A00C ("lea eax,
		// [esp + 0x20]; lea ecx, [esp + 0x28]", 8 bytes) it is at [esp + 0x18] (5 bytes) with the line (sender, then text)
		// in EDI. A line starting with the creator's name gets the rainbow, a friend's gold.
		std::uintptr_t chatResume = 0;
		void __cdecl ChatLineColor(const std::uint8_t* line, std::uint8_t* color) {
			if (line == nullptr || color == nullptr) return;
			// Our notices (the notify command), added with the game's special sender: white or red.
			if (*reinterpret_cast<const std::uint16_t*>(line) == InGameNoticeSender) {
				if (const auto alert = Notices::ShownLine(reinterpret_cast<const wchar_t*>(line + 2))) {
					std::memcpy(color, *alert ? AlertColor.data() : WhiteColor.data(), ColorSize);
					return;
				}
			}
			if (const auto* special = ColorOf(KindOfText(reinterpret_cast<const wchar_t*>(line + 2), false))) std::memcpy(color, special, ColorSize);
		}
		__declspec(naked) void ChatColorStub() {
			__asm {
				pushad
				lea eax, [esp + 0x20 + 0x18]
				push eax
				push edi
				call ChatLineColor
				add esp, 8
				popad
				lea eax, [esp + 0x20]
				lea ecx, [esp + 0x28]
				jmp dword ptr [chatResume]
			}
		}
		// The chat box is drawn into a picture of its own only when it changed (interface + 0x1F39); its 3 lines are at
		// interface + 0x1C27, 0x106 bytes each { shown until (ms), sender, text }. While a creator line is shown, it is drawn
		// again at each step of the rainbow.
		constexpr std::uintptr_t ChatLines = 0x1C27;
		constexpr std::uintptr_t ChatLineSize = 0x106;
		constexpr std::uintptr_t ChatChanged = 0x1F39;
		using ChatDrawFn = void(__fastcall*)(std::uint8_t* playerInterface);
		ChatDrawFn originalChatDraw = nullptr;
		std::size_t lastStep = 0;
		void __fastcall ChatDrawDetour(std::uint8_t* playerInterface) {
			// Notices waiting (the notify command) go into the in-game chat.
			if (playerInterface != nullptr) {
				while (const auto notice = Notices::Next()) {
					SayInGame(playerInterface, notice->label + L": " + notice->text);
					Notices::Pop();
				}
			}
			const auto step = Step();
			if (playerInterface != nullptr && step != lastStep) {
				lastStep = step;
				for (std::size_t i = 0; i < 3; ++i) {
					const auto* line = playerInterface + ChatLines + i * ChatLineSize;
					if (*reinterpret_cast<const std::uint32_t*>(line) == 0) continue;
					if (KindOfText(reinterpret_cast<const wchar_t*>(line + 6), false) == Kind::Creator) {
						*reinterpret_cast<std::int32_t*>(playerInterface + ChatChanged) = 1;
						break;
					}
				}
			}
			originalChatDraw(playerInterface);
		}

		bool Hook(std::uintptr_t target, void* detour, void** original) {
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
	}

	void CreatorName::Install() {
		const auto list = Game::Resolve(Game::Address::PlayerListColorPush);
		const auto chat = Game::Resolve(Game::Address::ChatLineColorChosen);
		const auto draw = Game::Resolve(Game::Address::ChatDraw);
		listResume = list + 8;
		chatResume = chat + 8;
		const bool listOk = Memory::Matches(list, { 0x83, 0xC0, 0x14, 0x50, 0x8B, 0x44, 0x24, 0x7C })
			&& Memory::WriteJump(list, reinterpret_cast<const void*>(&PlayerListStub), 8);
		bool chatOk = Memory::Matches(chat, { 0x8D, 0x44, 0x24, 0x20, 0x8D, 0x4C, 0x24, 0x28 })
			&& Memory::Matches(draw, { 0x83, 0xEC, 0x20, 0x53, 0x55, 0x56 });
		if (chatOk) {
			chatOk = Hook(draw, reinterpret_cast<void*>(&ChatDrawDetour), reinterpret_cast<void**>(&originalChatDraw));
			if (chatOk) chatOk = Memory::WriteJump(chat, reinterpret_cast<const void*>(&ChatColorStub), 8);
		}
		const auto lobbyCall = Game::Resolve(Game::Address::LobbySlotColorSet);
		lobbySlots = Game::Resolve(Game::Address::LobbySlots);
		textColorSet = Game::Resolve(Game::Address::TextColorSet);
		const bool lobbyOk = Memory::Matches(lobbyCall - 11, { 0x51, 0x52, 0xB9, 0xC0, 0x13, 0x74, 0x00, 0x88, 0x44, 0x24, 0x3E, 0xE8, 0x9E, 0x77, 0x0E, 0x00 })
			&& Memory::WriteCall(lobbyCall, reinterpret_cast<const void*>(&LobbyColorStub));
		if (!listOk) Logger::Warning("Creator name: unexpected game code, the creator's name is not colored in the in-game player list.");
		if (!chatOk) Logger::Warning("Creator name: unexpected game code, the creator's name is not colored in the chat.");
		if (!lobbyOk) Logger::Warning("Creator name: unexpected game code, the creator's name is not colored in the lobby.");
		const auto lobbyChatCall = Game::Resolve(Game::Address::LobbyChatColorSet);
		// mov byte ptr [esp + 0x20], 0 / call 0x62C3C0
		const bool lobbyChatOk = Memory::Matches(lobbyChatCall - 5, { 0xC6, 0x44, 0x24, 0x20, 0x00, 0xE8, 0xEA, 0x71, 0x0E, 0x00 })
			&& Memory::WriteCall(lobbyChatCall, reinterpret_cast<const void*>(&LobbyChatColorStub));
		if (!lobbyChatOk) Logger::Warning("Creator name: unexpected game code, the creator's name is not colored in the lobby chat.");
		// The lobby chat notices: the chat handler's start (frame setup, then "push eax; mov eax, [esp+0x10]", its sender)
		// and the chat drawing's start (frame setup, 13 bytes).
		lobbyChatReceive = Game::Resolve(Game::Address::LobbyChatReceive);
		chatCallback = Game::Resolve(Game::Address::NetworkChatCallback);
		screenState = Game::Resolve(Game::Address::FrontEndScreen);
		const auto lobbyChatDraw = Game::Resolve(Game::Address::LobbyChatDraw);
		const bool noticesOk = Memory::Matches(lobbyChatReceive, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x5B, 0xDF, 0x64, 0x00, 0x50, 0x8B, 0x44, 0x24, 0x10 })
			&& Memory::Matches(lobbyChatDraw, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x9B, 0xDF, 0x64, 0x00 })
			&& lobbyChatOk
			&& Hook(lobbyChatDraw, reinterpret_cast<void*>(&LobbyChatDrawDetour), reinterpret_cast<void**>(&originalLobbyChatDraw));
		if (!noticesOk) Logger::Warning("Creator name: unexpected game code, no lobby chat notices when the creator, an admin or a friend joins or leaves.");
	}
}
