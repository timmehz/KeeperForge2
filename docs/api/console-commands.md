# Console commands

The launcher's console window (which the game's runtime writes its log to as well) takes commands: type one and press
Enter. The answer is printed there and added to the runtime's log, whatever `log_level` is.

| Command | What it shows |
|---|---|
| `cpu` | the game's processor use |
| `ram` (or `memory`) | the game's memory use |
| `gpu` (or `vram`) | the game's graphics card use: load and video memory |
| `fps` (or `ms`, `frametime`) | frames per second and frame time (ms) |
| `stats` (or `all`) | all four |
| `reset` | starts every figure again from now; `reset cpu`, `reset ram`, `reset gpu`, `reset fps` (or `fps reset`...) for one |
| `online` | how many KeeperForge2 players are online over Steam: in a multiplayer game, in a multiplayer lobby, elsewhere |
| `ping` | latency in ms to Steam's nearest servers, and to each player in your Steam lobby |
| `help` | the list |
| `rank <SteamID64> <none\|friend\|admin\|creator>` | moderation: sets a player's rank for everyone in the Steam lobby |
| `ranks` (or `rank` alone) | the rank of everyone in the Steam lobby, and the ranks set this session |
| `notify <system\|alert> <message>` | moderation: a message to every KeeperForge2 player on Steam, in any lobby |
| `kick <SteamID64>` | moderation: kicks a player from the Steam lobby |
| `ban <SteamID64>` | moderation: bans a player from KeeperForge2 lobbies for good |
| `tempban <time> <SteamID64>` | moderation: bans for a time, such as `tempban 3D 3h 20m <id>` |
| `unban <SteamID64>` | moderation: lifts a ban |
| `steamid64 <userid>` | moderation: the SteamID64 of a Steam profile link name, and of lobby members with that Steam name |
| `bans` | moderation: who is banned, for how long, and by whom (Admins and the Creator only) |

`help` lists the moderation commands (rank, notify, kick, ban, tempban, unban, bans) only to Admins and the Creator,
by this machine's ranks; None and Friends see the others only. Commands are not case sensitive. The main figures are given as `minimum | maximum | current | average`, measured once
a second since the game started or since that part's last `reset`. Nothing here touches the simulation, so it changes
nothing in multiplayer.

## What each one measures
- **cpu**: the game's processor time over the time passed, as a share of every logical processor (as Task Manager
  shows it). Also the current share of one logical processor (the game does most of its work on one thread, see
  [cpu-threads.md](../game/cpu-threads.md)) and the whole PC's current use.
- **ram**: the game's working set (its memory in RAM, minimum to average). Also its committed private memory, its
  32-bit address space (used, limit, largest free block; from MemoryWatch: a 32-bit game runs out of address space
  before it runs out of RAM), and the whole PC's RAM in use.
- **gpu**: the graphics card the game uses most (by name).
  - load: the busiest of the game's GPU engines, from Windows' "GPU Engine" performance counters, which Task Manager
    reads too.
  - VRAM (minimum to average): the card's own memory the game uses, from DXGI's per-process video memory figures.
    These count whatever draws the game: DirectX 11, DirectX 12, Vulkan or the original renderer.
  - Also the shared system memory the card uses for the game, and the VRAM budget Windows gives the game now, beside
    the card's own memory.
  - Both need Windows 10 or later; where one is missing, the command says so.
