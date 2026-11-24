# Logging and config.yml

## Folder layout
Everything KeeperForge2 writes is in the game folder's `KeeperForge2`
folder (Shared `Paths`):
- `KeeperForge2\config.yml`
- `KeeperForge2\Logs\` (logs and crash dumps)
- `KeeperForge2\SCRSHOTS\`: the game's screenshots. The runtime points the
  game's folder name pointer (0x66F338) at it; see
  [game/frame-timing-and-screenshots.md](../game/frame-timing-and-screenshots.md).
- `KeeperForge2\mods\`: see [api/mods.md](mods.md).
- `KeeperForge2\bans.dat`: bans from the ban and tempban commands (sealed like ranks.dat).
- `KeeperForge2\ranks.dat`: player ranks (sealed with Windows' data protection) set with the console's rank command, see
  [console-commands.md](console-commands.md).

The launcher creates the folders at start. It moves an old `Logs\`,
`SCRSHOTS\` and `config.yml` from the game folder into them; files already
present in the new place are not overwritten. It logs what it moved.

## config.yml
- `KeeperForge2\config.yml`. If it is missing, the first process to start
  (normally the launcher) creates it with defaults.
- Simple `key: value` lines; `#` starts a comment. Values may be quoted.
- Kept up to date automatically (Shared `ConfigFile`), for ours and for
  every mod's:
  - settings missing from the file are added with their default;
  - keys not listed in the code (old or misspelt settings) are removed,
    with any other stray lines;
  - a value the setting cannot take (a `ConfigSetting::valid` check in
    Shared `Config.cpp`, such as `wheel_zoom: maybe` or `field_of_view: 999`)
    is set back to the default. `renderer`, `anti_aliasing` and the key
    settings have no check there: their own code reads several spellings
    and fixes unknown words itself;
  - other values already set are kept as written;
  - each key removed and value reset is logged as a warning by the first
    process to load the file (normally the launcher);
  - the file is rewritten (with the standard comments) only when that
    changes something.
  So comments added by hand are lost on the next rewrite.
