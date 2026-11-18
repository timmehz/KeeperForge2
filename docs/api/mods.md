# Mods

## Where mods go
```
KeeperForge2\mods\
  <Mod Name>\          any folder name, for the player only
    <mod_name>.dll     the mod; known by this file name
    config.yml         the mod's settings (created if missing)
    (other .dll files) the mod's own libraries, if any
```
- Each mod is its own project, built into its own DLL; KeeperForge2 has no
  mods built in.
- The runtime (injected by the launcher) loads the mods when it starts
  (ModManager::Initialize). Folders, then the DLLs
  in each, are taken in sorted order, so the order is the same on every
  machine.
- Each DLL is loaded with its own folder searched for its libraries. A DLL
  that exports `KeeperForge2_CreateMod` is a mod; others are left loaded
  as libraries.
- Two mods with the same DLL name: the second is skipped with a warning.
- Any DLL in there runs inside the game with full rights. Only install mods
  you trust.
- Not done yet: turning mods on and off. Players' mods are compared in
  multiplayer (see "Same mods for every player").

## Game data (mods and Flame packs)
- A mod folder may hold a `data\` folder with files at the same paths as the game's `Data` folder
  (`KeeperForge2\mods\<Mod Name>\data\Meshes\...`, `...\data\editor\Creatures.kwd`, ...). A mod may be data only (no DLL).
- Flame resource packs are read too, as they are: `flame\resources\<pack>\data\...` in the game folder. config.yml
  `flame_packs`: `all` (default: every pack folder, the first by name wins), `off`, or pack names joined by `;` (the
  first named wins, as in Flame's own list).
- Which file wins: a mod over a Flame pack over the game; among mods, the later one by folder name.
- What is covered (ModData.cpp):
  - the game's file lists for Meshes, EngineTextures, sprite, frontend, paths, editor, text, Texture and palette: our
    folders are put in front of the game's own after it has made its lists (0x55C700);
  - files the game opens by path for reading (0x5B3980): level files and the shared lists such as Creatures.kwd. A
    path under the game's `Data` folder is taken from the first mod or pack holding the same file.
- Not covered yet: sounds and music (read by their own paths), and KeeperForge2's own level editor, which reads level
  files itself (LevelFiles) and still sees the game's.
- The log says which mods and packs give files (Info); each folder and the first file taken from one are at Debug.

## Adding creature types (ModContent.h)
A mod DLL may also export `KeeperForge2_RegisterContent`. It is called once, right after the mod's `Initialize`, in the
mods' load order (folders sorted by name), before the game has read any of its data:
```cpp
#include "IMod.h"
#include "ModContent.h"
KEEPERFORGE2_MOD_EXPORT void KeeperForge2_RegisterContent(KeeperForge2::IModContent& content) {
	auto& creatures = content.Creatures();
	// A new type made from the Dark Angel (23), with its own name.
	if (auto* angel = creatures.AddCopy(23, "Fallen Angel")) {
		std::uint8_t* record = angel->GetRecord();
		// Change fields in place; the layout is in docs/game/creature-types.md (health, flags, attractions, art names...).
	}
}
```
- `AddCopy(from, name)` copies a type (a base type, or one added before) under a new name (up to 31 characters).
  `AddRecord(bytes, size)` adds a whole record as a creature list holds it (cut or filled with zeros to the list's
  record size). `Find(name)` finds a type any mod added. `GetBaseTypeCount()` is how many the game's own list has.
- A type's id is its place in the list from 1: the base list's count plus its place among the added ones. With the
  same mods in the same order every machine gives the same ids. Ids are bytes, so there are at most 255 types.
- How it reaches the game (CreatureRegistry.cpp): the base list is the Creatures.kwd the game would read, with mods'
  and Flame packs' data folders (see above). When the game opens `Data\editor\Creatures.kwd` it gets
  `KeeperForge2\cache\Creatures.kwd`, written once with the added types at the end. Types added after that are left
  out with a warning.
- The art (meshes, textures, portraits), text and sounds a new type names come from the mod's data folder.
- Portals: types past the 46th get places in each player's portal table (see docs/game/creature-types.md).
- A level that carries its own creature list (a `...Creatures.kld` or `...Globals.kld` its file list names, in place
  of the shared list) gets the added types too: when the game opens such a file, it gets a copy in
  `KeeperForge2\cache\levels` with the added types after the level's own, made again at each open. A type the list
  already has by name (case ignored) is not added again: a level our editor saved with the mods' types in it keeps its
  own copy of them. Ids follow the list's own types, so in a level with more types of its own than the shared list a
  mod's type has another id there (a warning names it); `GetId()` is its id in the shared list. Lists are only
  recognized by those two file name endings. [unverified in game]
- Our level editor shows the added types the same way (shared list as the game reads it, from mods' and Flame packs'
  data folders, or the level's own list, then the added types), with the icon of the stock type a copy was made from.
  They are written into the level only when its creature types are changed and saved. Added types cannot be deleted
  in the editor (they would come back).
- Every player having the same mods is checked by ModCheck (below).

## Same mods for every player (multiplayer)
- ModCheck.cpp knows this machine's KeeperForge2 version (from the build), a hash of its KeeperForge2 DLL (another
  build of the same version may play differently) and a fingerprint of what mods change in the game: every mod DLL
  (folder and file name, contents) and the editor data (creature lists, levels: any file under the data folder's
  editor folder) of every mod and Flame pack in use, in priority order. Art, text and sounds are left out: they do not
  put the game out of step.
- The host writes them into the lobby's creature list with the session options (the free bytes of rows 1 to 4, the
  game's own creature rows; bit 0x40 of the first row's options byte says they are there), so they reach every player.
- Steam games: each machine also puts them on itself in the Steam lobby (lobby member data "kf2":
  "v=1.2.3;b=XXXXXXXX;m=XXXXXXXX", SteamMultiplayer.cpp). When the level starts, every machine (the host too) compares
  with every other member of the Steam lobby and the chat names who differs (at most two lines about players, then
  "any of them in this game will put it out of step"); the log has every difference. Members who are in the Steam
  lobby but not in this game are compared too. Without a Steam lobby, the lobby rows below are used (joining players
  only).
- Each machine logs its own (Info). A player who joins logs what differs (warnings), and when the level starts the
  game's chat shows it: the host's KeeperForge2 version when it is not the same, another build of the same version,
  mods or Flame packs not the same, and that the game will go out of step. A host that sent nothing (an older
  KeeperForge2) is noticed too. The chat lines come from GoodPlayer's hook of the interface set-up (0x419180), which
  calls ModCheck::OnInterfaceReady.
- Mods are not sent from the host to the players: mod DLLs are programs (taking them from any host would run its code
  on every player's machine), the game's own TCP/IP games have no way to send files, and mods cannot be swapped while
  the game runs (their hooks and the game's file lists are set up at start).
- Not done yet: keeping a player with other KeeperForge2 or mods from starting.

## Writing a mod
```cpp
#include "IMod.h"
class MyMod final : public KeeperForge2::IMod {
	const char* GetName() const override { return "My Mod"; }
	const char* GetAuthor() const override { return "Me, Someone Else"; }
	const char* GetDescription() const override { return "What it does."; }
	const KeeperForge2::ModSetting* GetSettings(std::size_t& count) const override {
		static const KeeperForge2::ModSetting settings[] = {
			{ "speed", "2", "How fast things go." },
		};
		count = std::size(settings);
		return settings;
	}
	bool Initialize(const KeeperForge2::IModConfig& config) override {
		const char* speed = config.Get("speed", "2");
		return true;
	}
	void Shutdown() override {}
};
KEEPERFORGE2_MOD_EXPORT KeeperForge2::IMod* KeeperForge2_CreateMod() { return new MyMod(); }
```
- The runtime deletes the mod at shutdown, before unloading its DLL.
- Build with the same compiler and settings as KeeperForge2 (32-bit, MSVC):
  the interface is C++. Settings are plain C strings and values are read
  through IModConfig, so no runtime functions need linking.

## The mod API: log, hooks, events, game state (ModApi.h)
A mod DLL may also export `KeeperForge2_UseApi`. It is called once, right before the mod's `Initialize`; the api stays
valid until the mod's `Shutdown` has returned. Keep the reference:
```cpp
#include "IMod.h"
#include "ModApi.h"
namespace { KeeperForge2::IModApi* api = nullptr; }
KEEPERFORGE2_MOD_EXPORT void KeeperForge2_UseApi(KeeperForge2::IModApi& given) { api = &given; }

