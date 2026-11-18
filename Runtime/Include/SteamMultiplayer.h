#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace KeeperForge2 {
	// Multiplayer through Steam. KeeperForge2 keeps a Steam lobby: public (steam_lobby: public, players running
	// KeeperForge2 are matched into the same lobby) or friends only; friends can also join through a Steam invite
	// ("Invite to Game" in the Steam friends list or overlay) or "Join Game". Everyone in the lobby can then
	// play through the game's own Multiplayer > TCP/IP screens: the game's UDP traffic (WEANETR.dll) goes through
	// Steam, each player having a made-up address built from their Steam account, so hosted games show up in the
	// session list as on a LAN. Needs steam_api.dll (32-bit) in the game folder or the KeeperForge2 folder and
	// Steam running. It runs as Steam app 480 (Steam's shared test app, which every account has; Dungeon Keeper 2 is
	// 2616460). Controlled by "steam_multiplayer" and "steam_lobby" in config.yml.
	// See docs/game/steam-multiplayer.md.
	class SteamMultiplayer {
		public:
			static void Install();
			// For the log, in a multiplayer game: each game turn, and how long the game's rolling save took.
			static void OnGameTurn();
			static void OnWorldSaved(std::uint64_t milliseconds);
			// The multiplayer lobby screen is being drawn (each frame while shown): F6 there opens Steam's invite window.
			static void OnLobbyDrawn();
			// What this machine runs, as text, put on this player in each Steam lobby it is in (lobby member data), so every
			// member can read it. Set before the lobby is made (ModCheck).
			static void SetIdentityProvider(std::string (*provider)());
			struct Member {
				std::uint64_t steamId = 0;
				std::string name;
				// What it put as its identity ("" if none: an older KeeperForge2).
				std::string identity;
			};
			// The other members of this machine's Steam lobby (empty without Steam multiplayer).
			static std::vector<Member> OtherMembers();
			// This machine's Steam account and name (0 and "" without Steam multiplayer).
			static std::uint64_t MySteamId();
			static std::string MyName();
			// KeeperForge2's own messages between the machines in the Steam lobby (Steam lobby chat, which the game never
			// shows). Sent to every member, this machine too; false when there is no lobby to send to. Any thread.
			static bool SendLobbyMessage(const std::string& text);
			// Called on the Steam thread for each message from a member of our lobby (same lobby password), and for each
			// message posted to everyone (PostToEveryone) by another machine. The sender is the Steam account Steam says
			// sent it, so it cannot be made up by the message. Every handler gets every message.
			static void AddLobbyMessageHandler(void (*handler)(std::uint64_t sender, const std::string& text));
			// A message for every KeeperForge2 player on Steam, in any lobby (public, friends only, invited) or none. This
			// machine keeps an invisible Steam lobby that only says it has messages (and their ids); every machine looks
			// for such lobbies every 30 s and asks the poster for each new message directly (Steam networking), so the
			// answer's sender is proven by Steam. It reaches everyone within about 30 s while it stays up (5 minutes).
			// Not delivered to this machine's own handlers. Any thread; false without Steam multiplayer.
			static bool PostToEveryone(const std::string& text);
			// Steam's server time (seconds since 1970, UTC): the same trusted clock for every player, whatever their PC's
			// clock says. nullopt without Steam multiplayer or while Steam's servers cannot be reached. Any thread.
			static std::optional<std::int64_t> ServerTime();
			// Asked on the Steam thread for each other member of our lobby: false leaves them out of it here (no packets
			// either way, their messages ignored), as for ShutOut (bans). Set before Install.
			static void SetMemberFilter(bool (*allowed)(std::uint64_t steamId));
			// Blocked (this account is banned): no Steam lobby, no joining or hosting, no game packets either way, until
			// unblocked (then a lobby of our own is made again). The notice board still works, so the unban can arrive.
			// Any thread; acted on by the Steam thread within a second.
			static void SetSelfBlocked(bool blocked);
			// Called on the Steam thread when the game or Steam tries to join or host while blocked.
			static void SetBlockedAttemptHandler(void (*handler)());
			// Called on the Steam thread about once a second (and once before the first lobby is made). Set before Install.
			static void AddTickHandler(void (*handler)());
			// This machine's standing message to everyone (see PostToEveryone), kept up for as long as it is set (""
			// takes it down). Changing it gives it a new id, so every machine asks for it again. Any thread.
			static void SetStandingPost(const std::string& text);
			// How many KeeperForge2 players are online over Steam (this one included): every machine with Steam
			// multiplayer on (and not banned) keeps an invisible "online" lobby saying where it is; one lobby search
			// counts them.
			struct OnlineCount {
				int total = 0;
				int inGame = 0;
				int inLobby = 0;
				// The search's limit was reached: there may be more.
				bool atLeast = false;
			};
			// Starts a count; `done` is called on the Steam thread with the answer. False without Steam multiplayer, or
			// while this account is banned.
			static bool RequestOnlineCount(void (*done)(const OnlineCount& count));
			// Round trip times (ms) to Steam's servers: Steam's relay network measures each of Steam's data centers again,
			// then the nearest are reported (ping through the relays, and straight there when Steam could measure it),
			// with each other member of our Steam lobby (Steam's own figure for our connection to them).
			struct PingReport {
				struct Server {
					std::string code;   // the data center's code, such as "ams" or "iad"
					int ping = -1;      // the best route, through the relays if quicker
					int direct = -1;    // straight from here, -1 when not measured
					std::string via;    // the data center the best route goes in by, when not this one
				};
				struct Player {
					std::uint64_t steamId = 0;
					std::string name;
					int ping = -1;      // -1: no connection to them yet
				};
				std::vector<Server> servers;
				std::vector<Player> members;
				bool relayReady = false;
				bool timedOut = false;
			};
			// Starts a measurement; `done` is called on the Steam thread with the answer (within about 15 s). False
			// without Steam multiplayer.
			static bool RequestPing(void (*done)(const PingReport& report));
			// A player's Steam name ("" when Steam does not know it). Any thread, the Steam thread too.
			static std::string NameOf(std::uint64_t steamId);
			// Whether the player is a member of our Steam lobby (and not shut out).
			static bool IsInLobby(std::uint64_t steamId);
			// Kicks: this machine treats the member as gone from our lobby (no packets either way, their messages
			// ignored) until Steam says they left it. Any thread.
			static void ShutOut(std::uint64_t steamId);
			// Kicked: leaves our Steam lobby and makes a new one of our own (after sending what is waiting). Any thread.
			static void LeaveLobbyKicked();
			// Called on the Steam thread a few seconds after another player joined our Steam lobby (time for them to be
			// fully in it), or this machine did (with its own id), so what one side missed can be sent again.
			static void AddMemberJoinedHandler(void (*handler)(std::uint64_t member));
	};
}
