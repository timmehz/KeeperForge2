#include "SessionOptions.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace KeeperForge2 {
	namespace {
		struct OptionInfo {
			const char* key;
			const char* onValue;
			const char* offValue;
			// One more value the key may hold, which is another option's (mercenary-portal-spawns has three).
			const char* otherValue;
			std::uint8_t flag;
		};
		// In the order of SessionOption. The flags go in a free byte of the lobby's creature list, first row.
		constexpr std::array<OptionInfo, 6> Options = { {
			{ "mercenary-portal-spawns", "normal", "mercenary", "off", 0x01 },
			{ "mercenary-portal-spawns", "off", "mercenary", "normal", 0x02 },
			{ "keeper-dwarf-workers", "on", "off", nullptr, 0x04 },
			{ "mercenary-portal-stone-knights", "off", "on", nullptr, 0x08 },
			{ "mercenary-portal-royals", "off", "on", nullptr, 0x10 },
			{ "create-imp-price-rises", "off", "on", nullptr, 0x20 },
		} };
		// Set whenever the flags were written, as a list that was never written holds zeros.
		constexpr std::uint8_t WrittenFlag = 0x80;
		// The front end's menu state while in the multiplayer lobby (the game tests it with the host flag, 0x545FC0).
		constexpr std::uint8_t MultiplayerMenuState = 0x10;
		std::array<bool, Options.size()> local{};
		std::array<bool, Options.size()> session{};
		bool sessionKnown = false;
		std::vector<void (*)()> levelStartCallbacks;
		std::vector<void (*)(std::uint8_t*)> lobbyWriteCallbacks;
		std::vector<void (*)(const std::uint8_t*)> lobbyReadCallbacks;

		std::size_t Index(SessionOption option) { return static_cast<std::size_t>(option); }
		bool Multiplayer() { return Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode)) == Game::PlayModeMultiplayer; }
		// The first row of the lobby's creature list, or null while there is none.
		std::uint8_t* FirstListRow(const std::uint8_t* frontEnd) {
			if (Memory::Read<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frontEnd) + Game::Offset::LobbyCreatureCount) == 0) return nullptr;
			return *reinterpret_cast<std::uint8_t* const*>(frontEnd + Game::Offset::LobbyCreatureList);
		}
		bool HasJoinedAnotherPlayersGame(const std::uint8_t* frontEnd) {
			return frontEnd[Game::Offset::FrontEndMenuState] == MultiplayerMenuState
				&& Memory::Read<std::int32_t>(reinterpret_cast<std::uintptr_t>(frontEnd) + Game::Offset::FrontEndIsHost) != 1;
		}

		using FrontEndFn = void(__fastcall*)(std::uint8_t* frontEnd);
		using WorldSetupFn = std::int32_t(__fastcall*)(void* world);
		FrontEndFn originalWriteOptions = nullptr;
		FrontEndFn originalReadOptions = nullptr;
		WorldSetupFn originalWorldSetup = nullptr;

		// The game writes the lobby options from its settings, then ours are added from this machine's config. On the
		// host, that is what the other players receive. A player who joined keeps what the host sent.
		void __fastcall WriteOptionsDetour(std::uint8_t* frontEnd) {
			originalWriteOptions(frontEnd);
			auto* row = FirstListRow(frontEnd);
			if (row == nullptr || HasJoinedAnotherPlayersGame(frontEnd)) return;
			std::uint8_t flags = WrittenFlag;
			for (std::size_t i = 0; i < Options.size(); ++i) {
				if (local[i]) flags |= Options[i].flag;
			}
			row[Game::CreatureListFreeByte] = flags;
			for (const auto callback : lobbyWriteCallbacks) { callback(frontEnd); }
		}
		// Reading the options back into the game's settings: keep ours as the session's values.
		void __fastcall ReadOptionsDetour(std::uint8_t* frontEnd) {
			const auto* row = FirstListRow(frontEnd);
			const std::uint8_t flags = row != nullptr ? row[Game::CreatureListFreeByte] : 0;
			sessionKnown = (flags & WrittenFlag) != 0;
			if (sessionKnown) {
				for (std::size_t i = 0; i < Options.size(); ++i) { session[i] = (flags & Options[i].flag) != 0; }
				for (const auto callback : lobbyReadCallbacks) { callback(frontEnd); }
			}
			originalReadOptions(frontEnd);
		}
		std::int32_t __fastcall WorldSetupDetour(void* world) {
			const bool multiplayer = Multiplayer();
			std::string text = "Session options:";
			for (std::size_t i = 0; i < Options.size(); ++i) {
				// Options sharing a key are written as the one value of that key.
				if (i > 0 && std::string_view(Options[i].key) == Options[i - 1].key) continue;
				const char* value = Options[i].offValue;
				for (std::size_t same = i; same < Options.size() && std::string_view(Options[same].key) == Options[i].key; ++same) {
					if (SessionOptions::Get(static_cast<SessionOption>(same))) value = Options[same].onValue;
				}
				text += std::string(" ") + Options[i].key + "=" + value;
			}
			Logger::Info(text + (multiplayer ? (sessionKnown ? " (the host's)." : " (the host's were not received; this machine's).") : "."));
			for (const auto callback : levelStartCallbacks) { callback(); }
			return originalWorldSetup(world);
		}

		bool Hook(std::uintptr_t target, void* detour, void** original) {
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
		std::vector<std::uint8_t> WithAddress(std::vector<std::uint8_t> prefix, std::uintptr_t address, std::initializer_list<std::uint8_t> suffix) {
			const auto value = static_cast<std::uint32_t>(address);
			for (int shift = 0; shift < 32; shift += 8) prefix.push_back(static_cast<std::uint8_t>(value >> shift));
			prefix.insert(prefix.end(), suffix);
			return prefix;
		}
		bool installed = false;
	}

	bool SessionOptions::Get(SessionOption option) {
		const auto i = Index(option);
		return UsesSessionValues() ? session[i] : local[i];
	}
	bool SessionOptions::GetLocal(SessionOption option) { return local[Index(option)]; }
	bool SessionOptions::UsesSessionValues() { return installed && Multiplayer() && sessionKnown; }
	void SessionOptions::OnLevelStart(void (*callback)()) { levelStartCallbacks.push_back(callback); }
	void SessionOptions::OnLobbyWrite(void (*callback)(std::uint8_t* frontEnd)) { lobbyWriteCallbacks.push_back(callback); }
	void SessionOptions::OnLobbyRead(void (*callback)(const std::uint8_t* frontEnd)) { lobbyReadCallbacks.push_back(callback); }

	void SessionOptions::Install() {
		for (std::size_t i = 0; i < Options.size(); ++i) {
			const std::string value = Config::GetString(Options[i].key, Options[i].offValue);
			local[i] = value == Options[i].onValue;
			const bool valid = local[i] || value == Options[i].offValue || (Options[i].otherValue != nullptr && value == Options[i].otherValue);
			if (!valid && !(i > 0 && std::string_view(Options[i].key) == Options[i - 1].key)) {
				Logger::Warning(std::string("Session options: ") + Options[i].key + " '" + value + "' in config.yml is not a known value, using " + Options[i].offValue + ".");
			}
		}
		const auto worldSetup = Game::Resolve(Game::Address::WorldSetup);
		const auto writeOptions = Game::Resolve(Game::Address::LobbyWriteOptions);
		const auto readOptions = Game::Resolve(Game::Address::LobbyReadOptions);
		// "sub esp, 0x10; mov eax, [play mode]; push ebx...", "sub esp, 0x24; push ebx; push esi; mov esi, ecx" and
		// "mov eax, [0x75AE97]; push ebx; push esi; mov esi, ecx".
		if (!Memory::Matches(worldSetup, WithAddress({ 0x83, 0xEC, 0x10, 0xA1 }, Game::Resolve(Game::Address::PlayMode), { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9 }))
			|| !Memory::Matches(writeOptions, { 0x83, 0xEC, 0x24, 0x53, 0x56, 0x8B, 0xF1 })
			|| !Memory::Matches(readOptions, WithAddress({ 0xA1 }, Game::Resolve(Game::Address::LobbyOptionsMode), { 0x53, 0x56, 0x8B, 0xF1 }))) {
			Logger::Warning("Session options: unexpected game code, multiplayer games use each machine's own settings.");
			return;
		}
		// The two lobby hooks or neither: one without the other would let players disagree.
		if (!Hook(writeOptions, reinterpret_cast<void*>(&WriteOptionsDetour), reinterpret_cast<void**>(&originalWriteOptions))) {
			Logger::Warning("Session options: could not hook the lobby options, multiplayer games use each machine's own settings.");
			return;
		}
		if (!Hook(readOptions, reinterpret_cast<void*>(&ReadOptionsDetour), reinterpret_cast<void**>(&originalReadOptions))) {
			HookManager::RemoveHook(writeOptions);
			Logger::Warning("Session options: could not hook the lobby options, multiplayer games use each machine's own settings.");
			return;
		}
		if (!Hook(worldSetup, reinterpret_cast<void*>(&WorldSetupDetour), reinterpret_cast<void**>(&originalWorldSetup))) {
			HookManager::RemoveHook(writeOptions);
			HookManager::RemoveHook(readOptions);
			Logger::Warning("Session options: could not hook level setup, multiplayer games use each machine's own settings.");
			return;
		}
		installed = true;
	}
}
