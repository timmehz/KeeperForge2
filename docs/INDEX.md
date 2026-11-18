# Docs index

## api
- [api/logging-and-config.md](api/logging-and-config.md): KeeperForge2 folder layout, log levels, config.yml and its automatic updating, fatal Error/Critical behavior
- [api/mods.md](api/mods.md): mods folder, game data from mods and Flame packs (data folders, flame_packs), adding creature types (ModContent.h), the mod API (ModApi.h: log, hooks, events, game state, shared random number), writing a mod (IMod, KeeperForge2_CreateMod), mod config.yml header
- [api/console-commands.md](api/console-commands.md): commands typed in the launcher's console (cpu, ram, gpu, fps, stats, reset, help) and how each figure is measured
- [api/launcher.md](api/launcher.md): launcher start-up order, game file checks (name, size, SHA-256)

## game
- [game/screen-resolution.md](game/screen-resolution.md): how DK2 picks its resolution, what overflows above 1024x768
- [game/frame-timing-and-screenshots.md](game/frame-timing-and-screenshots.md): game turns vs frames, running while minimized (1 fps, muted), patch.dll vsync, panel text cost and its cache, the pointer in screenshots, screenshot saving
- [game/skirmish-lobby.md](game/skirmish-lobby.md): skirmish player slots, how the local keeper is chosen, text buttons, Swap Keeper, the multiplayer lobby and its Swap Keeper
- [game/mercenary-portal.md](game/mercenary-portal.md): portal creature choice (requirements, weights, cap), our hero room rules and the one-royal-per-keeper rule
- [game/session-options.md](game/session-options.md): settings every player must share, play mode, lobby packed options and how the host's values reach every player, the lobby options window (creature sliders, numbers) and typing the creature counts
- [game/renderer.md](game/renderer.md): our DirectDraw/Direct3D 6 for DirectX 11/12 and Vulkan: what the game calls, the import slots, switching, the borderless window, anti-aliasing (FXAA and SMAA, both our own; MSAA), bump mapping of water and lava
- [game/cpu-threads.md](game/cpu-threads.md): why the game stays on one CPU thread (patch.dll), the cpu_threads setting, what it can and cannot speed up
- [game/panel-keys.md](game/panel-keys.md): keys for the side panel's tabs, the tab buttons, the interface's key event
- [game/game-fixes.md](game/game-fixes.md): the game's own bugs fixed by Flame/Ember, verified and done here, and the list still to do
- [game/creature-behavior.md](game/creature-behavior.md): creature states, the walk reset after map changes and the make-a-home fix, worker task list and the untagged-tile digging fix
- [game/camera-controls.md](game/camera-controls.md): key actions and how they are read, WASD movement, mouse wheel zoom, middle click camera reset (camera fields, the smooth view move)
- [game/coop-campaign.md](game/coop-campaign.md): campaign levels over multiplayer: shared keeper, Good player and second keeper parts, campaign rules, scripted camera sync, lobby rows, what is not done yet
- [game/steam-multiplayer.md](game/steam-multiplayer.md): the game's UDP network library, Steam lobby and invites, carrying the game's packets over Steam, public lobbies (hosted games listed in the session list), lobby passwords, the creator's rainbow name, friends' gold names, lobby chat join/leave notices
- [game/creatures.md](game/creatures.md): every creature type: id, code name, how it is known, where the game keeps creature data
- [game/creature-types.md](game/creature-types.md): a creature type's record in a creature list (Creatures.kwd or a level's own): fields, flags, the record size the game reads, fixed tables that limit types past 46
- [game/rooms.md](game/rooms.md): every room type: id, code name
- [game/good-player-in-game.md](game/good-player-in-game.md): Good player tick, workers (Workers row, Dwarf), Create Imp spell data and casting, per-player spell states, text lookup
- [game/level-files.md](game/level-files.md): the level file format: sections, a level's file list, the map's tiles, a level's own creature list, shared lists, saved copies and the check over all stock levels
- [game/sound.md](game/sound.md): the game's sound system (category lookup, play, handles, commands) and the sound log (log_level debug)
- [game/level-editor.md](game/level-editor.md): in-game editor: stages, Single Player menu buttons and its click handler, holding the simulation, restarting a level in place, level files and sections, changing tiles at run time, how the hand's box is drawn and the action point outlines, what is still to find

## symbols
- [symbols/DKII-DX-addresses.md](symbols/DKII-DX-addresses.md): addresses the runtime uses

## log
- [research-log.md](research-log.md): findings and dead ends
