#include "CoopCampaign.h"
#include "CoopHands.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "LevelEditor.h"
#include "Logger.h"
#include "Memory.h"
#include "ModApiHost.h"
#include "SessionOptions.h"
#include "SteamMultiplayer.h"
#include <array>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>
#include <Windows.h>

namespace KeeperForge2 {
	namespace {
		template<typename T> T GameFunction(std::uintptr_t address) { return reinterpret_cast<T>(Game::Resolve(address)); }
		// Game structures are packed, so their fields are read and written byte-wise.
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// ---- What the host chose. Level 0 is off.
		enum Role : std::uint8_t { RoleShared, RoleGood, RoleSecondKeeper, RoleCount };
		struct Choices {
			std::uint8_t level = 0;
			std::array<std::uint8_t, 2> roles{ RoleShared, RoleShared };
		};
		Choices choices;
		// A level started with these choices.
		bool inSession = false;
		constexpr std::uint8_t MainKeeper = Game::FirstKeeperPlayer;
		constexpr std::uint8_t SecondKeeper = Game::FirstKeeperPlayer + 1;
		constexpr std::uint8_t GoodPlayerNumber = Game::GoodPlayer;
		std::uint8_t RolePlayer(std::uint8_t role) {
			return role == RoleGood ? GoodPlayerNumber : role == RoleSecondKeeper ? SecondKeeper : MainKeeper;
		}
		// Started from the skirmish screen: one person on one machine, with the game's local transport. It plays a level the
		// same way, so one person can use and test all of this alone.
		bool soloSession = false;
		bool EveryoneShares() { return choices.roles[0] == RoleShared && (soloSession || choices.roles[1] == RoleShared); }

