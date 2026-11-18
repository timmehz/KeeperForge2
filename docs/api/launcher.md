# Launcher

## Icon
Launcher/Resources/KeeperForge2Launcher.ico, built into the exe by Launcher.rc. Sizes 16,
20, 24, 32, 40, 48 and 64 as 32-bit bitmaps and 256 as PNG, each cropped from one sheet
made with Google's Gemini, with the corners made transparent.

## Start-up order
1. FolderLayout::Prepare: create the KeeperForge2 folders and move old
   files in (see [logging-and-config.md](logging-and-config.md)).
2. The logger starts (reads `KeeperForge2\config.yml`).
3. With `steam_multiplayer` on: an error and stop if no steam.exe is running
   ([game/steam-multiplayer.md](../game/steam-multiplayer.md)). Then
   GameFiles::Verify (below).
4. Start DKII-DX.exe paused, in a kill-on-close job object, with the
   chosen renderer in the environment variable `KEEPERFORGE2_RENDERER`.
5. Load KeeperForge2 first (below), then resume the game. If that cannot be
   done, resume the game and inject KeeperForge2.dll the older way
   (LoadLibrary in a remote thread, then call `KeeperForge2_Initialize`);
   the runtime then races the game's start.
6. Wait for the game to close. If the game's crash report starts and the game
   is still open 20 seconds later, close it (see
   [logging-and-config.md](logging-and-config.md)).

## 4 GB memory (memory_4gb)
- With `memory_4gb: on` (the default), after the file checks the launcher writes
  `KeeperForge2-DKII-DX.exe` into the game folder: DKII-DX.exe with only the
  large address aware flag set. It rewrites it only if it differs, and starts it
  instead of DKII-DX.exe. The original is not changed. Off removes the copy.
- The copy is made on each player's PC and is never shared. Its code is the
  same, so all addresses still apply. It stays in the game folder because
  Windows looks for the game's DLLs (ddraw.dll) next to the exe first.
- [unverified] Setting the flag on the paused game is too late, as Windows
  should read it from the file when creating the process.
- [verified in export] The game reads total memory as a signed number in one
  place and adds two free memory figures in another, which large values break.
  So MemoryWatch keeps what GlobalMemoryStatus reports at 2 GB, as without the flag.
- [verified in export] The lobby's Ram column is the only use of 0x543820, which
  was stuck at 2048 for that reason. With the flag it now shows the PC's memory, up
  to 4096. Each player's number is their own, sent to the others in the slot data.
- Not tested yet. Pointers above 2 GB may break the game or its DLLs. A new exe
  name may also need a new Windows firewall rule for direct TCP/IP games.

## Loading KeeperForge2 with the game's DLLs (LoadRuntimeFirst)
- While the game is paused only the exe and ntdll are in memory. The launcher
  writes a new import table into the game, above the exe: an entry for
  KeeperForge2.dll (importing `KeeperForge2_Initialize`) first, then the
  game's own entries. The exe's header in memory is pointed at it and its
  pre-bound import list is dropped. The exe file is not changed.
- Windows then loads KeeperForge2.dll with the game's DLLs (it has to be in
  the game folder, as it is found by name) and starts it before patch.dll
  and what patch.dll brings in (ddraw.dll).
- In DllMain, `reserved` is set for such a load. The runtime then
  (StartEarly): lets the Renderer act on the other DLLs while none has
  started (it keeps DDrawCompat from starting when a new renderer is chosen,
  see [game/renderer.md](../game/renderer.md)), and hooks the exe's start
  routine through HookManager, so `Initialize` runs on the game's main
  thread before any game code. This also ends the race where the game could
  set up its display before the runtime was in.
- Tested: the launcher logs that KeeperForge2.dll loads with the game's
  DLLs, the runtime starts before the game sets up its display, and
  DDrawCompat is kept from starting.

## Game file checks (GameFiles)
- Each required file is checked by name. The game's own files are also
  checked by size and SHA-256 (Windows BCrypt): Aweman32.dll, ddraw.dll,
  dinput.dll, DKII.EXE, DKII-DX.exe, patch.dll, QMIXER.DLL, SFMAN32.DLL
  and WEANETR.dll.
- KeeperForge2.dll is checked by name only, since it changes with every
  build.
- Each file can list several known original copies (size + checksum), for
  example for other store builds. A file matches if one copy matches both.
- Known copies so far: the Steam release (v1.70), taken from the user's
  install on 2026-10-26. A GOG copy may differ; add its values as a second
  copy if so.
- A file with no known copies listed yet is not checked. The launcher logs
  a warning with its size and checksum, so the values can be added to the
  table in Launcher/Source/GameFiles.cpp.
- Anything missing, unreadable or not matching is listed in one message,
  with the expected and found values, and the launcher stops
  (Logger::Error).
