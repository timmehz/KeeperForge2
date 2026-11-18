# Steam multiplayer (SteamMultiplayer.cpp)

Multiplayer with Steam friends through the game's own TCP/IP screens. Dungeon
Keeper 2 on Steam is app 2616460; the game does not ship steam_api.dll, so the
player puts a 32-bit one in the game folder or the KeeperForge2 folder.

## The game's network library (WEANETR.dll) [verified in our export]
- Its own service (BullfrogNET in Flame [reference]) is UDP only: socket
  (AF_INET, SOCK_DGRAM), SO_BROADCAST, bind, getsockname, sendto, recvfrom,
  select, ioctlsocket (FIONBIO, FIONREAD). No connect/accept.
- 0x10005D90 area: makes a socket, sets SO_BROADCAST, binds it to its port
  (or 0) and the given address (or any), reads the port back, then takes
  its own address from gethostname and FUN_10005C20, which calls
  gethostbyname and returns the first address.
- The port is 7575 (0x1D97) by default (set in three places).
- Finding games: the session list sends to 255.255.255.255:7575, or to the
  address typed in (inet_addr, else FUN_10005C20 on the text) [verified in
  the C; reference: Flame EnumerateSessions].
- [reference] Session and player records carry each player's IPv4 address
  and port inside the packets, so every machine must see the same address
  for each player.

## How KeeperForge2 carries it over Steam
- Built with the Steamworks SDK (1.65), used as Steam games use it: SteamAPI_InitEx, SteamAPI_RunCallbacks with
  STEAM_CALLBACK handlers and CCallResult for the asynchronous calls (CreateLobby, JoinLobby, RequestLobbyList), and
  the ISteamMatchmaking, ISteamFriends, ISteamUser and ISteamNetworkingMessages interfaces.
- The SDK is not shared (Valve's license): it goes in ThirdParty/steamworks_sdk_165/sdk, which .gitignore leaves out.
  Without it CMake builds the runtime with Steam multiplayer off (KEEPERFORGE2_STEAMWORKS=0); the CMake cache entry
  KEEPERFORGE2_STEAMWORKS_SDK points at another SDK folder.
- steam_api.dll is delay-loaded, so the game starts without it. KeeperForge2 loads it first from the KeeperForge2
  folder, then the game folder; the delay-load then uses that one. Its exports are checked before the first call
  (an older steam_api.dll lacking one would otherwise end the game), and SteamAPI_InitEx checks the interface
  versions. Use the 32-bit steam_api.dll from the SDK's redistributable_bin (Valve allows shipping it with a game).
- SteamAppId / SteamGameId are set before Steam starts, since the game was not started by Steam.
  SteamAPI_RestartAppIfNecessary is not called: the launcher starts the game, not Steam.
- A friends-only lobby (4 players) is made at start. Rich presence, as Steam games set it: "connect"
  ("+connect_lobby <id>", so the Steam friends list and overlay offer Invite to Game / Join Game),
  "steam_player_group" (the lobby id) and "steam_player_group_size" (members), which show the lobby's members together
  in the friends list, and "status". Accepting an invite while playing (GameLobbyJoinRequested_t /
  GameRichPresenceJoinRequested_t) leaves our lobby and joins the friend's. Started from an invite, Steam passes
  "+connect_lobby <id>" to the launcher, which hands it to the runtime in KEEPERFORGE2_STEAM_LOBBY.
- Invite key: F6, with the game in front, on the multiplayer screens only (the session list, open while its
  broadcasts keep coming, or the multiplayer lobby, known from the lobby chat's drawing 0x544FA0 that CreatorName
  hooks; both count for 3 s after). It opens Steam's invite window for our lobby
  (ActivateGameOverlayInviteDialog), as a Steam game's "Invite friends" button does. That window only shows through
  the Steam overlay; when SteamUtils IsOverlayEnabled is false (the overlay is not drawn into the game, or is turned
  off) Steam's friends list is opened instead (steam://open/friends), where Invite to Game works the same.
  [unverified] Whether the overlay draws over our DirectX 11/12 and Vulkan renderers when the game is not started by
  Steam. The game's own keys on those screens were not checked for F6 [unverified].
- Members are refreshed on Steam's lobby updates (LobbyChatUpdate_t: someone came or went; LobbyDataUpdate_t: lobby or
  member data changed, such as a new member's password tag), and once a second besides.
- A broken session with a player (SteamNetworkingMessagesSessionFailed_t) is logged with Steam's reason (first 10).
- Game threads that read Steam's text (member names and data, for ModCheck and the creator's name) call
  SteamAPI_ReleaseCurrentThreadMemory afterwards, as Steam asks of threads that do not run its callbacks.
