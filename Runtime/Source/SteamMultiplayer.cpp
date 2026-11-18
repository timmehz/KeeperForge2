// Winsock first: Windows.h would otherwise bring in the old winsock.h.
#include <winsock2.h>
#include "SteamMultiplayer.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "Paths.h"
#include "StringUtils.h"
#include <Windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <timeapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The Steamworks SDK (ThirdParty/, not shared: Valve's license). Without it the runtime is built with Steam
// multiplayer off; see Runtime/CMakeLists.txt.
#if KEEPERFORGE2_STEAMWORKS
#include <steam/steam_api.h>

namespace KeeperForge2 {
	namespace {
		using SteamId = std::uint64_t;
		constexpr const char* AppId = "480"; // 2616460 - do not use since Dungeon Keeper 2 does nt have the features enabled that this project requires
		// ---- steam_lobby public: every hosted game has a public Steam lobby, owned by its host and marked with this lobby
		// data ("0" when not listed), the host's Steam account as "kf2host". Only marked lobbies are looked for: other
		// programs use the same app id (480 is Steam's shared test app).
		constexpr const char* PublicLobbyKey = "keeperforge2";
		constexpr const char* PublicLobbyValue = "1";
		constexpr const char* UnlistedValue = "0";
		constexpr const char* HostKey = "kf2host";
		
		// While the game's session list is open (it broadcasts for games), Steam's lobby list is asked again this often;
		// F5 asks at once, at most once per cooldown.
		constexpr ULONGLONG ListMilliseconds = 15000;
		constexpr ULONGLONG RefreshCooldownMilliseconds = 5000;
		// The session list counts as open while its broadcasts keep coming.
		constexpr ULONGLONG SessionListOpenMilliseconds = 3000;
		// How long hosting or joining waits for Steam's lobby.
		constexpr ULONGLONG LobbyWaitMilliseconds = 5000;
		// Dungeon Keeper 2 has at most 4 players.
		constexpr int LobbyMaxMembers = 4;
		// Reliable and in order, no delay, and a broken session is set up again. The game's protocol copes with lost packets
		// (made for UDP), but only by noticing a gap and asking again, with the game stopped until then. Steam resends sooner.
		constexpr int SendFlags = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession;
		constexpr int Channel = 0;
		SteamId mySteam = 0;

		// ---- steam_api.dll is delay-loaded (Runtime/CMakeLists.txt): the game starts without it, and it is loaded from
		// the KeeperForge2 folder or the game folder before the first Steam call. These are the functions this file's use
		// of the SDK imports; a steam_api.dll from an older SDK that lacks one would end the game at its first call, so all
		// are checked first. Interface versions are checked by SteamAPI_InitEx.
		constexpr std::array RequiredExports{
			"SteamInternal_SteamAPI_Init", "SteamInternal_ContextInit", "SteamInternal_FindOrCreateUserInterface",
			"SteamAPI_GetHSteamUser", "SteamAPI_RunCallbacks", "SteamAPI_RegisterCallback", "SteamAPI_UnregisterCallback",
			"SteamAPI_RegisterCallResult", "SteamAPI_UnregisterCallResult", "SteamAPI_ReleaseCurrentThreadMemory",
		};
		bool HasRequiredExports(HMODULE library) {
			for (const char* name : RequiredExports) {
				if (GetProcAddress(library, name) == nullptr) {
					Logger::Warning(std::string("Steam: steam_api.dll lacks ") + name + " (an older Steam SDK?); use the steam_api.dll from Steamworks SDK 1.65 or newer.");
					return false;
				}
			}
			return true;
		}

		// ---- Made-up addresses: 10.a.b.c from the low 24 bits of a player's Steam account, the same on every machine.
		// The game puts addresses into its packets, so every player must see the same one for each player.
		constexpr std::uint8_t AddressFirstByte = 10;
		std::uint32_t AddressOf(SteamId id) {
			const auto account = static_cast<std::uint32_t>(id);
			// In memory (network order): 10, a, b, c.
			return AddressFirstByte | ((account >> 16) & 0xFF) << 8 | ((account >> 8) & 0xFF) << 16 | (account & 0xFF) << 24;
		}
		bool IsOurAddress(std::uint32_t address) { return (address & 0xFF) == AddressFirstByte; }
		constexpr std::uint32_t Broadcast = 0xFFFFFFFF;

		// ---- The lobby and its members (address -> Steam id), refreshed by the Steam thread.
		std::mutex stateMutex;
		SteamId lobby = 0;
		std::unordered_map<std::uint32_t, SteamId> members;
		std::atomic<bool> active = false;
		// This account banned (SetSelfBlocked): no lobby and no game traffic.
		std::atomic<bool> selfBlocked = false;
		SteamId wantedLobby = 0;
		// The lobby password's tag ("none" without one; see steam_lobby_password below).
		constexpr const char* NoPassword = "none";
		std::string passwordTag = NoPassword;
		// steam_lobby public: the hosts of the listed lobbies (by address) and the players whose broadcasts reached us
		// while they look for games (by address, with when). Packets from them reach the game although they are not in
		// our lobby, so session lists can find games in other lobbies.
		bool publicLobbies = false;
		struct ListedHost {
			SteamId host = 0;
			SteamId lobby = 0;
		};
		std::unordered_map<std::uint32_t, ListedHost> listedHosts;
		struct Asker {
			SteamId id = 0;
			ULONGLONG seen = 0;
		};
		std::unordered_map<std::uint32_t, Asker> askers;
		constexpr ULONGLONG AskerMilliseconds = 60000;
		// Work for the Steam thread from the game's threads: host a game (make our lobby public and listed, a new one if
		// it is not ours) or join a listed host's lobby. Done is set when it is over (lobbyRequestOk: whether it worked).
		std::atomic<bool> hostRequested = false;
		std::atomic<SteamId> joinRequested = 0;
		std::atomic<bool> lobbyRequestDone = false;
		std::atomic<bool> lobbyRequestOk = false;
		std::atomic<ULONGLONG> lastBroadcast = 0;

		// Packets from Steam waiting for the game, by the port they were sent to.
		struct Packet {
			std::uint32_t from;
			std::uint16_t fromPort;
			std::vector<std::uint8_t> data;
		};
		std::unordered_map<std::uint16_t, std::deque<Packet>> waiting;
		constexpr std::size_t MaxWaiting = 512;
		// Each packet over Steam starts with the sender's and the receiver's port (network order), its kind, and the first
		// 8 characters of the sender's password tag (broadcasts and answers to them come from outside the lobby).
		constexpr std::size_t TagSize = 8;
		constexpr std::size_t HeaderSize = 5 + TagSize;
		enum PacketKind : std::uint8_t { PacketNormal = 0, PacketDiscovery = 1 };
		bool SameTag(const std::uint8_t* tag) {
			std::array<char, TagSize> mine{};
			std::memcpy(mine.data(), passwordTag.data(), (std::min)(passwordTag.size(), TagSize));
			return std::memcmp(mine.data(), tag, TagSize) == 0;
		}