		std::int32_t& PlayMode() { return *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::PlayMode)); }
		std::uint8_t* World() { return *reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::WorldPointer)); }
		std::uint8_t* Thing(std::uint16_t tag) { return reinterpret_cast<std::uint8_t**>(Game::Resolve(Game::Address::ThingTable))[tag]; }

		// ---- The levels: the game's own table of campaign file names, by level number. 25 is not a level [reference].
		constexpr std::uint8_t LastLevel = 30;
		constexpr std::uint8_t NotALevel = 25;
		const wchar_t* LevelName(std::uint8_t level) {
			if (level == 0 || level > LastLevel || level == NotALevel) return nullptr;
			const auto* name = reinterpret_cast<const wchar_t* const*>(Game::Resolve(Game::Address::CampaignLevelNames))[level];
			return name != nullptr && std::wcslen(name) < Game::LevelNameLength ? name : nullptr;
		}
		std::uint8_t NextLevel(std::uint8_t level, std::int32_t step) {
			for (std::int32_t i = 0; i <= LastLevel; ++i) {
				level = static_cast<std::uint8_t>((level + step + LastLevel + 1) % (LastLevel + 1));
				if (level == 0 || LevelName(level) != nullptr) return level;
			}
			return 0;
		}

		// ---- The campaign's rules on the multiplayer transport. These instructions read the play mode to choose between
		// the two. The rest of the game, the network above all, keeps seeing multiplayer.
		std::int32_t SessionPlayMode() { return soloSession ? Game::PlayModeSkirmish : Game::PlayModeMultiplayer; }
		bool CampaignRules() { return inSession && PlayMode() == SessionPlayMode(); }
		// The first read in level setup sits in the bytes SessionOptions' hook moved, so it cannot be patched. The mode
		// is campaign for that one read, and the next read, a little further on, puts it back.
		bool modeLent = false;
		void CheckSecondKeeper();
		void LendCampaignMode() {
			if (!CampaignRules()) return;
			CheckSecondKeeper();
			PlayMode() = Game::PlayModeCampaign;
			modeLent = true;
		}
		std::int32_t __cdecl RulesMode() {
			if (modeLent) {
				modeLent = false;
				PlayMode() = SessionPlayMode();
			}
			// A level being edited also keeps what its file says: all its keepers, its own settings, none of the lobby's.
			return CampaignRules() || LevelEditor::InSession() ? Game::PlayModeCampaign : PlayMode();
		}
		// Whether one player's win or loss decides the others'. Sharing one keeper there are no others. With parts apart,
		// the game's multiplayer rule is what ends the level for the machine that did not win.
		std::int32_t __cdecl OutcomeMode() { return CampaignRules() && EveryoneShares() ? 0 : PlayMode(); }
		// Winning and losing as in the campaign, the victory cinematic included, while everyone shares one keeper. With
		// parts apart a win is someone's loss, so that stays multiplayer's.
		std::int32_t __cdecl EndingMode() { return CampaignRules() && EveryoneShares() ? Game::PlayModeCampaign : PlayMode(); }
		// The campaign's progress is the host's. A joining player's own stays as it was.
		bool hostMachine = false;
		std::int32_t __cdecl ProgressMode() { return hostMachine && !soloSession ? EndingMode() : PlayMode(); }

		// In place of "mov eax, [play mode]".
		__declspec(naked) void ModeIntoEax() {
			__asm {
				pushfd
				pushad
				call RulesMode
				mov dword ptr [esp + 28], eax
				popad
				popfd
				ret
			}
		}
		__declspec(naked) void EndingModeIntoEax() {
			__asm {
				pushfd
				pushad
				call EndingMode
				mov dword ptr [esp + 28], eax
				popad
				popfd
				ret
			}
		}
		__declspec(naked) void ProgressModeIntoEax() {
			__asm {
				pushfd
				pushad
				call ProgressMode
				mov dword ptr [esp + 28], eax
				popad
				popfd
				ret
			}
		}
		// In place of "mov edi, [play mode]".
		__declspec(naked) void OutcomeIntoEdi() {
			__asm {
				pushfd
				pushad
				call OutcomeMode
				mov dword ptr [esp], eax
				popad
				popfd
				ret
			}
		}
		// In place of "cmp [play mode], 1".
		__declspec(naked) void CompareWithCampaign() {
			__asm {
				push eax
				pushfd
				pushad
				call RulesMode
				mov dword ptr [esp + 28], eax
				popad
				popfd
				cmp eax, 1
				pop eax
				ret
			}
		}
		std::vector<std::uint8_t> ModeOperand(std::vector<std::uint8_t> bytes, std::initializer_list<std::uint8_t> suffix = {}) {
			const auto mode = static_cast<std::uint32_t>(Game::Resolve(Game::Address::PlayMode));
			for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(mode >> shift));
			bytes.insert(bytes.end(), suffix);
			return bytes;
		}
		struct ModeRead {
			std::uintptr_t address;
			std::vector<std::uint8_t> expected;
			void (*stub)();
		};
		bool PatchModeReads() {
			const auto eaxRead = ModeOperand({ 0xA1 });
			const auto ediRead = ModeOperand({ 0x8B, 0x3D });
			const auto compare = ModeOperand({ 0x83, 0x3D }, { 0x01 });
			const std::vector<ModeRead> reads = {
				// Level setup: fog, walls, gold, time limit, what is available, computer players and alliances.
				{ Game::Address::WorldSetupModeRead1, eaxRead, &ModeIntoEax },
				{ Game::Address::WorldSetupModeRead2, eaxRead, &ModeIntoEax },
				{ Game::Address::WorldSetupModeRead3, eaxRead, &ModeIntoEax },
				{ Game::Address::WorldSetupModeRead4, eaxRead, &ModeIntoEax },
				{ Game::Address::WorldSetupModeRead5, eaxRead, &ModeIntoEax },
				{ Game::Address::WorldSetupModeRead6, eaxRead, &ModeIntoEax },
				// The time limit's end, which defeats every keeper in multiplayer.
				{ Game::Address::TimeLimitModeRead, eaxRead, &ModeIntoEax },
				// A destroyed heart, and whether one keeper left has won.
				{ Game::Address::HeartLostModeCompare1, compare, &CompareWithCampaign },
				{ Game::Address::HeartLostModeCompare2, compare, &CompareWithCampaign },
				{ Game::Address::PlayerStatusModeRead, ediRead, &OutcomeIntoEdi },
				// Writing a level's result, and starting the victory cinematic.
				{ Game::Address::LevelLostModeRead, eaxRead, &ProgressModeIntoEax },
				{ Game::Address::LevelWonModeRead, eaxRead, &ProgressModeIntoEax },
				{ Game::Address::LevelEndingModeRead, eaxRead, &EndingModeIntoEax },
				{ Game::Address::GemEndingModeRead, eaxRead, &EndingModeIntoEax },
				{ Game::Address::OtherEndingModeRead, eaxRead, &EndingModeIntoEax },
			};
			for (const auto& read : reads) {
				if (!Memory::Matches(Game::Resolve(read.address), read.expected)) return false;
			}
			for (const auto& read : reads) {
				const auto site = Game::Resolve(read.address);
				if (!Memory::WriteCall(site, reinterpret_cast<const void*>(read.stub))) return false;
				if (read.expected.size() > 5) Memory::Fill(site + 5, 0x90, read.expected.size() - 5);
			}
			return true;
		}

		// ---- Starting the level. The game component's main routine runs once for the menu (its 3D front end flag set) and
		// once for each game: there, every machine has the choices, and nothing of the level is loaded yet. The level, and
		// which player this machine is, replace what the lobby's map would have used.
		// The result of a level is written under the current level's number, so that is the chosen level's while the
		// session lasts, and back after it: a session must not move the host's own campaign.
		std::uint8_t keptLevelNumber = 0;
		std::int32_t keptSecretLevel = 0;
		bool levelNumberKept = false;
		constexpr std::uint8_t LastMainLevel = 24;
		void RestoreLevelNumber() {
			if (!levelNumberKept) return;
			levelNumberKept = false;
			*reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::PlayerLevelNumber)) = keptLevelNumber;
			*reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::PlayerSecretLevel)) = keptSecretLevel;
		}
		void UseLevelNumber(std::uint8_t level) {
			auto& number = *reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::PlayerLevelNumber));
			auto& secret = *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::PlayerSecretLevel));
			keptLevelNumber = number;
			keptSecretLevel = secret;
			levelNumberKept = true;
			if (level <= LastMainLevel) {
				number = level;
				secret = 0;
			}
			else secret = level - NotALevel;
		}
		bool secondKeeperUsable = true;
		std::array<std::int32_t, 8> seenStatus{};
		ULONGLONG lastEndingLog = 0;
		void ResetSharedCamera();
		void StartSession() {
			const auto* name = LevelName(choices.level);
			const auto mode = PlayMode();
			if (name == nullptr || (mode != Game::PlayModeMultiplayer && mode != Game::PlayModeSkirmish)) {
				Logger::Debug("Co-op campaign: this game is not a co-op level (level " + std::to_string(choices.level) + ", play mode " + std::to_string(mode) + ").");
				return;
			}
			if (Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameConfigHasSaveFile)) != 0) return;
			soloSession = mode == Game::PlayModeSkirmish;
			if (soloSession) hostMachine = true;
			if (hostMachine && !soloSession && EveryoneShares()) UseLevelNumber(choices.level);
			secondKeeperUsable = true;
			seenStatus = {};
			ResetSharedCamera();
			CoopHands::Reset();
			std::wcsncpy(reinterpret_cast<wchar_t*>(Game::Resolve(Game::Address::GameConfigLevelName)), name, Game::LevelNameLength);
			const std::uint8_t role = choices.roles[hostMachine ? 0 : 1];
			*reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::LocalPlayerNumber)) = RolePlayer(role);
			// The lobby's lists of creatures, rooms and the rest would overrule what the level makes available.
			for (const auto count : { Game::Address::GameConfigCreatureCount, Game::Address::GameConfigRoomCount, Game::Address::GameConfigDoorCount,
				Game::Address::GameConfigTrapCount, Game::Address::GameConfigSpellCount }) {
				*reinterpret_cast<std::uint32_t*>(Game::Resolve(count)) = 0;
			}
			inSession = true;
			Logger::Info("Co-op campaign: level " + std::to_string(choices.level) + ", this machine plays player " + std::to_string(RolePlayer(role)) + ".");
		}
		using EnterScreenFn = std::int32_t(__fastcall*)(void* resources, void* unused, std::int32_t* hardware);
		// In place of the call to the screen setup at the start of that routine.
		std::int32_t __fastcall BeforeGameComponent(void* resources, void* unused, std::int32_t* hardware) {
			inSession = false;
			modeLent = false;
			RestoreLevelNumber();
			const bool frontEnd = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameConfigFrontEnd3d)) != 0;
			const bool noWorld = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::GameConfigNoGame)) != 0;
			const bool playing = !frontEnd && !noWorld;
			const bool editing = LevelEditor::OnGameComponentStart(playing);
			if (playing && !editing && choices.level != 0) StartSession();
			return GameFunction<EnterScreenFn>(Game::Address::EnterGameScreen)(resources, unused, hardware);
		}

		// ---- A level without the player a machine asked for (no second keeper): it shares the main keeper instead.
		using GetTagFn = std::uint16_t(__fastcall*)(std::uint8_t* world);
		GetTagFn originalGetLocalTag = nullptr;
		std::uint16_t __fastcall GetLocalTagDetour(std::uint8_t* world) {
			const auto tag = originalGetLocalTag(world);
			if (tag != 0 || !inSession) return tag;
			for (const auto* player = Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); player != nullptr; player = Thing(Load<std::uint16_t>(player + Game::Offset::PlayerNext))) {
				if (player[Game::Offset::PlayerNumber] != MainKeeper) continue;
				const auto main = Load<std::uint16_t>(player + Game::Offset::PlayerTag);
				Store<std::uint16_t>(world + Game::Offset::WorldLocalPlayerTag, main);
				Logger::Warning("Co-op campaign: this level has no second keeper, so this machine shares the main keeper.");
				return main;
			}
			return tag;
		}

		// ---- A keeper a person plays has no computer player, whatever the level says.
		using InitAiFn = void(__fastcall*)(std::uint8_t* player);
		using SetAiLevelFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, std::int32_t level);
		InitAiFn originalInitAi = nullptr;
		SetAiLevelFn originalSetAiLevel = nullptr;
		void __fastcall InitAiDetour(std::uint8_t* player) {
			if (!CoopCampaign::HumanPlays(player[Game::Offset::PlayerNumber])) originalInitAi(player);
		}
		std::int32_t __fastcall SetAiLevelDetour(std::uint8_t* player, void* unused, std::int32_t level) {
			return CoopCampaign::HumanPlays(player[Game::Offset::PlayerNumber]) ? 1 : originalSetAiLevel(player, unused, level);
		}
		// The heart's place starts at the map's corner (1,1) until the level's heart sets it.
		constexpr std::uint16_t NoHeartTile = 1;
		// Whether the level really has a second dungeon, the same on every machine. Decided once the level's things exist
		// and before its first turn: without one, whoever chose it shares the main keeper.
		void CheckSecondKeeper() {
			auto* world = World();
			if (!inSession || world == nullptr) return;
			if (choices.roles[0] != RoleSecondKeeper && choices.roles[1] != RoleSecondKeeper) return;
			std::uint8_t* second = nullptr;
			std::uint8_t* main = nullptr;
			for (auto* player = Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); player != nullptr; player = Thing(Load<std::uint16_t>(player + Game::Offset::PlayerNext))) {
				if (player[Game::Offset::PlayerNumber] == SecondKeeper) second = player;
				if (player[Game::Offset::PlayerNumber] == MainKeeper) main = player;
			}
			if (second != nullptr && !(Load<std::uint16_t>(second + Game::Offset::PlayerHeartX) == NoHeartTile && Load<std::uint16_t>(second + Game::Offset::PlayerHeartY) == NoHeartTile)) return;
			secondKeeperUsable = false;
			Logger::Warning("Co-op campaign: this level has no second dungeon heart, so the second keeper's player shares the main keeper.");
			if (main == nullptr || choices.roles[hostMachine ? 0 : 1] != RoleSecondKeeper) return;
			const auto tag = Load<std::uint16_t>(main + Game::Offset::PlayerTag);
			Store<std::uint16_t>(world + Game::Offset::WorldLocalPlayerTag, tag);
			auto* session = *reinterpret_cast<std::uint8_t**>(world + Game::Offset::WorldSession);
			auto* playerInterface = session != nullptr ? *reinterpret_cast<std::uint8_t**>(session + Game::Offset::SessionInterface) : nullptr;
			if (playerInterface != nullptr) Store<std::uint16_t>(playerInterface + Game::Offset::InterfacePlayerTag, tag);
		}
		void StopAiOfHumans(std::uint8_t* world) {
			for (auto* player = Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); player != nullptr; player = Thing(Load<std::uint16_t>(player + Game::Offset::PlayerNext))) {
				if (CoopCampaign::HumanPlays(player[Game::Offset::PlayerNumber])) player[Game::Offset::PlayerAiActive] = 0;
			}
		}

		// ---- Scripted camera paths. A level's script waits for the camera, and each machine's camera runs on its own
		// clock, so one machine's script would go on before the other's and the game would go out of step. While in a
		// session these waits count shared game turns, which every machine agrees on; the picture is unchanged.
		struct SharedCamera {
			const std::uint8_t* camera = nullptr;
			bool pathRunning = false;
			std::uint32_t pathEndTurn = 0;
			std::uint32_t moveTurns = 0;
			std::uint32_t turnTurns = 0;
			std::uint32_t lastTurn = 0;
			bool turnKnown = false;
			bool paused = false;
		};
		SharedCamera shared;
		bool RestoreCameraSave(std::uint32_t turn);
		constexpr std::uint32_t CameraPathMode = 18;
		constexpr std::int32_t WaitForCameraTrigger = 72;
		constexpr std::int32_t ActionMoveCamera = 4;
		constexpr std::uint32_t CameraMoveMs = 500;
		constexpr std::uint32_t PathSamplesPerSecond = 30;
		std::uint32_t GameTurn() { return GameFunction<std::uint32_t(__fastcall*)(void*)>(Game::Address::WorldGameTurn)(World()); }
		std::uint32_t TurnsPerSecond() { return Load<std::uint32_t>(*reinterpret_cast<const std::uint8_t* const*>(World() + Game::Offset::WorldSession) + Game::Offset::SessionTurnsPerSecond); }
		std::uint32_t MsToTurns(std::uint32_t ms) { return static_cast<std::uint32_t>((static_cast<std::uint64_t>(ms) * TurnsPerSecond() + 999) / 1000); }
		// A path holds the movement's and rotation's time still while it runs.
		void CountTurns(std::uint32_t turn) {
			if (shared.turnKnown && turn < shared.lastTurn && !RestoreCameraSave(turn)) shared = {};
			if (shared.turnKnown && !shared.paused) {
				const auto passed = turn - shared.lastTurn;
				shared.moveTurns = shared.moveTurns > passed ? shared.moveTurns - passed : 0;
				shared.turnTurns = shared.turnTurns > passed ? shared.turnTurns - passed : 0;
			}
			shared.lastTurn = turn;
			shared.turnKnown = true;
			shared.paused = shared.pathRunning;
		}
		std::uint32_t CameraMode(const std::uint8_t* camera) { return Load<std::uint32_t>(camera + Game::Offset::CameraMode); }

		// The game keeps two rolling saves, and goes back to one when a machine falls out of step. The waits go back with
		// it, or one that ended since would be missing on the machine that reloaded.
		struct CameraSave { SharedCamera state; std::uint32_t turn = 0; bool valid = false; };
		std::array<CameraSave, 2> cameraSaves{};
		std::size_t nextCameraSave = 0;
		using SaveWorldFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, const char* name);
		SaveWorldFn originalSaveWorld = nullptr;
		std::int32_t __fastcall SaveWorldDetour(std::uint8_t* world, void* unused, const char* name) {
			const auto started = GetTickCount64();
			const std::int32_t result = originalSaveWorld(world, unused, name);
			if (PlayMode() == Game::PlayModeMultiplayer) SteamMultiplayer::OnWorldSaved(GetTickCount64() - started);
			if (result != 0 && CampaignRules()) {
				cameraSaves[nextCameraSave] = { shared, GameTurn(), true };
				nextCameraSave = (nextCameraSave + 1) % cameraSaves.size();
			}
			return result;
		}
		void ResetSharedCamera() {
			shared = {};
			cameraSaves = {};
			nextCameraSave = 0;
		}
		bool RestoreCameraSave(std::uint32_t turn) {
			for (const auto& save : cameraSaves) {
				if (!save.valid || save.turn != turn) continue;
				shared = save.state;
				return true;
			}
			return false;
		}

		using LoadPathFn = char(__fastcall*)(std::uint8_t* camera, void* unused, std::uint32_t path, std::uint32_t x, std::uint32_t y, std::int32_t flags);
		using FinishPathFn = std::int32_t(__fastcall*)(std::uint8_t* camera);
		using FinishTweenFn = void(__fastcall*)(std::uint8_t* camera, void* unused, std::int32_t immediate);
		LoadPathFn originalLoadPath = nullptr;
		FinishPathFn originalFinishPath = nullptr;
		std::uintptr_t loadPathBody = 0;
		// The game's load, past its checks of what this machine's camera is doing: another machine's camera may be free,
		// and they all have to take the path.
		__declspec(naked) char __fastcall LoadPathWhateverTheCamera(std::uint8_t*, void*, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) {
			__asm {
				sub esp, 0x10C
				push ebx
				push esi
				mov esi, ecx
				push edi
				xor ebx, ebx
				jmp dword ptr [loadPathBody]
			}
		}
		char __fastcall LoadPathDetour(std::uint8_t* camera, void* unused, std::uint32_t path, std::uint32_t x, std::uint32_t y, std::int32_t flags) {
			if (!CampaignRules() || World() == nullptr) return originalLoadPath(camera, unused, path, x, y, flags);
			if (shared.camera != camera) shared = {};
			const auto turn = GameTurn();
			CountTurns(turn);
			const auto before = CameraMode(camera);
			char result = 0;
			if (shared.pathRunning || before == CameraPathMode) result = originalLoadPath(camera, unused, path, x, y, flags);
			else {
				// A local camera move is finished first, so the path ends in the right mode. The path's entry clears the
				// camera's target, which only the path's end needs back.
				if (Load<std::uint32_t>(camera + Game::Offset::CameraTweenEnd) != 0) GameFunction<FinishTweenFn>(Game::Address::CameraFinishTween)(camera, nullptr, 0);
				const auto target = Load<std::uint32_t>(camera + Game::Offset::CameraPathTarget);
				result = LoadPathWhateverTheCamera(camera, nullptr, path, x, y, flags);
				if (CameraMode(camera) == CameraPathMode) Store<std::uint32_t>(camera + Game::Offset::CameraPathTarget, target);
			}
			Logger::Debug("Co-op campaign: camera path " + std::to_string(path) + " at turn " + std::to_string(turn) + ", camera mode " + std::to_string(before) + " to " + std::to_string(CameraMode(camera)) + ".");
			if (!shared.pathRunning && before != CameraPathMode && CameraMode(camera) == CameraPathMode) {
				const auto points = Load<std::int32_t>(camera + Game::Offset::CameraPathPoints);
				const auto first = Load<std::int32_t>(camera + Game::Offset::CameraPathPoint);
				if (first >= 0 && points > first + 1) {
					// A path starts halfway through its first sample.
					const std::uint64_t halfSamples = 2ull * (points - 1 - first) - 1;
					shared.pathEndTurn = turn + static_cast<std::uint32_t>((halfSamples * TurnsPerSecond() + 2 * PathSamplesPerSecond - 1) / (2 * PathSamplesPerSecond));
					shared.pathRunning = true;
					shared.camera = camera;
				}
			}
			CountTurns(turn);
			return result;
		}
		// Drawing finishes a path when its own clock runs out, and a key can skip it. In a session only the shared turn
		// ends it: until then the picture holds on the last sample.
		std::int32_t __fastcall FinishPathDetour(std::uint8_t* camera) {
			if (!CampaignRules() || !shared.pathRunning || shared.camera != camera || CameraMode(camera) != CameraPathMode) return originalFinishPath(camera);
			const auto points = Load<std::int32_t>(camera + Game::Offset::CameraPathPoints);
			if (points >= 2 && Load<std::int32_t>(camera + Game::Offset::CameraPathPoint) >= points - 1) {
				Store<std::int32_t>(camera + Game::Offset::CameraPathPoint, points - 2);
				Store<std::uint32_t>(camera + Game::Offset::CameraPathBlend, 0x3F800000);
			}
			return 0;
		}

		using WorldActionFn = std::int32_t(__fastcall*)(std::uint8_t* world, void* unused, const std::uint8_t* action);
		WorldActionFn originalWorldAction = nullptr;
		std::int32_t __fastcall WorldActionDetour(std::uint8_t* world, void* unused, const std::uint8_t* action) {
			if (CampaignRules() && Load<std::int32_t>(action + Game::Offset::ActionKind) == ActionMoveCamera) {
				CountTurns(GameTurn());
				shared.moveTurns = MsToTurns(CameraMoveMs);
				const auto rotationMs = Load<std::uint32_t>(action + Game::Offset::ActionData3);
				if (rotationMs != 0) shared.turnTurns = MsToTurns(rotationMs);
			}
			return originalWorldAction(world, unused, action);
		}
		using CheckTriggerFn = std::int32_t(__fastcall*)(std::uint8_t* player, void* unused, const std::uint8_t* trigger);
		CheckTriggerFn originalCheckTrigger = nullptr;
		std::int32_t __fastcall CheckTriggerDetour(std::uint8_t* player, void* unused, const std::uint8_t* trigger) {
			const std::int32_t result = originalCheckTrigger(player, unused, trigger);
			if (!CampaignRules() || trigger[Game::Offset::TriggerType] != WaitForCameraTrigger) return result;
			return !shared.paused && shared.moveTurns == 0 && shared.turnTurns == 0 ? 1 : 0;
		}

		// ---- The victory cinematic's creature waits for the camera on each machine's clock too. Two states of its walk
		// take a shared wait instead, then go on past their own check of the camera.
		using StateFn = std::int32_t(__fastcall*)(std::uint8_t* state);
		StateFn originalGemPathWait = nullptr;
		StateFn originalGemMoveWait = nullptr;
		std::uintptr_t gemPathReady = 0;
		std::uintptr_t gemMoveReady = 0;
		std::uintptr_t gemPathHandler = 0x0064AFE8;
		std::uintptr_t gemMoveHandler = 0x0064B010;
