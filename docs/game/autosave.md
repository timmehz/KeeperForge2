# Autosave

KeeperForge2 saves the game every few minutes of play, the way the game's own Save does. The idea comes from Flame's
autosave [reference: Flame]. Code: Runtime/Source/Autosave.cpp.

## What it does
- Every `autosave` minutes of play (config.yml, default 5, `off` to turn it off), the game is saved to
  `autosave-<date>-<time>.SAV` in the game's saves folder. Only the newest `autosave_keep` (default 3) autosaves are
  kept; older ones are deleted. Other saves are never touched.
- Single player only: campaign, skirmish and My Pet Dungeon. Never in multiplayer, never in a co-op campaign, never in
  the level editor.
- Play time is counted in game turns (minutes x 60 x the session's turns per second), not with the clock. So time in the
  options menu or while paused does not count, as the game runs no turns then. Flame uses the clock and subtracts menu
  time; turns give the same result more simply.
- A new level, or loading a saved game (seen as a jump of more than 5 seconds of turns), starts the count again, so
  loading never triggers an autosave straight away.
- The save happens right after a frame is shown (FrameRate::AddAfterPresent), on the game's thread and outside the world
  tick, as the game's own Save runs from its menus.

## Game facts used [verified]
- The world's save to a file: 0x50FD10 (world vtable + 0x28), thiscall (world, path) ret 4, returns 1 when written. It
  opens the path as given (0x5B3980), so a full path is passed.
- The game's quicksave (world vtable + 0x20, 0x50FBA0) builds `<saves folder><mode prefix>QuickSave.QSAV` and makes the
  saves folder first when it is missing (0x5B3C30 tests it, 0x5B3900 makes it). Autosave makes the folder the same way.
- The saves folder: 0x758754, a full path (the game's folder + `data\Save\`) [reference: Flame].
- The in-game save and load windows' list of saved games: a file list at 0x6CE980 (0x12 bytes: + 0 the names, + 4 a
  flag, + 5 the pattern, + 9 the folder; pattern and folder are kept as pointers). Emptied with 0x54F0D0 (thiscall (list))
  and filled with 0x54F0F0 (thiscall (list, folder, pattern, flag) ret 0xC). After each autosave it is filled again with
  the saves folder, "*.sav" and 1, so the new file shows. The main menu's load window fills its own list when it opens
  [reference: Flame].
- A game is running while the player interface's session (0x6CEA10 + 4) is set. The play mode is 0x75AD4B (3 is
  multiplayer). The game turn is 0x517490 (thiscall (world)); turns per second at the world's session (+ 4) + 8.

## Not done yet
- Flame also adds two buttons to the in-game save window (autosave interval and how many to keep). Here both are only in
  config.yml for now.