		// ---- The game's network library's socket functions (WEANETR.dll's imports), as they were.
		using SendToFn = int(WINAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
		using RecvFromFn = int(WINAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
		using SelectFn = int(WINAPI*)(int, fd_set*, fd_set*, fd_set*, const timeval*);
		using IoctlFn = int(WINAPI*)(SOCKET, long, u_long*);
		using HostNameFn = int(WINAPI*)(char*, int);
		using HostByNameFn = hostent*(WINAPI*)(const char*);
		using SockNameFn = int(WINAPI*)(SOCKET, sockaddr*, int*);
		SendToFn realSendTo = nullptr;
		RecvFromFn realRecvFrom = nullptr;
		SelectFn realSelect = nullptr;
		IoctlFn realIoctl = nullptr;
		HostNameFn realHostName = nullptr;
		HostByNameFn realHostByName = nullptr;
		SockNameFn realSockName = nullptr;
		// What the game is told its machine is called, and the address that name has.
		constexpr const char* OurHostName = "keeperforge2-steam";
		std::uint32_t myAddress = 0;
		hostent myHost{};
		in_addr myHostAddress{};
		std::array<char*, 2> myHostAddresses{};
		std::array<char*, 1> myHostAliases{};

		std::uint16_t PortOf(SOCKET socket) {
			sockaddr_in address{};
			int size = sizeof(address);
			if (realSockName(socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) return 0;
			return address.sin_port;
		}

		std::atomic<int> failedSends = 0;
		std::atomic<bool> firstPacketSent = false;
		// Sends one packet to a lobby member (or to this machine's own waiting packets).
		void SendTo(SteamId to, std::uint16_t fromPort, std::uint16_t toPort, const char* data, int size, PacketKind kind = PacketNormal) {
			if (selfBlocked && to != mySteam) return;
			if (to == mySteam) {
				std::lock_guard lock(stateMutex);
				auto& queue = waiting[toPort];
				if (queue.size() < MaxWaiting) queue.push_back({ myAddress, fromPort, std::vector<std::uint8_t>(data, data + size) });
				return;
			}
			std::vector<std::uint8_t> packet(HeaderSize + size);
			std::memcpy(packet.data(), &fromPort, 2);
			std::memcpy(packet.data() + 2, &toPort, 2);
			packet[4] = kind;
			std::memcpy(packet.data() + 5, passwordTag.data(), (std::min)(passwordTag.size(), TagSize));
			std::memcpy(packet.data() + HeaderSize, data, size);
			SteamNetworkingIdentity identity;
			identity.SetSteamID64(to);
			const EResult sent = SteamNetworkingMessages()->SendMessageToUser(identity, packet.data(), static_cast<std::uint32_t>(packet.size()), SendFlags, Channel);
			// For the log: whether packets get out at all.
			if (sent != k_EResultOK) {
				if (failedSends.fetch_add(1) < 3) Logger::Warning("Steam: a packet could not be sent (result " + std::to_string(sent) + ").");
			}
			else if (!firstPacketSent.exchange(true)) Logger::Debug("Steam: first packet sent to another player.");
		}

		int WINAPI SendToDetour(SOCKET socket, const char* data, int size, int flags, const sockaddr* to, int toSize) {
			if (!active || to == nullptr || toSize < static_cast<int>(sizeof(sockaddr_in)) || to->sa_family != AF_INET) return realSendTo(socket, data, size, flags, to, toSize);
			const auto* target = reinterpret_cast<const sockaddr_in*>(to);
			const std::uint32_t address = target->sin_addr.s_addr;
			if (address != Broadcast && !IsOurAddress(address)) return realSendTo(socket, data, size, flags, to, toSize);
			const std::uint16_t fromPort = PortOf(socket);
			std::vector<SteamId> targets;
			PacketKind kind = PacketNormal;
			SteamId hostLobby = 0;
			{
				std::lock_guard lock(stateMutex);
				if (address == Broadcast) {
					// As on a LAN, a broadcast also reaches this machine; with public lobbies, also every listed host.
					kind = PacketDiscovery;
					lastBroadcast = GetTickCount64();
					for (const auto& [memberAddress, id] : members) targets.push_back(id);
					for (const auto& [hostAddress, listed] : listedHosts) {
						if (listed.host != mySteam && !members.contains(hostAddress)) targets.push_back(listed.host);
					}
				}
				else if (const auto member = members.find(address); member != members.end()) targets.push_back(member->second);
				else if (const auto asker = askers.find(address); asker != askers.end() && GetTickCount64() - asker->second.seen < AskerMilliseconds) {
					// The answer to a player looking for games (our game's description).
					kind = PacketDiscovery;
					targets.push_back(asker->second.id);
				}
				else if (const auto listed = listedHosts.find(address); listed != listedHosts.end()) {
					// Only joining sends straight to a host found in the list: we move into its lobby first.
					hostLobby = listed->second.lobby;
					targets.push_back(listed->second.host);
				}
			}
			if (hostLobby != 0) {
				lobbyRequestDone = false;
				joinRequested = hostLobby;
				const ULONGLONG started = GetTickCount64();
				while (!lobbyRequestDone && GetTickCount64() - started < LobbyWaitMilliseconds) Sleep(10);
				if (!lobbyRequestDone || !lobbyRequestOk) {
					Logger::Warning("Steam: could not move into that game's Steam lobby, so it cannot be joined.");
					return size;
				}
			}
			for (const SteamId id : targets) SendTo(id, fromPort, target->sin_port, data, size, kind);
			// Like UDP: a packet to nobody is simply lost.
			return size;
		}

		bool TakeWaiting(SOCKET socket, char* data, int size, sockaddr* from, int* fromSize, int& result) {
			const std::uint16_t port = PortOf(socket);
			std::lock_guard lock(stateMutex);
			const auto found = waiting.find(port);
			if (found == waiting.end() || found->second.empty()) return false;
			Packet packet = std::move(found->second.front());
			found->second.pop_front();
			const int copied = std::min<int>(size, static_cast<int>(packet.data.size()));
			std::memcpy(data, packet.data.data(), copied);
			if (from != nullptr && fromSize != nullptr && *fromSize >= static_cast<int>(sizeof(sockaddr_in))) {
				sockaddr_in sender{};
				sender.sin_family = AF_INET;
				sender.sin_port = packet.fromPort;
				sender.sin_addr.s_addr = packet.from;
				std::memcpy(from, &sender, sizeof(sender));
				*fromSize = sizeof(sender);
			}
			result = copied;
			return true;
		}
		bool HasWaiting(SOCKET socket, std::size_t* size = nullptr) {
			const std::uint16_t port = PortOf(socket);
			std::lock_guard lock(stateMutex);
			const auto found = waiting.find(port);
			if (found == waiting.end() || found->second.empty()) return false;
			if (size != nullptr) *size = found->second.front().data.size();
			return true;
		}

		// The library's session list and join threads (0x10005000, 0x10004C30) make their sockets blocking and sit in
		// recvfrom with no select first, and it never makes a socket non-blocking. Going straight to the real call would
		// wait for network data that never comes while Steam's packets pile up here, so the wait is done in short steps.
		int WINAPI RecvFromDetour(SOCKET socket, char* data, int size, int flags, sockaddr* from, int* fromSize) {
			if (!active) return realRecvFrom(socket, data, size, flags, from, fromSize);
			for (;;) {
				int result = 0;
				if (TakeWaiting(socket, data, size, from, fromSize, result)) return result;
				fd_set thisSocket{};
				thisSocket.fd_count = 1;
				thisSocket.fd_array[0] = socket;
				const timeval step{ 0, 2000 };
				// Closing the socket, which is how the library stops those threads, makes this fail: the real call reports it.
				if (realSelect(0, &thisSocket, nullptr, nullptr, &step) != 0) return realRecvFrom(socket, data, size, flags, from, fromSize);
			}
		}
		int WINAPI IoctlDetour(SOCKET socket, long command, u_long* argument) {
			std::size_t size = 0;
			if (active && command == static_cast<long>(FIONREAD) && argument != nullptr && HasWaiting(socket, &size)) {
				*argument = static_cast<u_long>(size);
				return 0;
			}
			return realIoctl(socket, command, argument);
		}
		// Sockets waited on for reading are ready when Steam brought them a packet. Waits are done in short steps so a
		// packet from Steam ends one at once.
		int WINAPI SelectDetour(int count, fd_set* read, fd_set* write, fd_set* except, const timeval* timeout) {
			if (!active || read == nullptr) return realSelect(count, read, write, except, timeout);
			const fd_set wantRead = *read;
			const fd_set wantWrite = write != nullptr ? *write : fd_set{};
			const fd_set wantExcept = except != nullptr ? *except : fd_set{};
			const ULONGLONG started = GetTickCount64();
			const bool forever = timeout == nullptr;
			const ULONGLONG limit = forever ? 0 : static_cast<ULONGLONG>(timeout->tv_sec) * 1000 + timeout->tv_usec / 1000;
			for (;;) {
				fd_set ready{};
				for (u_int i = 0; i < wantRead.fd_count; ++i) {
					if (HasWaiting(wantRead.fd_array[i])) ready.fd_array[ready.fd_count++] = wantRead.fd_array[i];
				}
				*read = wantRead;
				if (write != nullptr) *write = wantWrite;
				if (except != nullptr) *except = wantExcept;
				const timeval step{ 0, ready.fd_count != 0 ? 0 : 2000 };
				const int real = realSelect(count, read, write, except, &step);
				if (real < 0) return real;
				if (ready.fd_count != 0) {
					// Ours, plus whatever the real wait found readable.
					for (u_int i = 0; i < read->fd_count && ready.fd_count < FD_SETSIZE; ++i) {
						bool known = false;
						for (u_int j = 0; j < ready.fd_count; ++j) known = known || ready.fd_array[j] == read->fd_array[i];
						if (!known) ready.fd_array[ready.fd_count++] = read->fd_array[i];
					}
					const int others = real - static_cast<int>(read->fd_count);
					*read = ready;
					return static_cast<int>(ready.fd_count) + (others > 0 ? others : 0);
				}
				if (real > 0) return real;
				if (!forever && GetTickCount64() - started >= limit) return 0;
			}
		}
		int WINAPI HostNameDetour(char* name, int size) {
			if (!active) return realHostName(name, size);
			const std::size_t length = std::strlen(OurHostName);
			if (name == nullptr || size <= static_cast<int>(length)) return SOCKET_ERROR;
			std::memcpy(name, OurHostName, length + 1);
			return 0;
		}
		hostent* WINAPI HostByNameDetour(const char* name) {
			if (!active || name == nullptr || std::strcmp(name, OurHostName) != 0) return realHostByName(name);
			return &myHost;
		}

		// ---- The Steam thread: Steam's callbacks (SteamAPI_RunCallbacks), lobby members and incoming packets. Every lobby and
		// friends call below runs on that thread, so the state here needs no lock except what the game's threads also read.
		// steam_lobby public: when the lobby list was last asked for.
		ULONGLONG lastSearch = 0;
		// A lobby being made or entered for a game request (hosting or joining), answered when it is ready.
		bool hostingAfterCreate = false;
		SteamId joiningForGame = 0;
		void RefreshMembers();
		// What this machine runs, put on this player in every lobby it is in (lobby member data "kf2").
		constexpr const char* IdentityKey = "kf2";
		std::string (*identityProvider)() = nullptr;
		// KeeperForge2's own lobby messages (SendLobbyMessage): waiting to be sent from the Steam thread (under
		// stateMutex), and who is told of messages and new members.
		std::deque<std::string> outgoingMessages;
		constexpr std::size_t MaxOutgoingMessages = 256;
		std::vector<void (*)(std::uint64_t sender, const std::string& text)> lobbyMessageHandlers;
		void Deliver(SteamId from, const std::string& text) {
			for (const auto handler : lobbyMessageHandlers) handler(from, text);
		}
		// ---- Messages to everyone (PostToEveryone). The poster keeps an invisible lobby (which a player may be in
		// besides their normal one) with data "kf2notice" = "1", "kf2nfrom" = its Steam id and "kf2nid" = the ids of its
		// messages; it holds no text, as anyone could make such a lobby. Every machine looks for these lobbies and asks
		// the named poster for each message it has not seen, on a channel of its own; the poster answers with the text,
		// and Steam tells the asker who answered.
		constexpr int NoticeChannel = 1;
		constexpr const char* NoticeKey = "kf2notice";
		constexpr const char* NoticeFromKey = "kf2nfrom";
		constexpr const char* NoticeIdsKey = "kf2nid";
		constexpr ULONGLONG NoticeLifetimeMilliseconds = 5 * 60 * 1000;
		constexpr ULONGLONG NoticeSearchMilliseconds = 30000;
		constexpr ULONGLONG SeenNoticeMilliseconds = 15 * 60 * 1000;
		constexpr std::size_t MaxPostedNotices = 5;
		constexpr std::size_t MaxNoticeAsksPerSearch = 10;
		struct PostedNotice {
			std::string id;
			std::string text;
			ULONGLONG until = 0;
		};
		// Waiting to be posted (any thread, under stateMutex); then on the Steam thread only.
		std::deque<PostedNotice> noticesToPost;
		std::deque<PostedNotice> postedNotices;
		// The standing post (SetStandingPost): its text waiting (under stateMutex), and the one up (Steam thread).
		std::string standingText;
		bool standingChanged = false;
		PostedNotice standing;
		SteamId noticeLobby = 0;
		bool noticeLobbyCreating = false;
		std::uint32_t noticeCounter = 0;
		// Notice ids already asked for (id -> when), and which poster each open question went to.
		std::unordered_map<std::string, ULONGLONG> seenNotices;
		std::unordered_map<std::string, SteamId> askedNotices;
		ULONGLONG lastNoticeSearch = 0;
		std::vector<void (*)(std::uint64_t member)> memberJoinedHandlers;
		// Players who joined, and when to tell memberJoinedHandlers (Steam thread only).
		struct Joined {
			SteamId id = 0;
			ULONGLONG at = 0;
		};
		std::vector<Joined> pendingJoins;
		// Kicked members of our lobby, left out of it here until Steam says they left (Steam thread), and the requests
		// for it and for leaving our lobby from other threads (under stateMutex).
		std::vector<SteamId> shutOut;
		std::deque<SteamId> shutOutRequests;
		std::atomic<bool> leaveRequested = false;
		bool (*memberFilter)(std::uint64_t steamId) = nullptr;
		void (*blockedAttemptHandler)() = nullptr;
		std::vector<void (*)()> tickHandlers;
		void BlockedAttempt() {
			if (blockedAttemptHandler != nullptr) blockedAttemptHandler();
		}
		bool IsShutOut(SteamId id) {
			return std::ranges::find(shutOut, id) != shutOut.end() || (memberFilter != nullptr && id != mySteam && !memberFilter(id));
		}
		constexpr ULONGLONG JoinedDelayMilliseconds = 3000;
		constexpr int MaxLobbyMessageBytes = 4000;
		// ---- steam_lobby_password. Its tag (a SHA-256 of the password, never the password itself; "none" without one) is
		// put on the lobby when this machine makes it and on this player in every lobby (both as data "kf2pw"). Public
		// searches look only for lobbies with the same tag; a lobby entered with another tag is left; and only members
		// with the same tag are players here (packets from others are ignored, ours do not go to them).
		constexpr const char* PasswordKey = "kf2pw";
		std::string PasswordTag(const std::string& password) {
			if (password.empty()) return NoPassword;
			const std::string salted = "KeeperForge2 lobby password:" + password;
			std::array<std::uint8_t, 32> digest{};
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			bool done = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
			done = done && BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
			done = done && BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(salted.data())), static_cast<ULONG>(salted.size()), 0));
			done = done && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
			if (hash != nullptr) BCryptDestroyHash(hash);
			if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
			if (!done) return {};
			constexpr const char* Hex = "0123456789abcdef";
			std::string tag;
			for (const auto byte : digest) {
				tag += Hex[byte >> 4];
				tag += Hex[byte & 15];
			}
			return tag;
		}
		// A member's tag ("none" when it put none: an older KeeperForge2 without a password).
		std::string MemberTag(SteamId inLobby, SteamId member) {
			const char* tag = SteamMatchmaking()->GetLobbyMemberData(CSteamID(inLobby), CSteamID(member), PasswordKey);
			return tag != nullptr && tag[0] != 0 ? tag : NoPassword;
		}
		SteamId LobbyDataId(SteamId inLobby, const char* key) {
			const char* text = SteamMatchmaking()->GetLobbyData(CSteamID(inLobby), key);
			return text != nullptr ? std::strtoull(text, nullptr, 10) : 0;
		}
		SteamId LobbyFromConnect(const char* connect) {
			const char* at = std::strstr(connect, "+connect_lobby");
			if (at == nullptr) return 0;
			at += std::strlen("+connect_lobby");
			while (*at == ' ') ++at;
			return std::strtoull(at, nullptr, 10);
		}