- **fps**: frames shown in each second, counted in FrameRate's present hook. Seconds when the player cannot see the
  game are left out, as it may run at 1 fps then (BackgroundRunning). So are seconds without any frame (loading). The
  command says how many were left out, and the `frame_rate` set in config.yml. Without the present hook (unexpected
  game code) there are no figures.
  Frame time is the time from one frame shown to the next, so a frame limiter's wait is part of it:
  - minimum and maximum: the shortest and longest single frame. The maximum shows a stutter that a whole second's
    fps hides. Frames of a second or more (loading, a pause) are not counted.
  - current: the last second's average.
  - average: all time counted over all frames counted.
  - It is the whole frame (the game's work, drawing, the wait), not the graphics card's own time. That would need
    timing queries in each renderer.

## How it works
- ConsoleCommands.cpp opens the console's input (`CONIN$`) after the logger has attached the game to the launcher's
  console. It reads whole lines on a thread of its own. When the game was not started by the launcher there is no
  console, and no commands. The launcher itself never reads the console, so only the game gets the lines. Keys
  pressed before the game starts are thrown away.
- PerformanceStats.cpp samples every second on a thread of its own, at below-normal priority.
- Sampling uses local clocks. That is fine here, since none of it is simulation code.
- Log lines can appear while a command is being typed. The typed text is still taken as a whole when Enter is pressed.

## Ranks and the rank command (PlayerRanks.cpp)
- Ranks, lowest first: None, Friend, Admin, Creator. The built-in lists (Steam accounts) are in PlayerRanks.cpp. A rank
  changes the name color in multiplayer (CreatorName: rainbow, dark red, gold) and what the player may do.
- `rank <SteamID64> <rank>`: the id is the 17-digit SteamID64. Who may set what:
  - Admins: only players who are None or Friend now, and only to None or Friend. Not other Admins, not themselves,
    not the Creator.
  - The Creator: everyone else, to None, Friend or Admin. There is only one Creator, so nobody can be made Creator.
  - The Creator's own rank: only the Creator's account can set it, to any rank. That account keeps the Creator's
    rights whatever rank it shows (PlayerRanks::AuthorityOf): every command, the moderation commands and help's
    moderation list included, and setting itself back to Creator or any rank. Every machine judges it so, as the
    Creator's account is built into KeeperForge2. Nobody can kick it (except itself), ban it, or change its rank, and
    Bans::IsBanned never counts it as banned, whatever a bans.dat or a message says.
  - None and Friends: nothing.
- How it reaches everyone: the command goes as a Steam lobby chat message "kf2rank 2 <id> <rank number> <time>"
  (SteamMultiplayer::SendLobbyMessage; the game never shows Steam lobby chat). Steam tells each receiver which
  account sent it, so a message cannot claim to be from someone else. Every machine, the sender's included, checks
  with its own ranks that the sender may make the change, then applies it and prints it. Refused changes are logged.
- Saved: every machine keeps each rank change it took in `KeeperForge2\ranks.dat` ("<SteamID64> <rank> <time>" per
  line) and loads it at start, so ranks last between games. Each change carries when it was made (seconds since 1970
  UTC, the issuing machine's clock). For each player, the newest change wins, wherever it comes from; in the same
  second, the higher rank wins. A machine that missed changes therefore cannot bring an old rank back. A change back
  to the built-in rank is kept as well, for its time. Changes dated more than 5 minutes ahead of the receiver's clock
  are refused. The command never dates a change older than the one it replaces, even with a clock behind.
- Keeping machines in step: 3 s after a player joins the Steam lobby (another player, or this machine), each Admin or
  Creator there sends every saved rank it may set itself, with its time (admins first). Every receiver checks it and
  keeps it only when it is newer. A change only an absent moderator could make (for example the Creator's) reaches
  others once that moderator is in a lobby with them again.
- ranks.dat is sealed with Windows' data protection (CryptProtectData, with KeeperForge2's own extra key text):
  only the same Windows account on the same PC can open it, and a file changed by hand no longer opens. Such a file
  is not used (a warning says so), and the ranks come back from the Steam lobby's moderators.
- What really protects ranks is that every machine checks every change against its own ranks: a player who gets
  around the seal, or changes their own copy of KeeperForge2, fools only their own machine; their changes are refused
  everywhere else.
- Ranks only work with Steam multiplayer (they need the Steam lobby to send through and to know who is who).

## The notify command (Notices.cpp)
- `notify system <message>` or `notify alert <message>`: Admins and the Creator only. The message keeps its case and
  spaces; at most 100 characters, printable only.
- Shown on every machine as `SYSTEM: <message>` in white or `ALERT: <message>` in red. It appears once, in the
  multiplayer lobby's chat or the in-game chat (whichever is on screen first), and is printed in the console. A
  notice nothing could show within 5 minutes (no lobby or level on screen) is dropped.
  - Lobby chat: added like the join notices, as slot 0 named SYSTEM or ALERT.
  - In-game chat: through the game's local chat line call (0x409EC0) with its special sender 0x8181.
  - The color comes from matching the shown line against the notices shown.
- Message: "kf2notify 1 <id> <system|alert> <text>", the id being "<sender's SteamID64>-<time>-<count>".
- Every receiver refuses a notice when:
  - its id does not start with the account Steam says sent it;
  - its time is more than 10 minutes old, or more than 5 minutes ahead;
  - by its own ranks, the sender is not an Admin or the Creator.
  Ids already taken are skipped.
- How it reaches every lobby (SteamMultiplayer::PostToEveryone):
  - At once to the sender's own Steam lobby (lobby chat message).
  - For 5 minutes on a notice board: the sender keeps an invisible Steam lobby (allowed besides the normal one, not
    shown to friends) with data `kf2notice` = 1, `kf2nfrom` = its id and `kf2nid` = its last 5 message ids.
  - Every KeeperForge2 machine with Steam searches for these every 30 s, worldwide, never during the session list's
    own search. For each id not seen yet (ids must start with the named poster's account), it asks the poster
    directly over Steam networking (channel 1, "kf2nq <id>"). The poster answers "kf2na <id> <message>". Only an
    answer from the account that was asked is taken, so Steam proves who sent it.
  - The board holds no text: anyone can make such a lobby, and nobody joins it.
  - A machine with messages up accepts Steam networking sessions from anyone, so it can answer.
  - Reaches every lobby (public, friends only, invited) and players in none, within about 30 s.

## The kick command (ModerationCommands.cpp)
- `kick <SteamID64>`: the player must be in your Steam lobby (or be you). Who may kick whom (PlayerRanks::CanKick):
  - Admins: None, Friends and themselves. Not other Admins, not the Creator.
  - The Creator: None, Friends, Admins and themselves.
  - None and Friends: nobody.
- The kick goes to the lobby as "kf2kick 1 <id>". Every machine checks it with its own ranks (refusals are logged),
  then acts on its own:
  - The kicked player's machine shows "ALERT: You were kicked from the lobby by <name>". It leaves the Steam lobby,
    after sending what was waiting, and makes a new lobby of its own.
  - Every other machine shuts the player out (SteamMultiplayer::ShutOut): left out of the member list, so no game
    packets either way and no KeeperForge2 messages from them. It shows "SYSTEM: <name> was kicked from the lobby
    by <name>".
  - A shut-out player is let back in once Steam reports them gone from the lobby (left, disconnected, kicked or
    banned), so they can come back later through an invite. A changed copy of KeeperForge2 that does not leave stays
    shut out.
- The kicker's machine acts at once and skips its own message when Steam brings it back. Kicking yourself just
  leaves the lobby.
- In a game being played, the kicked player stops reaching the others; the game itself then deals with the lost
  player. [unverified in game]

## Bans: ban, tempban, unban, bans (Bans.cpp)
- Who may ban, tempban or unban whom: PlayerRanks::CanModerate, only players of a lower rank and never themselves.
  Admins: None and Friends. The Creator: Admins too. Nobody: the Creator.
- `tempban <time> <SteamID64>`: the time is one or more parts, a number and a unit, in any order. Units: `s` seconds,
  `m` minutes, `h` hours, `D` days, `M` months (30 days), `Y` years (365 days). Case matters: `m` is minutes, `M`
  months. At most 100 years. Example: `tempban 3D 3h 20m 76561198000000000`.
- The clock is Steam's server time (ISteamUtils::GetServerRealTime, with SteamUser::BLoggedOn to know Steam is
  reachable), the same for every player whatever their PC's clock says.
  - A tempban ends at a moment on that clock.
  - While Steam cannot be reached, a tempban still counts on that machine (its end cannot be checked), and `tempban`
    refuses to start one. Steam's clock keeps running meanwhile, so no time is lost or gained.
  - Rank changes and notices are dated with Steam's clock too, when it can be read.
- A ban change goes as "kf2ban 1 <id> <until> <time>": until 0 means for good, -1 means lifted, otherwise the end on
  Steam's clock. It goes to this lobby at once and to every KeeperForge2 player through the notice board
  (PostToEveryone, 5 minutes).
- Every machine checks it with its own ranks, then keeps the newest change for each player (same second: the longer
  ban). These are saved in `KeeperForge2\bans.dat`, sealed like ranks.dat (SealedFile.cpp). 3 s after anyone
  joins a lobby, every moderator there passes on the bans it may set itself, lifted ones too.
- Enforcing it:
  - Every machine leaves banned players out of its lobby (SteamMultiplayer's member filter): no game packets either
    way, no KeeperForge2 messages from them.
  - A banned player's own machine turns Steam multiplayer off while its ban lasts (checked every second;
    SteamMultiplayer::SetSelfBlocked, also before the first lobby at start). It leaves its lobby and makes none. It
    does not join lobbies: not through invites, Join Game, or the session list. It does not host. No game packets go
    out or come in. It shows a red alert ("You are banned ... from KeeperForge2 lobbies ... Steam multiplayer is off
    until the ban ends") when the ban starts, and again at each try to join or host (at most every 10 s). When the
    ban ends (tempban over, or unban) it shows so, makes its own lobby again, and everything works as before.
  - So a banned player cannot play with anyone over Steam, even with players who have never heard of the ban.
  - A changed copy of KeeperForge2 that ignores its ban is still left out by everyone who knows it.
- Standing ban lists: every Admin and the Creator online keeps, on the notice board, a list of the bans it may set
  (lifted ones too, newest first, as many as fit in one message: "kf2banlist 1 <id>:<until>:<time> ...").
  SteamMultiplayer::SetStandingPost keeps it up as long as it is set; it gets a new id when it changes, so every
  machine asks for it again. It is remade within a second of a ban change, and every 5 minutes (ranks may have
  changed). Every machine reads these lists every 30 s, banned machines too, as they have no lobby. So an unban, or
  a ban, reaches a player within about 30 s of a moderator being online, whatever lobby either is in.
- Limits: there is no server, so bans travel between machines. A player's ban reaches them only once a moderator
  who may set it is online, or was in a lobby with them since. A player who deletes their bans.dat is banned again
  as soon as a moderator's standing list reaches them.

## The online command (SteamMultiplayer::RequestOnlineCount)
- Anyone can use it. It prints how many KeeperForge2 players are online over Steam (you included), and how many of
  them are in a multiplayer game, in a multiplayer lobby, or elsewhere in the game (menus, campaign, skirmish).
- Steam has no count of its own for this: the game runs as Steam's shared test app 480, whose player count covers
  every game using it. Instead:
  - Every machine with Steam multiplayer on, unless banned, keeps an invisible lobby of its own (a player may be in
    up to two besides their normal lobby; the other one is the notice board).
  - Its data is `kf2online` = 1, `kf2pfrom` = its Steam id and `kf2pstate` = game, lobby or menu.
  - "game" means a multiplayer game turn in the last 5 s; "lobby" means the multiplayer lobby screen drawn in the
    last 5 s. The data is updated when this changes.
  - `online` runs one worldwide lobby search for these (up to 500 results) and counts one per Steam account.
- Nobody joins these lobbies, so no search result can pull a player out of their own lobby. Anyone could make fake
  ones and raise the count; that does no other harm.
- At 500 results the answer says "at least". A banned account has none and cannot count.

## The ping command (SteamMultiplayer::RequestPing)
- Anyone can use it, banned accounts too (it only measures). It prints the round trip time in ms to Steam's 5
  nearest data centers, and to each other member of your Steam lobby.
- Steam's relay network (Steam Datagram Relay, started with `InitRelayNetworkAccess` when Steam multiplayer starts)
  measures its ping to each of Steam's data centers itself. `ping` asks it to measure again now
  (`CheckPingDataUpToDate(0)`), waits on the Steam thread until the measurement has run (at most 15 s; after that
  the last figures are shown and marked so), then reads `GetPingToDataCenter` for each data center.
- Each line is the data center's short code (such as ams or iad) and the best route's time. When the best route goes
  in through another data center, that one is named. When Steam also measured the straight route and it differs,
  that time is shown too.
- Lobby members: Steam's own ping figure for our networking session with each (`GetSessionConnectionInfo`); "no
  connection yet" before any packets have gone between you.

## The steamid64 command (ModerationCommands.cpp)
- `steamid64 <userid>`: Admins and the Creator only. The userid is the name in a Steam profile link
  (steamcommunity.com/id/<userid>); a whole link works too.
- That name is a Steam custom URL, which Steam keeps unique, so it gives at most one account. It is looked up on
  Steam's public profile page `https://steamcommunity.com/id/<userid>/?xml=1`: its `steamID64` element, and
  `steamID` (the shown name). WinHTTP is used, on a thread of its own, with 10 s timeouts and no key. The answer is
  printed when it comes. A profile without a custom URL has no such name: its link already holds the SteamID64.
- Steam names (the names shown) are not unique, so it also lists every member of your Steam lobby whose name is the
  text (any case, spaces kept), each with their SteamID64.
- What a profile shows can be changed by its owner, and the lookup only reads what Steam's page says; the SteamID64
  itself is Steam's own.

Not built or tested yet. [unverified]