- Addresses: each lobby member is 10.a.b.c, from the low 24 bits of their
  Steam account id, the same on every machine. The game is told its machine
  is "keeperforge2-steam", whose address is the player's own 10.a.b.c.
- WEANETR.dll's import slots for sendto, recvfrom, select, ioctlsocket,
  gethostname and gethostbyname point at ours (the game's other code and
  other programs are not touched):
  - sendto to a 10.x member or to 255.255.255.255 (every member, this
    machine included, as on a LAN) goes over Steam (unreliable, no delay)
    with the sender's and receiver's ports in front; other addresses use the
    network as before;
  - packets from Steam wait by the port they were sent to; recvfrom hands
    them out first with the sender's 10.x address and port; select reports
    those sockets readable (it waits in 2 ms steps); FIONREAD includes them.
  - [verified in our export] The session list thread (0x10005000) and the join
    thread (0x10004C30) set their sockets to blocking (FIONBIO 0, the only two
    FIONBIO calls) and call recvfrom with no select first. So our recvfrom waits
    in 2 ms steps for either a packet from Steam or the real socket; closing
    the socket, which is how the library stops those threads, ends the wait.
  - Only lobby members are accepted as senders.
- A thread of ours runs Steam's callbacks (SteamAPI_RunCallbacks; every lobby
  call is made there) and collects packets every 2 ms.