		// Rich presence, as Steam games set it for a lobby: "connect" is what a friend's Join Game or an accepted invite
		// starts the game with (or hands to a running game, GameRichPresenceJoinRequested_t); "steam_player_group" and its
		// size show the lobby's members together in the Steam friends list.
		void SetRichPresence(int players) {
			ISteamFriends* friends = SteamFriends();
			if (lobby == 0) {
				friends->ClearRichPresence();
				return;
			}
			const std::string id = std::to_string(lobby);
			friends->SetRichPresence("connect", ("+connect_lobby " + id).c_str());
			friends->SetRichPresence("steam_player_group", id.c_str());
			friends->SetRichPresence("steam_player_group_size", std::to_string(players).c_str());
			friends->SetRichPresence("status", ("In a lobby (" + std::to_string(players) + "/" + std::to_string(LobbyMaxMembers) + ")").c_str());
		}
		// Entering a lobby (made or joined): this player's password tag and identity go on it as member data.
		void EnteredLobby(SteamId id) {
			{
				std::lock_guard lock(stateMutex);
				lobby = id;
			}
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			matchmaking->SetLobbyMemberData(CSteamID(id), PasswordKey, passwordTag.c_str());
			if (identityProvider != nullptr) matchmaking->SetLobbyMemberData(CSteamID(id), IdentityKey, identityProvider().c_str());
			RefreshMembers();
			// This machine joining counts as a member joining too: what it knows is sent to the others a little later.
			pendingJoins.push_back({ mySteam, GetTickCount64() + JoinedDelayMilliseconds });
			SetRichPresence(matchmaking->GetNumLobbyMembers(CSteamID(id)));
		}
		void LeaveLobby() {
			if (lobby != 0) SteamMatchmaking()->LeaveLobby(CSteamID(lobby));
			shutOut.clear();
			{
				std::lock_guard lock(stateMutex);
				lobby = 0;
			}
			RefreshMembers();
			SetRichPresence(0);
		}
		void AnswerGameRequest(bool ok) {
			lobbyRequestOk = ok;
			lobbyRequestDone = true;
		}

		// Steam's callbacks (STEAM_CALLBACK, registered while this object lives) and the results of its asynchronous calls
		// (CCallResult), all delivered by SteamAPI_RunCallbacks on the Steam thread. Made after SteamAPI_InitEx and never
		// destroyed: the process ends with it.
		class SteamEvents {
			public:
				void CreateLobby();
				void JoinLobby(SteamId id);
				void RequestLobbyList();
				bool Creating() const { return created.IsActive(); }
				bool Joining() const { return entered.IsActive(); }
				bool Searching() const { return listed.IsActive() || noticesListed.IsActive() || onlineListed.IsActive(); }
				void CreatePresenceLobby();
				void SearchOnline();
				void CreateNoticeLobby();
				void SearchNotices();
			private:
				CCallResult<SteamEvents, LobbyCreated_t> created;
				CCallResult<SteamEvents, LobbyEnter_t> entered;
				CCallResult<SteamEvents, LobbyMatchList_t> listed;
				CCallResult<SteamEvents, LobbyCreated_t> noticeCreated;
				CCallResult<SteamEvents, LobbyMatchList_t> noticesListed;
				CCallResult<SteamEvents, LobbyCreated_t> presenceCreated;
				CCallResult<SteamEvents, LobbyMatchList_t> onlineListed;
				void OnPresenceLobbyCreated(LobbyCreated_t* result, bool ioFailure);
				void OnOnlineList(LobbyMatchList_t* result, bool ioFailure);
				void OnNoticeLobbyCreated(LobbyCreated_t* result, bool ioFailure);
				void OnNoticeList(LobbyMatchList_t* result, bool ioFailure);
				void OnLobbyCreated(LobbyCreated_t* result, bool ioFailure);
				void OnLobbyEntered(LobbyEnter_t* result, bool ioFailure);
				void OnLobbyList(LobbyMatchList_t* result, bool ioFailure);
				// A friend's invite accepted, or Join Game picked, in the Steam friends list or overlay while playing.
				STEAM_CALLBACK(SteamEvents, OnLobbyJoinRequested, GameLobbyJoinRequested_t);
				STEAM_CALLBACK(SteamEvents, OnRichPresenceJoinRequested, GameRichPresenceJoinRequested_t);
				// Members coming and going, and lobby or member data changing (a new member's password tag).
				STEAM_CALLBACK(SteamEvents, OnLobbyChatUpdate, LobbyChatUpdate_t);
				STEAM_CALLBACK(SteamEvents, OnLobbyDataUpdate, LobbyDataUpdate_t);
				// KeeperForge2's own lobby messages (SendLobbyMessage).
				STEAM_CALLBACK(SteamEvents, OnLobbyChatMessage, LobbyChatMsg_t);
				// Another player's first packet to us, and a session with a player that broke.
				STEAM_CALLBACK(SteamEvents, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
				STEAM_CALLBACK(SteamEvents, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);
		};
		SteamEvents* events = nullptr;

		// The lobby made at start is friends only (for invites); a lobby is made public and listed when its owner hosts.
		void SteamEvents::CreateLobby() {
			if (selfBlocked) return;
			const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, LobbyMaxMembers);
			if (call != k_uAPICallInvalid) created.Set(call, this, &SteamEvents::OnLobbyCreated);
		}
		void SteamEvents::JoinLobby(SteamId id) {
			const SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(id));
			if (call != k_uAPICallInvalid) entered.Set(call, this, &SteamEvents::OnLobbyEntered);
		}
		void SteamEvents::RequestLobbyList() {
			lastSearch = GetTickCount64();
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			matchmaking->AddRequestLobbyListStringFilter(PublicLobbyKey, PublicLobbyValue, k_ELobbyComparisonEqual);
			matchmaking->AddRequestLobbyListStringFilter(PasswordKey, passwordTag.c_str(), k_ELobbyComparisonEqual);
			matchmaking->AddRequestLobbyListFilterSlotsAvailable(1);
			matchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
			const SteamAPICall_t call = matchmaking->RequestLobbyList();
			if (call != k_uAPICallInvalid) listed.Set(call, this, &SteamEvents::OnLobbyList);
		}

		void Join(SteamId id) {
			if (selfBlocked) {
				BlockedAttempt();
				return;
			}
			if (id == lobby) return;
			LeaveLobby();
			events->JoinLobby(id);
			Logger::Info("Steam: joining a lobby.");
		}
		// Our lobby (which we own) becomes the listed lobby of the game we host.
		void ListOurLobby() {
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			matchmaking->SetLobbyType(CSteamID(lobby), k_ELobbyTypePublic);
			matchmaking->SetLobbyData(CSteamID(lobby), HostKey, std::to_string(mySteam).c_str());
			matchmaking->SetLobbyData(CSteamID(lobby), PublicLobbyKey, PublicLobbyValue);
			Logger::Info("Steam: your game is listed; players running KeeperForge2 see it in their session list.");
		}
		// The game is about to host: list our lobby, or make a new one when the one we are in is someone else's.
		void HostRequested() {
			if (selfBlocked) {
				BlockedAttempt();
				AnswerGameRequest(false);
				return;
			}
			if (lobby != 0 && !events->Joining() && !events->Creating() && SteamMatchmaking()->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64() == mySteam) {
				ListOurLobby();
				AnswerGameRequest(true);
				return;
			}
			// The lobby made at start, or one being made, is listed when it comes.
			hostingAfterCreate = true;
			if (events->Creating()) return;
			LeaveLobby();
			events->CreateLobby();
			if (!events->Creating()) {
				hostingAfterCreate = false;
				AnswerGameRequest(false);
			}
		}
		// The game is joining a listed host: move into its lobby first.
		void JoinRequested(SteamId target) {
			if (lobby == target) {
				AnswerGameRequest(true);
				return;
			}
			joiningForGame = target;
			Join(target);
			if (!events->Joining()) {
				joiningForGame = 0;
				AnswerGameRequest(false);
			}
		}
		// A lobby we own whose listing names another host (it left and Steam made us the owner) is no longer listed.
		void CheckListing() {
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			if (!publicLobbies || lobby == 0 || events->Joining() || matchmaking->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64() != mySteam) return;
			const char* listedValue = matchmaking->GetLobbyData(CSteamID(lobby), PublicLobbyKey);
			if (listedValue == nullptr || std::strcmp(listedValue, PublicLobbyValue) != 0) return;
			if (LobbyDataId(lobby, HostKey) != mySteam) matchmaking->SetLobbyData(CSteamID(lobby), PublicLobbyKey, UnlistedValue);
		}
		// Members without this lobby's password, already reported in the log.
		std::vector<SteamId> reportedOutsiders;
		void RefreshMembers() {
			std::unordered_map<std::uint32_t, SteamId> now;
			if (lobby != 0) {
				ISteamMatchmaking* matchmaking = SteamMatchmaking();
				const int count = matchmaking->GetNumLobbyMembers(CSteamID(lobby));
				for (int i = 0; i < count; ++i) {
					const SteamId id = matchmaking->GetLobbyMemberByIndex(CSteamID(lobby), i).ConvertToUint64();
					if (id == 0 || IsShutOut(id)) continue;
					if (id != mySteam && MemberTag(lobby, id) != passwordTag) {
						if (std::ranges::find(reportedOutsiders, id) == reportedOutsiders.end()) {
							reportedOutsiders.push_back(id);
							Logger::Info("Steam: a player in the Steam lobby does not have this lobby's password (steam_lobby_password); they are left out.");
						}
						continue;
					}
					now[AddressOf(id)] = id;
				}
			}
			const std::size_t count = now.size();
			std::size_t before = 0;
			{
				std::lock_guard lock(stateMutex);
				before = members.size();
				members = std::move(now);
			}
			if (count != before) {
				Logger::Info("Steam: " + std::to_string(count) + " player(s) in the lobby.");
				if (lobby != 0) SetRichPresence(SteamMatchmaking()->GetNumLobbyMembers(CSteamID(lobby)));
			}
		}
		bool IsMember(SteamId id) {
			std::lock_guard lock(stateMutex);
			return members.contains(AddressOf(id));
		}

