#include "SkirmishSwapKeeper.h"
#include "CoopCampaign.h"
#include "CustomCampaign.h"
#include "FrontEndTexts.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }
		// Config tables are packed, so their fields are read and written byte-wise.
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		using RenderFn = char(__cdecl*)(std::uint8_t* button, std::uint8_t* frontEnd);
		using SetupFn = void(__cdecl*)(std::uint8_t* frontEnd);

		// Our buttons: copies of "Add Computer Player", one and two rows lower, level with the right-hand buttons.
		constexpr std::int32_t SwapButtonId = 9000;
		constexpr std::int32_t GoodButtonId = 9001;
		constexpr std::int32_t AddComputerButtonId = 528;
		constexpr std::int16_t SwapButtonY = 1336;
		constexpr std::int16_t GoodButtonY = 1484;
		// Our texts, in order after the game's: Swap Keeper, Pick a Keeper (Cancel), Swap to Good Player,
		// Swap Back to Keeper.
		constexpr std::int32_t GoodTextOffset = 2;
		// The heroes' player number.
		constexpr std::uint32_t GoodPlayer = 1;
		constexpr std::uint8_t SlotComputer = 1;
		// Slot flags: the type in the low 3 bits; computer slots also get 0x18 (as Add Computer Player sets them),
		// human slots have bit 3 clear.
		constexpr std::uint8_t SlotTypeMask = 0x07;
		constexpr std::uint8_t ComputerFlags = 0x18;
		constexpr std::uint8_t HumanClearedFlags = 0x08;
		// Computer slot fields Add Computer Player clears: AI type (+0), and two values at +0x22 and +0x26.
		constexpr std::uintptr_t SlotAiType = 0x00;
		constexpr std::uintptr_t SlotValueA = 0x22;
		constexpr std::uintptr_t SlotValueB = 0x26;
		constexpr std::int32_t ButtonListEnd = -1;
		constexpr std::uint8_t SkirmishMenuState = 7;
		constexpr std::uint8_t MultiplayerMenuState = 0x10;
		// The multiplayer lobby's Swap Keeper: in the free gap between the player list and Kick Player, lined up with Kick
		// Player. Add Computer Player's own box is much wider and the row above it holds a computer player's Keeper choice.
		constexpr std::int16_t MultiplayerSwapX = 64;
		constexpr std::int16_t MultiplayerSwapY = 900;
		constexpr std::int16_t MultiplayerSwapWidth = 560;
		std::vector<std::uint8_t> multiplayerButtonList;
		std::int32_t multiplayerTextIndex = -1;
		constexpr std::size_t MaxButtons = 64;
		constexpr std::uint8_t SlotEmpty = 0;
		constexpr std::uint8_t SlotHuman = 2;
		constexpr std::uint8_t SlotHumanAlt = 3;

		SetupFn originalSetup = nullptr;
		std::vector<std::uint8_t> buttonList;
		// Index of our "Swap Keeper" text; the "pick a keeper" text follows it. -1 until the texts are made.
		std::int32_t textIndex = -1;
		bool picking = false;
		std::uint32_t pickedSlot = 0;
		bool rowSeen = false;
		std::int32_t lastRow = -1;
		// Playing as the Good player: the human's slot was handed to a computer player, kept here to swap back.
		bool playingGood = false;
		std::array<std::uint8_t, Game::PlayerSlotSize> humanSlot{};
		std::uint32_t leftSlot = 0;

		std::uint8_t* Slot(std::uint32_t index) { return reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::PlayerSlots)) + index * Game::PlayerSlotSize; }
		std::uint8_t* SlotText(std::uint32_t index) { return reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::PlayerSlotTexts)) + index * Game::PlayerSlotTextSize; }
		std::uint8_t SlotType(std::uint32_t index) { return Slot(index)[Game::Offset::PlayerSlotFlags] & 7; }
		std::int32_t& Selection() { return *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::PlayerListSelection)); }
		std::uint32_t SlotCount(const std::uint8_t* frontEnd) { return std::min<std::uint32_t>(frontEnd[Game::Offset::FrontEndSlotCount], Game::PlayerSlotCount); }
		bool InSkirmish(const std::uint8_t* frontEnd) { return frontEnd != nullptr && frontEnd[Game::Offset::FrontEndMenuState] == SkirmishMenuState; }

		// The list shows only the slots in use, in slot order.
		std::int32_t RowToSlot(const std::uint8_t* frontEnd, std::int32_t row) {
			if (row < 0) return -1;
			for (std::uint32_t slot = 0, count = SlotCount(frontEnd); slot < count; ++slot) {
				if (SlotType(slot) == SlotEmpty) continue;
				if (row-- == 0) return static_cast<std::int32_t>(slot);
			}
			return -1;
		}
		std::int32_t SlotToRow(std::uint32_t slot) {
			std::int32_t row = 0;
			for (std::uint32_t index = 0; index < slot; ++index) {
				if (SlotType(index) != SlotEmpty) ++row;
			}
			return row;
		}

		// Skirmish start stores a fixed 3 (keeper 1) as the local player; point it at the human's slot instead.
		void SetStartPlayer(std::uint32_t player) {
			const auto immediate = Game::Resolve(Game::Address::SkirmishStartLocalPlayer) + 6;
			if (Memory::Read<std::uint32_t>(immediate) != player) Memory::Write<std::uint32_t>(immediate, player);
		}
		bool IsHuman(std::uint8_t type) { return type == SlotHuman || type == SlotHumanAlt; }
		std::int32_t HumanSlot(const std::uint8_t* frontEnd) {
			for (std::uint32_t slot = 0, count = SlotCount(frontEnd); slot < count; ++slot) {
				if (IsHuman(SlotType(slot))) { return static_cast<std::int32_t>(slot); }
			}
			return -1;
		}
		void UpdateStartPlayer(const std::uint8_t* frontEnd) {
			const auto human = HumanSlot(frontEnd);
			// A human back in the list (put back, or the screen opened again) ends playing as the Good player.
			if (human >= 0) { playingGood = false; }
			SetStartPlayer(human >= 0 ? Game::FirstKeeperPlayer + human : (playingGood ? GoodPlayer : Game::FirstKeeperPlayer));
		}
		std::uint32_t& HumanCount() { return *reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::LobbyHumanCount)); }
		std::uint32_t& ComputerCount() { return *reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::LobbyComputerCount)); }

		// Hands the human's slot to a computer player (of the first AI type, as Add Computer Player makes them) and
		// starts the game as the Good player.
		void SwapToGood(const std::uint8_t* frontEnd) {
			const auto human = HumanSlot(frontEnd);
			if (human < 0) return;
			auto* slot = Slot(static_cast<std::uint32_t>(human));
			std::memcpy(humanSlot.data(), slot, humanSlot.size());
			leftSlot = static_cast<std::uint32_t>(human);
			std::memset(slot + SlotAiType, 0, 0x20);
			Store<std::uint32_t>(slot + SlotValueA, 0);
			Store<std::uint32_t>(slot + SlotValueB, 0);
			slot[Game::Offset::PlayerSlotFlags] = static_cast<std::uint8_t>((slot[Game::Offset::PlayerSlotFlags] & ~SlotTypeMask) | SlotComputer | ComputerFlags);
			if (HumanCount() > 0) --HumanCount();
			++ComputerCount();
			playingGood = true;
			Selection() = SlotToRow(leftSlot);
			Logger::Info("Skirmish: playing as the Good player; keeper " + std::to_string(leftSlot + 1) + " is now a computer player.");
		}
		// Puts the human back: into the selected computer row, else the slot left (a computer player or empty).
		void SwapBackToKeeper(const std::uint8_t* frontEnd) {
			std::int32_t target = RowToSlot(frontEnd, Selection());
			if (target < 0 || SlotType(static_cast<std::uint32_t>(target)) != SlotComputer) {
				target = leftSlot < SlotCount(frontEnd) ? static_cast<std::int32_t>(leftSlot) : -1;
			}
			if (target < 0 || IsHuman(SlotType(static_cast<std::uint32_t>(target)))) return;
			auto* slot = Slot(static_cast<std::uint32_t>(target));
			if (SlotType(static_cast<std::uint32_t>(target)) == SlotComputer && ComputerCount() > 0) --ComputerCount();
			// The human's own fields; the relations with the other slots stay those of this slot.
			std::memcpy(slot, humanSlot.data(), Game::Offset::PlayerSlotRelations);
			slot[Game::Offset::PlayerSlotFlags] = static_cast<std::uint8_t>((slot[Game::Offset::PlayerSlotFlags] & ~(SlotTypeMask | HumanClearedFlags | ComputerFlags))
				| (humanSlot[Game::Offset::PlayerSlotFlags] & (SlotTypeMask | HumanClearedFlags | ComputerFlags)));
			++HumanCount();
			playingGood = false;
			Selection() = SlotToRow(static_cast<std::uint32_t>(target));
			Logger::Info("Skirmish: back to playing keeper " + std::to_string(target + 1) + ".");
		}

		void SwapSlots(std::uint32_t first, std::uint32_t second) {
			std::swap_ranges(Slot(first), Slot(first) + Game::PlayerSlotSize, Slot(second));
			std::swap_ranges(SlotText(first), SlotText(first) + Game::PlayerSlotTextSize, SlotText(second));
			// Each slot also stores how it stands with every other slot, by slot number.
			for (std::uint32_t slot = 0; slot < Game::PlayerSlotCount; ++slot) {
				for (const auto table : { Game::Offset::PlayerSlotRelations, Game::Offset::PlayerSlotAlliances }) {
					std::swap(Slot(slot)[table + first], Slot(slot)[table + second]);
				}
			}
		}

		// ---- Multiplayer: only the host changes the lobby's slots (the game sends them to everyone), and never swaps
		// two humans. The game takes a player's slot to be the one it gave them on joining: it is kept in MyLobbySlot
		// (each player's slot updates, ready included, are sent for that slot number) and the local player number at
		// the start is 3 + the network place. So each machine remembers its human's network id (slot + 0x3B) while
		// that slot is still theirs, keeps MyLobbySlot on the slot holding it, and at the start plays that slot.
		bool InMultiplayer(const std::uint8_t* frontEnd) { return frontEnd != nullptr && frontEnd[Game::Offset::FrontEndMenuState] == MultiplayerMenuState; }
		bool IsHost(const std::uint8_t* frontEnd) { return Load<std::int32_t>(frontEnd + Game::Offset::FrontEndIsHost) == 1; }
		bool myIdKnown = false;
		std::uint32_t myNetworkId = 0;
		ULONGLONG lastLobbyFrame = 0;
		std::int32_t& MyLobbySlot() { return *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::MyLobbySlot)); }
		std::int32_t SlotWithMyId() {
			if (!myIdKnown) return -1;
			for (std::uint32_t slot = 0; slot < Game::PlayerSlotCount; ++slot) {
				if (IsHuman(SlotType(slot)) && Load<std::uint32_t>(Slot(slot) + Game::Offset::PlayerSlotNetworkId) == myNetworkId) return static_cast<std::int32_t>(slot);
			}
			return -1;
		}
		// Each network slot's player id, learned the first time that slot holds a human (the game puts every joining
		// player into its network slot), so their messages can be sent to wherever a swap moved them.
		std::array<std::uint32_t, Game::PlayerSlotCount> networkSlotIds{};
		std::array<bool, Game::PlayerSlotCount> networkSlotKnown{};
		std::int32_t SlotWithId(std::uint32_t id) {
			for (std::uint32_t slot = 0; slot < Game::PlayerSlotCount; ++slot) {
				if (IsHuman(SlotType(slot)) && Load<std::uint32_t>(Slot(slot) + Game::Offset::PlayerSlotNetworkId) == id) return static_cast<std::int32_t>(slot);
			}
			return -1;
		}
		void FollowMySlot(const std::uint8_t* frontEnd) {
			const ULONGLONG now = GetTickCount64();
			// Back in the lobby after a game (or a new one): find this machine's human and the others again.
			if (now - lastLobbyFrame > 1000) {
				myIdKnown = false;
				picking = false;
				networkSlotKnown.fill(false);
			}
			lastLobbyFrame = now;
			for (std::uint32_t slot = 0, count = SlotCount(frontEnd); slot < count; ++slot) {
				if (networkSlotKnown[slot] || !IsHuman(SlotType(slot))) continue;
				networkSlotIds[slot] = Load<std::uint32_t>(Slot(slot) + Game::Offset::PlayerSlotNetworkId);
				networkSlotKnown[slot] = true;
			}
			if (!myIdKnown) {
				const auto mine = MyLobbySlot();
				if (mine < 0 || static_cast<std::uint32_t>(mine) >= SlotCount(frontEnd) || !IsHuman(SlotType(static_cast<std::uint32_t>(mine)))) return;
				myNetworkId = Load<std::uint32_t>(Slot(static_cast<std::uint32_t>(mine)) + Game::Offset::PlayerSlotNetworkId);
				myIdKnown = true;
			}
			const auto slot = SlotWithMyId();
			if (slot >= 0 && slot != MyLobbySlot()) {
				MyLobbySlot() = slot;
				Logger::Info("Multiplayer: your keeper is now keeper " + std::to_string(slot + 1) + " (swapped in the lobby).");
			}
		}
		// A message from network slot n is for the slot that now holds that player (also this machine's own, which the
		// game then ignores as it should).
		using LobbyMessageFn = void(__cdecl*)(const std::uint8_t* message, std::uint32_t size, std::uint32_t sender, std::uint8_t* frontEnd);
		LobbyMessageFn originalLobbyMessage = nullptr;
		void __cdecl LobbyMessageDetour(const std::uint8_t* message, std::uint32_t size, std::uint32_t sender, std::uint8_t* frontEnd) {
			if (CoopCampaign::OnLobbyMessage(message, size)) return;
			if (sender < Game::PlayerSlotCount && networkSlotKnown[sender]) {
				const auto slot = SlotWithId(networkSlotIds[sender]);
				if (slot >= 0) sender = static_cast<std::uint32_t>(slot);
			}
			originalLobbyMessage(message, size, sender, frontEnd);
		}
		using NetworkIndexFn = std::uint32_t(__fastcall*)(void* network);
		NetworkIndexFn originalNetworkIndex = nullptr;
		std::uint32_t __fastcall NetworkIndexDetour(void* network) {
			const std::uint32_t index = originalNetworkIndex(network);
			const auto slot = SlotWithMyId();
			return slot >= 0 ? static_cast<std::uint32_t>(slot) : index;
		}

		// The second row picked while Swap Keeper waits: the two keepers swap. In multiplayer two humans never do.
		void ContinuePicking(std::uint8_t* frontEnd, bool multiplayer) {
			if (!picking) return;
			if (pickedSlot >= SlotCount(frontEnd) || SlotType(pickedSlot) == SlotEmpty) {
				picking = false;
				return;
			}
			// Wait for the selection to move to another row; the row selected when the button was pressed is the
			// keeper being moved.
			const auto row = Selection();
			if (!rowSeen) {
				rowSeen = true;
				lastRow = row;
				return;
			}
			if (row == lastRow) return;
			lastRow = row;
			const auto target = RowToSlot(frontEnd, row);
			if (target < 0 || static_cast<std::uint32_t>(target) == pickedSlot) return;
			if (multiplayer && IsHuman(SlotType(pickedSlot)) && IsHuman(SlotType(static_cast<std::uint32_t>(target)))) return;
			// The game ties the host to its own slot (keeper 1): the host's keeper never moves.
			if (multiplayer && target == SlotWithMyId()) return;
			SwapSlots(pickedSlot, static_cast<std::uint32_t>(target));
			Selection() = SlotToRow(static_cast<std::uint32_t>(target));
			picking = false;
			if (!multiplayer) UpdateStartPlayer(frontEnd);
			Logger::Info(std::string(multiplayer ? "Multiplayer" : "Skirmish") + ": swapped keeper " + std::to_string(pickedSlot + 1) + " with keeper "
				+ std::to_string(target + 1) + ".");
		}
		void Update(std::uint8_t* frontEnd) {
			if (!InSkirmish(frontEnd)) {
				if (!InMultiplayer(frontEnd)) picking = false;
				return;
			}
			UpdateStartPlayer(frontEnd);
			ContinuePicking(frontEnd, false);
		}

		void __cdecl OnSwapClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) {
			if (!InSkirmish(frontEnd) && !(InMultiplayer(frontEnd) && IsHost(frontEnd))) return;
			if (picking) {
				picking = false;
				return;
			}
			const auto slot = RowToSlot(frontEnd, Selection());
			if (slot < 0) return;
			// Only the host swaps in multiplayer, and never its own keeper (see ContinuePicking).
			if (InMultiplayer(frontEnd) && slot == SlotWithMyId()) {
				Logger::Info("Multiplayer: the host's keeper cannot be swapped (the game ties the host to it).");
				return;
			}
			picking = true;
			pickedSlot = static_cast<std::uint32_t>(slot);
			rowSeen = false;
		}

		char __cdecl RenderSwapButton(std::uint8_t* button, std::uint8_t* frontEnd) {
			Update(frontEnd);
			if (textIndex < 0) return 0;
			Store<std::int32_t>(button + Game::Offset::ButtonTextIndex, textIndex + (picking ? 1 : 0));
			return GameFunction<RenderFn>(Game::Address::ClickButtonRender)(button, frontEnd);
		}
		// The multiplayer lobby's button: every machine remembers its human here; only the host sees and uses it.
		char __cdecl RenderMultiplayerSwapButton(std::uint8_t* button, std::uint8_t* frontEnd) {
			if (!InMultiplayer(frontEnd)) return 0;
			FollowMySlot(frontEnd);
			if (!IsHost(frontEnd)) {
				picking = false;
				return 0;
			}
			ContinuePicking(frontEnd, true);
			if (multiplayerTextIndex < 0) return 0;
			Store<std::int32_t>(button + Game::Offset::ButtonTextIndex, multiplayerTextIndex + (picking ? 1 : 0));
			return GameFunction<RenderFn>(Game::Address::ClickButtonRender)(button, frontEnd);
		}

		void __cdecl OnGoodClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) {
			if (!InSkirmish(frontEnd)) return;
			picking = false;
			if (playingGood) { SwapBackToKeeper(frontEnd); }
			else { SwapToGood(frontEnd); }
			UpdateStartPlayer(frontEnd);
		}
		char __cdecl RenderGoodButton(std::uint8_t* button, std::uint8_t* frontEnd) {
			if (textIndex < 0) return 0;
			Store<std::int32_t>(button + Game::Offset::ButtonTextIndex, textIndex + GoodTextOffset + (playingGood ? 1 : 0));
			return GameFunction<RenderFn>(Game::Address::ClickButtonRender)(button, frontEnd);
		}

		// World setup makes each player from the map with 0x4C2340 (thiscall on world + 0x7A: number, tag out), and
		// only keeper players (3 and up) are then checked against the local player number to become the player you
		// control. The Good and neutral players are made on another path, so for them the same is done here. The local
		// player number is only 1 or 2 when playing as the Good player.
		// Player init gives no task list (which a controlled player's commands go through) to the Good player. While the
		// Good player you control is being made, its jump past the task list is turned off, and put back right after,
		// so every other player (and every other game) is set up as before.
		bool taskSkipPatchable = false;
		void SetGoodTaskSkip(bool skip) {
			const auto jump = Game::Resolve(Game::Address::PlayerInitTaskSkip) + 2;
			Memory::Write<std::uint16_t>(jump, skip ? std::uint16_t{ 0x6374 } : std::uint16_t{ 0x9090 });
		}

		using CreatePlayerFn = std::int32_t(__fastcall*)(void* players, void* unused, std::uint32_t number, std::uint16_t* tag);
		CreatePlayerFn originalCreatePlayer = nullptr;
		std::int32_t __fastcall CreatePlayerDetour(void* players, void* unused, std::uint32_t number, std::uint16_t* tag) {
			const auto playerNumber = number & 0xFF;
			const auto local = *reinterpret_cast<const std::uint32_t*>(Game::Resolve(Game::Address::LocalPlayerNumber));
			// In a co-op campaign every machine has to make the human Good player the same way.
			const bool controlledGood = playerNumber == GoodPlayer && (playerNumber == local || CoopCampaign::HumanPlays(GoodPlayer));
			if (controlledGood && taskSkipPatchable) SetGoodTaskSkip(false);
			const std::int32_t result = originalCreatePlayer(players, unused, number, tag);
			if (controlledGood && taskSkipPatchable) SetGoodTaskSkip(true);
			if (result != 0 && tag != nullptr && playerNumber < Game::FirstKeeperPlayer && playerNumber == local) {
				auto* world = reinterpret_cast<std::uint8_t*>(players) - Game::Offset::WorldPlayers;
				if ((world[Game::Offset::WorldLocalFlags] & Game::LocalPlayerChosenFlag) == 0) {
					Store<std::uint16_t>(world + Game::Offset::WorldLocalPlayerTag, *tag);
					Logger::Info("Skirmish: you are the Good player.");
				}
			}
			return result;
		}

		void __cdecl SetupDetour(std::uint8_t* frontEnd) {
			picking = false;
			playingGood = false;
			originalSetup(frontEnd);
			SetStartPlayer(Game::FirstKeeperPlayer);
			// The campaign list or a campaign's level takes the skirmish screen's place before it is drawn.
			CustomCampaign::AfterSkirmishSetup(frontEnd);
		}
		bool setupHooked = false;

		void MakeButton(std::uint8_t* entry, const std::uint8_t* addComputer, std::int32_t id, std::int16_t y, void (__cdecl* click)(std::int32_t, std::int32_t, std::uint8_t*), char (__cdecl* render)(std::uint8_t*, std::uint8_t*)) {
			std::memcpy(entry, addComputer, Game::ButtonConfigSize);
			Store<std::int32_t>(entry + Game::Offset::ButtonConfigId, id);
			Store<std::uint32_t>(entry + Game::Offset::ButtonConfigClick, reinterpret_cast<std::uint32_t>(click));
			Store<std::int16_t>(entry + Game::Offset::ButtonConfigY, y);
			Store<std::uint32_t>(entry + Game::Offset::ButtonConfigRender, reinterpret_cast<std::uint32_t>(render));
			Store<std::int32_t>(entry + Game::Offset::ButtonConfigTextId, 0);
			Store<std::int32_t>(entry + Game::Offset::ButtonConfigTextIndex, 0);
		}
		// Copies the skirmish button list with our buttons added before the end marker.
		bool BuildButtonList(const std::uint8_t* buttons) {
			std::size_t count = 0;
			const std::uint8_t* addComputer = nullptr;
			for (; count < MaxButtons; ++count) {
				const auto* entry = buttons + count * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				if (id == ButtonListEnd) break;
				if (id == SwapButtonId || id == GoodButtonId) return false;
				if (id == AddComputerButtonId) addComputer = entry;
			}
			if (count == MaxButtons || addComputer == nullptr) return false;
			buttonList.assign((count + 3) * Game::ButtonConfigSize, 0);
			std::memcpy(buttonList.data(), buttons, count * Game::ButtonConfigSize);
			auto* ours = buttonList.data() + count * Game::ButtonConfigSize;
			MakeButton(ours, addComputer, SwapButtonId, SwapButtonY, &OnSwapClick, &RenderSwapButton);
			MakeButton(ours + Game::ButtonConfigSize, addComputer, GoodButtonId, GoodButtonY, &OnGoodClick, &RenderGoodButton);
			std::memcpy(ours + 2 * Game::ButtonConfigSize, buttons + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			return true;
		}

		bool Hook(std::uintptr_t target, void* detour, void** original) {
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}

		// The multiplayer lobby's list with Swap Keeper (a copy of its Add Computer Player) added before the end marker.
		bool BuildMultiplayerButtonList(const std::uint8_t* buttons) {
			std::size_t count = 0;
			const std::uint8_t* addComputer = nullptr;
			for (; count < MaxButtons; ++count) {
				const auto* entry = buttons + count * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				if (id == ButtonListEnd) break;
				if (id == SwapButtonId) return false;
				if (id == AddComputerButtonId) addComputer = entry;
			}
			if (count == MaxButtons || addComputer == nullptr) return false;
			multiplayerButtonList.assign((count + 2) * Game::ButtonConfigSize, 0);
			std::memcpy(multiplayerButtonList.data(), buttons, count * Game::ButtonConfigSize);
			auto* ours = multiplayerButtonList.data() + count * Game::ButtonConfigSize;
			MakeButton(ours, addComputer, SwapButtonId, MultiplayerSwapY, &OnSwapClick, &RenderMultiplayerSwapButton);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigX, MultiplayerSwapX);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigWidth, MultiplayerSwapWidth);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigTextWidth, MultiplayerSwapWidth);
			std::memcpy(ours + Game::ButtonConfigSize, buttons + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			return true;
		}
		void InstallMultiplayer() {
			const auto windowConfig = Game::Resolve(Game::Address::MultiplayerWindowConfig);
			const auto buttons = Memory::Read<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons);
			const auto networkIndex = Game::Resolve(Game::Address::NetworkLocalIndex);
			if (Memory::Read<std::uint8_t>(windowConfig) != Game::MultiplayerWindowId || buttons != Game::Resolve(Game::Address::MultiplayerButtons)
				|| !Memory::Matches(networkIndex, { 0x81, 0xEC, 0x50, 0x01, 0x00, 0x00 }) || !BuildMultiplayerButtonList(reinterpret_cast<const std::uint8_t*>(buttons))) {
				Logger::Warning("Multiplayer: unexpected game code, Swap Keeper left out of the multiplayer lobby.");
				return;
			}
			const auto lobbyMessage = Game::Resolve(Game::Address::LobbyMessage);
			if (!Memory::Matches(lobbyMessage, { 0x83, 0xEC, 0x34, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x4C })) {
				Logger::Warning("Multiplayer: unexpected lobby message code, Swap Keeper left out of the multiplayer lobby.");
				return;
			}
			if (!Hook(networkIndex, reinterpret_cast<void*>(&NetworkIndexDetour), reinterpret_cast<void**>(&originalNetworkIndex))) {
				Logger::Warning("Multiplayer: could not hook the game start, Swap Keeper left out of the multiplayer lobby.");
				return;
			}
			if (!Hook(lobbyMessage, reinterpret_cast<void*>(&LobbyMessageDetour), reinterpret_cast<void**>(&originalLobbyMessage))) {
				HookManager::RemoveHook(networkIndex);
				Logger::Warning("Multiplayer: could not hook the lobby messages, Swap Keeper left out of the multiplayer lobby.");
				return;
			}
			FrontEndTexts::Add(Game::MultiplayerWindowId, {
				{ SwapButtonId, L"Swap Keeper" },
				{ SwapButtonId, L"Pick Keeper" },
			}, &multiplayerTextIndex);
			Memory::Write<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(multiplayerButtonList.data()));
			Logger::Debug("Multiplayer: Swap Keeper added to the lobby (host only, a human only with a computer player).");
		}
	}

	bool SkirmishSwapKeeper::SetupHooked() { return setupHooked; }

	void SkirmishSwapKeeper::Install() {
		const auto windowConfig = Game::Resolve(Game::Address::SkirmishWindowConfig);
		const auto buttons = Memory::Read<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons);
		const auto setup = Game::Resolve(Game::Address::SkirmishSetup);
		const auto keeperCheck = Game::Resolve(Game::Address::WorldSetupKeeperCheck);
		const auto startPlayer = Game::Resolve(Game::Address::SkirmishStartLocalPlayer);
		const auto localPlayer = static_cast<std::uint32_t>(Game::Resolve(Game::Address::LocalPlayerNumber));
		std::vector<std::uint8_t> startPattern = { 0xC7, 0x05 };
		for (int shift = 0; shift < 32; shift += 8) startPattern.push_back(static_cast<std::uint8_t>(localPlayer >> shift));
		startPattern.insert(startPattern.end(), { 0x03, 0x00, 0x00, 0x00 });
		const bool matches = buttons == Game::Resolve(Game::Address::SkirmishButtons)
			&& Memory::Matches(setup, { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x56, 0x6A, 0x09, 0x6A, 0x01 })
			&& Memory::Matches(keeperCheck, { 0x3C, 0x03, 0x72, 0x4A, 0x76, 0x48 })
			&& Memory::Matches(startPlayer, startPattern);
		if (!matches || !BuildButtonList(reinterpret_cast<const std::uint8_t*>(buttons))) {
			Logger::Warning("Skirmish: unexpected game code, Swap Keeper left out.");
			return;
		}
		if (!FrontEndTexts::Install()) {
			Logger::Warning("Skirmish: failed to hook the button texts, Swap Keeper left out.");
			return;
		}
		if (!Hook(setup, reinterpret_cast<void*>(&SetupDetour), reinterpret_cast<void**>(&originalSetup))) {
			Logger::Warning("Skirmish: failed to hook the skirmish setup, Swap Keeper left out.");
			return;
		}
		setupHooked = true;
		FrontEndTexts::Add(Game::SkirmishWindowId, {
			{ SwapButtonId, L"Swap Keeper" },
			{ SwapButtonId, L"Pick a Keeper (Cancel)" },
			{ GoodButtonId, L"Swap to Good Player" },
			{ GoodButtonId, L"Swap Back to Keeper" },
		}, &textIndex);
		const auto createPlayer = Game::Resolve(Game::Address::WorldCreatePlayer);
		taskSkipPatchable = Memory::Matches(Game::Resolve(Game::Address::PlayerInitTaskSkip), { 0x3C, 0x01, 0x74, 0x63, 0x3C, 0x02, 0x74, 0x5F });
		if (!taskSkipPatchable) Logger::Warning("Skirmish: unexpected player setup code, playing as the Good player may crash.");
		if (!Memory::Matches(createPlayer, { 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xC0, 0x66, 0x8B, 0x47, 0x0E })
			|| !Hook(createPlayer, reinterpret_cast<void*>(&CreatePlayerDetour), reinterpret_cast<void**>(&originalCreatePlayer))) {
			Logger::Warning("Skirmish: could not hook player setup, Swap to Good Player will not take control of the heroes.");
		}
		// Keeper 1 can now be a computer player or empty, so let world setup handle it like the other keepers.
		Memory::Write<std::uint8_t>(keeperCheck + 1, 0x02);
		Memory::Write<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(buttonList.data()));
		Logger::Debug("Skirmish: Swap Keeper and Swap to Good Player buttons added.");
		InstallMultiplayer();
	}
}