## Stalls in a game (freezes that then go on)
Three things stop a lockstep game, and the log now tells them apart. Reported, cause
not known yet: pauses with a friend (one log: 1000 frames usually 30 s, up to 43 s,
worse late in the game, none over 10 s).
- [reference; the save function's start checked in our export] Every 30 game seconds each machine saves the
  whole world (0x50FD10) in the middle of a frame, for resync. It grows with the
  dungeon. All machines must save at the same turn and replay from it, so its
  interval was not changed: the history kept for replay was not checked. Log:
  "the game's automatic resync save took N ms" from 100 ms.
- Out of step: seed or checksum differ. Each machine loads its save and replays. The
  game's own log lines (0x5595C0) with "Sync" now reach ours as "Game: ...".
- Waiting on the network. "Game stall: no game turn for N ms" from 750 ms, with
  Steam's ping and quality for each other player.
Changes made without knowing which it is:
- Packets are reliable and in order (flags 8 | 1 | 32) instead of unreliable: a lost one
  no longer needs the game to notice a gap and ask. [unverified in a real game]
- timeBeginPeriod(1): the Steam thread's Sleep(2) could take about 16 ms.
- Steam's relays are started at once when the library offers that (optional names).
- The possession navigation fix (0x40E910, 0x6EC9E4) applies to all multiplayer games.

## What each player runs (lobby member data)
- On entering a lobby (made or joined), the player's KeeperForge2 version, build hash and mods fingerprint are set as
  lobby member data "kf2" (ISteamMatchmaking SetLobbyMemberData). ModCheck reads every other member's
  (GetLobbyMemberData) and their names (ISteamFriends GetFriendPersonaName) at level start. These three are optional:
  without them there is no comparison through Steam. See docs/api/mods.md.

## The creator's name (CreatorName.cpp)
- KeeperForge2's creator is known by their Steam account (a 64-bit id in the code). Every machine checks whether that
  account is itself or a member of its Steam lobby (Steam vouches for members' ids), and takes that account's Steam
  name, cut to the game's 16 characters. A player whose game name is that name is drawn in rainbow colors (red,
  orange, yellow, green, blue, indigo, violet, a step every 0.75 s, then again): copying the name alone does nothing.
  Game names are the Steam names when steam_player_name is on.
- [verified code] In-game player list (0x40A0F0, multiplayer, while shown; redrawn each frame): rows "number: name"
  from player + 0xEEA; the color pushed at 0x40A1FB is replaced for the creator's row.
- [verified code] In-game chat (0x409F30): a color is 5 bytes { blue, green, red, alpha, palette index } (the game's
  table at 0x756F34, made by 0x559820: red, green, blue, black, white); senders 0x8181, 0x8383 and 0x8282 pick red,
  green and blue, others white. At 0x40A00C a line whose text starts with the creator's name gets the rainbow color
  (the whole line), and the chat box, drawn into its own picture only when + 0x1F39 is set, is marked changed at each
  step while such a line is shown. [unverified] That a player's chat line starts with their name.
- [verified code] Multiplayer lobby (0x5445F0): each slot's row is drawn in that slot's player color (table 0x6E5058,
  12 bytes per entry, by slot + 3), set through 0x62C3C0 by the call at 0x544C1D. The call is redirected so a human slot
  (slots at 0x73F5C8, 0x3F bytes: wide name at + 0) named as the creator gets the rainbow; the whole row is colored.
  [unverified] That the lobby is redrawn often enough for the color to change every step.
- [verified code] Multiplayer lobby chat (button 0x1CD, drawn by 0x544FA0, which the export did not disassemble): each
  line is the sender's lobby slot as one digit (read by 0x636990), then the text. The line is drawn in the sender's
  player color plus 0x20 per channel, set through 0x62C3C0 by the call at 0x5451D1 with the text in ESI. That call is
  redirected so lines from the creator's slot get the rainbow (the whole line). [verified] The first try only colored
  the in-game chat, so the lobby chat kept the player color.

- Ranks (PlayerRanks.cpp holds the Steam account lists): None -> Friend -> Admin -> Creator. Admins and the creator's
  friends are checked the same way as the creator. In the same places, admins' names and chat lines are drawn in dark
  red (#8B0000) and friends' in gold; neither is animated. An account in two lists gets the higher rank.
  - Join and leave notices for admins too: "SYSTEM: Admin of KeeperForge2 has joined the lobby!" (or "has left"),
    white like the others.
  - Moderation rule (PlayerRanks::CanModerate): a player may moderate only players of a lower rank, never
    themselves. Admins: None and Friends. The creator: None, Friends and Admins. Nobody: the creator.
  - Ranks can be changed in the game with the console's rank command, sent to the lobby as Steam lobby chat
    messages; see docs/api/console-commands.md.
- KeeperForge2's own messages: SendLobbyMessage (Steam lobby chat, to our lobby) and PostToEveryone (every
  KeeperForge2 player on Steam: an invisible notice board lobby plus direct questions on Steam networking channel 1;
  details under the notify command in docs/api/console-commands.md). Both give receivers the sender's account as
  Steam reports it. [unverified in game] That a player can keep an invisible lobby besides their normal one (the SDK
  says one regular and up to two invisible).
  - Commands to come must be checked on the receiving machine, against the sender's Steam account as the Steam lobby
    gives it, never against a name or a rank sent in the message.
- [verified code; unverified in game] Lobby chat notices. The lobby chat handler 0x542BF0 (cdecl: sender, message,
  front end) is the network's chat callback (0x7581C0, its front end at 0x7581C4, set at 0x542647 while the lobby is
  up; called at 0x559F01). It adds "<sender's slot digit><slot's name>: <text>" to the chat (front end + 0x30CAB, wide
  lines ended by 0xFFFF, its size in bytes at + 0x30CAF; rows per line at + 0x2E5F). The message is one digit n, then
  n digits of a number that must equal the word at 0x759C35, then the text. The sender must be below the player count
  (front end + 0x5AAC, a byte), or 0xFFFC / 0xFFFE. Before each draw of the lobby chat (0x544FA0, hooked), the human
  slots are compared with the last look: when the creator or a friend appears or goes, the handler is called as slot 0
  with slot 0's name swapped for "SYSTEM" for the call, giving "SYSTEM: Creator of KeeperForge2 has joined the lobby!"
  (or "has left", or "Friend of Creator of KeeperForge2 ...", or "Admin of KeeperForge2 ..."). Only on each machine; nothing is sent. The notices are
  drawn in white (easier to read than the rainbow or gold they had first). After 3 s without the lobby being drawn, everyone counts as new.