- Keys:
  - `log_level` — minimum level to log. One of `trace`, `debug`, `info`,
    `warning`, `error`, `critical` (case-insensitive). Default `info`.
    An unknown value falls back to `info` and logs a warning.
  - `resolution` — in-game resolution. `auto` (default) uses the desktop
    resolution, `WIDTHxHEIGHT` (e.g. `1920x1080`) forces one, `off` keeps
    the game's own 4:3 list. While it is not `off`, it overrides the
    resolution picked in the game's options menu. See
    [game/screen-resolution.md](../game/screen-resolution.md).
  - `gui_layout` — `anchored` (default) keeps the 2D GUI's 4:3 proportions
    on wide screens and sticks the left/right parts to the screen edges;
    `stretched` is the game's original behavior.
  - `widescreen_menus` — `on` (default) runs the main menu at the game
    resolution's shape; `off` keeps it at 640x480.
  - `menu_height` — height (at least 480, default 480) the main menu is
    drawn at before being scaled up; the width follows the game's shape
    (e.g. 854x480 on 16:9).
  - `renderer` — `vulkan` (default), `dx11` or `dx12`. The game's own Direct3D
    (`original`) is no longer offered: that word, or any unknown one, gives
    Vulkan and is rewritten in the file. See [game/renderer.md](../game/renderer.md).
  - `window_mode` — `borderless` (default): the game's window covers its monitor
    with no frame and is not minimized when another program takes focus;
    `fullscreen`: minimized on focus loss, as the original. Unknown words give
    `borderless` and are rewritten.
  - `anti_aliasing` — `off`, or `fxaa`, `smaa` or `msaa` with `x2`, `x4` or `x8`
    (default `smaa x4`). FXAA and SMAA: x2/x4/x8 are the low/high/ultra presets;
    MSAA: samples per pixel, lowered to what the card supports. Unknown words give
    `smaa x4` and are rewritten. See [game/renderer.md](../game/renderer.md).
  - `keeper_color_red`, `keeper_color_blue`, `keeper_color_green`, `keeper_color_yellow`, `keeper_color_purple` —
    each keeper's color, six hex digits (RRGGBB) or `game` (default) for the game's own; set by the color boxes beside
    the skirmish screen's player list. See [game/keeper-colors.md](../game/keeper-colors.md).
  - `ray_tracing` — `on` or `off` (default `off`): ray traced ambient occlusion, shadows, reflections (water and
    lava) and bounce light, with every renderer (KeeperForge2's own tracer in compute shaders). The log's lines
    starting `Ray tracing:` say what runs and what the card offers. Its parts: `ray_tracing_occlusion`,
    `ray_tracing_shadows`, `ray_tracing_reflections` and `ray_tracing_bounce`, `on` or `off` (all `on`);
    `ray_tracing_quality` — `high`, `medium` (default) or `low`: traced at a half, a third or a quarter of the
    screen's size; `ray_tracing_rate` — traces a second, 5 to 240 or 0 for every frame (default 30; frames between
    show the last result, lower is lighter). See [game/ray-tracing.md](../game/ray-tracing.md).
  - `cpu_threads` — how many CPU threads the game may use: a number (default 4),
    `all`, or `game` for the original 1. See [game/cpu-threads.md](../game/cpu-threads.md).
  - `panel_key_creatures`, `panel_key_rooms`, `panel_key_spells`,
    `panel_key_workshop` — keys that open the side panel's tabs (default 1 to 4),
    or `off`. `sell_key` — key that turns Sell on and off (default `grave`, the ` and ~
    key), or `off`. `options_key` — key that opens and closes the options menu (default
    `escape`), or `off`. See [game/panel-keys.md](../game/panel-keys.md).
  - `frame_rate` — frames per second (15 to 1000, default 60), `0` for no
    limit, or `vsync` for the original pacing. Game speed is not affected.
    See [game/frame-timing-and-screenshots.md](../game/frame-timing-and-screenshots.md).
  - `wasd_camera`, `wheel_zoom` — `on` (default) adds WASD movement and mouse
    wheel zoom in a level. See [game/camera-controls.md](../game/camera-controls.md).
  - `middle_click_camera_reset` — `on` (default): the middle mouse button
    puts the view's zoom and rotation back as they were at the level's start,
    without moving it. Same doc as above.
  - `windows_mouse` — `on` (default, as in Flame): the game takes its mouse
    from Windows' messages instead of DirectInput, so the pointer moves at
    Windows' pointer speed and acceleration, without lag, and at the same
    speed at any resolution (DirectInput's moved it a fixed number of pixels,
    slow at 4K). The pointer is held to the game's window while it is in
    front. Needs `dx11`, `dx12` or `vulkan`. `off` uses the game's own
    DirectInput mouse. See [game/mouse.md](../game/mouse.md).
  - `title_screen_ms` — how long the title screen shows at start, 0 to 10000
    milliseconds (default `600`; the game's own is 10000). A key or click
    skips it. See [game/game-fixes.md](../game/game-fixes.md).
  - `wide_zoom` — `on` (default): the view zooms further out and a little
    closer than the game allows. See [game/camera-controls.md](../game/camera-controls.md).
  - `autosave` — minutes of play between autosaves in single player (default
    `5`, 1 to 600), or `off`. `autosave_keep` — how many autosaves to keep
    (default `3`). See [game/autosave.md](../game/autosave.md).
  - `run_in_background` — `on` (default) keeps the game running when it is
    minimized or loses focus (dx11, dx12, vulkan only); while unseen its
    sound is muted and campaign and skirmish run at 1 fps. `off` pauses as
    the original. Same doc as above.
  - `mercenary-portal-spawns` — `mercenary` (default), `normal` or `off`. Heroes
    come through a claimed Mercenary Portal once their rooms are built, plus one
    royal per keeper; `mercenary` also lets the game's own mercenaries through;
    `off` keeps the game's own rules and takes the heroes out of the lobby's list.
    See [game/mercenary-portal.md](../game/mercenary-portal.md).
  - `mercenary-portal-stone-knights`, `mercenary-portal-royals` — `on` (default)
    or `off`.
  - `mercenary-portal-<hero>-room-ids`, `mercenary-portal-royal-room-ids` — the
    rooms needed, one or two ids joined with +, such as `12 + 16`. See
    [game/rooms.md](../game/rooms.md).
  - `keeper-dwarf-workers` — `off` (default) or `on`: keepers' workers are
    Dwarfs, as for the Good player. See
    [game/good-player-in-game.md](../game/good-player-in-game.md).
  - All of these mercenary and worker settings are session values: in
    multiplayer the host's are used
    ([game/session-options.md](../game/session-options.md)).
  - `memory_4gb` — `on` (default) or `off`: start the game with 4 GB of address
    space instead of 2 GB. See [launcher.md](launcher.md). Experimental.
  - `editor_outline_color` — `80C0FF` (default), any six hex digits, or `game`: the color of the
    outlines round action points in the editor (`game` keeps the hand's box's own).
  - `editor_dark_windows` — `on` (default) or `off`: the editor's own Windows windows (creature
    properties and the name window it opens) in dark colors, with a dark title bar where Windows
    has one. `off` keeps Windows' own colors. Read each time a window opens.
  - `editor_check_maps` — `off` (default) or `on`: for one start, checks every stock
    map and makes copies in `KeeperForge2\maps`, then turns itself off. See
    [game/level-files.md](../game/level-files.md).
  - `steam_multiplayer` — `on` (default) or `off`. While on, the launcher stops
    with an error if Steam is not running.
  - `steam_player_name` — `on` (default) or `off`: with `steam_multiplayer` on,
    use the Steam name in multiplayer games. See
    [game/steam-multiplayer.md](../game/steam-multiplayer.md).
  - `field_of_view` — 50 to 300, default 100. How much the 3D view shows;
    100 matches the original game at 640x480, with extra width on wide
    screens.

## Log levels
Lowest to highest: Trace, Debug, Info, Warning, Error, Critical.
Messages below `log_level` are skipped.

Which level a message gets:
- Info (the default level, what players see): settings in effect, the
  graphics card and API drawing the game, the screen mode, files saved,
  multiplayer joins and lobby changes, a player's own actions (swapping
  keepers, screenshots), and anything worth a bug report.
- Debug: how a feature did its work (window placement, video areas,
  buffers grown, first-time notices of a fix stepping in, "installed"
  lines, editor details, renderer statistics every few seconds).
- Trace: floods (every focus or size message to the game's window, every
  caller of a centered menu).
- Warning and above: something did not work as meant.

Debug logs for finding game bugs have no settings of their own: they
write at Debug level, and the hooks they need are only put in when
`log_level` is `debug` or `trace` at start-up (so normal play is not
slowed). Today: the sound log (docs/game/sound.md) and the Combat Pit
log (docs/game/game-fixes.md, "Neutral creatures in the Combat Pit").

Each message that passes the filter goes to:
- `KeeperForge2\Logs\<name>-<timestamp>.log` (`KeeperForge2Launcher-...` for the
  launcher). The runtime inside the game adds the renderer chosen in
  config.yml: `KeeperForge2-<timestamp>_<renderer>.log`, with `dx11`, `dx12` or
  `vulkan` (read with Render::ApiConfigName, so spellings such as `d3d11` come
  out as `dx11`, and unknown words as `vulkan`).
- The launcher's console window. The runtime attaches to its parent
  process's console (the launcher), so both write to the same window.
  Commands can be typed there too, see [console-commands.md](console-commands.md).

## Old logs
When a process opens its log (launcher and runtime alike), the oldest `.log`
files (by last change time) are deleted so that, with the new one, the Logs
folder holds at most `logs_kept` of them (config.yml, default 10,
`Logger::DefaultLogsKept`; 0 keeps all). Each game start writes two logs (the
launcher's and the runtime's). Other files there, such as crash dumps, are not
counted or deleted. The new log then says how many were deleted; a value that
is not a whole number gives a warning and the default.

## config.yml layout
Each setting has one short comment line (a few have two). A blank line comes
only before a setting with its own comment, so settings without one (the
other panel keys, the mercenary portal room ids) sit right under the first of
their group.

## Error and Critical are fatal
`Logger::Error` and `Logger::Critical` never return. They log the message,
show a message box if boxes are on, and close the process (after OK, when
a box is shown).
- Boxes are on by default and turned off with `Logger::SetErrorBox(false)`.
  The runtime turns them off at start: in fullscreen the box ends up behind
  the game, which then never closes (tested: the PC had to be powered off).
  So in the game, a fatal error only goes to the log and the console, and
  the game closes straight away. The launcher keeps its boxes.
- In the game: the game process closes. The launcher is waiting on it, so
  it closes too.
- In the launcher: the launcher closes. The game is in the launcher's job
  object (kill-on-close), so it closes too.
- The box is still shown even if `log_level` filters the message out.
- If several threads hit a fatal error at once, only the first one goes on;
  the others wait for the process to close.

## Crash handler (runtime)
An unhandled exception logs a CRITICAL report (exception, registers, memory,
code addresses on the stack) and writes `KeeperForge2\Logs\Crash-<time>.dmp`,
then closes the game with exit code 1.
- A thread started in advance writes the report, formatting without the heap,
  so a full or damaged heap or a used-up stack should not stop it. Each line
  reaches the log as soon as it is ready; the dump comes last.
- The crashed thread waits 10 seconds for the report, then logs that it did not
  finish and closes the game. A crash inside the report logs that too. If a
  second thread crashes, it waits for the first crash's report.
- Writing the dump pauses the game's other threads, so the handler also sets an
  event the launcher waits on. The launcher closes the game if it is still open
  20 seconds later.

## Memory log (runtime, MemoryWatch)
At start, then whenever free memory has changed by 16 MB or more (checked once
a minute), the log gets the game's address space: committed, free, largest free
block, limit. It warns once when the largest free block drops below 64 MB.

Use Warning for problems that should not end the session.