// In Initialize:
api->Log().Info("Hello from my mod.");
api->Events().OnGameTurn([](void*) {
	auto& game = api->Game();
	if (game.GetGameTurn() % 200 == 0 && game.Random(100) < 10) api->Log().Info("A tenth of the time, every 200 turns.");
}, nullptr);
const auto target = api->Hooks().FindPattern("8B 44 24 04 56 8B F1 ?? ??");
if (target != 0) api->Hooks().Hook(target, reinterpret_cast<void*>(&MyDetour), reinterpret_cast<void**>(&original));
```
- **Log**: Trace, Debug, Info, Warning and Error, into the KeeperForge2 log with the mod's name in front. A mod's Error
  does not close the game. Any thread.
- **Hooks**, through KeeperForge2's own hook engine (mods never write game code or name raw addresses):
  `FindPattern` gives the start of the only place in the game's exe matching a byte pattern (`??` any byte); 0 when
  there is none or more than one. `Hook(target, detour, &original)` redirects the game's function there; it fails
  (logged) outside the game's exe, or where KeeperForge2 or another mod already hooks. `Unhook` takes one back; all
  of a mod's hooks are taken back when it shuts down, before its DLL is unloaded. Hook in Initialize, before the game
  runs.
- **Events**: `OnLevelStart` (each level start, before the simulation runs, after multiplayer settings are settled)
  and `OnGameTurn` (before each game turn of a level being played; not while our level editor holds the level). Each
  takes a function and a context pointer. What a callback throws is logged and the game goes on. Game turns come from
  the world tick hook of CoopCampaign.cpp: if that cannot be installed (unexpected game code), there are none.
- **Game state** (read only; on the game's thread): whether a level is played, play mode, game turn, the local player,
  a player by number (mana, heart tile), map size, a tile (terrain, owner, room type), every creature of a player or of
  all (tag, type, level, owner, health, position in 1/4096ths of a tile) in the game's own order, and a creature by tag.
- **Random**: `Random(bound)` is the shared random number. It starts again at each game turn (and level start) from
  the game's own synced seed (world + 0x15A0) and the turn, and never steps the game's seed, so the game's own random
  numbers are unchanged. The same on every machine as long as every machine calls it alike: only in simulation code
  (events, hooks of simulation functions), never for what only this machine shows. All mods share it, in load order.
- Staying in step in multiplayer: change the game only from simulation events and hooks, read only the game's state and
  `Random`, no clocks. [unverified] Whether the game's resync (each machine replays from its last save when out of
  step) runs world ticks again; a mod that keeps state of its own should take it from the game each turn.
- `GetVersion()` is KeeperForge2's version.
- Interfaces only grow at their end, so a mod built against an older ModApi.h keeps working.
- Not offered yet: changing game state through the api (a mod does that from its hooks), the game's texts, sounds.

## A mod's config.yml
```
### HEADER ###
# author: <authors, separated by ','>
# description: <description, may go on over more "# " lines>
######

# How fast things go.
speed: 2
```
- A file without the header gets one made from GetAuthor/GetDescription.
  A header already there is kept as it is and read (authors, description).
  The authors are logged when the mod loads.
- The settings follow the same automatic rules as our config.yml (see
  [logging-and-config.md](logging-and-config.md)), using the mod's
  GetSettings list. A mod that lists no settings gets every value in its
  file as it is, and the file is not changed beyond the header.