		void SteamEvents::OnLobbyCreated(LobbyCreated_t* result, bool ioFailure) {
			const bool forHosting = hostingAfterCreate;
			hostingAfterCreate = false;
			if (!ioFailure && result->m_eResult == k_EResultOK && selfBlocked) {
				SteamMatchmaking()->LeaveLobby(CSteamID(result->m_ulSteamIDLobby));
				if (forHosting) AnswerGameRequest(false);
				return;
			}
			if (ioFailure || result->m_eResult != k_EResultOK) {
				Logger::Warning("Steam: could not make a lobby (result " + std::to_string(ioFailure ? 0 : static_cast<int>(result->m_eResult)) + "); friends cannot join.");
				if (forHosting) AnswerGameRequest(false);
				return;
			}
			SteamMatchmaking()->SetLobbyData(CSteamID(result->m_ulSteamIDLobby), PasswordKey, passwordTag.c_str());
			EnteredLobby(result->m_ulSteamIDLobby);
			if (passwordTag != NoPassword) Logger::Info("Steam: the lobby has a password; only players with the same steam_lobby_password take part.");
			Logger::Info("Steam: lobby ready, friends can be invited (F6 on the multiplayer screens, or the Steam friends list: Invite to Game).");
			if (forHosting) {
				ListOurLobby();
				AnswerGameRequest(true);
			}
		}
		void SteamEvents::OnLobbyEntered(LobbyEnter_t* result, bool ioFailure) {
			const bool forGame = joiningForGame != 0;
			joiningForGame = 0;
			const auto response = ioFailure ? static_cast<std::uint32_t>(k_EChatRoomEnterResponseError) : result->m_EChatRoomEnterResponse;
			if (response != static_cast<std::uint32_t>(k_EChatRoomEnterResponseSuccess)) {
				Logger::Warning("Steam: could not join the lobby (response " + std::to_string(response) + "); making one of our own.");
				// Not left without a lobby: invites still work.
				CreateLobby();
				if (forGame) AnswerGameRequest(false);
				return;
			}
			const SteamId joined = result->m_ulSteamIDLobby;
			// Banned while the join was on its way.
			if (selfBlocked) {
				SteamMatchmaking()->LeaveLobby(CSteamID(joined));
				if (forGame) AnswerGameRequest(false);
				return;
			}
			// A lobby made with another password: no one there could play with us.
			const char* lobbyTag = SteamMatchmaking()->GetLobbyData(CSteamID(joined), PasswordKey);
			const std::string tag = lobbyTag != nullptr && lobbyTag[0] != 0 ? lobbyTag : NoPassword;
			if (tag != passwordTag) {
				SteamMatchmaking()->LeaveLobby(CSteamID(joined));
				Logger::Warning(passwordTag == NoPassword
					? "Steam: that lobby has a password; set the same steam_lobby_password in config.yml to join it. Making a lobby of our own."
					: "Steam: that lobby's password is not your steam_lobby_password; making a lobby of our own.");
				CreateLobby();
				if (forGame) AnswerGameRequest(false);
				return;
			}
			// The host is among the members (EnteredLobby refreshes them) before the game's join packets go out.
			EnteredLobby(joined);
			Logger::Info("Steam: in another player's lobby. Open Multiplayer > TCP/IP to see their game.");
			if (forGame) AnswerGameRequest(true);
		}
		// The search's answer: every listed lobby's host, by address, is what the session list's broadcasts reach next.
		void SteamEvents::OnLobbyList(LobbyMatchList_t* result, bool ioFailure) {
			const std::uint32_t found = ioFailure ? 0 : result->m_nLobbiesMatching;
			std::unordered_map<std::uint32_t, ListedHost> hosts;
			for (std::uint32_t i = 0; i < found; ++i) {
				const SteamId id = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i)).ConvertToUint64();
				if (id == 0) continue;
				const SteamId host = LobbyDataId(id, HostKey);
				if (host == 0 || host == mySteam) continue;
				hosts[AddressOf(host)] = { host, id };
			}
			const std::size_t count = hosts.size();
			std::size_t before = 0;
			{
				std::lock_guard lock(stateMutex);
				before = listedHosts.size();
				listedHosts = std::move(hosts);
			}
			if (count != before) Logger::Info("Steam: " + std::to_string(count) + " listed game(s) found.");
		}
		void SteamEvents::OnLobbyJoinRequested(GameLobbyJoinRequested_t* request) {
			const SteamId id = request->m_steamIDLobby.ConvertToUint64();
			if (id != 0) Join(id);
		}
		void SteamEvents::OnRichPresenceJoinRequested(GameRichPresenceJoinRequested_t* request) {
			const SteamId id = LobbyFromConnect(request->m_rgchConnect);
			if (id != 0) Join(id);
		}
		void SteamEvents::OnLobbyChatUpdate(LobbyChatUpdate_t* update) {
			if (update->m_ulSteamIDLobby != lobby) return;
			// A kicked player who left can come back later like anyone.
			constexpr std::uint32_t Gone = k_EChatMemberStateChangeLeft | k_EChatMemberStateChangeDisconnected
				| k_EChatMemberStateChangeKicked | k_EChatMemberStateChangeBanned;
			if ((update->m_rgfChatMemberStateChange & Gone) != 0) std::erase(shutOut, static_cast<SteamId>(update->m_ulSteamIDUserChanged));
			RefreshMembers();
			if ((update->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) != 0 && update->m_ulSteamIDUserChanged != mySteam) {
				pendingJoins.push_back({ update->m_ulSteamIDUserChanged, GetTickCount64() + JoinedDelayMilliseconds });
			}
		}
		void SteamEvents::OnLobbyChatMessage(LobbyChatMsg_t* message) {
			if (message->m_ulSteamIDLobby != lobby || lobby == 0 || lobbyMessageHandlers.empty()) return;
			std::array<char, MaxLobbyMessageBytes + 1> buffer{};
			CSteamID sender;
			EChatEntryType type = k_EChatEntryTypeInvalid;
			const int size = SteamMatchmaking()->GetLobbyChatEntry(CSteamID(lobby), static_cast<int>(message->m_iChatID), &sender, buffer.data(),
				MaxLobbyMessageBytes, &type);
			if (size <= 0 || type != k_EChatEntryTypeChatMsg) return;
			const SteamId from = sender.ConvertToUint64();
			// Only players of this lobby (same lobby password); this machine's own messages come back too.
			if (from != mySteam && !IsMember(from)) {
				RefreshMembers();
				if (!IsMember(from)) return;
			}
			const auto length = static_cast<std::size_t>((std::min)(size, MaxLobbyMessageBytes));
			std::string text(buffer.data(), strnlen(buffer.data(), length));
			Deliver(from, text);
		}

		// ---- Messages to everyone: posting (our invisible notice lobby), looking for others' and asking for them.
		std::string NoticeIds() {
			std::string ids = standing.id;
			for (const auto& notice : postedNotices) ids += (ids.empty() ? "" : ",") + notice.id;
			return ids;
		}
		void SetNoticeLobbyData() {
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			matchmaking->SetLobbyData(CSteamID(noticeLobby), NoticeKey, "1");
			matchmaking->SetLobbyData(CSteamID(noticeLobby), NoticeFromKey, std::to_string(mySteam).c_str());
			matchmaking->SetLobbyData(CSteamID(noticeLobby), NoticeIdsKey, NoticeIds().c_str());
		}
		void SteamEvents::CreateNoticeLobby() {
			// Invisible: found by searches, not shown to friends, and allowed besides the normal lobby. Nobody needs to
			// join it.
			const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypeInvisible, 2);
			if (call == k_uAPICallInvalid) return;
			noticeLobbyCreating = true;
			noticeCreated.Set(call, this, &SteamEvents::OnNoticeLobbyCreated);
		}
		void SteamEvents::OnNoticeLobbyCreated(LobbyCreated_t* result, bool ioFailure) {
			noticeLobbyCreating = false;
			if (ioFailure || result->m_eResult != k_EResultOK) {
				Logger::Warning("Steam: could not make the lobby that shares messages with everyone (result "
					+ std::to_string(ioFailure ? 0 : static_cast<int>(result->m_eResult)) + "); only this lobby got them.");
				return;
			}
			noticeLobby = result->m_ulSteamIDLobby;
			SetNoticeLobbyData();
		}
		void SteamEvents::SearchNotices() {
			lastNoticeSearch = GetTickCount64();
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			matchmaking->AddRequestLobbyListStringFilter(NoticeKey, "1", k_ELobbyComparisonEqual);
			matchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
			matchmaking->AddRequestLobbyListResultCountFilter(50);
			const SteamAPICall_t call = matchmaking->RequestLobbyList();
			if (call != k_uAPICallInvalid) noticesListed.Set(call, this, &SteamEvents::OnNoticeList);
		}
		bool SendNoticePacket(SteamId to, const std::string& text) {
			SteamNetworkingIdentity identity;
			identity.SetSteamID64(to);
			return SteamNetworkingMessages()->SendMessageToUser(identity, text.data(), static_cast<std::uint32_t>(text.size()),
				k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession, NoticeChannel) == k_EResultOK;
		}
		void SteamEvents::OnNoticeList(LobbyMatchList_t* result, bool ioFailure) {
			const std::uint32_t found = ioFailure ? 0 : result->m_nLobbiesMatching;
			const ULONGLONG now = GetTickCount64();
			std::size_t asked = 0;
			for (std::uint32_t i = 0; i < found && asked < MaxNoticeAsksPerSearch; ++i) {
				const CSteamID board = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i));
				const SteamId poster = LobbyDataId(board.ConvertToUint64(), NoticeFromKey);
				if (poster == 0 || poster == mySteam) continue;
				const char* ids = SteamMatchmaking()->GetLobbyData(board, NoticeIdsKey);
				std::string_view rest = ids != nullptr ? ids : "";
				while (!rest.empty() && asked < MaxNoticeAsksPerSearch) {
					const auto comma = rest.find(',');
					const std::string id(rest.substr(0, comma));
					rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
					// An id names its poster first, so one poster cannot answer for another's.
					if (id.empty() || id.size() > 64 || !id.starts_with(std::to_string(poster) + "-") || seenNotices.contains(id)) continue;
					seenNotices[id] = now;
					askedNotices[id] = poster;
					SendNoticePacket(poster, "kf2nq " + id);
					++asked;
				}
			}
		}
		// Questions to us and answers from posters, on the notice channel.
		void ReceiveNotices() {
			std::array<SteamNetworkingMessage_t*, 16> messages{};
			for (;;) {
				const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(NoticeChannel, messages.data(), static_cast<int>(messages.size()));
				if (count <= 0) return;
				for (int i = 0; i < count; ++i) {
					SteamNetworkingMessage_t* message = messages[i];
					const SteamId from = message->m_identityPeer.GetSteamID64();
					const auto* data = static_cast<const char*>(message->m_pData);
					const std::string text(data, static_cast<std::size_t>(std::clamp(message->m_cbSize, 0, MaxLobbyMessageBytes)));
					message->Release();
					if (text.starts_with("kf2nq ")) {
						const auto id = text.substr(6);
						for (const auto& notice : postedNotices) {
							if (notice.id == id) SendNoticePacket(from, "kf2na " + id + " " + notice.text);
						}
						if (!standing.id.empty() && standing.id == id) SendNoticePacket(from, "kf2na " + id + " " + standing.text);
					}
					else if (text.starts_with("kf2na ")) {
						const auto space = text.find(' ', 6);
						if (space == std::string::npos) continue;
						const auto id = text.substr(6, space - 6);
						const auto asked = askedNotices.find(id);
						// Only an answer we asked for, from the poster we asked.
						if (asked == askedNotices.end() || asked->second != from) continue;
						askedNotices.erase(asked);
						Deliver(from, text.substr(space + 1));
					}
				}
			}
		}
		void ServiceNotices() {
			const ULONGLONG now = GetTickCount64();
			std::deque<PostedNotice> newNotices;
			{
				std::lock_guard lock(stateMutex);
				newNotices.swap(noticesToPost);
			}
			bool changed = false;
			{
				std::lock_guard lock(stateMutex);
				if (standingChanged) {
					standingChanged = false;
					changed = true;
					// A new id for new text, so every machine asks for it again.
					standing = standingText.empty() ? PostedNotice{}
						: PostedNotice{ std::to_string(mySteam) + "-s" + std::to_string(std::hash<std::string>{}(standingText)), standingText, 0 };
				}
			}
			for (auto& notice : newNotices) {
				notice.until = now + NoticeLifetimeMilliseconds;
				postedNotices.push_back(std::move(notice));
				if (postedNotices.size() > MaxPostedNotices) postedNotices.pop_front();
				changed = true;
			}
			while (!postedNotices.empty() && now >= postedNotices.front().until) {
				postedNotices.pop_front();
				changed = true;
			}
			if (postedNotices.empty() && standing.id.empty()) {
				if (noticeLobby != 0) {
					SteamMatchmaking()->LeaveLobby(CSteamID(noticeLobby));
					noticeLobby = 0;
				}
			}
			else if (noticeLobby != 0) {
				if (changed) SetNoticeLobbyData();
			}
			else if (!noticeLobbyCreating) events->CreateNoticeLobby();
			// Others' messages: a search every 30 s, never at the same time as the session list's.
			if (now - lastNoticeSearch >= NoticeSearchMilliseconds && !events->Searching()) events->SearchNotices();
			if (seenNotices.size() > 64) {
				std::erase_if(seenNotices, [now](const auto& seen) { return now - seen.second > SeenNoticeMilliseconds; });
				std::erase_if(askedNotices, [](const auto& asked) { return !seenNotices.contains(asked.first); });
			}
			ReceiveNotices();
		}
		// Our own lobby messages out, and players who joined a few seconds ago (Steam thread).
		void SendOutgoingMessages() {
			std::deque<std::string> sending;
			SteamId current = 0;
			{
				std::lock_guard lock(stateMutex);
				current = lobby;
				if (current != 0) sending.swap(outgoingMessages);
			}
			for (const auto& text : sending) {
				if (!SteamMatchmaking()->SendLobbyChatMsg(CSteamID(current), text.c_str(), static_cast<int>(text.size() + 1))) {
					Logger::Warning("Steam: a KeeperForge2 message to the lobby could not be sent.");
				}
			}
			// Kicks, after the messages above (a kick's own message goes out first).
			std::deque<SteamId> outs;
			{
				std::lock_guard lock(stateMutex);
				outs.swap(shutOutRequests);
			}
			for (const auto id : outs) if (id != mySteam && !IsShutOut(id)) shutOut.push_back(id);
			if (!outs.empty()) RefreshMembers();
			if (leaveRequested.exchange(false) && lobby != 0) {
				Logger::Info("Steam: left the Steam lobby (kicked); making a lobby of our own.");
				LeaveLobby();
				events->CreateLobby();
			}
			if (pendingJoins.empty()) return;
			const ULONGLONG now = GetTickCount64();
			for (auto joined = pendingJoins.begin(); joined != pendingJoins.end();) {
				if (now < joined->at) {
					++joined;
					continue;
				}
				const SteamId id = joined->id;
				joined = pendingJoins.erase(joined);
				if (IsMember(id)) for (const auto handler : memberJoinedHandlers) handler(id);
			}
		}
		void SteamEvents::OnLobbyDataUpdate(LobbyDataUpdate_t* update) {
			if (update->m_ulSteamIDLobby == lobby && update->m_bSuccess) RefreshMembers();
		}
		void SteamEvents::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* request) {
			// With public lobbies, players outside our lobby look for games too, so anyone is accepted; each packet is
			// checked when it arrives. Otherwise only players in our lobby (one who just joined may not be listed yet).
			// While this machine has messages up for everyone (PostToEveryone), anyone may ask for them.
			const SteamId from = request->m_identityRemote.GetSteamID64();
			if (!publicLobbies && noticeLobby == 0 && !IsMember(from)) RefreshMembers();
			if (publicLobbies || noticeLobby != 0 || IsMember(from)) SteamNetworkingMessages()->AcceptSessionWithUser(request->m_identityRemote);
		}
		int failedSessions = 0;
		void SteamEvents::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t* failure) {
			if (failedSessions++ >= 10) return;
			Logger::Warning("Steam: the connection to a player failed (reason " + std::to_string(failure->m_info.m_eEndReason) + ": "
				+ failure->m_info.m_szEndDebug + "); Steam sets it up again with the next packet.");
		}

		// Whether a packet reaches the game: from a member of our lobby; or, with public lobbies and the same password tag,
		// a broadcast from a player looking for games (remembered, so the game's answer can go back) or an answer from a
		// listed host.
		ULONGLONG lastStrangerRefresh = 0;
		bool Accepted(SteamId from, const std::uint8_t* data) {
			if (selfBlocked || IsShutOut(from)) return false;
			if (IsMember(from)) return true;
			if (!publicLobbies || !SameTag(data + 5)) return false;
			const auto address = AddressOf(from);
			{
				std::lock_guard lock(stateMutex);
				if (data[4] == PacketDiscovery) {
					if (const auto listed = listedHosts.find(address); listed != listedHosts.end() && listed->second.host == from) return true;
					askers[address] = { from, GetTickCount64() };
					return true;
				}
			}
			// A normal packet from a player who has just joined our lobby: the member list may be behind.
			const ULONGLONG now = GetTickCount64();
			if (now - lastStrangerRefresh < 100) return false;
			lastStrangerRefresh = now;
			RefreshMembers();
			return IsMember(from);
		}
		void ReceivePackets() {
			std::array<SteamNetworkingMessage_t*, 32> messages{};
			for (;;) {
				const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(Channel, messages.data(), static_cast<int>(messages.size()));
				if (count <= 0) return;
				for (int i = 0; i < count; ++i) {
					SteamNetworkingMessage_t* message = messages[i];
					const auto* data = static_cast<const std::uint8_t*>(message->m_pData);
					const int size = message->m_cbSize;
					const SteamId from = message->m_identityPeer.GetSteamID64();
					if (size >= static_cast<int>(HeaderSize) && Accepted(from, data)) {
						static bool firstPacketLogged = false;
						if (!firstPacketLogged) {
							firstPacketLogged = true;
							Logger::Debug("Steam: first packet received from another player.");
						}
						std::uint16_t fromPort = 0;
						std::uint16_t toPort = 0;
						std::memcpy(&fromPort, data, 2);
						std::memcpy(&toPort, data + 2, 2);
						std::lock_guard lock(stateMutex);
						auto& queue = waiting[toPort];
						if (queue.size() < MaxWaiting) queue.push_back({ AddressOf(from), fromPort, std::vector<std::uint8_t>(data + HeaderSize, data + size) });
					}
					message->Release();
				}
			}
		}
		// ---- The invite key: Steam's invite window for our lobby (ISteamFriends::ActivateGameOverlayInviteDialog), as a
		// Steam game's "Invite friends" button opens it. Only on the multiplayer screens: the session list (open while its
		// broadcasts keep coming) or the multiplayer lobby (drawn each frame while shown; OnLobbyDrawn).
		constexpr int InviteKey = VK_F6;
		std::atomic<ULONGLONG> lastLobbyDrawn = 0;
		// The window only shows through the Steam overlay. Without it (not drawn into the game, or turned off) Steam's
		// friends list is opened instead, where Invite to Game works the same. ShellExecute wants COM, on a thread of its own.
		DWORD WINAPI OpenFriendsList(LPVOID) {
			const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			ShellExecuteW(nullptr, L"open", L"steam://open/friends", nullptr, nullptr, SW_SHOWNORMAL);
			if (SUCCEEDED(com)) CoUninitialize();
			return 0;
		}
		void OpenInviteDialog() {
			if (lobby == 0) {
				Logger::Info("Steam: the Steam lobby is not ready yet, so friends cannot be invited; try again in a moment.");
				return;
			}
			if (SteamUtils()->IsOverlayEnabled()) {
				SteamFriends()->ActivateGameOverlayInviteDialog(CSteamID(lobby));
				Logger::Info("Steam: invite window opened.");
				return;
			}
			Logger::Info("Steam: the Steam overlay is not running in the game, so Steam's friends list is opened instead: right-click a friend, Invite to Game.");
			if (HANDLE thread = CreateThread(nullptr, 0, &OpenFriendsList, nullptr, 0, nullptr)) CloseHandle(thread);
		}

		// The Steam thread's id: Steam's returned text is freed by SteamAPI_RunCallbacks there, and must not be freed by
		// hand in the middle of a callback.
		std::atomic<DWORD> steamThreadId = 0;
		// ---- Who is online (RequestOnlineCount). Every machine with Steam multiplayer on, unless banned, keeps an
		// invisible lobby of its own (its second one besides the notice board) with data "kf2online" = 1, "kf2pfrom" =
		// its Steam id and "kf2pstate" = game, lobby or menu. Nobody joins them; a search counts them, one per account.
		constexpr const char* PresenceKey = "kf2online";
		constexpr const char* PresenceFromKey = "kf2pfrom";
		constexpr const char* PresenceStateKey = "kf2pstate";
		constexpr int MaxOnlineResults = 500;
		SteamId presenceLobby = 0;
		bool presenceCreating = false;
		ULONGLONG lastPresenceTry = 0;
		std::string presenceState;
		// A game turn of a multiplayer game lately (OnGameTurn).
		std::atomic<ULONGLONG> lastGameTurn = 0;
		std::atomic<bool> onlineRequested = false;
		std::atomic<void (*)(const SteamMultiplayer::OnlineCount&)> onlineDone = nullptr;
		const char* PresenceStateNow() {
			const ULONGLONG now = GetTickCount64();
			if (lastGameTurn != 0 && now - lastGameTurn < 5000) return "game";
			if (lastLobbyDrawn != 0 && now - lastLobbyDrawn < 5000) return "lobby";
			return "menu";
		}
		void SteamEvents::CreatePresenceLobby() {
			const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypeInvisible, 1);
			if (call == k_uAPICallInvalid) return;
			presenceCreating = true;
			presenceCreated.Set(call, this, &SteamEvents::OnPresenceLobbyCreated);
		}
		void SteamEvents::OnPresenceLobbyCreated(LobbyCreated_t* result, bool ioFailure) {
			presenceCreating = false;
			if (ioFailure || result->m_eResult != k_EResultOK) {
				Logger::Debug("Steam: could not make the online lobby (result " + std::to_string(ioFailure ? 0 : static_cast<int>(result->m_eResult)) + "); trying again later.");
				return;
			}
			if (selfBlocked) {
				SteamMatchmaking()->LeaveLobby(CSteamID(result->m_ulSteamIDLobby));
				return;
			}
			presenceLobby = result->m_ulSteamIDLobby;
			presenceState.clear();
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			matchmaking->SetLobbyData(CSteamID(presenceLobby), PresenceKey, "1");
			matchmaking->SetLobbyData(CSteamID(presenceLobby), PresenceFromKey, std::to_string(mySteam).c_str());
		}
		void SteamEvents::SearchOnline() {
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			matchmaking->AddRequestLobbyListStringFilter(PresenceKey, "1", k_ELobbyComparisonEqual);
			matchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
			matchmaking->AddRequestLobbyListResultCountFilter(MaxOnlineResults);
			const SteamAPICall_t call = matchmaking->RequestLobbyList();
			if (call != k_uAPICallInvalid) onlineListed.Set(call, this, &SteamEvents::OnOnlineList);
			else if (const auto done = onlineDone.exchange(nullptr)) done({ 1, 0, 0, false });
		}
		void SteamEvents::OnOnlineList(LobbyMatchList_t* result, bool ioFailure) {
			const std::uint32_t found = ioFailure ? 0 : result->m_nLobbiesMatching;
			std::unordered_map<SteamId, std::string> players;
			for (std::uint32_t i = 0; i < found; ++i) {
				const CSteamID entry = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i));
				const SteamId player = LobbyDataId(entry.ConvertToUint64(), PresenceFromKey);
				if (player == 0) continue;
				const char* state = SteamMatchmaking()->GetLobbyData(entry, PresenceStateKey);
				players[player] = state != nullptr ? state : "";
			}
			// This machine, whether or not the search showed its own.
			players[mySteam] = PresenceStateNow();
			SteamMultiplayer::OnlineCount count;
			count.total = static_cast<int>(players.size());
			for (const auto& [player, state] : players) {
				if (state == "game") ++count.inGame;
				else if (state == "lobby") ++count.inLobby;
			}
			count.atLeast = found >= static_cast<std::uint32_t>(MaxOnlineResults);
			if (const auto done = onlineDone.exchange(nullptr)) done(count);
		}
		// Steam thread, every pass: our online lobby kept (left while banned) and its state current, and a count asked for.
		void ServicePresence() {
			const ULONGLONG now = GetTickCount64();
			if (selfBlocked) {
				if (presenceLobby != 0) {
					SteamMatchmaking()->LeaveLobby(CSteamID(presenceLobby));
					presenceLobby = 0;
				}
			}
			else if (presenceLobby == 0) {
				if (!presenceCreating && (lastPresenceTry == 0 || now - lastPresenceTry >= 30000)) {
					lastPresenceTry = now;
					events->CreatePresenceLobby();
				}
			}
			else if (const std::string state = PresenceStateNow(); state != presenceState) {
				presenceState = state;
				SteamMatchmaking()->SetLobbyData(CSteamID(presenceLobby), PresenceStateKey, state.c_str());
			}
			if (onlineRequested && !events->Searching()) {
				onlineRequested = false;
				events->SearchOnline();
			}
		}

		// ---- Ping (RequestPing): Steam's relay network measures its round trip time to each of Steam's data centers
		// (points of presence); a request has it measured again now, then reports the nearest ones and our lobby members.
		constexpr ULONGLONG PingWaitMilliseconds = 15000;
		constexpr std::size_t PingServersShown = 5;
		std::atomic<void (*)(const SteamMultiplayer::PingReport&)> pingDone = nullptr;
		std::atomic<bool> pingRequested = false;
		ULONGLONG pingStarted = 0;
		bool pingSawMeasuring = false;
		// A data center's code: up to four letters packed into the id (the fourth in the top byte).
		std::string DataCenterCode(SteamNetworkingPOPID id) {
			std::string code;
			for (const int shift : { 16, 8, 0, 24 }) {
				const char c = static_cast<char>((id >> shift) & 0xFF);
				if (c != 0) code += c;
			}
			return code;
		}
		void FinishPing(bool timedOut) {
			pingStarted = 0;
			SteamMultiplayer::PingReport report;
			report.timedOut = timedOut;
			ISteamNetworkingUtils* utils = SteamNetworkingUtils();
			SteamRelayNetworkStatus_t status{};
			report.relayReady = utils->GetRelayNetworkStatus(&status) == k_ESteamNetworkingAvailability_Current;
			const int count = utils->GetPOPCount();
			std::vector<SteamNetworkingPOPID> pops(static_cast<std::size_t>(count > 0 ? count : 0));
			const int listed = pops.empty() ? 0 : utils->GetPOPList(pops.data(), count);
			for (int i = 0; i < listed; ++i) {
				SteamMultiplayer::PingReport::Server server;
				server.code = DataCenterCode(pops[static_cast<std::size_t>(i)]);
				SteamNetworkingPOPID via = 0;
				server.ping = utils->GetPingToDataCenter(pops[static_cast<std::size_t>(i)], &via);
				if (server.ping < 0) continue;
				server.direct = utils->GetDirectPingToPOP(pops[static_cast<std::size_t>(i)]);
				if (via != 0 && via != pops[static_cast<std::size_t>(i)]) server.via = DataCenterCode(via);
				report.servers.push_back(std::move(server));
			}
			std::ranges::sort(report.servers, [](const auto& a, const auto& b) { return a.ping < b.ping; });
			if (report.servers.size() > PingServersShown) report.servers.resize(PingServersShown);
			std::vector<SteamId> others;
			{
				std::lock_guard lock(stateMutex);
				for (const auto& [address, id] : members) if (id != mySteam) others.push_back(id);
			}
			std::ranges::sort(others);
			for (const SteamId id : others) {
				SteamNetworkingIdentity identity;
				identity.SetSteamID64(id);
				SteamNetConnectionRealTimeStatus_t connection{};
				SteamNetworkingMessages()->GetSessionConnectionInfo(identity, nullptr, &connection);
				const char* name = SteamFriends()->GetFriendPersonaName(CSteamID(id));
				report.members.push_back({ id, name != nullptr && name[0] != 0 ? name : "a player", connection.m_nPing > 0 ? connection.m_nPing : -1 });
			}
			if (const auto done = pingDone.exchange(nullptr)) done(report);
		}
		// Steam thread, every pass.
		void ServicePing() {
			const ULONGLONG now = GetTickCount64();
			if (pingRequested.exchange(false)) {
				// Measured again from now (what was measured stays usable meanwhile).
				SteamNetworkingUtils()->CheckPingDataUpToDate(0.0f);
				pingStarted = now;
				pingSawMeasuring = false;
			}
			if (pingStarted == 0) return;
			SteamRelayNetworkStatus_t status{};
			const auto availability = SteamNetworkingUtils()->GetRelayNetworkStatus(&status);
			if (status.m_bPingMeasurementInProgress) pingSawMeasuring = true;
			const bool measured = availability == k_ESteamNetworkingAvailability_Current && !status.m_bPingMeasurementInProgress;
			// Done once a new measurement has run (or none was needed after a second).
			if (measured && (pingSawMeasuring || now - pingStarted >= 1000)) FinishPing(false);
			else if (now - pingStarted >= PingWaitMilliseconds) FinishPing(true);
		}

		DWORD WINAPI SteamThread(LPVOID) {
			steamThreadId = GetCurrentThreadId();
			// Callbacks are registered by this object; SteamAPI_RunCallbacks below delivers them on this thread.
			events = new SteamEvents();
			// A ban this machine already knows of is known before any lobby.
			for (const auto handler : tickHandlers) handler();
			bool blocked = selfBlocked;
			if (blocked) Logger::Info("Steam: this account is banned from KeeperForge2 lobbies; Steam multiplayer stays off until the ban ends.");
			else if (wantedLobby != 0) Join(wantedLobby);
			else events->CreateLobby();
			ULONGLONG lastTick = GetTickCount64();
			ULONGLONG lastRefresh = 0;
			ULONGLONG lastListingCheck = 0;
			bool refreshKeyDown = false;
			bool inviteKeyDown = false;
			for (;;) {
				SteamAPI_RunCallbacks();
				const ULONGLONG now = GetTickCount64();
				// Keys count only with the game's window in front.
				const HWND front = GetForegroundWindow();
				DWORD frontProcess = 0;
				if (front != nullptr) GetWindowThreadProcessId(front, &frontProcess);
				const bool gameInFront = frontProcess == GetCurrentProcessId();
				const bool inviteDown = gameInFront && (GetAsyncKeyState(InviteKey) & 0x8000) != 0;
				if (inviteDown && !inviteKeyDown && (now - lastBroadcast < SessionListOpenMilliseconds || now - lastLobbyDrawn < SessionListOpenMilliseconds)) OpenInviteDialog();
				inviteKeyDown = inviteDown;
				if (now - lastTick >= 1000) {
					lastTick = now;
					for (const auto handler : tickHandlers) handler();
					if (const bool blockedNow = selfBlocked; blockedNow != blocked) {
						blocked = blockedNow;
						if (blocked) {
							Logger::Info("Steam: this account is banned from KeeperForge2 lobbies; leaving the lobby, Steam multiplayer is off until the ban ends.");
							LeaveLobby();
						}
						else {
							Logger::Info("Steam: the ban is over; Steam multiplayer is on again.");
							events->CreateLobby();
						}
					}
				}
				// Members are refreshed on Steam's lobby updates; this only catches anything missed.
				if (now - lastRefresh >= 1000) {
					RefreshMembers();
					lastRefresh = now;
				}
				if (hostRequested.exchange(false)) HostRequested();
				if (const SteamId target = joinRequested.exchange(0); target != 0) JoinRequested(target);
				if (publicLobbies) {
					if (now - lastListingCheck >= 2000) {
						lastListingCheck = now;
						CheckListing();
					}
					// The session list is open while its broadcasts keep coming: the lobby list every 15 s, or at once on
					// F5 (with the game in front), at most once per cooldown.
					const bool listOpen = now - lastBroadcast < SessionListOpenMilliseconds;
					const bool keyDown = gameInFront && (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
					const bool pressed = keyDown && !refreshKeyDown;
					refreshKeyDown = keyDown;
					if (listOpen && !events->Searching()) {
						if (lastSearch == 0 || now - lastSearch >= ListMilliseconds) events->RequestLobbyList();
						else if (pressed) {
							if (now - lastSearch >= RefreshCooldownMilliseconds) {
								Logger::Info("Steam: refreshing the list of games.");
								events->RequestLobbyList();
							}
							else Logger::Info("Steam: the list of games was refreshed less than 5 s ago; wait a moment.");
						}
					}
				}
				SendOutgoingMessages();
				ServiceNotices();
				ServicePresence();
				ServicePing();
				ReceivePackets();
				Sleep(2);
			}
		}

		// ---- WEANETR.dll's import table: its slots for these Winsock functions are pointed at ours.
		bool PatchImport(HMODULE module, void* function, void* replacement, void** original) {
			if (function == nullptr) return false;
			const auto base = reinterpret_cast<std::uint8_t*>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (directory.VirtualAddress == 0) return false;
			bool patched = false;
			for (const auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name != 0; ++entry) {
				for (auto* slot = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + entry->FirstThunk); slot->u1.Function != 0; ++slot) {
					if (slot->u1.Function != reinterpret_cast<DWORD>(function)) continue;
					*original = function;
					Memory::Write<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&slot->u1.Function), reinterpret_cast<std::uint32_t>(replacement));
					patched = true;
				}
			}
			return patched;
		}
	}

	namespace {
		// ---- The player's name in multiplayer games: their Steam name instead of the one they typed. The game hosts and
		// joins with the name it keeps at LobbyPlayerName, then copies that into the lobby slot and saves it, so the kept
		// name is swapped just before. Saving is skipped, so the typed name is still there when this is turned off.
		// A lobby slot has 0x22 bytes for the name, before its ping.
		constexpr std::size_t MaxPlayerNameLength = 16;
		std::wstring steamName;
		const wchar_t* swappedName = nullptr;
		wchar_t* UseSteamName(wchar_t* typed) {
			auto*& kept = *reinterpret_cast<wchar_t**>(Game::Resolve(Game::Address::LobbyPlayerName));
			if (typed == nullptr || typed != kept || steamName.empty()) return typed;
			const auto alloc = reinterpret_cast<void*(__cdecl*)(std::size_t)>(Game::Resolve(Game::Address::GameAlloc));
			const auto release = reinterpret_cast<void(__cdecl*)(void*)>(Game::Resolve(Game::Address::GameFree));
			auto* name = static_cast<wchar_t*>(alloc((steamName.size() + 1) * sizeof(wchar_t)));
			if (name == nullptr) return typed;
			std::memcpy(name, steamName.c_str(), (steamName.size() + 1) * sizeof(wchar_t));
			release(typed);
			kept = name;
			swappedName = name;
			return name;
		}
		using CreateSessionFn = std::int32_t(__fastcall*)(void* network, void* unused, void* session, wchar_t* player, void* slot, std::int32_t players);
		CreateSessionFn originalCreateSession = nullptr;
		std::int32_t __fastcall CreateSessionDetour(void* network, void* unused, void* session, wchar_t* player, void* slot, std::int32_t players) {
			// steam_lobby public: the game is hosted from a lobby of our own, listed for everyone looking for games.
			if (active && publicLobbies) {
				lobbyRequestDone = false;
				hostRequested = true;
				const ULONGLONG started = GetTickCount64();
				while (!lobbyRequestDone && GetTickCount64() - started < LobbyWaitMilliseconds) Sleep(10);
				if (!lobbyRequestDone || !lobbyRequestOk) Logger::Warning("Steam: the game's Steam lobby is not ready, so the game is not listed; friends can still be invited.");
			}
			return originalCreateSession(network, unused, session, UseSteamName(player), slot, players);
		}
		using JoinSessionFn = std::int32_t(__fastcall*)(void* network, void* unused, void* session, wchar_t* player, void* slot);
		JoinSessionFn originalJoinSession = nullptr;
		std::int32_t __fastcall JoinSessionDetour(void* network, void* unused, void* session, wchar_t* player, void* slot) {
			return originalJoinSession(network, unused, session, UseSteamName(player), slot);
		}
		using SavePlayerNameFn = void(__fastcall*)(void* settings, void* unused, const wchar_t* name);
		SavePlayerNameFn originalSavePlayerName = nullptr;
		void __fastcall SavePlayerNameDetour(void* settings, void* unused, const wchar_t* name) {
			if (name != swappedName) originalSavePlayerName(settings, unused, name);
		}
		bool HookName(std::uintptr_t target, void* detour, void** original) {
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}
		void ReadSteamName() {
			if (StringUtils::ToLower(Config::GetString("steam_player_name", "on")) == "off") return;
			const char* persona = SteamFriends()->GetPersonaName();
			const int length = persona != nullptr ? MultiByteToWideChar(CP_UTF8, 0, persona, -1, nullptr, 0) : 0;
			if (length <= 1) {
				Logger::Warning("Steam: could not read your Steam name, multiplayer games use the name you type.");
				return;
			}
			steamName.resize(static_cast<std::size_t>(length));
			MultiByteToWideChar(CP_UTF8, 0, persona, -1, steamName.data(), length);
			steamName.resize(static_cast<std::size_t>(length) - 1);
			if (steamName.size() > MaxPlayerNameLength) {
				steamName.resize(MaxPlayerNameLength);
				// Not half of a character made of two wide ones.
				if (IS_HIGH_SURROGATE(steamName.back())) steamName.pop_back();
			}
		}
		// Hosting and joining are hooked for the Steam name and for public lobbies (hosting lists the game); saving the
		// name only for the Steam name.
		void InstallSessionHooks() {
			ReadSteamName();
			if (steamName.empty() && !publicLobbies) return;
			const auto create = Game::Resolve(Game::Address::NetworkCreateSession);
			const auto join = Game::Resolve(Game::Address::NetworkJoinSession);
			const auto save = Game::Resolve(Game::Address::SavePlayerName);
			if (!Memory::Matches(create, { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x8B, 0xD9, 0x57 })
				|| !Memory::Matches(join, { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x40, 0x56, 0x85, 0xED })
				|| !Memory::Matches(save, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xE8, 0xEE, 0x64, 0x00 })) {
				Logger::Warning("Steam: unexpected game code, multiplayer games use the name you type and hosted games are not listed.");
				steamName.clear();
				return;
			}
			if (!steamName.empty() && !HookName(save, reinterpret_cast<void*>(&SavePlayerNameDetour), reinterpret_cast<void**>(&originalSavePlayerName))) {
				Logger::Warning("Steam: could not hook the game's name saving, multiplayer games use the name you type.");
				steamName.clear();
				if (!publicLobbies) return;
			}
			if (!HookName(create, reinterpret_cast<void*>(&CreateSessionDetour), reinterpret_cast<void**>(&originalCreateSession))
				|| !HookName(join, reinterpret_cast<void*>(&JoinSessionDetour), reinterpret_cast<void**>(&originalJoinSession))) {
				HookManager::RemoveHook(create);
				HookManager::RemoveHook(save);
				steamName.clear();
				Logger::Warning("Steam: could not hook hosting and joining, multiplayer games use the name you type and hosted games are not listed.");
				return;
			}
			if (!steamName.empty()) Logger::Info("Steam: multiplayer games use your Steam name.");
		}
	}

	// A stall of the game while a game is on: what the log needs to tell a slow connection from everything else. The
	// connection numbers are the start of Steam's SteamNetConnectionRealTimeStatus_t.
	namespace {
		constexpr ULONGLONG StallMs = 750;
		constexpr int MaxStallLines = 100;
		ULONGLONG lastTurnTime = 0;
		int stallLines = 0;
		std::string ConnectionText() {
			if (!active) return "";
			std::vector<SteamId> others;
			{
				std::lock_guard lock(stateMutex);
				for (const auto& [address, id] : members) if (id != mySteam) others.push_back(id);
			}
			std::string text;
			for (const SteamId id : others) {
				SteamNetworkingIdentity identity;
				identity.SetSteamID64(id);
				SteamNetConnectionRealTimeStatus_t status{};
				const auto state = SteamNetworkingMessages()->GetSessionConnectionInfo(identity, nullptr, &status);
				text += " Steam connection: state " + std::to_string(static_cast<int>(state)) + ", ping " + std::to_string(status.m_nPing) + " ms, packets arriving "
					+ std::to_string(static_cast<int>(status.m_flConnectionQualityLocal * 100)) + "% here and "
					+ std::to_string(static_cast<int>(status.m_flConnectionQualityRemote * 100)) + "% there.";
			}
			return text;
		}
	}
	void SteamMultiplayer::SetIdentityProvider(std::string (*provider)()) { identityProvider = provider; }

	std::vector<SteamMultiplayer::Member> SteamMultiplayer::OtherMembers() {
		std::vector<Member> found;
		if (!active) return found;
		std::vector<SteamId> ids;
		SteamId current = 0;
		{
			std::lock_guard lock(stateMutex);
			current = lobby;
			for (const auto& [address, id] : members) if (id != mySteam) ids.push_back(id);
		}
		if (current == 0) return found;
		std::ranges::sort(ids);
		for (const auto id : ids) {
			Member member;
			member.steamId = id;
			const char* data = SteamMatchmaking()->GetLobbyMemberData(CSteamID(current), CSteamID(id), IdentityKey);
			member.identity = data != nullptr ? data : "";
			const char* name = SteamFriends()->GetFriendPersonaName(CSteamID(id));
			member.name = name != nullptr && name[0] != 0 ? name : "a player";
			found.push_back(std::move(member));
		}
		// Steam keeps the returned text per thread; this is a game thread, which does not run Steam's callbacks.
		SteamAPI_ReleaseCurrentThreadMemory();
		return found;
	}

	std::uint64_t SteamMultiplayer::MySteamId() { return active ? mySteam : 0; }

	bool SteamMultiplayer::SendLobbyMessage(const std::string& text) {
		if (!active || text.empty() || text.size() > static_cast<std::size_t>(MaxLobbyMessageBytes)) return false;
		std::lock_guard lock(stateMutex);
		if (lobby == 0 || outgoingMessages.size() >= MaxOutgoingMessages) return false;
		outgoingMessages.push_back(text);
		return true;
	}
	void SteamMultiplayer::AddLobbyMessageHandler(void (*handler)(std::uint64_t, const std::string&)) {
		// Set before Steam starts (Install order), so the Steam thread never sees the list change.
		if (handler != nullptr) lobbyMessageHandlers.push_back(handler);
	}
	std::optional<std::int64_t> SteamMultiplayer::ServerTime() {
		if (!active || !SteamUser()->BLoggedOn()) return std::nullopt;
		const std::uint32_t time = SteamUtils()->GetServerRealTime();
		if (time == 0) return std::nullopt;
		return static_cast<std::int64_t>(time);
	}
	void SteamMultiplayer::SetMemberFilter(bool (*allowed)(std::uint64_t)) { memberFilter = allowed; }
	void SteamMultiplayer::SetSelfBlocked(bool blocked) { selfBlocked = blocked; }
	void SteamMultiplayer::SetBlockedAttemptHandler(void (*handler)()) { blockedAttemptHandler = handler; }
	void SteamMultiplayer::AddTickHandler(void (*handler)()) {
		// Set before Steam starts (Install order), so the Steam thread never sees the list change.
		if (handler != nullptr) tickHandlers.push_back(handler);
	}
	bool SteamMultiplayer::RequestOnlineCount(void (*done)(const OnlineCount&)) {
		if (!active || selfBlocked || done == nullptr) return false;
		onlineDone = done;
		onlineRequested = true;
		return true;
	}
	bool SteamMultiplayer::RequestPing(void (*done)(const PingReport&)) {
		if (!active || done == nullptr) return false;
		pingDone = done;
		pingRequested = true;
		return true;
	}
	void SteamMultiplayer::SetStandingPost(const std::string& text) {
		if (text.size() > static_cast<std::size_t>(MaxLobbyMessageBytes - 100)) return;
		std::lock_guard lock(stateMutex);
		if (text == standingText) return;
		standingText = text;
		standingChanged = true;
	}
	std::string SteamMultiplayer::NameOf(std::uint64_t steamId) {
		if (!active || steamId == 0) return {};
		const char* name = SteamFriends()->GetFriendPersonaName(CSteamID(steamId));
		std::string text = name != nullptr ? name : "";
		if (GetCurrentThreadId() != steamThreadId.load()) SteamAPI_ReleaseCurrentThreadMemory();
		return text;
	}
	bool SteamMultiplayer::IsInLobby(std::uint64_t steamId) { return active && steamId != 0 && IsMember(steamId); }
	void SteamMultiplayer::ShutOut(std::uint64_t steamId) {
		if (!active || steamId == 0) return;
		std::lock_guard lock(stateMutex);
		shutOutRequests.push_back(steamId);
	}
	void SteamMultiplayer::LeaveLobbyKicked() { if (active) leaveRequested = true; }
	bool SteamMultiplayer::PostToEveryone(const std::string& text) {
		if (!active || text.empty() || text.size() > static_cast<std::size_t>(MaxLobbyMessageBytes - 100)) return false;
		std::lock_guard lock(stateMutex);
		const std::string id = std::to_string(mySteam) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++noticeCounter);
		noticesToPost.push_back({ id, text, 0 });
		return true;
	}
	void SteamMultiplayer::AddMemberJoinedHandler(void (*handler)(std::uint64_t)) {
		// Set before Steam starts (Install order), so the Steam thread never sees the list change.
		if (handler != nullptr) memberJoinedHandlers.push_back(handler);
	}

	void SteamMultiplayer::OnLobbyDrawn() { lastLobbyDrawn = GetTickCount64(); }

	std::string SteamMultiplayer::MyName() {
		if (!active) return {};
		const char* name = SteamFriends()->GetPersonaName();
		std::string text = name != nullptr ? name : "";
		SteamAPI_ReleaseCurrentThreadMemory();
		return text;
	}

	void SteamMultiplayer::OnGameTurn() {
		const ULONGLONG now = GetTickCount64();
		lastGameTurn = now;
		const ULONGLONG gap = lastTurnTime != 0 ? now - lastTurnTime : 0;
		lastTurnTime = now;
		if (gap < StallMs || gap > 600000 || stallLines >= MaxStallLines) return;
		++stallLines;
		Logger::Warning("Game stall: no game turn for " + std::to_string(gap) + " ms." + ConnectionText());
	}
	void SteamMultiplayer::OnWorldSaved(std::uint64_t milliseconds) {
		if (milliseconds < 100 || stallLines >= MaxStallLines) return;
		++stallLines;
		Logger::Debug("Game stall: the game's automatic resync save took " + std::to_string(milliseconds) + " ms.");
	}

	void SteamMultiplayer::Install() {
		if (StringUtils::ToLower(Config::GetString("steam_multiplayer", "on")) == "off") {
			Logger::Info("Steam: steam_multiplayer is off, multiplayer uses the network as the original game.");
			return;
		}
		HMODULE network = GetModuleHandleW(L"WEANETR.dll");
		HMODULE winsock = GetModuleHandleW(L"wsock32.dll");
		if (network == nullptr || winsock == nullptr) {
			Logger::Warning("Steam: the game's network library is not loaded, Steam multiplayer is off.");
			return;
		}
		HMODULE library = LoadLibraryW((Paths::Root() / L"steam_api.dll").c_str());
		if (library == nullptr) library = LoadLibraryW(L"steam_api.dll");
		if (library == nullptr) {
			Logger::Info("Steam: no steam_api.dll in the game folder or the KeeperForge2 folder, Steam multiplayer is off.");
			return;
		}
		if (!HasRequiredExports(library)) {
			Logger::Warning("Steam: Steam multiplayer is off.");
			return;
		}
		// Without these Steam would not know which game this is: it was not started by Steam. (SteamAPI_RestartAppIfNecessary,
		// which a Steam game calls first to be restarted through Steam, does not apply: the launcher starts the game.)
		SetEnvironmentVariableA("SteamAppId", AppId);
		SetEnvironmentVariableA("SteamGameId", AppId);
		SteamErrMsg error{};
		const ESteamAPIInitResult started = SteamAPI_InitEx(&error);
		if (started != k_ESteamAPIInitResult_OK) {
			const char* hint = started == k_ESteamAPIInitResult_NoSteamClient ? "is Steam running?"
				: started == k_ESteamAPIInitResult_VersionMismatch ? "Steam or steam_api.dll is out of date; update Steam, and use the steam_api.dll from Steamworks SDK 1.65 or newer."
				: "is Steam running and signed in?";
			Logger::Warning(std::string("Steam: could not start (") + error + "); " + hint + " Steam multiplayer is off.");
			return;
		}
		// Steam's relays are set up now rather than with the first packet.
		SteamNetworkingUtils()->InitRelayNetworkAccess();
		// Windows' timer otherwise rounds the Steam thread's short sleep up to about 16 ms, which every packet then waits.
		timeBeginPeriod(1);
		mySteam = SteamUser()->GetSteamID().ConvertToUint64();
		myAddress = AddressOf(mySteam);
		myHostAddress.s_addr = myAddress;
		myHostAddresses = { reinterpret_cast<char*>(&myHostAddress), nullptr };
		myHostAliases = { nullptr };
		myHost.h_name = const_cast<char*>(OurHostName);
		myHost.h_aliases = myHostAliases.data();
		myHost.h_addrtype = AF_INET;
		myHost.h_length = sizeof(in_addr);
		myHost.h_addr_list = myHostAddresses.data();
		realSockName = reinterpret_cast<SockNameFn>(GetProcAddress(winsock, "getsockname"));
		const bool hooked = realSockName != nullptr
			&& PatchImport(network, reinterpret_cast<void*>(GetProcAddress(winsock, "sendto")), reinterpret_cast<void*>(&SendToDetour), reinterpret_cast<void**>(&realSendTo))
			&& PatchImport(network, reinterpret_cast<void*>(GetProcAddress(winsock, "recvfrom")), reinterpret_cast<void*>(&RecvFromDetour), reinterpret_cast<void**>(&realRecvFrom))
			&& PatchImport(network, reinterpret_cast<void*>(GetProcAddress(winsock, "select")), reinterpret_cast<void*>(&SelectDetour), reinterpret_cast<void**>(&realSelect))
			&& PatchImport(network, reinterpret_cast<void*>(GetProcAddress(winsock, "ioctlsocket")), reinterpret_cast<void*>(&IoctlDetour), reinterpret_cast<void**>(&realIoctl))
			&& PatchImport(network, reinterpret_cast<void*>(GetProcAddress(winsock, "gethostname")), reinterpret_cast<void*>(&HostNameDetour), reinterpret_cast<void**>(&realHostName))
			&& PatchImport(network, reinterpret_cast<void*>(GetProcAddress(winsock, "gethostbyname")), reinterpret_cast<void*>(&HostByNameDetour), reinterpret_cast<void**>(&realHostByName));
		if (!hooked) {
			Logger::Warning("Steam: could not reach the game network library's socket calls, Steam multiplayer is off.");
			return;
		}
		// A friend's lobby to join, when the game was started from a Steam invite (the launcher passes it on).
		char wanted[32]{};
		if (GetEnvironmentVariableA("KEEPERFORGE2_STEAM_LOBBY", wanted, sizeof(wanted)) > 0) wantedLobby = std::strtoull(wanted, nullptr, 10);
		else wantedLobby = LobbyFromConnect(GetCommandLineA());
		const auto lobbyKind = StringUtils::ToLower(Config::GetString("steam_lobby", "public"));
		if (lobbyKind != "public" && lobbyKind != "friends") {
			Logger::Warning("Steam: \"" + lobbyKind + "\" in config.yml is not public or friends; using public and writing that to config.yml.");
			Config::SetString("steam_lobby", "public");
		}
		publicLobbies = lobbyKind != "friends";
		const auto password = Config::GetString("steam_lobby_password", "");
		if (!password.empty()) {
			passwordTag = PasswordTag(password);
			if (passwordTag.empty()) {
				Logger::Warning("Steam: the lobby password could not be hashed, so Steam multiplayer is off.");
				return;
			}
			Logger::Info("Steam: a lobby password is set; only players with the same steam_lobby_password are matched and can play together.");
		}
		if (publicLobbies) Logger::Info("Steam: hosted games are listed for everyone running KeeperForge2; the session list shows them (every 15 s, F5 for at once).");
		active = true;
		if (HANDLE thread = CreateThread(nullptr, 0, &SteamThread, nullptr, 0, nullptr)) CloseHandle(thread);
		const auto bytes = reinterpret_cast<const std::uint8_t*>(&myAddress);
		Logger::Info("Steam: multiplayer through Steam is on; this player is " + std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "."
			+ std::to_string(bytes[2]) + "." + std::to_string(bytes[3]) + " in the game's TCP/IP games.");
		InstallSessionHooks();
	}
}

