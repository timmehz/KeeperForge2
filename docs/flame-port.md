# Flame port

[Flame](https://github.com/DiaLight/Flame) by DiaLight fixes the game's bugs and adds features. On 2026-11-18
DiaLight allowed KeeperForge2 to fork and embed Flame. The goal is to bring every Flame fix and feature over. This page
lists them all and tracks the port.

How we port:
- Flame works by rebuilding many of the game's own functions and swapping them in. We do **not** copy those rebuilt game
  functions (LICENSE.md section 5.2: no decompiled game code). Instead we hook the original function through
  HookManager and add only what Flame changes: code before or after it, or a small stub over the faulty part.
- Flame's own code (its fixes, features and tools) may be ported as it is, marked `// Ported from Flame: <name>`.
- Flame's bundled libraries (imgui, toml11, CImg, lodepng, stb, lzma2, d3dx) need the project owner's OK first.
- Every address is checked against our DKII-DX.exe export before use; Flame's struct offsets can differ from ours
  (for creatures, Flame's are 4 lower).

Status: **done** (in KeeperForge2, see the linked doc), **covered** (a KeeperForge2 feature already does the job),
**todo** (to port), **ask** (needs a decision first), **skip** (not needed here, with the reason).
Names are Flame's switch names, so each one can be found in Flame's code.

## Game bug fixes

| Flame name | What it fixes | Status | How we do it / notes |
|---|---|---|---|
| backstab_fix | Melee bonus damage used the map direction, not the target's facing | done | game/game-fixes.md |
| workshop_manufacture_build_time_fix | Workshop read the first item's build time, not the current one | done | game/game-fixes.md |
| response_to_threat_fix | Computer keeper never dropped creatures to defend | done | game/game-fixes.md |
| blocking_response_to_threat_fix | Computer keeps picking a creature it cannot drop | done | game/game-fixes.md ("Computer player stuck on one creature") |
| creatures_setup_lair_fix | Creatures on their way to make a home were sent back after map changes | done | game/creature-behavior.md |
| wooden_bridge_burn_fix | Selling one tile of a burning Wooden Bridge put the fire out | done | game/game-fixes.md |
| sleeping_possession_fix | A possessed sleeping creature could not be attacked | done | game/game-fixes.md |
| drop_thing_from_hand_fix | Drop rules only checked on the clicking machine | done | game/game-fixes.md |
| hero_party_spawn_limit_fix | Hero party spawn crashes at the creature limit (no leader made) | done | game/game-fixes.md. Also skips the leader effect, which Flame leaves |
| fix_chat_buffer_invalid_memory_access | Chat message copied past its end | done | game/game-fixes.md |
| buffer_overrun_fix | Exit function list too small | done | game/game-fixes.md |
| while_without_syscall_fix | Network wait loop spins without yielding | done | game/game-fixes.md. Flame's sound thread stop wait is still todo (below) |
| (sound thread stop) | Sound list thread waits in a loop without yielding | done | game/game-fixes.md ("Sound thread stop wait") |
| null_surf_fix | A missing picture gives no surface and crashes | done | game/game-fixes.md. Also logs the missing file's name |
| max_host_port_number_fix | Port number box takes one character too few | done | game/game-fixes.md ("Host port box") |
| override_max_room_count | Room limit of 96 crashes when splitting rooms | done (our way) | We keep 96 and allow splits; Flame raises the limit. Raising it: ask |
| (bridge without room) | Drawing a room tile with no room crashes | done | game/game-fixes.md ("Room tile with no room") |
| (save path) | Long save path overflows a 64-byte buffer when loading | done | game/game-fixes.md ("Long save path") |
| fix_keyboard_state_on_alt_tab | Keys stay held after alt-tab | done | game/game-fixes.md |
| fix_close_window | Closing the window mid-game ends it abruptly | done | game/game-fixes.md ("Closing the window during a game") |
| multi_interface_fix | LAN games pick the wrong address with several network adapters | todo | Hook the address lookup and connect; option for the address to use. Steam games are not affected |
| display_incompatible_reason | The session list does not say why a game cannot be joined | done | game/game-fixes.md ("Why a game cannot be joined") |
| print_game_start_errors | Start-up failures close without a message | done | game/game-fixes.md ("Start-up errors") |

## Features and options

| Flame name | What it does | Status | How we do it / notes |
|---|---|---|---|
| big_resolution_fix, screen_resolution, expand_surf_idx_array | Resolutions above 1024x768 | covered | Widescreen (game/screen-resolution.md) |
| increase_zoom_level | Zoom out further and in closer | done | game/camera-controls.md ("Wider zoom"), `wide_zoom` |
| use_wheel_to_zoom | Mouse wheel zoom | covered | CameraControls |
| use_wasd_by_default_patch | WASD camera keys | covered | CameraControls |
| hide_mouse_cursor_in_window | Hide Windows' pointer over the game | todo | Our window procedure (WM_SETCURSOR) |
| bring_to_foreground | Window comes to the front at start | todo | Our window procedure (on create) |
| control_windowed_mode, windowed | Windowed mode | covered | Renderer's borderless window; a real windowed mode: ask |
| remember_window_location_and_size | Window keeps its place and size | todo | Only matters with a windowed mode |
| limit_tps | Cap the game loop at a rate for fast displays | covered | FrameRate (frame_rate) |
| single core | Run on one CPU | covered | CpuThreads (cpu_threads) |
| force_32bit_everything | 32-bit textures, depth and display | todo | Set the video settings after the command line is read; check with our renderers first |
| use_cwd_as_dk2_home_dir | Use the working folder as the game folder | todo | Check whether the launcher needs it |
| modern_windows_support | The game's Windows version check | done | game/game-fixes.md ("Windows version check") |
| notify_another_instance_is_running | Message when the game is already running | done | game/game-fixes.md ("Start-up errors") |
| skippable_title_screen | Skip the title screen with a key | done | game/game-fixes.md ("Title screen wait"), `title_screen_ms` |
| autosave | Saves every few minutes, with switches in the save menu | done | game/autosave.md. Settings in config.yml; the save menu buttons are not done yet |
| custom_campaign | Single Player menu button to play a custom campaign (folder of levels) | design | Flame's is switched off and unfinished (an empty list); our own design in game/custom-campaign.md, waiting for an OK |
| alternative_resources | Load data folders (Flame packs) before the game's own | covered | ModData (flame_packs) |
| registry_to_config, replace_gog_config | Game settings in a file instead of the registry | todo | Hook the game's registry reads/writes and keep them in our config; ask about the format |
| replace_mouse_dinput_to_user32 | Mouse from Windows messages instead of DirectInput | done | game/mouse.md, `windows_mouse` (WindowsMouse). Not tested yet. Adds holding the pointer to the window; drops Flame's modifier tracking (the game sets those bits itself) |
| disable_bonus_damage | Option: no melee bonus damage | todo | Session option (all players must share it) |
| auto_network | Start straight into a network menu | skip | Developer shortcut |
| original_compatible | Turns off fixes that break play with the original 1.7 | skip | All KeeperForge2 players run the same build (ModCheck) |
| game_version_patch | Shows Flame's version | covered | MenuVersionText |
| welcome_window | Start-up window with options (imgui) | skip | Our launcher and config.yml do this; imgui not allowed |
| wine_support | Sets Wine's video memory size | todo | Launcher: detect Wine and offer the setting |

## Developer tools

| Flame name | What it does | Status | Notes |
|---|---|---|---|
| console, logging | Console window and log channels | covered | Launcher console, Logger, log_level |
| bug_hunter, ExceptionWindow | Stack walk and crash window | covered | CrashHandler; Flame's stack walker could improve our crash reports: todo |
| replace_heap | Guard pages around allocations to catch overruns | todo | Debug option only |
| protocol_dump | Logs the network packets | todo | Debug option (log_level trace) |
| inspect_tools, visual_debug, show_wireframe | Debug views | todo | Low priority |
| scheduler | Run a function later | skip | Helper; we use our own events |

## Not ported

- Flame's rebuilt game functions (most of its `src/dk2`): game code, LICENSE.md 5.2. Covered by hooks as listed above.
- Flame's rebuilt GOG patch.dll and network dll (`gog_patch_dll`, `weanetr_dll`): rebuilt from other programs. Our
  Renderer and SteamMultiplayer take their place.
- The level editor patches by Quuz shipped in Flame's `Data` folder: someone else's work, not covered by the permission.

## Order

1. Crash fixes: hero party spawn (done), missing picture (done), bridge without room (done), save path (done), sound thread wait (done), close window (done).
2. Small fixes: port box (done), incompatible reason (done), start-up errors (done), Windows version check (done), blocking threat response (done).
3. Features: autosave (done; menu buttons left), zoom range (done), skippable title (done), custom campaign, mouse from Windows messages (done, not tested), settings out of the
   registry.
4. Developer tools.