## Public lobbies (steam_lobby, default public)
- Steam app 480 (Steam's shared test app). Every machine starts in a friends-only lobby of its own (for invites).
- Hosting: the game's host call (0x55A5B0, hooked) first waits (up to 5 s) for the Steam thread to make the host's lobby
  public (type 2) and listed: lobby data "keeperforge2" = "1" and "kf2host" = the host's Steam id. If the lobby the host
  is in belongs to someone else, a new lobby is made for the game. A lobby we come to own whose "kf2host" is someone
  else (its host left) is unlisted ("keeperforge2" = "0").
- The session list: while the game broadcasts for games (the session list is open; broadcasts within the last 3 s),
  Steam's lobby list is asked every 15 s, and at once on F5 with the game in front (at most once per 5 s), for lobbies
  with "keeperforge2" = "1", our password tag and a free slot, worldwide. Each lobby's "kf2host" is kept by address.
  The game's broadcasts then also go to every listed host, so each hosted game answers and shows in the game's own list.
  The game has no refresh button there (window 11: Create, Join, Exit) [reference: Flame], hence F5.
- [reference: Flame's network library notes; unverified in our export] Looking for games only broadcasts; the host
  answers straight to the asker with its game's description; only joining sends straight to a host.
- Packets carry, after the two ports, a kind (0 normal, 1 discovery: a broadcast or the answer to one) and the first 8
  characters of the sender's password tag. From outside our lobby only discovery packets with our tag are taken: from
  a listed host (its answer), or from a player looking for games (remembered for 60 s, so our game's answer can go back).
  Session requests are accepted from anyone; packets are checked one by one.
- Joining: the first packet the game sends straight to a listed host makes the Steam thread join that host's lobby
  (the sending thread waits up to 5 s); then the packets go as to any member. A player who just joined may send before
  the host's member list has them: the host refreshes its list (at most every 100 ms) and takes the packet then.
- friends gives the old friends-only lobby with no listing.
- Failing to join a lobby makes a new one (it used to leave the player without a lobby).

## Lobby password (steam_lobby_password, default empty)
- The password is never sent: its tag is the SHA-256 (Windows' BCrypt) of a fixed prefix and the password, in hex, or
  "none" without one. The tag is put on the lobby by the machine that makes it and on each player in each lobby it
  enters (lobby and member data "kf2pw").
- Public searches add a filter on the tag, so players only find lobbies with the same password (or with none).
- A lobby entered (an invite, Join Game, a search) whose tag is not ours is left at once and a lobby of our own is made.
- Members whose tag is not ours are left out of the member list: their packets are ignored, ours and broadcasts do not
  go to them, and they do not count for ModCheck or the creator's name. Members with no tag count as "none".
- Limits: anyone who is in the Steam lobby can read the tag in Steam's member data, so a changed KeeperForge2 could copy
  it; it keeps out everyone else, not a determined cheater. A short password could be guessed from its tag offline.
- Not tested.
- [unverified] Whether GetNumLobbyMembers gives counts for lobbies from a search (Steamworks says lobby list results
  carry them). Not tested.

## Defaults
`steam_multiplayer` and `steam_player_name` default to on only when a steam_api.dll is in the game folder (the
working directory) or the KeeperForge2 folder when config.yml is loaded (Config::Load, by both the launcher and the
runtime), else off. Only the default changes: a value already in config.yml is kept.

## Steam has to be running
With `steam_multiplayer` on, the launcher looks for a steam.exe process once at
start. If there is none, it shows an error and closes before starting the game.

## The player's name (`steam_player_name`, default on)
- [verified] The name the player typed is wide text at 0x73FE68, from the game's
  allocator 0x5B75F0 (freed with 0x6341B0). Hosting (0x55A5B0) and joining
  (0x55A650) pass it to the network library. The caller then saves it to the
  registry as "Multiplayer Name" (0x561F60) and copies it into the lobby slot.
- With this on, both calls are hooked and the kept name is swapped for the Steam
  name just before them. Saving that name is skipped, so the typed name stays
  saved for when the setting is off.
- A lobby slot has 0x22 bytes before its ping (+0x22), so the name is cut to 16
  characters. Characters the game's fonts lack may not draw [unverified].
- Not tested yet.

## Use
- Put steam_api.dll (32-bit, from Steamworks SDK 1.65 or newer) in the game
  folder or KeeperForge2, have Steam running, and start the game with the
  launcher. The log says
  "Steam: lobby ready".
- Invite a friend with F6 on the multiplayer screens, or from the Steam
  friends list (Invite to Game).
  When they are in your lobby ("2 player(s) in the lobby"), one of you
  hosts a TCP/IP game and the other finds it in the session list.
- steam_multiplayer: off in config.yml gives the original networking. While
  on, TCP/IP games are Steam only.

## Not tested yet
- Joining a game from start to finish. Tested so far: joining the Steam lobby
  works (log: 2 players). Open questions: whether the game's session list
  shows a game found through a broadcast reply from 10.x; Steam overlay with
  our renderers when the game is not started by Steam.
- The log says when the first packet is sent and received, and when sending
  fails, to tell a Steam problem from a problem in the game's own join steps.