#else // Built without the Steamworks SDK: Steam multiplayer is off.

namespace KeeperForge2 {
	void SteamMultiplayer::Install() {
		Logger::Info("Steam: this KeeperForge2 was built without the Steamworks SDK, Steam multiplayer is off.");
	}
	void SteamMultiplayer::OnGameTurn() {}
	void SteamMultiplayer::OnWorldSaved(std::uint64_t) {}
	void SteamMultiplayer::SetIdentityProvider(std::string (*)()) {}
	std::vector<SteamMultiplayer::Member> SteamMultiplayer::OtherMembers() { return {}; }
	std::uint64_t SteamMultiplayer::MySteamId() { return 0; }
	bool SteamMultiplayer::SendLobbyMessage(const std::string&) { return false; }
	void SteamMultiplayer::AddLobbyMessageHandler(void (*)(std::uint64_t, const std::string&)) {}
	bool SteamMultiplayer::PostToEveryone(const std::string&) { return false; }
	std::optional<std::int64_t> SteamMultiplayer::ServerTime() { return std::nullopt; }
	void SteamMultiplayer::SetMemberFilter(bool (*)(std::uint64_t)) {}
	void SteamMultiplayer::SetSelfBlocked(bool) {}
	void SteamMultiplayer::SetBlockedAttemptHandler(void (*)()) {}
	void SteamMultiplayer::AddTickHandler(void (*)()) {}
	void SteamMultiplayer::SetStandingPost(const std::string&) {}
	bool SteamMultiplayer::RequestOnlineCount(void (*)(const OnlineCount&)) { return false; }
	bool SteamMultiplayer::RequestPing(void (*)(const PingReport&)) { return false; }
	std::string SteamMultiplayer::NameOf(std::uint64_t) { return {}; }
	bool SteamMultiplayer::IsInLobby(std::uint64_t) { return false; }
	void SteamMultiplayer::ShutOut(std::uint64_t) {}
	void SteamMultiplayer::LeaveLobbyKicked() {}
	void SteamMultiplayer::AddMemberJoinedHandler(void (*)(std::uint64_t)) {}
	void SteamMultiplayer::OnLobbyDrawn() {}
	std::string SteamMultiplayer::MyName() { return {}; }
}

#endif