#pragma warning(push)
#pragma warning(disable: 4733)
		__declspec(naked) std::int32_t __fastcall GemPathReady(std::uint8_t*) {
			__asm {
				mov eax, dword ptr fs:[0]
				push -1
				push dword ptr [gemPathHandler]
				push eax
				mov dword ptr fs:[0], esp
				sub esp, 0x14
				push esi
				mov esi, ecx
				push edi
				jmp dword ptr [gemPathReady]
			}
		}
		__declspec(naked) std::int32_t __fastcall GemMoveReady(std::uint8_t*) {
			__asm {
				mov eax, dword ptr fs:[0]
				push -1
				push dword ptr [gemMoveHandler]
				push eax
				mov dword ptr fs:[0], esp
				sub esp, 0x1C
				push ebx
				push ebp
				push esi
				mov esi, ecx
				push edi
				jmp dword ptr [gemMoveReady]
			}
		}
#pragma warning(pop)
		// A state still waiting does not age, as the game's own wait.
		std::int32_t HoldState(std::uint8_t* state) {
			Store<std::int32_t>(state + Game::Offset::StateAge, Load<std::int32_t>(state + Game::Offset::StateAge) - 1);
			return 1;
		}
		bool SharedEnding() { return CampaignRules() && EveryoneShares(); }
		std::int32_t __fastcall GemPathWaitDetour(std::uint8_t* state) {
			if (!SharedEnding()) return originalGemPathWait(state);
			return shared.pathRunning ? HoldState(state) : GemPathReady(state);
		}
		std::int32_t __fastcall GemMoveWaitDetour(std::uint8_t* state) {
			if (!SharedEnding()) return originalGemMoveWait(state);
			return shared.moveTurns != 0 ? HoldState(state) : GemMoveReady(state);
		}

		// ---- How a level ends for the Good player. The game only gives keepers a result, so a person playing the heroes
		// would play on forever once the level was decided. Worked out from the world, so every machine does it on the same
		// turn: the main keeper's result decides, the other way round.
		constexpr std::int32_t StatusWon = 2;
		constexpr std::int32_t StatusLost = 3;
		constexpr ULONGLONG EndingLogMs = 10000;
		void SettleGoodPlayer(std::uint8_t* world) {
			std::uint8_t* good = nullptr;
			std::uint8_t* main = nullptr;
			bool decided = false;
			for (auto* player = Thing(Load<std::uint16_t>(world + Game::Offset::WorldPlayers)); player != nullptr; player = Thing(Load<std::uint16_t>(player + Game::Offset::PlayerNext))) {
				const auto number = player[Game::Offset::PlayerNumber];
				const auto status = Load<std::int32_t>(player + Game::Offset::PlayerStatus);
				if (status != 0) decided = true;
				if (number < seenStatus.size() && status != seenStatus[number]) {
					seenStatus[number] = status;
					Logger::Debug("Co-op campaign: player " + std::to_string(number) + " has result " + std::to_string(status) + " at turn " + std::to_string(GameTurn()) + ".");
				}
				if (number == GoodPlayerNumber) good = player;
				if (number == MainKeeper) main = player;
			}
			if (good != nullptr && main != nullptr && CoopCampaign::HumanPlays(GoodPlayerNumber) && Load<std::int32_t>(good + Game::Offset::PlayerStatus) == 0) {
				const auto mainStatus = Load<std::int32_t>(main + Game::Offset::PlayerStatus);
				if (mainStatus != 0) GameFunction<void(__fastcall*)(std::uint8_t*, void*, std::int32_t)>(Game::Address::PlayerSetStatus)(good, nullptr, mainStatus == StatusWon ? StatusLost : StatusWon);
			}
			// For the log only: what a level's end is waiting for. If these lines stop, the world has stopped.
			const auto now = GetTickCount64();
			if ((decided || shared.pathRunning) && now - lastEndingLog >= EndingLogMs) {
				lastEndingLog = now;
				Logger::Debug("Co-op campaign: turn " + std::to_string(GameTurn()) + ", path " + (shared.pathRunning ? "running to turn " + std::to_string(shared.pathEndTurn) : std::string("not running")) + ", camera mode " + (shared.camera != nullptr ? std::to_string(CameraMode(shared.camera)) : std::string("unknown")) + ", camera waits " + std::to_string(shared.moveTurns) + " and " + std::to_string(shared.turnTurns) + ".");
			}
		}

		using WorldTickFn = std::uint32_t(__fastcall*)(std::uint8_t* world, void* unused, const std::uint8_t* actions);
		WorldTickFn originalWorldTick = nullptr;
		std::uint32_t __fastcall WorldTickDetour(std::uint8_t* world, void* unused, const std::uint8_t* actions) {
			if (LevelEditor::InSession()) {
				const auto* kept = LevelEditor::BeforeWorldTick(world, actions);
				const auto held = originalWorldTick(world, unused, kept);
				LevelEditor::AfterWorldTick(world);
				return held;
			}
			// Mods' game turn events (ModApi.h), on every machine at the same turn.
			ModApiHost::BeforeGameTurn();
			if (LevelEditor::Testing()) {
				const auto result = originalWorldTick(world, unused, actions);
				LevelEditor::AfterTestTick(world);
				return result;
			}
			if (PlayMode() == Game::PlayModeMultiplayer) SteamMultiplayer::OnGameTurn();
			if (!CampaignRules()) {
				const auto result = originalWorldTick(world, unused, actions);
				LevelEditor::AfterPlayTick();
				return result;
			}
			{
				const auto turn = GameTurn();
				if (turn <= 1) StopAiOfHumans(world);
				CountTurns(turn);
				if (shared.pathRunning && turn >= shared.pathEndTurn) {
					shared.pathRunning = false;
					// Past FinishPathDetour's hold, so the game does its own ending: the mode it saved, the camera's clean-up.
					if (CameraMode(shared.camera) == CameraPathMode) originalFinishPath(const_cast<std::uint8_t*>(shared.camera));
				}
				CountTurns(turn);
			}
			CoopHands::BeforeWorldTick(world, actions);
			const auto result = originalWorldTick(world, unused, actions);
			CoopHands::AfterWorldTick(world);
			SettleGoodPlayer(world);
			LevelEditor::AfterPlayTick();
			return result;
		}

		// ---- The lobby. The host sends its choices as it draws them, so the others can read them. The list the game sends
		// at the start (SessionOptions) has the last word: every machine reads the same one.
		constexpr std::uint8_t LobbyMessageId = 0xD0;
		constexpr std::uint8_t MessageMark[2] = { 'K', 'F' };
		constexpr ULONGLONG LobbySendMs = 500;
		constexpr ULONGLONG LobbyGoneMs = 1000;
		ULONGLONG lastSend = 0;
		ULONGLONG lastLobbyFrame = 0;
		constexpr std::uint32_t BroadcastTarget = 0xFFFF;
		constexpr std::uint8_t MultiplayerMenuState = 0x10;
		constexpr std::uint8_t SkirmishMenuState = 7;
		bool IsHost(const std::uint8_t* frontEnd) { return Load<std::int32_t>(frontEnd + Game::Offset::FrontEndIsHost) == 1; }
		void SendChoices() {
			const std::uint8_t message[] = { LobbyMessageId, MessageMark[0], MessageMark[1], choices.level, choices.roles[0], choices.roles[1] };
			using SendFn = std::int32_t(__fastcall*)(void* network, void* unused, const void* data, std::uint32_t size, std::uint32_t target);
			GameFunction<SendFn>(Game::Address::NetworkSendData)(reinterpret_cast<void*>(Game::Resolve(Game::Address::NetworkObject)), nullptr, message, sizeof(message), BroadcastTarget);
		}
		// Each frame in the lobby, from the first of our buttons.
		void LobbyFrame(const std::uint8_t* frontEnd) {
			const auto now = GetTickCount64();
			// A new lobby: a joining player starts from nothing until the host says, a host keeps what it set last.
			if (now - lastLobbyFrame > LobbyGoneMs && !IsHost(frontEnd)) choices = {};
			lastLobbyFrame = now;
			hostMachine = IsHost(frontEnd);
			if (!hostMachine) return;
			if (now - lastSend >= LobbySendMs) {
				lastSend = now;
				SendChoices();
			}
		}
		// In the last free byte of the first rows of the creature list: the level, and each part with a mark, as zeros
		// also mean a list that was never written.
		constexpr std::uint32_t ChoiceRows = 3;
		constexpr std::size_t ChoiceByte = Game::CreatureListFreeByte + 2;
		constexpr std::uint8_t ChoiceWritten = 0x80;
		std::uint8_t* ChoiceRow(const std::uint8_t* frontEnd, std::uint32_t row) {
			if (Memory::Read<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frontEnd) + Game::Offset::LobbyCreatureCount) < ChoiceRows) return nullptr;
			return *reinterpret_cast<std::uint8_t* const*>(frontEnd + Game::Offset::LobbyCreatureList) + row * Game::CreatureListEntrySize;
		}
		void WriteChoicesToList(std::uint8_t* frontEnd) {
			if (ChoiceRow(frontEnd, 0) == nullptr) return;
			ChoiceRow(frontEnd, 0)[ChoiceByte] = choices.level;
			ChoiceRow(frontEnd, 1)[ChoiceByte] = static_cast<std::uint8_t>(choices.roles[0] | ChoiceWritten);
			ChoiceRow(frontEnd, 2)[ChoiceByte] = static_cast<std::uint8_t>(choices.roles[1] | ChoiceWritten);
		}
		void ReadChoicesFromList(const std::uint8_t* frontEnd) {
			// The game reads the list back when a game starts, just before the host sends it. The list was written when the
			// screen opened, so the choices made since go in now: on the machine that decides, they are the truth.
			if (IsHost(frontEnd) || frontEnd[Game::Offset::FrontEndMenuState] == SkirmishMenuState) {
				hostMachine = true;
				WriteChoicesToList(const_cast<std::uint8_t*>(frontEnd));
				return;
			}
			hostMachine = false;
			if (ChoiceRow(frontEnd, 0) == nullptr || (ChoiceRow(frontEnd, 1)[ChoiceByte] & ChoiceWritten) == 0) return;
			const auto role = [](std::uint8_t value) { const auto part = static_cast<std::uint8_t>(value & ~ChoiceWritten); return part < RoleCount ? part : static_cast<std::uint8_t>(RoleShared); };
			choices.level = LevelName(ChoiceRow(frontEnd, 0)[ChoiceByte]) != nullptr ? ChoiceRow(frontEnd, 0)[ChoiceByte] : 0;
			choices.roles = { role(ChoiceRow(frontEnd, 1)[ChoiceByte]), role(ChoiceRow(frontEnd, 2)[ChoiceByte]) };
		}

		// Our lobby rows: text the host can click (left: next, right: back). They share a space left free in the lobby.
		constexpr std::int32_t LevelButtonId = 9100;
		constexpr std::int32_t HostRoleButtonId = 9101;
		constexpr std::int32_t PartnerRoleButtonId = 9102;
		constexpr std::int32_t AddComputerButtonId = 528;
		constexpr std::int32_t ButtonListEnd = -1;
		constexpr std::size_t MaxButtons = 64;
		struct RowPlace { std::int16_t x, y, width, height; };
		// Beside Swap Keeper, and under Game Settings up to the Exit button.
		constexpr RowPlace LevelPlace{ 640, 900, 752, 116 };
		constexpr RowPlace HostRolePlace{ 1480, 1612, 620, 76 };
		constexpr RowPlace PartnerRolePlace{ 1480, 1696, 620, 76 };
		// On the skirmish screen: above Kick Player, and under its Swap to Good Player.
		constexpr RowPlace SkirmishLevelPlace{ 64, 900, 1360, 116 };
		constexpr RowPlace SkirmishRolePlace{ 64, 1612, 1360, 76 };
		std::vector<std::uint8_t> buttonList;
		std::vector<std::uint8_t> skirmishButtonList;

		void DrawRow(std::uint8_t* button, std::uint8_t* frontEnd, const wchar_t* text) {
			using ButtonRectFn = std::int32_t*(__fastcall*)(void*, void*, std::int32_t*);
			using BoxToScreenFn = std::int32_t*(__fastcall*)(void*, void*, std::int32_t*, const std::int32_t*);
			using TextDrawerFn = void(__fastcall*)(void*, void*);
			using TextAlignFn = std::int32_t*(__fastcall*)(void*, void*, std::int32_t*, std::int32_t);
			using DrawTextFn = std::int32_t*(__fastcall*)(void*, void*, std::int32_t*, std::int32_t*, const void*, void*, std::int32_t);
			using ConvertTextFn = void(__cdecl*)(const wchar_t*, void*, std::int32_t);
			std::int32_t layoutBox[4]{};
			std::int32_t box[4]{};
			GameFunction<ButtonRectFn>(Game::Address::GuiButtonRect)(button, nullptr, layoutBox);
			GameFunction<BoxToScreenFn>(Game::Address::GuiBoxToScreen)(frontEnd + Game::Offset::MenuGuiManager, nullptr, box, layoutBox);
			alignas(16) std::uint8_t drawer[0x200]{};
			alignas(4) std::uint8_t converted[0x100]{};
			std::int32_t status = 0;
			GameFunction<TextDrawerFn>(Game::Address::TextDrawerConstruct)(drawer, nullptr);
			GameFunction<TextAlignFn>(Game::Address::TextDrawerAlignHorizontal)(drawer, nullptr, &status, 0);
			GameFunction<TextAlignFn>(Game::Address::TextDrawerAlignVertical)(drawer, nullptr, &status, 2);
			GameFunction<ConvertTextFn>(Game::Address::ConvertText)(text, converted, 0x40);
			GameFunction<DrawTextFn>(Game::Address::TextDrawerDraw)(drawer, nullptr, &status, box, converted, reinterpret_cast<void*>(Game::Resolve(Game::Address::VersionFont)), 0);
			GameFunction<TextDrawerFn>(Game::Address::TextDrawerDestruct)(drawer, nullptr);
		}
		const wchar_t* RoleName(std::uint8_t role) { return role == RoleGood ? L"Good player" : role == RoleSecondKeeper ? L"Second keeper" : L"Shared keeper"; }
		bool InSkirmish(const std::uint8_t* frontEnd) { return frontEnd != nullptr && frontEnd[Game::Offset::FrontEndMenuState] == SkirmishMenuState; }
		bool InLobby(const std::uint8_t* frontEnd) { return frontEnd != nullptr && (frontEnd[Game::Offset::FrontEndMenuState] == MultiplayerMenuState || InSkirmish(frontEnd)); }
		char __cdecl RenderLevelRow(std::uint8_t* button, std::uint8_t* frontEnd) {
			if (!InLobby(frontEnd)) return 0;
			if (InSkirmish(frontEnd)) hostMachine = true;
			else LobbyFrame(frontEnd);
			wchar_t text[64]{};
			const auto* name = LevelName(choices.level);
			std::swprintf(text, std::size(text), L"Co-op campaign: %ls", name != nullptr ? name : L"off");
			DrawRow(button, frontEnd, text);
			return 0;
		}
		char RenderRoleRow(std::uint8_t* button, std::uint8_t* frontEnd, const wchar_t* who, std::uint8_t role) {
			// Only with a level chosen: the row means nothing in an ordinary game.
			if (!InLobby(frontEnd) || choices.level == 0) return 0;
			wchar_t text[64]{};
			std::swprintf(text, std::size(text), L"%ls: %ls", who, RoleName(role));
			DrawRow(button, frontEnd, text);
			return 0;
		}
		char __cdecl RenderHostRoleRow(std::uint8_t* button, std::uint8_t* frontEnd) { return RenderRoleRow(button, frontEnd, InSkirmish(frontEnd) ? L"You" : L"Host", choices.roles[0]); }
		char __cdecl RenderPartnerRoleRow(std::uint8_t* button, std::uint8_t* frontEnd) { return RenderRoleRow(button, frontEnd, L"Other player", choices.roles[1]); }
		bool HostMayChange(const std::uint8_t* frontEnd) { return InSkirmish(frontEnd) || (InLobby(frontEnd) && IsHost(frontEnd)); }
		void __cdecl OnLevelClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) { if (HostMayChange(frontEnd)) choices.level = NextLevel(choices.level, 1); }
		void __cdecl OnLevelRightClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) { if (HostMayChange(frontEnd)) choices.level = NextLevel(choices.level, -1); }
		void NextRole(std::uint8_t* frontEnd, std::uint8_t& role, std::int32_t step) {
			if (HostMayChange(frontEnd) && choices.level != 0) role = static_cast<std::uint8_t>((role + step + RoleCount) % RoleCount);
		}
		void __cdecl OnHostRoleClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) { NextRole(frontEnd, choices.roles[0], 1); }
		void __cdecl OnHostRoleRightClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) { NextRole(frontEnd, choices.roles[0], -1); }
		void __cdecl OnPartnerRoleClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) { NextRole(frontEnd, choices.roles[1], 1); }
		void __cdecl OnPartnerRoleRightClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) { NextRole(frontEnd, choices.roles[1], -1); }

		using ClickFn = void(__cdecl*)(std::int32_t, std::int32_t, std::uint8_t*);
		using RenderFn = char(__cdecl*)(std::uint8_t*, std::uint8_t*);
		void MakeRow(std::uint8_t* entry, const std::uint8_t* addComputer, std::int32_t id, const RowPlace& place, ClickFn click, ClickFn rightClick, RenderFn render) {
			std::memcpy(entry, addComputer, Game::ButtonConfigSize);
			Store<std::int32_t>(entry + Game::Offset::ButtonConfigId, id);
			Store<std::uint32_t>(entry + Game::Offset::ButtonConfigClick, reinterpret_cast<std::uint32_t>(click));
			Store<std::uint32_t>(entry + Game::Offset::ButtonConfigRightClick, reinterpret_cast<std::uint32_t>(rightClick));
			Store<std::int16_t>(entry + Game::Offset::ButtonConfigX, place.x);
			Store<std::int16_t>(entry + Game::Offset::ButtonConfigY, place.y);
			Store<std::int16_t>(entry + Game::Offset::ButtonConfigWidth, place.width);
			Store<std::int16_t>(entry + Game::Offset::ButtonConfigHeight, place.height);
			Store<std::int16_t>(entry + Game::Offset::ButtonConfigTextWidth, place.width);
			Store<std::uint32_t>(entry + Game::Offset::ButtonConfigRender, reinterpret_cast<std::uint32_t>(render));
			Store<std::int32_t>(entry + Game::Offset::ButtonConfigTextId, 0);
			Store<std::int32_t>(entry + Game::Offset::ButtonConfigTextIndex, 0);
		}
		bool AddLobbyRows() {
			const auto windowConfig = Game::Resolve(Game::Address::MultiplayerWindowConfig);
			const auto* buttons = reinterpret_cast<const std::uint8_t*>(Memory::Read<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons));
			std::size_t count = 0;
			const std::uint8_t* addComputer = nullptr;
			for (; count < MaxButtons; ++count) {
				const auto* entry = buttons + count * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				if (id == ButtonListEnd) break;
				if (id == AddComputerButtonId) addComputer = entry;
			}
			if (count == MaxButtons || addComputer == nullptr) return false;
			buttonList.assign((count + 4) * Game::ButtonConfigSize, 0);
			std::memcpy(buttonList.data(), buttons, count * Game::ButtonConfigSize);
			auto* ours = buttonList.data() + count * Game::ButtonConfigSize;
			MakeRow(ours, addComputer, LevelButtonId, LevelPlace, &OnLevelClick, &OnLevelRightClick, &RenderLevelRow);
			MakeRow(ours + Game::ButtonConfigSize, addComputer, HostRoleButtonId, HostRolePlace, &OnHostRoleClick, &OnHostRoleRightClick, &RenderHostRoleRow);
			MakeRow(ours + 2 * Game::ButtonConfigSize, addComputer, PartnerRoleButtonId, PartnerRolePlace, &OnPartnerRoleClick, &OnPartnerRoleRightClick, &RenderPartnerRoleRow);
			std::memcpy(ours + 3 * Game::ButtonConfigSize, buttons + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			Memory::Write<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(buttonList.data()));
			return true;
		}
		bool AddSkirmishRows() {
			const auto windowConfig = Game::Resolve(Game::Address::SkirmishWindowConfig);
			const auto* buttons = reinterpret_cast<const std::uint8_t*>(Memory::Read<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons));
			std::size_t count = 0;
			const std::uint8_t* addComputer = nullptr;
			for (; count < MaxButtons; ++count) {
				const auto* entry = buttons + count * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				if (id == ButtonListEnd) break;
				if (id == AddComputerButtonId) addComputer = entry;
			}
			if (count == MaxButtons || addComputer == nullptr) return false;
			skirmishButtonList.assign((count + 3) * Game::ButtonConfigSize, 0);
			std::memcpy(skirmishButtonList.data(), buttons, count * Game::ButtonConfigSize);
			auto* ours = skirmishButtonList.data() + count * Game::ButtonConfigSize;
			MakeRow(ours, addComputer, LevelButtonId, SkirmishLevelPlace, &OnLevelClick, &OnLevelRightClick, &RenderLevelRow);
			MakeRow(ours + Game::ButtonConfigSize, addComputer, HostRoleButtonId, SkirmishRolePlace, &OnHostRoleClick, &OnHostRoleRightClick, &RenderHostRoleRow);
			std::memcpy(ours + 2 * Game::ButtonConfigSize, buttons + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			Memory::Write<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(skirmishButtonList.data()));
			return true;
		}

		bool Hook(std::uintptr_t target, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			if (!Memory::Matches(target, expected)) return false;
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
	}

	bool CoopCampaign::InSession() { return inSession; }
	bool CoopCampaign::HumanPlays(std::uint8_t playerNumber) {
		if (!inSession) return false;
		if (playerNumber == SecondKeeper && !secondKeeperUsable) return false;
		return RolePlayer(choices.roles[0]) == playerNumber || (!soloSession && RolePlayer(choices.roles[1]) == playerNumber);
	}
	bool CoopCampaign::OnLobbyMessage(const std::uint8_t* message, std::size_t size) {
		if (size < 6 || message[0] != LobbyMessageId || message[1] != MessageMark[0] || message[2] != MessageMark[1]) return false;
		// The host's own message comes back to it, as on a LAN.
		if (hostMachine) return true;
		choices.level = LevelName(message[3]) != nullptr ? message[3] : 0;
		choices.roles = { message[4] < RoleCount ? message[4] : static_cast<std::uint8_t>(RoleShared), message[5] < RoleCount ? message[5] : static_cast<std::uint8_t>(RoleShared) };
		return true;
	}

	void CoopCampaign::Install() {
		const auto warn = [] { Logger::Warning("Co-op campaign: unexpected game code, it is left out of the lobby."); };
		// Everything is checked before the first change, and each piece does nothing until a level starts in a session.
		const auto send = Game::Resolve(Game::Address::NetworkSendData);
		const auto entryCall = Game::Resolve(Game::Address::GameComponentScreenCall);
		const auto entryOffset = static_cast<std::uint32_t>(Game::Resolve(Game::Address::EnterGameScreen) - (entryCall + 5));
		loadPathBody = Game::Resolve(Game::Address::CameraLoadPathBody);
		gemPathReady = Game::Resolve(Game::Address::GemPathReadyBody);
		gemMoveReady = Game::Resolve(Game::Address::GemMoveReadyBody);
		if (Memory::Read<std::uint8_t>(entryCall) != 0xE8 || Memory::Read<std::uint32_t>(entryCall + 1) != entryOffset
			|| !Memory::Matches(gemPathReady, { 0x8B, 0x46, 0x1C, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x3B, 0xC1 })
			|| !Memory::Matches(gemMoveReady, { 0x8B, 0x46, 0x08, 0x8B, 0x4E, 0x1C, 0x83, 0xF9, 0x01, 0x8D, 0x78, 0x44 })
			|| !Memory::Matches(send, { 0x8B, 0x54, 0x24, 0x04, 0x81, 0xEC, 0x84, 0x00, 0x00, 0x00 })
			|| !Memory::Matches(Game::Resolve(Game::Address::PlayerSetStatus), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xA0, 0xC5, 0x64, 0x00, 0x50 })
			|| !Memory::Matches(Game::Resolve(Game::Address::CameraFinishTween), { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1, 0x85, 0xFF })
			|| !Memory::Matches(loadPathBody, { 0x8B, 0xBC, 0x24, 0x28, 0x01, 0x00, 0x00, 0x8B, 0x8E, 0x3C, 0x0E, 0x00, 0x00 })) { warn(); return; }
		if (!PatchModeReads()) { warn(); return; }
		if (!Hook(Game::Resolve(Game::Address::WorldGetLocalTag), { 0x66, 0x8B, 0x81, 0x43, 0x72, 0x00, 0x00, 0xC3 }, reinterpret_cast<void*>(&GetLocalTagDetour), reinterpret_cast<void**>(&originalGetLocalTag))
			|| !Hook(Game::Resolve(Game::Address::PlayerInitAi), { 0x8B, 0xC1, 0x8B, 0x88, 0x01, 0x11, 0x00, 0x00 }, reinterpret_cast<void*>(&InitAiDetour), reinterpret_cast<void**>(&originalInitAi))
			|| !Hook(Game::Resolve(Game::Address::PlayerSetAiLevel), { 0x8B, 0x89, 0x01, 0x11, 0x00, 0x00 }, reinterpret_cast<void*>(&SetAiLevelDetour), reinterpret_cast<void**>(&originalSetAiLevel))
			|| !Hook(Game::Resolve(Game::Address::CameraLoadPath), { 0x81, 0xEC, 0x0C, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8B, 0xF1, 0x57 }, reinterpret_cast<void*>(&LoadPathDetour), reinterpret_cast<void**>(&originalLoadPath))
			|| !Hook(Game::Resolve(Game::Address::CameraFinishPath), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF }, reinterpret_cast<void*>(&FinishPathDetour), reinterpret_cast<void**>(&originalFinishPath))
			|| !Hook(Game::Resolve(Game::Address::WorldAction), { 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x5C, 0x24, 0x10, 0x8B, 0x47, 0x04 }, reinterpret_cast<void*>(&WorldActionDetour), reinterpret_cast<void**>(&originalWorldAction))
			|| !Hook(Game::Resolve(Game::Address::PlayerCheckTrigger), { 0x51, 0x8B, 0x44, 0x24, 0x08, 0x53, 0x55, 0x56, 0x8B, 0xF1 }, reinterpret_cast<void*>(&CheckTriggerDetour), reinterpret_cast<void**>(&originalCheckTrigger))
			|| !Hook(Game::Resolve(Game::Address::WorldTick), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x50, 0xD3, 0x64, 0x00, 0x50 }, reinterpret_cast<void*>(&WorldTickDetour), reinterpret_cast<void**>(&originalWorldTick))
			|| !Hook(Game::Resolve(Game::Address::WorldSaveFile), { 0x51, 0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B, 0xF1 }, reinterpret_cast<void*>(&SaveWorldDetour), reinterpret_cast<void**>(&originalSaveWorld))
			|| !Hook(Game::Resolve(Game::Address::GemPathWait), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xE8, 0xAF, 0x64, 0x00, 0x50 }, reinterpret_cast<void*>(&GemPathWaitDetour), reinterpret_cast<void**>(&originalGemPathWait))
			|| !Hook(Game::Resolve(Game::Address::GemMoveWait), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x10, 0xB0, 0x64, 0x00, 0x50 }, reinterpret_cast<void*>(&GemMoveWaitDetour), reinterpret_cast<void**>(&originalGemMoveWait))
			|| !CoopHands::Install()
			|| !Memory::WriteCall(entryCall, reinterpret_cast<const void*>(&BeforeGameComponent))
			|| !AddLobbyRows()) {
			warn();
			return;
		}
		const bool onSkirmish = AddSkirmishRows();
		SessionOptions::OnLobbyWrite(&WriteChoicesToList);
		SessionOptions::OnLobbyRead(&ReadChoicesFromList);
		// Last of the level start callbacks (Runtime installs this after the others): they may read the real mode.
		SessionOptions::OnLevelStart(&LendCampaignMode);
		Logger::Debug(onSkirmish ? "Co-op campaign: added to the multiplayer lobby and the skirmish screen." : "Co-op campaign: added to the multiplayer lobby only, the skirmish screen's buttons were not as expected.");
	}
}
