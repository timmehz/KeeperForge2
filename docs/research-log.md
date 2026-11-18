# Research log

## 2026-10-24: Widescreen / high resolution
- Confirmed our DKII-DX.exe is the same v1.70 build the reference projects
  map: every address checked matched the expected instructions.
- The game drops any resolution outside its 4:3 list to 640x480 (0x525D30),
  so the resolution has to be injected right before that function reads the
  settings. Hooked its entry.
- Found four things that break above about 1366x768: the engine heap's 2 MB
  chunk limit, the 140-slot GUI tile table, the surface cache page count, and
  (windowed only) GetClientRect-based buffer sizes. The first three are fixed
  in Runtime/Source/Widescreen.cpp. Details: docs/game/screen-resolution.md.
- Design choices that differ from the references: the GUI tile fix redirects
  the three table instructions using the index already in a register, and
  keeps per-view tables on the side (falling back to the game's own table
  for views built before the patch). The heap fix sends blocks from the
  game's original chunks back to the original free, so it is safe to
  install after the game has started allocating.
- First in-game test: no change. The log showed the byte check failing at
  0x42CE95, because patch.dll had already put its own jmp there. The check
  now accepts that form too, and our GUI jumps are re-checked and re-applied
  each time the game screen is set, in case patch.dll ever writes after us.
  Lesson: verify patch sites against patch.dll's writes as well, not just
  the exe export.
- Second test: patches applied (auto = 3840x2160) but the game quit cleanly
  (exit 0) after ~3 s. Screen setup rejects sizes missing from the
  driver-reported mode list, and the startup call to 0x525D30 quits on
  failure. Fix: read that list and use the requested size only if it is
  there, otherwise the closest aspect ratio / largest size; the list is
  logged so we can see what DDrawCompat offers. Also stopped writing the
  settings at DLL init (only inside the hook now) and made the hook
  thiscall-correct.
- Third test: game runs, but the driver list was only
  640x480x32, 800x600x32, 1024x768x32 (no "native" mode came through), so we
  fell back to 1024x768 and DDrawCompat letterboxed it (black side bars).
  DDrawCompat reads its process config (DDrawCompat-DKII-DX.ini in the game
  folder) at game start, before injection. The launcher now writes that file
  (only if missing or created by us) with SupportedResolutions including the
  wanted size. [unverified] that DDrawCompat then offers it; needs a test.
- Fourth test: DDrawCompat did load the setting, but the game's list was
  unchanged. Dead end: patch.dll wraps DirectDraw and makes up its own mode
  list. Removed the launcher's ini writer. The runtime now adds the wanted
  mode to the game's list through the game's mode-list callback when
  patch.dll is loaded; patch.dll's SetDisplayMode accepts any size.
- Fifth test: no black bars, but "the game is stretched". No screenshots
  available yet. The main 3D camera uses one focal length for both axes
  (focal = lens * 320 at 0x598D26 via the fixed-point multiply 0x4363F0,
  frustum scales at 0x58A7F1..0x58A86A are focal/halfWidth and
  focal/halfHeight), so the 3D view should not be stretched. The 2D GUI is
  (see screen-resolution.md). Added anchored GUI mapping and the menu at
  game resolution. Needs screenshots to tune.
- Sixth test: menus "kinda good", but menu text, the loading screen and
  videos were tiny at 3840x2160. All three are fixed-size assets made for the
  original resolutions (fonts max 36/72 px, loading screens per 4:3 size,
  videos drawn unscaled). Menu now runs at 480 height and is scaled up;
  in-game fonts follow screen height; loading screen is stretched.
- Seventh test (first screenshot): menu fully good. HUD anchoring looks
  right (panels at edges, bars spanning). 3D view is a fish-eye wide angle,
  not a stretch: the focal length is fixed at 320 px. Now scaled with screen
  height. Loading screen flickered (my black fill before every redraw) and
  had side bars; now cropped to fill with no fill. Videos are 4:3 films drawn
  by the EA player; still pillarboxed.
- Eighth test: focal x(height/480) alone zoomed the view in ~4.5x (camera
  inside the Dungeon Heart). Cause [verified]: the player camera distance is
  [cam+0xD80] * const / screenWidth (0x598CDD loads the width, FIDIV at
  0x598D22), so wider screens move the camera closer. The first screenshot's
  look was close camera + wide angle, not a stretch. Fix: distance uses 640
  ("mov eax,640") plus focal scaled by height. Dead end recorded: scaling the
  focal alone.
- [verified] patch.dll FakeSurface::Blt forwards to the real surface's Blt
  (stretching works) and presents immediately when the target is the screen,
  so a black fill before a draw shows as a flicker. FakeSurface::BltFast
  skips the screen surfaces when its flag 0x10024DD9 is set.
- Videos play in the menu screen mode (intro at 0x52F336 right after the menu
  mode is set), so they show as 4:3 at full height. Filling the screen would
  need a temporary mode switch around 0x53BCD0 (thiscall + an implicit ESI
  argument). Not done yet.
- Ninth test: 3D view correct, picking works, no loading flicker. HUD sits
  in the right places but is too small at 4K. Added gui_scale (default
  140%): the in-game GUI mapping now also pins vertically (top 15% to the
  top, bottom 30% to the bottom) so it can be enlarged; fonts grow with it.
  The menu (480 lines) is excluded.
- Tenth test (with an original 4:3 screenshot for comparison): at 140% the
  left panels outgrow the middle and overlap (gold counter vs info text,
  oversized tabs). At 100% the earlier screenshot already matched the
  original's proportions 1:1, so the default is back to 100.
- GUI buttons could not be clicked. Cause [verified]: the hover test
  0x52BFC0 and click handler 0x52C0A0 convert the mouse themselves with
  `x*2560/W, y*1920/H` (game object size) instead of calling 0x52C5F0, so
  hit-testing used the stretched mapping while drawing used ours. Fix: hook
  both and hand them a mouse position their formula maps to our layout point.
- Eleventh test: at 100% the spells/workshop tabs were ~3x wide (they cross
  the 30% zone line) and the imp portrait was missing. Replaced the in-game
  zone mapping with per-window anchoring by window id (see
  screen-resolution.md). Imp portrait: not traced yet; the width/640 code
  paths (0x404000, 0x598A00) belong to other views, not the panel.
- Twelfth test: with the centered in-game mapping, icons, digits and the
  minimap became huge. Cause: the point conversion also converts sizes, and
  the centring offset was added to every size. Dead end: any mapping with an
  offset for the in-game GUI. Now a plain scale from the left edge, with
  windows moved into the spare width on the right.
- Thirteenth test: HUD "almost there". Only the minimap "N" was too small;
  traced to the three in-game fonts loaded once by fixed id at startup.
  Screen-mode hook moved into Widescreen.cpp so camera and fonts both follow
  every mode change.
- Fourteenth test: the fixed-font swap did not change the "N". It is the
  sprite gui\map\north drawn at a hardcoded 8x8 pixels (0x41EE7D); now
  scaled with screen height.
- Fifteenth test: HUD and "N" good. New requests: PNG screenshots and a
  frame rate setting. Confirmed game turns are timer-based (0x521BA0), so the
  frame rate can change without changing game speed. Found patch.dll's
  per-frame vertical blank wait (Vwait). Details in
  game/frame-timing-and-screenshots.md.
- Sixteenth test: PNG screenshots work and the frame rate is limited to 60.
  Esc options menu broken: icons sat over their labels. Cause: text buttons
  ignore their window's position, so moving dialog windows split them up.
  Dialogs stay in place now.
- Crash after minimizing/alt-tabbing back: exit code 0xC0000005 (access
  violation), no other information. Not documented in Flame/Ember. Added
  CrashHandler (unhandled exception filter: module+offset, registers, stack
  code addresses, minidump in Logs) to find where it happens.
- Seventeenth test: menu screenshots had no menu text (taken before the
  menu's 2D layer); title logo left of center (box stretched by the menu
  zones). Both fixed. Videos: a mode switch around 0x53BCD0 to crop-fill.
  Loading screen still reported "not fullscreen" although the log shows it
  crop-filled into 854x480; waiting for a capture of what it looks like.
- First crash report (CrashHandler): access violation writing at 0x5AF379,
  in the video decoder's thread, ~14 s after entering the menu (the intro).
  Caused by our 640x360 video mode (decoder writes 480 rows). Videos now use
  640x480, optionally stretched by patch.dll. The earlier alt-tab crash
  happened before the video change, so it may be a different problem.
- Eighteenth test: no crash. "Loading screen" left-aligned with black on the
  right was the startup title screen (0x5340F0), copied unscaled at 0,0.
  Now fitted and centered.
- Open: 16:9 projection/HUD behavior, windowed mode, menu above 640x480.
  All need in-game testing.
- Swap Keeper (skirmish lobby). The local keeper is not taken from the lobby
  slots. Skirmish start hard-codes player 3 (0x531B3E), and world setup
  skips player 3 when it hands out AI (0x50CB97). Both are now patched, so
  swapping slots moves the human. Menu text buttons draw pre-rendered images
  (0x5329A0 list), so the new button gets its text by adding entries to that
  list instead of hooking the string table. Dead end: 0x55D530 returns a
  constant 3, but it is a resource-flags getter, not the local player.
  Needs in-game testing: swap 1<->2 and 3<->4, start the game, check that
  you control the swapped keeper and that keeper 1 gets its AI.
- Swap Keeper tested: on Siege the player started as keeper 2.
- Menu layout: the zone mapping split Map Select's list from its scroll bar
  and "Size" header. Switched the menu to a centered 4:3 layout with one
  rigid offset (user's choice). Full-width boxes still span the screen.
- Centered menu, first try: shifting the box conversion moved the text but
  not the list rows, bars, map preview or scroll bar. Those are blitted at
  fixed 640x480-design pixel positions. Now the front end's frame
  description (+0x2DD65) is moved to the centered 4:3 area while the menu
  draws (hook on 0x52BBD0), so everything moves together. Needs testing:
  Skirmish, Map Select, main menu hover/clicks, Options.
- Dead end: shifting the front end's frame description. Tested and worse.
  The menu still drew at the left, and some pieces (sliders, labels on Game
  Settings) used the full 854 width and overlapped. The menu is back to its
  original 640x480 (4:3 with patch.dll side bars), at the user's choice.
  Removed the widescreen_menus/menu_height settings and the menu mode patch.
- Correction: the user wants widescreen menus, so the 4:3 revert was undone.
  On the frame-shift SHOT captures, the blits had moved (title, sliders,
  list rows) but the text had not. Text goes to the bound draw surface
  (0x79D060, set by 0x5B3710), which was still the unshifted frame. The
  render hook now binds the shifted frame too. A patch.dll crash
  (PATCH.dll+0x3461, read of 0) was seen once with the 640x480 menu. Watch
  for it.
- Centered menu test: Game Settings, the skirmish rows and the map preview
  were right. Still unshifted: the Map Select list text, the skirmish
  "Player Name" header and the version text. 0x5334C0 draws window 0x2F's
  elements (0x547C20) after the window renderer returns. Our hook had
  already put the text binding back by then, while blits still used the
  shifted frame. The shift now lasts until 0x5334C0 returns. The version
  text draws across the whole screen through
  WidescreenGui::BeginFullScreenMenuDraw/End.
- Centered menu, next round. User report: still wrong are the map previews,
  "Max Players", "Player Name", the slider knobs, and the Map Select size
  column and scroll bar. [verified] The slider renderers (0x55079D to
  0x5523E7) unlock the back buffer, copy the knob with the DirectDraw blit
  0x5B57F0 (cdecl, 7 args, destination = front end +0x30340), then lock
  again (0x5B5BE0, Lock at vtable +0x64). The blits skip the frame
  description, so the blit is now hooked and shifted while the menu draws.
  The draw-surface binder 0x5B3710 is hooked too: any other unshifted
  description of the same frame bound during the pass gets a shifted copy,
  and each caller is logged once ("centered menu moved a frame binding from
  0x..."). If anything is still off, the log shows where to look.
- Centered menu: drawing positions confirmed right by the user, but the
  hover/drag areas of sliders and scroll bars were off. [verified] The front
  end's mouse handler 0x52FBF0 (cdecl (int, int, x, y, front end), screen
  pixels, registered as a callback at 0x52F036) tests slider-type buttons
  (+0x73 == 8) itself. It compares the raw mouse with boxes from 0x52C630,
  which are now relative to the centered area. It also passes the position
  on to GuiClick (0x52FE21). The handler is hooked and given the position
  relative to the centered area. Our conversions track whether the stored
  mouse (shifted while drawing) and positions passed in (shifted inside the
  handler) are already relative. 0x52FAF0 and 0x52FE50 are keyboard
  handlers.
- Slider and scroll bar hover still wrong after the mouse handler fix.
  [verified] Three menu button handlers read the stored mouse directly at
  front end +0x30FF0/+0x30FF4 (the menu manager's +0xAC/+0xB0) and compare
  it with converted boxes: 0x530440 (list box), 0x5501F0 (horizontal
  slider, sets the hovered part 6/7/... via vtable +0x3C, stored at button
  +0xAE) and 0x5507E0 (vertical slider / scroll bar). All are cdecl
  (button, front end) and run outside the drawing pass, so they saw the raw
  mouse. They are now hooked and given the stored mouse relative to the
  centered area while they run.
- Held-creature icons (next to the cursor while things are in the hand).
  [verified] 0x40CFE0 loops over the hand (world vtable +0x270 count, +0x274
  get) and draws up to 8 icons with 0x40D450. Each goes to the engine
  renderer's vtable +0x5C with render info size 0x20 x 0x20 ("mov eax,
  0x20" at 0x40D4FA; the "more" icon at 0x40D206). The position is the
  mouse plus W*82/640, H*10/480 and a per-icon offset from 0x6CE910. That
  table is filled at startup by 0x40AA40: two rows of four on a 32 pixel
  grid, then the "more" icon at (128,32). [reference] Flame calls the render
  info CRenderInfo, with +4/+8 width/height scale. All of it was fixed
  pixels, so the icons were tiny at 2160 lines. OnScreenMode now scales the
  size and the grid by height/480. Still unscaled: the W*82/640 sideways
  offset (it is width-based, so it sits a bit further right on wide
  screens).
- Hand icons confirmed working by the user. The icon of what the cursor will
  build or cast (room/spell/trap, next to the cursor) used the same fixed
  32x32 render info: "mov eax, 0x20" at 0x40CE27, 0x40CF44 and 0x40D3D2
  [verified]. Its number is placed at +0x1E/+0xA (8-bit "lea" offsets at
  0x40CE67/0x40CE6A). All are scaled by height/480 in OnScreenMode, with the
  offsets capped at 127.
- Hand missing from screenshots. [verified] The pointer is a software cursor
  drawn by its own thread (WaitForSingleObject loop around 0x5DB8E0)
  straight onto the screen surface, never into the frame. The screenshot
  (0x558A40) reads [0x79D364] (or the window's surface 0x5565E0 when
  windowed), so the cursor was never in it. The drawer instance is 0x79F638
  (getter 0x5DBE10). Its draw (0x5DC860) blits image +0x54+slot*0x5C, rect
  +0x194+slot*16 at +0x1C4/+0x1C8+slot*16 with 0x5B57F0, slot = +0x20.
  Screenshots.cpp now does the same blit onto the frame just before the
  screenshot reads it.
- Review Objective ("o", button handler 0x4288C0): opens window 0x21 and
  sets player interface +0x2726 = 1 (the same pause/options state as
  Esc). No frame throttling found yet (the 0x5228E8 Sleep is a separate
  thread). Asked the user what exactly looks slow.
- Review Objective choppy ("everything"). [verified] The session draw step
  (0x526C82 on) stores the turn (+0x278) and a 0..256 fraction of the
  current turn (+0x27C, time since the turn from 0x521C80 * 256 / period)
  for in-between frames. It skips the fraction while session +0x284 (a menu
  open over the game) is set, via "jnz" 75 1A at 0x526CC6. The world keeps
  running, so frames only changed once per turn. The jnz is now NOPed, and
  the call at 0x526CC8 goes to our TimeIntoTurn. That calls 0x521C80 and
  holds the result under one turn (the original allows up to one turn +
  999 ms), so a paused menu cannot draw things past the next turn. This
  only affects drawing, not the simulation.
- Correction (tested, no effect): the hand screenshot used the wrong cursor
  slot. [verified] Slots 1/2 (+0x20 index) are the saved background for
  erasing. The pointer image is slot 0 (+0x54, rect +0x194, position
  +0x1C4/+0x1C8), copied to the screen +0x224 with flags 1 (transparent) by
  0x5DC530. Now copied the same way.
- Dead end (tested, no effect): the in-menu turn-fraction patch at
  0x526CC6 did not fix the Review Objective lag, so it was taken out. The
  frame counter now logs at INFO when the 5-second frame rate is below 80%
  of the limit, to confirm whether the frame rate really drops (suspect:
  drawing the objective parchment at 4K).
- Review Objective lag measured: 40 fps in game, 17 fps over the 5 seconds
  with the panel open, 34 after. So the frame rate really drops. Added a
  temporary FrameProfiler (FrameProfiler.cpp, generic register-saving
  timing stubs) around 0x428C30 (objective text renderer), 0x42CB60 (text
  into a GUI view), 0x42CEE0 (draw a GUI view), 0x62CAE0 (text draw),
  0x52CF90 (sprite blit) and 0x526590 (session tick). Its per-frame times
  go on the low frame rate log line. Remove it once the cause is found.
- Review Objective lag found with the profiler: "view text" 0x42CB60 took
  191 ms per frame (7 calls), with 1.3 ms of that in text drawing.
  [verified] 0x42CB60 is thiscall on the player interface, with 0x70 bytes
  of arguments (ret 0x70): view, x, y, text, color*, alignment flags,
  clear first, a font object by value (destroyed by 0x62C2B0, which
  releases the object at +4), and upload. Clear does "rep stosd" over the
  view's whole buffer ([view+0x293] bytes), or 0x42C9B0 for view +0x2737.
  Upload (0x42CDF0) sends every 128x32 tile (+0x27F wide by +0x283 high)
  to the screen, one by one. At 4K that is thousands of tiles per call.
  GuiTextCache.cpp now leaves out calls that repeat the view's last run
  (all arguments, the text and the color; not the font's owned pointer).
  It destroys the font copy itself, and still draws each view for real at
  least every 0.5 s. The profiler now measures 0x42CDF0 ("view upload")
  instead of 0x42CB60.
- The first GuiTextCache build left nothing out: uploads were still 2-3
  per frame at about 25 ms each with the objective panel open. The font
  copy (arguments 7-25) is now left out of the comparison entirely. A
  diagnostic logs the first 20 differences and, every 5 s, the drawn /
  left out / without-clear counts.
- Second cache test: the objective panel's calls came without the clear
  flag (140 of 155), with 6 uploads per frame at ~170 ms. [verified] Panels
  clear their view with 0x42CA70 (thiscall (view, patterned), ret 8: zeros,
  or 0x42C9B0's fill), then draw each line through 0x42CAB0 (wide text ->
  0x741120 -> 0x42CB60) with upload on. So the whole view is uploaded once
  per line. 0x42CEE0 draws a view to the screen (thiscall (view, x, y,
  flags), ret 0x10). GuiTextCache now:
  - starts runs at 0x42CA70 and holds the clear back while the run repeats
    (done late on the first difference);
  - turns off the per-line upload and does one upload right before
    0x42CEE0 draws the view (or at the next run start);
  - redraws for real every 5 s instead of every 0.5 s.
  The profiler now measures 0x42CAB0 instead of 0x42CEE0.
- Third cache build tested: Review Objective smooth (no frame-rate drop).
  About 1300-2100 calls left out per 5 s against 10-94 drawn. Removed
  FrameProfiler and the cache's diagnostic logging. The frame-rate line is
  back to DEBUG.
- Swap to Good Player (skirmish): the human's slot becomes a computer
  player and the local player number at skirmish start becomes 1 (Good).
  Untested: whether the game runs well with the heroes as the local
  player (camera start, win/lose checks, keeper-only GUI).
- Swap to Good Player, first test: the user controlled keeper 1 (fighting
  its AI) and owned no heroes. [verified] Players 1 and 2 never reach the
  keeper-only local player check, so world +0x7243 was not set to the Good
  player. 0x4C2340 is now hooked to set it.
- Swap to Good Player, second test: crash at 0x5082A8 (ECX=0) from
  0x4481FE reading player +0x1105. [verified] Player init skips the task
  list for players 1 and 2 (0x4B87B5). The 0x4C2340 hook now NOPs that jz
  only while it creates the controlled Good player.
- Crash handler hang: the fatal message box sat behind the fullscreen game,
  so the game (and the launcher waiting on it) never closed. The runtime now
  turns the box off (`Logger::SetErrorBox(false)`), and the crash filter
  starts a 10 s watchdog that terminates the game if reporting hangs.
- Swap to Good Player, third test: the hand did not keep picked-up
  creatures. [verified] Player tick 0x4B9EC0 returns early for the Good
  player, so its timed hand action never finishes (pick up happens on
  finish). Hooked 0x4B9EC0 to run 0x4C0AA0 for the Good player when it has
  an action. Not the cause: player +0x1101 (computer player AI, also skipped
  for players 1/2) and the hand limit +0x1C0 (set for every player at
  0x50C8E7).
- Dwarfs for the Good player (GoodPlayer.cpp, moved the player tick hook
  there). Findings in good-player-in-game.md:
  - The Workers row type is chosen by data flags (0x80 and 1), not
    hardcoded.
  - Create Imp's creature is spell data +0x43.
  - The per-player spell state 3 is available.
  - Texts can be renamed through 0x62D960 using the game's own converters.
  Untested: whether hero Dwarfs do every imp job for a human Good player,
  and whether the spell panel shows spell 4 once its state is 3. Icon not
  done: the spell panel code (0x4113B0 area) is not disassembled in our
  export.
- Good player Dwarfs did not dig tagged tiles. [verified] Tags do make
  tasks. The creature "what next" state 0x4773D0 sends Good-owned
  creatures to hero behavior (0x5A) before checking the worker flag.
  Patched 0x47759E so a human-controlled Good player's workers take the
  worker path (Imp idle, state 0xA). Checked and not the cause: the tag
  routine 0x512A20 and the dig job search 0x469AF0 (no player-number
  checks), mana 0x4C10E0, room removal 0x4C7160, imp mana upkeep 0x4C1AE0.
- Flying notification icon was not scaled: it is a 2D animated mesh
  (0x42F040) with a fixed float size. Scaled by height/480 around the call.
  Found through Ember's CEngine2DAnimMesh name for 0x579A10 [reference],
  then the caller 0x42F040.
- The Good player could not build: the keeper part of the player tick
  (room placement, research, gold count) is skipped for Good (0x4B9F3A).
  A stub sends a human-controlled Good player through the whole tick
  (keeping unlimited mana), and keeper-available rooms/doors/traps are
  mirrored to it. Gold follows keeper rules.
- Upgraded Create Dwarf made a level 4 Imp: the upgraded cast reads the
  creature type from spell data +0x4C (not +0x43). The cast hook now swaps
  both.
- Good player research stopped after one spell, and Create Dwarf was
  upgraded early. [verified] The research picker only upgrades spells once
  nothing is researchable, and our copy had made everything available
  (and copied no spells). Now the keepers' starting states (2 stays 2,
  3 stays 3) are copied for rooms, doors, traps and spells during the
  first 20 game turns.
- Good creatures crowded a destroyed enemy heart: their hero job (+0x409,
  target player +0x40D) never ends. The job is now cleared when the target
  player is defeated (+0x132 set by 0x4C0060 when the heart object dies).
  Also found Flame's state names: state ids are one more than my earlier
  table arithmetic (0x459180 is state 71).
- Good Wizards still stopped after one research: after a job, state 1 sent
  them to the hero idle, which has no "return to job". The controlled
  Good player's creatures without a hero order now take the keeper path
  (0x4775EC chain, state 0x59 = keeper idle 0x4778E0).
- Crash on "Receive Imps" as the Good player (0x45495E): the special rings
  creatures around the heart tile, (1,1) for the heroes. Hooked 0x4A4980 to
  use the special's tile and make Dwarfs. The crash handler closed the game
  and launcher as intended.
- Screenshot folder: the game reads "SCRSHOTS" through the pointer at
  0x66F338 (to 0x6BC5C8) for the exists check (0x5B3BF0), the create
  (0x5B3900) and the file name format. [verified] Repointed to
  KeeperForge2\SCRSHOTS.
- Added the KeeperForge2 folder layout, config auto-update (ConfigFile),
  mod loading from KeeperForge2\mods, and launcher size/SHA-256 checks.
  Known-good sizes and checksums still need filling in from the user's own
  game copy (the launcher logs them).
- New reference: private Ghidra exports of the official DK2 level editor
  (GOG release, rc3, pro). For level file formats, triggers, hero parties
  and data field meanings, and later our in-game editor. They are not part
  of this repository; only facts verified against the game are written here.
- Mercenary Portal: the portal creature choice (0x4DBF90) uses data +0x68A
  bit 8 for room type 0x19. Added mercenary-portal-spawns (normal swaps
  the test to +0x686 bit 0x40000000, the random hero party set). For
  multiplayer the host's value rides in the free bit 4 of the lobby's
  packed options (front end +0x6029). Unverified that this block is what
  the host sends; needs a 2-player test (compare the level start log
  lines).
- Mercenary Portal test: nothing came out on a custom map. [verified
  mechanism] The portal's weight comes from the creature's first attraction
  room, and heroes have none, so their weight is 0. Rewrote MercenaryPortal:
  - our rooms are written into the heroes' attraction lists around each
    Mercenary Portal's choice;
  - a stub on the 0x4DC064 test decides who may come;
  - one royal per keeper per game.
  Moved the lobby-bit mechanism to SessionOptions and added
  keeper-dwarf-workers (bit 5), including the heart top-up count stub
  (0x4BB479).
- Mercenary Portal second test: still nothing came out. The portal room
  tick 0x4DBBE0 is not disassembled in our export, and Ghidra lists no
  callers of 0x4DBF90. Decoded the tick by hand; Ember's references give
  its two calls. Added diagnostic log lines (portal seen; per-hero state:
  in the map's pool / rooms missing / ready). Cause still open: a map
  creature pool without heroes, or a room that is not type 0x19, are the
  main suspects.
- Starting Imps as Dwarfs for keeper-dwarf-workers: hooked the creature
  creation wrapper 0x4B78D0 (not 0x4B7900, which saved games use).
- The creature pool is the lobby's (or map's) per-type number, used up by
  portal arrivals. Heroes are not in the lobby's list (it lists only
  keeper Portal creatures, record flag 0x400), so their pool was 0: very
  likely why nothing came out. Added the ten heroes to the lobby list
  (default 16), made the hero numbers "alive at a time", and let royals
  past the pool check. Also confirmed the host sends the packed options in
  its lobby message (type 199), so session options do reach every player.
- Tested: the portal now brings heroes. Two issues found:
  - Create Dwarf cost more than its panel price. Create Imp's price is
    base cost times the count of the spell's creature type (+0x43) above a
    free number. Swapping +0x43 to Dwarf for the whole cast made the cast
    count Dwarfs while the panel counted Imps. Now only the type handed to
    the effect is changed (0x4BBE4B), so the price is the panel's.
  - With `normal`, heroes still arrived as mercenaries: the portal's
    creature maker (0x4DC9A0) gives every Mercenary Portal creature the
    mercenary arrival (state 0x116, +0xF3 bit 0). Patched the room type
    split (0x4DCBEA) so `normal` uses the Portal's arrival (state 0xC1).
- Dead end: giving `normal` heroes the Portal's arrival (state 0xC1) made
  them appear over the Mercenary Portal's center and get stuck among its
  pillars, with no arrival effect (tested). Replaced: they keep the
  Mercenary Portal's arrival, and only the IsMercenary value (creature
  +0x3EA bit 9, named in the game's debug dump) passed to 0x48E0D0 at
  0x4DCC4F is 0 with `normal`.
- Tested: 150+ Dwarfs (flat Create Dwarf price) stopped the portals, made
  Create Dwarf take mana without a Dwarf, and the Horny talisman fail. Not a
  per-keeper limit: player +0x10F6 (MaxCreatures) is never read, and the
  portal cap counts only portal arrivals. It is the world's hard limit of
  448 live creatures (0x4B7945). The user chose a rising price: spell 4's
  creature type is now the Dwarf for the whole game with keeper-dwarf-workers,
  so every price site counts Dwarfs like Imps.
- New renderer (DirectX 11, DirectX 12, OpenGL, Vulkan). The game reaches
  DirectDraw only through two import slots (0x66C020/0x66C024, patch.dll's
  exports), and makes its DirectDraw once (kept at 0x79D3D0). What it calls
  was taken from the methods patch.dll's wrapper really implements (Flame's
  rebuild): a small set, all 32-bit, triangle lists of pre-transformed
  vertices (formats 0x144/0x344). So KeeperForge2 now has its own DirectDraw /
  Direct3D 6 layer keeping every surface in memory, with four backends behind
  it; switching between the four happens inside the layer. Original stays the
  default and needs a restart to switch to or from. Not tested yet.
- Note: the launcher starts the game without suspending it, so the runtime
  races the game. If the game set up its display first (0x79D3D0 set), the
  renderer stays original for that session and logs it.
- The front end text-list hook (0x5329A0) moved out of SkirmishSwapKeeper into
  FrontEndTexts, so Swap Keeper and the Renderer row share it.
- Tested: Vulkan started ("drawing with Vulkan") and the game ran into a level,
  but nothing showed, and closing the game left a crash dump. Likely cause:
  the layer kept the first window given to SetCooperativeLevel, and the game's
  mode list (0x5B4E20) first uses a hidden "ModeList" window it then destroys.
  Now the layer follows the latest window (restarting the backend on it) and
  logs each one. The backend is no longer destroyed when the process exits.
- Tested again with the present logging: one window only (the window theory
  was wrong). Pictures with real content were being shown (about 23 a
  second) and Vulkan reported no errors, but the screen stayed black. Cause
  found in the Vulkan present: the window image was cleared to black and the
  picture copied onto it with no barrier between the two transfer writes, so
  the clear could land last. A barrier now separates them. In the level the
  frames were 854x480 (the menu's size); screen mode, screen and 3D target
  sizes are now logged to see why.
- Tested with the barrier: Vulkan shows the menu and levels. New issues: the
  intro video was not right and pressing the Windows key during it left the
  video stopped with no sound. The log shows not one picture reached the
  renderer during the intro (the first is at the main menu), so the video
  player never flipped. Read the player (0x53BCD0 and 0x5A7A80-0x5AB9F0):
  it decodes into the back buffer on its own thread and flips; it pauses and
  resumes by toggling on each activate event (0x53C2E0 -> 0x5A8FF0), so one
  lost or doubled activation leaves it paused with its sound stopped. patch.dll
  minimizes the window when it loses focus and sends WM_ACTIVATE to it when
  the first video surface is made [reference]; our layer does neither.
  Unsupported calls were only logged at debug level, so they were hidden.
  Added logging (unsupported calls at info, DirectDraw 1/2 surface requests,
  screen/back buffer locks with thread, focus and minimize messages on the
  game's window, video start/end) to find where the player stops.
- Also seen: in the level the game made an 854x480 screen right after the
  3840x2160 one (Widescreen sets 3840x2160, then the main menu sets 854x480),
  and the level kept 854x480.
- Tested with the logging (Vulkan): pressing the Windows key in game gave
  WM_ACTIVATE 0, WM_ACTIVATEAPP 0 and WM_KILLFOCUS, then the log stopped: no
  reactivation and no shutdown, so the game froze rather than just pausing.
  The Vulkan present waited forever for a window image. Now: the wait is
  100 ms (the frame is skipped), nothing is shown while the game is in the
  background, and the window is minimized when it loses focus, as patch.dll
  does. The present counts restart at each screen mode so the log shows
  whether videos reach the window. Not tested yet.
- Tested: after those changes everything was black (video, menu), though the
  log showed pictures with content being shown and no Vulkan call failed;
  Windows key and back now works (minimize, restore, focus all logged). The
  only change in the Vulkan path since the last run that showed pictures was
  the 100 ms limit on getting a window image, so it is back to waiting as
  long as it takes (not proven to be the cause). The layer still shows
  nothing while the game is in the background, which should keep the freeze
  away. Asked for a DirectX 11 run to tell a Vulkan-only problem apart.
- Tested: Vulkan, DirectX 11 and DirectX 12 all black; OpenGL shows the menu
  and levels but not the Bullfrog logo or the intro video, and the loading
  screen flickers. The log keeps showing pictures with content and no failed
  calls. Added a TEMPORARY test: the picture is shown at 80% size on a dark
  purple background (DirectX 11 and Vulkan), plus a log of the window's
  visibility, styles, position and whether it is in front, and DirectX 11's
  present status codes (0x087A0001 = occluded). Purple visible = the
  presentation works and the picture draw is at fault; pure black = nothing
  drawn reaches the window.
- Tested the purple test: DirectX 11 and Vulkan both all black, no purple at
  all, but after Windows key and back the picture showed for about half a
  second, then black. The window is visible, in front, topmost popup
  (style 0x94080000, extended 0x8), 0,0 to 3840,2160; DirectX 11 reported no
  occluded status. So the pictures do reach the window and something covers
  them. Likely: the window class has a black background brush, and Windows
  painting it covers what DXGI flip-model and Vulkan present (OpenGL draws
  into the window's own pixels, so it showed). Now the layer drops
  WM_ERASEBKGND and WM_PAINT on the game's window and removes the class
  brush. Not tested yet. (The menu presents rarely: one present in 10 s.)
- Tested: with the paint fix Vulkan shows the game. DirectX 11 and 12 are
  still black with every call succeeding, on a visible window in front.
  DirectX 11 now makes the older blit swap chain first (it draws into the
  window's own pixels, like the original renderer), with flip as fallback;
  both DirectX backends log the graphics card, the screen the window is on
  and the swap chain kind. If blit shows and flip does not, DirectX 12
  (flip only) will need another way onto the window (DirectComposition).
- OpenGL removed on request (backend, GLSL sources, menu choice, config word;
  a config.yml still saying opengl now reads as original).
- Tested: DirectX 11 with the blit swap chain, DirectX 12 and now Vulkan too
  all black (Vulkan's code unchanged since the run that showed). Card NVIDIA
  RTX A4000 on \.\DISPLAY1 for all. So it is likely not the backends but how
  Windows puts a window that exactly covers the screen on the display: the
  picture showed for half a second after a restore, then black, which fits
  Windows handing such a window straight to the display after a moment
  (and Windows 11 can turn blit swap chains into flip ones). TEMPORARY
  test: the window is made one pixel shorter than the screen. Also asked for
  a Windows screenshot while black (shows whether Windows' own picture has
  the game).
- Tested: a Windows screenshot (Print Screen into Paint) while black is
  black, but the game's own screenshot (KeeperForge2\SCRSHOTS, from the
  game's memory) has the picture. So Windows itself sees a black window; the
  display handover theory is out, and the one-pixel-shorter test did not
  help (removed). Now logged at the 30th picture of each screen mode: every
  visible window above the game's that overlaps it (class, title, owner,
  always-on-top) and any window inside the game's, to see if something
  covers it. The game's screenshot is 854x480 (the level runs at the menu's
  size, still open).
- Tested: no window covers the game's (only a 1x1 "ThumbnailDeviceHelperWnd"
  at 0,0) and none is inside it, yet all three renderers stay black. Now
  logged at the 30th picture: the graphics libraries loaded (to see if the
  original ddraw/d3dim or patch.dll's renderer is also drawing into the
  window), and two pixels read back from the window (border and middle).
- The game folder's ddraw.dll is DDrawCompat (third-party; strings in our
  export of it, DDrawCompat_License.txt and DDrawCompat-DKII-DX.log in the
  game folder) [verified]. patch.dll imports it statically, so it loads at
  start even when the game uses our DirectDraw. Its log: at load it hooks
  display modes, the registry and the Direct3D driver (the NVIDIA user mode
  driver, nvldumd.dll); about a second later it installs its DirectDraw,
  Direct3D and GDI hooks, which it does on the first call of any of its
  exports (DirectDrawCreate*, DirectDrawEnumerate*, ...: each export wrapper
  calls the install routine first). patch.dll only calls the real
  DirectDrawCreate inside its own DirectDrawCreate, which we bypass; what
  made the first call is not known. Strong suspect for the black screen in
  all three renderers (it takes over how windows are presented). Asked the
  user to test with ddraw.dll renamed.
- Tested (DirectX 11, blit): D3DIM700.DLL and D3DIM.DLL are loaded although
  our renderer never uses them (DDrawCompat makes a real DirectDraw and
  Direct3D when it installs its hooks), and the window's own pixels read
  back black right after our present. DDrawCompat's log puts its DirectDraw,
  Direct3D and GDI hooks at 23:45:25.500, inside our startup DirectX 11 try
  (25.104 to 25.648), so starting our API is very likely what set them off.
  Its install routine checks an "installed" byte first (0x1015E078 in our
  export: cmp byte [0x102BCB2C], 0 / jnz / call [lock] / cmp again / later
  mov byte [flag], 1). Now, when one of our renderers is chosen, the runtime
  finds that byte by its code (not by address) in the loaded ddraw.dll, after
  checking it is DDrawCompat, and sets it before trying the API, so those
  hooks are never installed; it is cleared again if the original renderer is
  used after all. Not tested yet.
- Tested the "installed" byte fix: too late. The runtime found it already
  set (32.394) while DDrawCompat's log only says "Installing DirectDraw
  hooks" at 32.800: the byte is set at the start of its install, which began
  right after it hooked the NVIDIA driver while loading (32.319), not because
  of our API try (that timing was a coincidence). Removed.
- New (user's choice): the launcher starts the game paused and puts
  KeeperForge2.dll first in the game's import table, so Windows loads it with
  the game's DLLs and starts it first. From DllMain the runtime then turns
  DDrawCompat's start routine into "return TRUE" when a new renderer is
  chosen (its TLS callbacks have nothing to do then), and runs Initialize
  from a hook on the exe's start routine, on the main thread before any game
  code (this also ends the old launcher race). With DDrawCompat kept out,
  patch.dll's ddraw.dll imports go to Windows' own ddraw.dll. The old
  injection is kept as the fallback. Not tested yet.
- Tested the launcher change: KeeperForge2.dll loads with the game's DLLs,
  DDrawCompat is kept from starting, and DirectX 11, DirectX 12 and Vulkan
  all show the game. So DDrawCompat's hooks were the cause of the black
  screens [verified]. Removed the test code (purple background, 80% picture,
  window/library/pixel checks, lock and surface logging); DirectX 11 is back
  to a flip swap chain first.
- The 854x480 seen in that run was the menu: startup sets the in-game mode
  (0x525D30, Flame's MyWindow_prepareWithSettings), then the front end's
  launchGame (0x52F140) sets the menu mode (854x480 via menu_height), shows
  the title screen (10 s), the intro videos, and the 3D main menu (the menu is
  a 3D scene, hence "3D draws into a 854x480 back buffer"). No real level was
  started in that run. Asked for a run into an actual level to check its size.
- Tested: the Bullfrog logo and the intro video are not full screen with our
  renderers. The 640x480 video screen was stretched to the window, but the
  player only draws the film into a band of it: 0x5AABF0 doubles the film's
  width (flag 0x40) and not its height, and centers it. patch.dll scales the
  film itself through its FourCC video surfaces, which our DirectDraw does
  not offer (the player then decodes straight into the back buffer). Now
  Widescreen works out the film's area the same way and the renderer shows
  only that area (stretch: whole window; bars: its shape). It logs the area
  once per video. Not tested yet.
- Tested the film area: logged as 640x480 at 0,0 (film header width 320,
  flag 0x40 doubling it to 640), but on screen the film filled only the left
  half (screenshots). The doubling is made for a 16-bit screen (each film
  pixel written as 4 bytes = two 16-bit pixels); on our 32-bit screen that is
  one pixel, so the film covers its own 320 pixels. Now the film's own width
  is shown, in the doubled shape (4:3). Not tested yet.
- Reported, not reproduced: after minimizing and restoring from the taskbar
  the in-game hand (cursor) was gone. Log deleted. The window's focus and
  minimize messages are logged, so the next occurrence will show the
  sequence. Known: on restore Windows first sends WM_ACTIVATE with the
  minimized flag (0x200001), which the game's handler counts as inactive,
  then WM_ACTIVATE 1 after the restore.
- Crash (DirectX 11): Print Screen in the menu's Movies list, five seconds
  after a minimize and restore. Access violation reading 0 at 0x5B8167: the
  surface copy 0x5B57F0 (called from Screenshots' pointer copy) asks the
  source wrapper's DirectDraw surface (+0x24, read without a check by
  0x5B8150) for its description, and the pointer image (0x79F638 + 0x54)
  had none. Screenshots now skips the pointer copy when the frame or the
  image has no surface. The empty image is probably also why the hand was
  gone after the restore. Not found yet what empties it: the cursor drawer
  only frees its three surfaces (0x5DC0F0) to grow them, and rebuilds them
  at once (0x5DC010); no screen rebuild was logged after the restore. The
  frame hook now logs when the image loses or regains its surface.
- [verified] The game's window handler 0x558C70 treats WM_ACTIVATE (6),
  WM_ACTIVATEAPP (0x1C) and WM_NCACTIVATE (0x86) alike: low word of wParam
  set -> callbacks (list at 0x757D71, 8 entries, data +0x20) with event 0,
  active flag 0x757D59 = 1, and if 0x757D5D (was inactive) was set, it is
  cleared and 0x757D61 set, so 0x5587F0 sets the saved mode again (only
  when 0x757D69 is set: after the in-game mode, not in the menu). Low word
  0 -> event 1, 0x757D5D = 1, 0x757D59 = 0. The screen setup 0x5581B0 calls
  the same callbacks with 2 before and 3 after [reference, Flame]. Callers
  of 0x559690 (register): 0x409B10 (in-game interface: frees and rebuilds
  its surfaces on 2 / 3), 0x5108F0 (world: on 0 / 1 calls 0x75B468's +0x38
  with 0x608340 / 0x608320, which pause or resume something in the engine),
  0x53C2E0 (videos), 0x59D8D0 (engine).
- Movies-list video not full screen: before it played, the menu's 3D view
  was attached to the video's back buffer ("3D draws into a 640x480 back
  buffer"), so its frames came through the frame path, which did not use
  the film area. Both paths now use it. Not tested yet.
- Tested with the new logging: the pointer image lost its surface right
  after a Movies-list video ended (09:36:53), not at the minimize (09:37:04).
  The video was played with Widescreen's mode switch around it (854x480 ->
  640x480 -> 854x480); the game itself plays these in the menu's mode. So
  the switch in the menu takes the pointer image away. Which code empties it
  during the switch is not known. With our renderers the switch is now done
  only when the mode is smaller than 640x480 (the film is found in any mode
  it fits in); with patch.dll it is unchanged (whether the hand goes missing
  there too is not tested). Not tested yet.
- Tested (DirectX 11, build before the flip swap): the level runs at
  3840x2160 (setting game screen when the level starts), so the 854x480 was
  only the menu [verified]. The pointer image loses its surface at the
  level's mode change and gets it back 2 s later.
- Flicker in levels, now and then: the game sometimes shows its screen
  (primary) instead of a frame, after the pointer is drawn onto it. The
  layer's screen copy was taken lazily from the back buffer, by then often
  half drawn with the next frame. A flip now swaps the screen's and the 3D
  frame's memory when both copies of the frame match (as a real flip
  leaves the old screen in the back buffer). Not tested yet.
- Hang: the whole PC had to be switched off. The log just ends (09:51:45),
  no crash report, after a screen present that was almost all black. The
  game window is always on top and full screen, so a hung game covered
  everything. Cause not found. Added: the window is no longer always on top
  with our renderers (as patch.dll's NotOnTop), and a watchdog thread that
  logs, once per case, a thread holding the renderer's lock for over 5 s or
  nothing shown for 10 s while in front, with where the threads are
  (CrashHandler::DescribeThread: address and code addresses on the stack,
  copied while the thread is paused and formatted after).
- Lord of the Land (from the Mercenary Portal) kept stopping and turning with
  "make a home" as his task. Cause in the game itself: after every map change
  0x4E8D30 resets the player's walking creatures to idle, including one
  walking to the Lair with arrival state 0x50 (verified in 0x46F390). With
  workers digging all the time it never gets there. Flame fixes the same
  (reference). New CreatureFixes replaces 0x4E8D30 with the game's own
  choice minus that walk. See game/creature-behavior.md. Not tested yet.
- Requested: Dwarf workers droppable into the heart (to remove, as Imps) and
  not into the Mercenary Portal (as Imps). Both come from the Imp's data
  flag +0x686 bit 0x80 in the drop rule 0x450910; the heart removal itself
  (0x459180) only needs the worker flag. GoodPlayer now sets bit 0x80 on the
  Dwarf type while keeper-dwarf-workers is on. Not tested yet.
- Reported: "Dwarfs still count toward the creatures summoned by the
  Mercenary Portal". The portal cap counts only +0x3E6 bit 2, set only by
  the portal maker 0x4DC9A0, so not Dwarfs from spells or the heart. Asked
  the user where the count is seen.
- Started porting the game's own bug fixes from Flame/Ember (facts only,
  verified in our export). Inventory and status in game/game-fixes.md.
  First batch (GameFixes.cpp): workshop build time (0x506B50/0x506B70),
  computer threat response (0x4FE5AA jz -> jnz), network wait spin
  (0x61C090 yields), exit list size (0x634860, 32 -> 128 entries, early
  start only). Not tested yet.
- Tested: with mercenary-portal-spawns: mercenary, King, Lord of the Land,
  the Princes, Dwarfs and Stone Knights came out white (model and
  animations fine). 0x48E0D0 sets creature + 0xF0 (render info + 0xAC, the
  texture bank, passed to the mesh lookup 0x4B44E0 by the drawing code) to
  1 with IsMercenary; those types have no bank-1 textures. Hooked to put the
  bank back to 0 for them. Not tested yet.
- Tested: DirectX 12 had fps drops of about 2 s in busy moments, never
  while idle; DirectX 11 did not. The log (every 1000 frames) does not show
  them. Likely cause: the per-frame upload buffer (32 MB, grown only when a
  single upload is larger) filling at 4K (one frame is 33 MB) and the
  backend waiting for the GPU mid-frame. DirectX 11 renames its buffers and
  never waits. The buffer now doubles up to 128 MB, with log lines for
  growth, mid-frame waits and slow pipeline creation. Not tested yet.
- Wrote game/units.md: all unit ids (Flame's list, with what our code and
  tests confirm). Renamed Game::Creature to the game's names with "_"
  (Lord -> Lord_Of_The_Land, ElvenArcher -> Elven_Archer, ...), and moved
  DwarfCreature/ImpCreature into it (Creature::Dwarf, Creature::Imp).
  Unknown: the Maiden's id; which Prince is 2, 26 and 31 (no export names
  them; the names used are from memory, unverified).
- Searched for "Maiden Of The Nest" / "Maiden" / "Nest": the only hit is a
  "Maiden" string in the three editor exports, among the camera view effects
  ("Ghost", "Redout", "Blackout", "Vampire [ Red ]"), not a creature. Its
  id is still unknown. The user's unit list is the editor pro's order:
  Imp to Maiden evil, Dwarf to Prince Balder good.
- The level editor shows id 45 for Maiden Of The Nest (the user checked).
  Added Game::Creature::Maiden_Of_The_Nest = 45; nothing uses it yet.
- The user checked every unit id in DK2 Editor pro.exe. All match Flame
  except the Princes: Tristran 2, Felix 26, Balder 31 (our guess had Balder
  and Tristan swapped; fixed, and the name is spelt Tristran as the editor
  shows it). Dark Elf was given as 12, Firefly's id; Flame says 6, kept 6
  until checked again.
- Rechecked by the user: Dark Elf is 6 (the 12 was a typo), and the name
  is "Prince Tristran". All unit ids are now verified in the editor.
- Tested: the texture bank fix works on the ground, but those mercenaries
  were white again in the Hand of Evil. The hand sets its own copy's bank
  (hand + 0x1064) from IsMercenary at 0x40D7E1; patched to also check the
  type. The only other read of IsMercenary is the debug dump (0x4909B7).
  Not tested yet.
- Feature request: keep the game running while minimized, 1 fps while
  unseen, normal when seen. Found: 0x558C70 runs "inactive" callbacks on
  focus loss (the game pauses), and 0x521BA0 gives at most one turn per
  frame, so 1 fps nearly pauses the game. User chose: 1 fps in campaign and
  skirmish only (multiplayer keeps its frame rate), sound muted while
  unseen. BackgroundRunning.cpp, config run_in_background (default on), our
  renderers only. Not tested yet.
- Tested background running: the game kept running minimized (menu frames
  went on, sound muted), but after a restore the main menu's 3D background
  was black (sampled lit pixels ~3500 -> ~330). Likely the game releases
  3D things on its "inactive" callbacks and sets them up again on "active";
  it only got the second half. The held focus-loss messages are now
  replayed to the game right before focus comes back. Not tested yet.
- Feature: type the lobby's creature counts. Mapped the lobby options window
  (0x23 at 0x6B4438; sliders, numbers, toggles, ticks, slider range 0-32,
  0x532170 find button, 0x5B2C10 typed characters); see session-options.md.
  The number renderer 0x54A710 is not disassembled in our export, so the
  typed digits are shown by moving the slider (the game then shows the
  number) and the number blinks by swapping in an empty text. Not tested.
- Feature: Steam multiplayer by invite (app 2616460). The user exported a
  32-bit steam_api.dll (modern SDK: InitFlat, ManualDispatch, flat
  interfaces; all functions used are exported). WEANETR.dll is UDP only
  (verified: socket/bind/SO_BROADCAST/getsockname, own address from
  gethostname + gethostbyname in FUN_10005C20, port 7575, session search by
  broadcast). Design: Steam lobby + rich presence connect; each member gets
  10.a.b.c from their account id; WEANETR's Winsock imports routed over
  SteamNetworkingMessages. See game/steam-multiplayer.md. Not tested.
- Tested: clicking the multiplayer chat box crashed (EIP 0xFFFF0877 from
  0x4305E6). The game's text input subclass calls the previous window
  procedure directly; with something else having subclassed the window it
  is a handle for CallWindowProc. Fixed in GameFixes (both direct calls,
  0x43032C and 0x4305DD, go through CallWindowProcA). Not tested yet.
- The watchdog's "held the lock for 18446744073709551 s" was a false
  alarm: the clock was read before the lock's start time; fixed.
- Swap Keeper in multiplayer (user's choice: host only, never two humans):
  multiplayer lobby is window 32 (0x6B39A0); local player at start is
  3 + network place (0x5430E7), patched to 3 + the slot holding this
  machine's network id (slot + 0x3B). See skirmish-lobby.md. Not tested.
- Tested: after a multiplayer swap, all ready but the game did not start;
  the swap button sat in an odd place; chat lines show no sender. Ready:
  each player's slot updates are sent for 0x758334 (its game-given slot);
  now kept on the slot holding its network id, and 0x55AE30 (used for the
  local player number on all three start paths) hooked instead of only
  0x5430E7. Chat: the lobby's "%s : %s" lines (0x5404xx, 0x5408xx) put game
  text 0x5D4 before the message; screenshot asked for. Not tested.
- Tested: chat names fine now. Multiplayer swap made a duplicate of the
  swapped human: 0x544060 puts a player's slot update (0x68) into the
  sender's network slot, moving the computer player out. Hooked to send it
  to the slot holding that player's network id. Not tested yet.
- Tested: swapping the host out of keeper 1 gave "host is not responding"
  on the other machine. The check was not found. User chose: the host's
  keeper cannot be swapped (other humans with computers still can).
- Feature: WASD movement and wheel zoom (CameraControls.cpp, game/camera-controls.md).
  Flame rebinds the keys, which would lose the arrows, and zooms the camera
  directly; here the key action checks are hooked instead, so the game's own
  commands run. Dead end avoided: 0x4097E0 and 0x409740 look like plain functions
  in the C export but are thiscall in the assembly (ret 0xC, ret 0x10). No chat-open
  flag found. Not tested yet.
- Crash reported (2026-10-30, Vulkan, 3840x2160, campaign level with water, while
  possessing an Imp; did not repeat): no report and no .dmp. The log ends at
  16:03:16 with no warning; the launcher saw exit code 1 at 16:03:37. A normal quit
  gives 0. Code 1 with an empty log leaves two paths in CrashHandler: a second
  exception inside the filter (it terminates quietly), or the 10 s watchdog with the
  filter stuck before the dump file is created. The filter allocates and uses
  std::filesystem first, so running out of memory, heap damage or a stack overflow
  would all end this way. [unverified which] The session had run 2 h 40 min, about
  2 h 20 min of it minimized; the retry was a fresh start without Steam. Nothing ties
  the crash to possession or water yet. The user recalls the game vanishing at once,
  not freezing first, which favors the second exception over the watchdog (from
  memory, three days later).
- Looked at a 4 GB address space (large address aware flag) for the game. Not done.
  Our Ghidra export does not show the exe's header, so whether the flag is already
  set is unverified. Windows should read the flag when it creates the process, so
  changing the header of the paused game would be too late [unverified]; the launcher
  would have to start a flagged copy of the exe, made locally. Risks found in the game
  [verified in export]: 0x5B8210 returns GlobalMemoryStatus's dwTotalPhys, and the
  caller at the 0x12/0x20/0x40 setting (meaning unknown) compares it as signed, so 2 GB
  or more reads as negative and picks 0x12. The check using dwAvailPageFile +
  dwAvailPhys can also wrap. Keeping the reported values at 2 GB would avoid both.
  Pointers above 2 GB are untested in the game and its DLLs. The engine heap has its
  own limit of 32 chunks of 2 MB added as it grows, which the flag does not change.
- Done from the two entries above: crash report rewritten to avoid the heap, on its
  own thread; launcher closes the game if the report hangs; MemoryWatch logs the
  address space; `memory_4gb` config option, off by default. Found on the way: the old
  report's watchdog could never fire during a hung dump, as the dump pauses it too.
  See api/logging-and-config.md and api/launcher.md. Not tested yet.
- Mercenary Portal settings added: `off`, Stone Knights and royals on/off, rooms per
  hero. Found on the way, a bug of ours: front end +0x6029 bits 4-7 are the map's
  player count, not free bits. Our session options wrote bits 4 and 5, so Add Computer
  Player could be blocked on some maps, and on a 2 or 3 player map the count could read
  back as keeper-dwarf-workers on. [verified in export, not seen in game] Session values
  moved to free bytes of the creature list rows. Rows can now be hidden, as joining
  players need lists of the host's size (game/mercenary-portal.md). Not tested yet.
- Renamed game/units.md to game/creatures.md and added game/rooms.md, with Game::Room
  renamed to the same rule (Training_Room, Hero_Portal, ...). The portal's room settings
  now take ids, under new keys (`-room-ids`), so config.yml drops the old name values
  by itself. Launcher stops if steam.exe is not running while steam_multiplayer is on.
  `steam_player_name` added. memory_4gb is on by default. Not tested yet.
- Reported, cause not found: a friend joining through a Steam invite saw the game
  "frozen" (Vulkan, Radeon RX 570). From the joiner's log only: minimized for 3 min, an
  idle video played and ended while hidden (17:37:47-55), the invite was accepted
  (17:37:56, 2 players), the window came back at 17:37:57, was left at 17:38:11, and the
  game closed cleanly at 17:38:54 (exit code 0). Not a hang [verified in the log]: the
  skipped-picture count kept 60 a second after he left, no crash report, no Vulkan
  failure, and the watchdog's "nothing shown for 10 s in front" never fired, so pictures
  were being sent in those 13 s. The log cannot say whether they changed or whether input
  worked, nor whether he opened Multiplayer > TCP/IP. Untested ideas: DirectInput not
  coming back after a restore; the menu after a video played while hidden.
- Reported: tried again, the friend cannot join the host's game (no logs from that try).
  Found a bug that would stop it [verified in the WEANETR export]: its session list and
  join threads block in recvfrom with no select first, and our hook only looked at
  Steam's packets once on entry, then waited on the real socket, which gets nothing.
  Replies from the host stayed queued, so the joiner could not see the game's reply or
  the join's answer. recvfrom now waits for either in 2 ms steps. Logs added for the first
  packet sent and received and for failed sends. Not tested yet.
- Tested: joining works, and the friend beat a master keeper computer player. Two reports,
  no logs or screenshots, causes found by reading the code, neither confirmed in game:
  - In the side panel, rooms, spells and traps shared the space with Creatures. That is what
    a pinned Creatures panel does (game-fixes.md). Likely a Shift the game still saw as held
    at level start; Steam's overlay is on Shift+Tab, used for invites. Not proven. If it
    comes back and the log has no stuck modifier line, look for what else pinned it.
  - Something in the ally window flickered. Its text goes through GuiTextCache, which
    compared 8 color bytes when the game uses 5; the rest is old stack data, so a run could
    look changed and draw only part of the view, then all of it next frame. Not proven to
    be what was seen: the window's sprite buttons do not go through that cache.
- Co-op campaign, first stage (game/coop-campaign.md): lobby rows, level and player chosen at
  start, campaign rules on multiplayer, Good and second keeper parts, shared camera waits.
  dk2-coop (added by the user to the reference folder) showed the way for a shared keeper; its
  addresses match our export. It replaces whole functions through Flame's loader, so here
  they are hooks. Hands and possession are not done, on purpose: wait for a first test of the
  foundation. Not built or tested.
- Reported: with an AI in the friend's place, the chosen level did not load. No log held a
  "Co-op campaign: level" line. Cause found in the code: 0x52F140 only starts the 3D menu, it
  does not return when a game starts (0x525xxx routine), so the level was never set. Fixed at
  0x5253EA. The user cannot test with the friend for now and asked for the rest, so hands,
  possession, dig marks, endings, progress, the heart check and camera saves were added
  (coop-campaign.md, CoopHands.cpp). Not built or tested.
- Still "does not load". Log: "not a co-op level (level 1, play mode 2)": the game was started
  from the skirmish screen, where the choice had no effect. The rows were only in the multiplayer
  lobby. Added them to skirmish as a solo session. Also found: the host's list was stale at the
  start, so a click could be undone in real multiplayer too. Not built or tested.
- Added sell_key (default the ` key); pressed again it now lets go of Sell. Escape: first left
  alone, as action 24 calls the Options click in the code, but the user reports it does not open.
  Cause not found, so added options_key, which toggles the menu only if the key event did not, and
  a log of the checks on each of the first 8 presses. Dead end: no write to 0x2722 or the pause flag
  in our code, and the key table's default for action 24 is Escape.
- CPU threads test logs: clean, about 42 fps in a level at 3840x2160 as before (limit 60). Seen in
  passing: Vulkan swap chain 1280x720 for that 3840x2160 screen, and no DPI awareness set in
  Runtime, so Windows' scaling may be involved. 2.7 s between "shutting down" and "ModManager
  shutdown" once, in a run that exited with code 0.
- Removed the Renderer row from Graphics Options; config.yml is now the only way to choose.
  Render::Layer::Switch has no caller now.
- Added keys for the side panel's tabs (panel-keys.md) and cpu_threads (cpu-threads.md). Found
  that patch.dll is what limits the game to one logical processor. Neither tested.
- Tested alone: the level now loads. Level 24 as the Good player: after killing the King the
  game stayed in a cinematic, and the King did not show in the hand. Found: the Good player
  never gets a result from the game, added (not tested). Dead end so far: 0x4BF410 (Horny) is
  not reached with parts apart, and nothing in the ownership code is specific to the King.
  Added log lines for both, as neither cause is confirmed.
- Reported: games with the friend freeze, then go on. Cause not known; see "Stalls in a game"
  in steam-multiplayer.md for the three candidates, the logging added and what was changed.
  Not built or tested.
- Feature request: in-game level editor, button between Continue Campaign and Skirmish. First
  look only, no code (game/level-editor.md). The gap there is 92 and a button is 172, so
  Skirmish and Load Game must move down. Terrain, traps and doors are lists in
  Data\editor\*.kwd, not fixed in the exe. Those files are binary and CLAUDE.md only allowed
  reading text files in the game folder, so they were not opened; asked the user, who allowed
  it (read-only, now in CLAUDE.md) and agreed triggers wait for stage 5.
- Editor stage 1 (LevelEditor.cpp, game/level-editor.md). Found the game's own hold: the world
  tick stops after its actions while session +0x284 is set, the menu-open field. Setting it for
  good would stop the interface's input too, so it is set during the tick only. Editor goes
  through the skirmish screen for the map choice, to avoid a new screen for now. Dead end: the
  Single Player buttons' images do not come from 0x5329A0, so FrontEndTexts cannot add a text
  there; the button draws its text live, without the game's glow. Not built or tested.
- Tested (Swiss Cheese, Vulkan): button and level opened. Things bobbed and appeared and vanished.
  The log has actions 1, 125 and 102 dropped, then the user closed the game: Quit was blocked
  (1 and 125 leave the level). Fixed: those two carried out, turn and blend between turns held
  still as under a menu, menu open kept right. The button now uses the game's baked images and
  glow (0x533570, text id answered by a hook on 0x553A60). Correction: "the images are not found"
  was only that the baker is another function. Not tested yet.
- Tested: no option button worked, killed from the launcher (0xC000013A). No actions after the menu
  opened in the log. Cause not found: the menu field is set the way a normal game sets it. Dropping
  it brought the first version's working buttons, so the menu now takes multiplayer's branch, which
  has no pause (0x428710 with mode 3 for the call). Not tested yet. Open: the session's turn may
  stay under 2 while held, which makes the communication base 0x521BA0 hand out a turn each frame.
- Editor stage 2 (LevelFiles.cpp, game/level-files.md): reader, writer, check over all stock levels,
  copies in KeeperForge2\maps, Editor opens a copy. Bash is refused here, so the format comes from the
  editor's reader and viewing the files. Not built or tested.
- Tested: the options buttons work now (a save screen reached, screenshot). The save name's bar was too small: fixed
  pixels, scaled now. The log of that run has no "Level files" lines and KeeperForge2\maps does not exist, so the stage 2
  check has not been run. Stage 3 written on top of it at the user's request: paint, pick, undo, save. Not built or tested.
- Tested, nothing could be painted: "section 0xDC says it has 26919 bytes, 26907 left". A section's size counts its 12 byte
  header; my reading of the editor's skip code was wrong. Fixed. As the check is easy to miss (it was not run in this test
  either), a read-only check now runs at the first Editor click in a run. Not built or tested.
- Tested (Bottleneck): the check passes, 67 levels, 0 failed. Painting and undo ran with no crash. F5 was never pressed, so
  KeeperForge2\maps is still empty and a saved copy opening is untested. User asked for the terrain in the UI: a clickable
  palette at the left of the screen (level-editor.md), in the options menu's view. It replaces the chat line status. Not
  built or tested.
- User's design: the side panel's four tabs hold the editor's rooms, workshop items, spells and creatures, with the icons in
  Data\editor\Graphics. EditorPanel.cpp. The left palette keeps terrain, owner, Undo and Save. Icons cannot be viewed from
  here (Read refuses bitmaps), and each item's icon number is not found, so the file order is used. Not built or tested.
- Tested: tabs did nothing. The log had no icon sheet lines, which showed nothing was drawing: 0x409F30 is only called in
  multiplayer, so neither the palette nor the panel had ever run, and the status text never showed. Dead end worth keeping:
  a hook being installed does not mean it is reached; check its caller's conditions. Moved to 0x419DF0. Also: Spells tab
  replaced by Terrain, owner chosen with the heart. Not built or tested.
- Tested: panel draws, heart cycling correct in the log. Crash painting at y 0 (game's neighbor loop has no bounds check, see
  level-editor.md), outer ring now refused. Some items have no picture: sheets have fewer cells than records. The user's .png
  copies are BMP data under a new name (first bytes 42 4D), so they still cannot be viewed; asked for real PNG exports.
  Saving still not tried (KeeperForge2\maps empty). Not built or tested.
- User exported real PNGs (docs/game/tab images), so the sheets could be read by eye. They follow file order then end in "?";
  the first guess was close but ran off the end. Tables added, and terrain icons follow the owner's color. Not built or tested.
- Hand removed in editor sessions, Windows' pointer used instead (level-editor.md). I expected the hand to be a 3D model and it
  is not: it is the software pointer, with a null image path the game already uses. The other half is DirectInput's exclusive
  mode, which keeps Windows' pointer hidden and still, so the mouse goes shared for the session and the position comes from
  GetCursorPos, scaled to the screen. Editor only: a normal game needs the hand for held things. Not built or tested.
- Tested: Windows' pointer on in the log (full screen). Panels shown in screenshots. The user suspected Rock and Impenetrable
  Rock were swapped, and they were: my table was by eye, and Terrain.kwd has the real cell at +0x196. Claimed Path and
  Reinforced Wall were swapped too, and Hero Lair had none (the empty 12th cell). Terrain now reads the file. Traps, doors,
  rooms and creatures are still by-eye tables. Also: the Rooms tab showed the Creatures tab's icons and name past its own. The
  panel cleared through 0x42CA70, which GuiTextCache holds back until a text call differs, and no text means it never runs.
  Dead end worth keeping: a view with no text in a run must not rely on that clear. Not built or tested.
- Tested: 5 saves to KeeperForge2\maps with no warnings, and the game's pointer came back on leaving. Never reopened: after leaving,
  the log's two later games have no "opening" line, as the game returns to the skirmish screen and only the Editor button armed
  the session, so Start ran an ordinary skirmish on the stock map. Leaving a session now re-arms it. Not built or tested.
- One-off, not explained: "thread has held the renderer's lock for 5 s" while minimized in the menu (17:45:45 that run). Its stack
  scan shows the pointer draw 0x5DC85C and what could be exception dispatch in ntdll, but the scan can include stale values, and
  the game carried on with no crash report. Not related to the editor as far as the log shows. Watch for it again.
- Creatures: placing, a right click menu with Delete, undo and saving (level-editor.md stage 4, level-files.md Things). Best find:
  level load's per-record functions can be called directly, so nothing about making a creature was rewritten. Second tab named
  Terrain with a rock icon. Dead end: a shared menu in the palette's view, see the text cache note. Not built or tested.
- Crash deleting creatures: a removed creature stays in the thing table with its position and in-use mark, so scanning by those finds
  ghosts and removes them again. Check its list at +0x12 too. Dead end worth keeping: 0x6ED540 does not mean alive for pooled things.
  Also added a level choice to the creature menu. Not built or tested.
- Owner line vanishing: GuiTextCache's held-back clear wipes the text it skipped, and a view redrawn only on change never gets it back.
  Dead end worth keeping: drawing only on change does not mix with that cache, so such a view must not clear through 0x42CA70. Palette
  moved to its own view. Arrow left showing after a session: count restored by what was raised, and rechecked on mouse movement. Level
  numbers not showing: not explained, leading spaces removed and a log line added. Not built or tested.
- Traps and doors, as creatures: level load's make functions, vtable +0x30 to remove. Unlike creatures they are freed on removal, so no
  pool ghosts. Not built or tested.
- Sale of doors, traps, room tiles for 0 gold, blueprint/Unique/mercenary variants by right click on the panel. Found: 0x6A54B8's action
  table is zero in the export as code fills it at 0x511A80, so handlers are found there and not by reading the data. Painting over a room
  had been selling it all along, with gold and text. Keeper 1's heart looking unfinished: not found. 0x4DA560 and the heart tick were read
  with nothing local-player specific found, so a screenshot was asked for. Not built or tested.
- Keeper 1's heart: found from the user's screenshots. Local player's arches and rings fade in on the heart's tick, which a held world never runs.
  Same trap as sales: anything the game finishes in a tick has to be finished by hand here. Also seen: saved copy reopens, Unique and mercenary placing in the
  log, wand still on the Terrain tab, black minimap until painting. The 17 s "nothing was shown" warning was only the game being minimized. Not built or tested.
- Minimap: only draws when the turn changes, and the turn is held, so a later session on the same held turn stayed black. Dead end: revealing again helped only by
  timing. Terrain tab: log showed the icon was drawn, so the game's button covers it; that button is now left undrawn in a session. Not built or tested.
- Renderer: original dropped at the user's request, vulkan the default. An old "renderer: original" gives Vulkan and is rewritten. Not built or tested.
- Level numbers "not rendering" was the bottom bar drawn over the menu (screenshots). Dead end: the earlier text and spaces theory. Minimap
  needs both fixes. Mercenaries lost on reopen: not found, logging and a later recheck added. Not built or tested.
- Save as a named level in Data\editor\maps (stock levels protected, editor-made ones listed in KeeperForge2\maps\editor-levels.txt),
  thumbnails Data\editor\maps\Thumbnails\<name>144.bmp drawn from the tiles, Ctrl+S, and testing through the game's restart 0x513870 both
  ways. The description's first field is the wide file name, with nothing else in stock levels' first 0x1180 bytes. Not built or tested.
- Crash going back to editing after a test: 0x417310 divides by the shown panel count, 0 in the editor. Hooked to do nothing in a session.
  Mercenary marks are saved (3 in the log); the game's loads ignore them, so tests apply them too. Not built or tested.
- Map size: no largest in the game. 0x44E9E0 and 0x4D1840 allocate from the file's width and height with no check (16 byte tiles, a
  byte grid, a two byte grid). Our own 4096 limit removed; only a size that overflows 32 bits is refused. Byte-sized coordinates past
  255 not looked for. Stock .kwd files hold no copy of the size.
- New map: name, width, height typed on the palette; Swiss Cheese (no triggers, four keepers) as the template, Rock in an Impenetrable
  edge, keeper 1's heart (its room record moved) in the middle, loaded in place. Room record +0x12 type, +0x13 player. Not built or tested.
- Objects: level load's object path is 0x56F6E0, 0x56F730 and the maker 0x520BB0 (found from the reader's table at 0x51E13A), same
  calls as traps. Object vtable 0x66D654, removal frees through 0x4B7B10. Types are Objects.kwd's order from 1 (+0x35A). No icon
  number found in Objects.kwd; the official editor's mapping was not followed up, and looking at ObjectIcons.bmp is not allowed, so
  file order is assumed. Action points decoded from 0x56F790 and all stock levels. Placing, deleting and undo for both, with Skull
  markers on action point corners. Not built or tested.
- Room props: one tile at a time gives fewer (5x5 Hatchery, 3 pens). 0x4E6BC0 clears a room's own objects and 0x4EDFC0 places them
  for the whole room on every add or sale, but 0x4EF1F0 (from 0x4F04F0, rooms with data +0x43D bit 0x80) places over only what was
  added and keeps its earlier ones. Fix: rooms touched by a stroke are sold and built again over their box in one go. Wall props
  go through events to world +4 +0x10 (0x517480 to 0x527150, vtable +0x28); what handles them not found. Not built or tested.
- Correction to the room rebuild above: selling and building again is gone (it lost gold and played sounds). Found 0x4ECDB0, the
  game's own refresh of the rooms beside a tile after a terrain change; the editor never called it, which is why wall props did not
  follow painting. Pens: a room's objects are re-placed by walking its chain of tiles (room +0x2D first, +0x2F last, tile +4 next,
  bytes), which is in build order; the editor now sorts it into row order and refreshes. Rooms keep coordinates in bytes, so none
  past 255 on bigger maps. Not built or tested.
- Action point outline (looked into, nothing written): the hand's tile box is the only box the game draws on the map. 0x40ACD0 passes
  interface +0x132F (x1, y1, x2, y2, end not included: one tile is x to x+1, set in 0x40AE80) and the owner's tag at +0x10BC to
  0x44E130 and 0x44E1C0 on the engine interface (*(interface +4) +0x10), and bit 0x10 of interface +0x132E to its +0x36 [verified in
  the assembly]. Each keeps one box (+0x10 and +0x4E, with the last one joined into +0x24 and +0x62, likely the area to redraw). So
  more than one box at a time needs the terrain drawing that reads it, not found yet.
- Action point outlines: found where the box is drawn. 0x43CC30 (a tile's draw, vtable 0x66CDC8) puts the drawer's box piece on
  each border tile with its sides in piece +0x81 and draws it through renderer slot +0xE8. Hooked so every action point's border
  gets the piece too; Skull markers only if the hook fails. Color is the hand's box color; where its look comes from not found.
  Not built or tested.
- Editor batch 1: brush sizes, box, flood fill, copy and paste, redo, moving things, the Delete key, painting clears things off
  rock, hearts and hero gates refused for painting, and a warning on quitting unsaved. No new game research; built on what the
  editor already used. Not built or tested.
- Editor batch 2: the level reader 0x56C960 sends each section to its own reader (0x56CBD0): Players 0x56DAD0 (0xCD byte records,
  handled by 0x51EF80 and 0x51F0B0), Variables 0x5716E0 (16 byte records, 0x521790 by kind), the level 0x56C4E0 (description copied by
  0x56C100). Availability is variable kind 2, handled by 0x51CA60 and 0x51CAE0; categories found from the player lists each setter
  changes. Dead ends: the kind table's source 0x6A5CD0 is empty in the export; running a script over the stock files was not allowed
  here; the official editor's field labels are in its dialogs, not the export. Level texts, players, availability on the panel and
  mercenaries in real play written. Not built or tested.
- Availability values settled by a read-only survey of all 67 stock levels' Variables and Players (scripts in the session's
  scratchpad, paths kept inside the script as settings.json denies commands naming the game folder): 4 available from the start,
  3 researchable, 1 not available; kind 1 is the creature pool. The editor's labels and cycle were wrong (3 and 4 swapped) and are fixed.
- Editor batch 3: Triggers section read by 0x571290 (0xD5 triggers, 0xD6 actions, 16 bytes). Record links and tree roots (level
  description +0x1180, player +0xA6, action point +0x14, creatures +0x18/+0x14) found by surveys of all stock levels; type names from
  the official editor's tables, rebuilt from its listing by a read-only script. Hero party roots not found. Action point bytes in 8
  action types found by survey. A read-only trigger view, action point rename, and the delete warning written. Not built or tested.
- Editor batch 4: the level's trigger check 0x518270 and action handler 0x518AD0 give the Flag and Timer triggers and about 20 action
  types' bytes. Repeat count +0xF (0x508640). Hero party records decoded (0x570210, 0x50FA70) and checked on all 220 stock parties.
  Correction: the survey's byte 1 action point matches (Generate Creature, Camera Follow Path, Change Room Owner) were player numbers.
  Triggers block size field kept as is (it follows no rule over the stock files). Not done: dead bodies, effect generators, plinths,
  cameras (no stock examples), the outline color. Trigger and party editing written. Not built or tested.
- Editor batch 4, rest: thing kinds 0xCA (dead body), 0xCB (effect generator), 0xCD (the level's camera settings) traced from the things
  reader to level load's makers; plinths have no maker in the game. Dead bodies and effect generators placed through those makers;
  no removal found. Outline color: engine things keep a tint at +0x6E (0x436317, 0x43639B); the outline sets it from config while
  drawing, unverified that the box uses it. Not built or tested.
- Tested (2026-11-06): New map 1024x512 crashed at 0x4ECF7B while the level loaded, in the room code under the tile loader. Room tile
  places are bytes; the heart at 512,256 wrapped and a neighbor was read at y -1. Fixed: heart at most at 251,251, room tiles past
  254 refused. Before that in the same run: painting, undo, Save as, Test and back, dead bodies and an effect generator placed, no crash.
- Renderer (2026-11-06): borderless window (window_mode, the game's _BullfrogLibScreen restyled to a popup over its monitor, not
  minimized on focus loss) and anti-aliasing (anti_aliasing: FXAA, SMAA, MSAA x2/x4/x8, default smaa x4) in DirectX 11, 12 and
  Vulkan. Vulkan needs compile_vulkan_shaders.bat run again for the new shaders. Not built or tested.
- Tested (2026-11-06, Vulkan, SMAA x4): starts, logs SMAA x4 (high), window made borderless, ~17000 frames, exit code 0. About 35 fps
  at 3840x2160 (42 before anti-aliasing in an earlier log). Window and swap chain were 1280x720: the process was not DPI-aware on a
  scaled 4K desktop. Added per-monitor DPI awareness in Renderer::Install. Not built or tested.
- Frame rate (2026-11-06): the per-frame cost at 4K is the frame read back for the 2D panels (a GPU wait plus 33 MB) and uploads.
  Added: whole-frame clears on the card (Backend::ClearFrame; a whole clear used to read the frame back first, then fill and
  upload it), and a frame timing line in the log every 10 s. Not built or tested; the timing decides the next step.
- Tested (2026-11-06, Vulkan, SMAA x4, 4K, 26 min): average 56 fps (35 before), many 10 s windows at 60. Per frame: 1 read back
  (8 ms, all from blits, no locks), 1 upload (3 ms), 1 clear on the card. Added: blits onto the frame drawn on the card, and the
  shown frame kept on the card at a flip for the screen surface. Not built or tested.
- Tested (2026-11-07): gpu_blits on froze Vulkan (VkResult -4, device lost) and crashed DirectX 12 (0x887A0005 device removed,
  then an access violation in D3D12Core.dll drawing with a pipeline that could not be made) on the first loading-screen frame;
  DirectX 11 fine; all three fine with gpu_blits off. Cause: DestroyTexture between a present and the next Commands() put the texture
  on a slot that frees its list as it starts, before the frame just sent finished with it (an old bug, hit every frame now that
  per-frame loading pictures become textures). Fixed by starting the frame first; draws with no pipeline are skipped. Not built or tested.
- Tested (2026-11-07, gpu_blits on, 4K, SMAA x4): Vulkan 59.6 fps avg (7 min), DirectX 12 58.5 (1.5 min), uploads 0 per frame,
  1 blit on the card and 1 picture uploaded per frame, 5 screen read backs in 7 min. Left: 1 read back per frame by a blit, about 9 ms,
  with no upload after it, so a blit out of the frame (likely the save-under of the mouse pointer). Added: such blits read back only
  their area (Backend::ReadFrameArea), and the first 5 are logged with their size. Not built or tested.
- User reports (2026-11-07): imps kept mining tiles after they were untagged. Found: untag removes the task, but the dig and gold
  mining states (0x4655D0, 0x465870) skip the task check while the creature's action runs. Fixed in CreatureFixes by stopping the
  action first when the task is gone. Also: the possession HUD's bottom rod stopped at the 4:3 width (screenshot); it is sized
  through the point conversion with x 2560, which now maps to the screen edge. Neither built or tested.
- Renderer (2026-11-07): copies out of the frame into a whole surface are now done on the card (the surface keeps its picture in its
  blit texture until the game uses it otherwise, then it is read back). Not built or tested.
- Tested (2026-11-07, Vulkan, 52 min, 4K): 59.3 fps; untag and possession bar fixes work. Copies out of the frame: 504x387 whole
  (3 times, now on the card) and every frame 492x238 into that same surface at 12,153 (likely the save-under of the hand cursor), still
  read back by area at ~2.5 ms. Partial copies into a surface are now done on the card too (its picture uploaded once first). The 40 s and
  12 s "stalls" were the game in the background (timing and the watchdog now restart there). Review Objective showed a black screen
  with only the two headings: the parchment and text (a view uploaded in tiles, 0x42CDF0, drawn by 0x42CEE0) were missing. Cause not
  found yet; gpu_blits now also takes "clears" (only clears on the card) and "off" also turns the clears off, to find which part.
- Review Objective black at 4K is not the renderer (same with gpu_blits on, clears and off; screenshots). Cause: the surface cache was
  capped at 127 pages (signed byte push at 0x592FAC) but a full-screen GUI view at 4K needs 128 on top of the game's 24. The setup
  0x592DB0 is now hooked to get the full count (168 at 4K). Not built or tested.
- Panels on wide screens (user request): the editor's icon grid now uses the spare width (27 columns at 16:9). The game's own panels:
  window 1 (rooms) has a fixed button list (0x68FC18, 0x50-byte buttons); each creature page (windows 7-17) has one button whose
  draw 0x417850 lays out the whole icon grid in code. Widening those needs each panel's draw and click code worked out. Not done yet.
- Editor (2026-11-07, Patrol): saved changes missing in Test and after Back to editing. A stock level without a copy when opened kept
  the game's level name on the stock files; Test and Back reload by name. Save now points the name at the new copy. Not built or tested.
- Tested (2026-11-09): surface cache at 168 pages, Review Objective still black with only its headings. So the cap was not (all of) the
  cause. Found: the headings are a text button (0x428C30 -> 0x428A30, only drawn when [0x75AD4B] is 1 or 4); the parchment and text are
  the view at interface + 0x2737, cleared with the patterned fill and drawn by 0x419DF0 at layout (0, 180) while + 0x2726 is set.
  Added for the next test: a log of how many tiles of each large view have no surface, and panel_text_cache (off turns GuiTextCache off).
- Tested (2026-11-11): every tile of the objective view has a surface (31x45, 0 of 1395 empty), and panel_text_cache off changes nothing.
  The user's reference screenshot (lower resolution) shows the 3D world darkened behind the objective (darker on the left) and an
  "Exit" button; at 4K the world is black and "Exit" is garbled. Both are in the menu view (interface 0x6CEA10 + 0x2737). Added a dump
  of that view's fields and pixels (Logs\objective-view.bmp) the first time it is drawn with the objective open.
- Review Objective black at 4K, found with a renderer trace (2026-11-12): the world is still drawn (about 900 draws a frame), but with
  the objective open its textures were all black with alpha 128 (the view's gradient) and there were 2547 texture uploads a frame
  against 206 normally. The surface cache is shared by the world's textures and the GUI views; the full-screen view pushed the world
  out every frame. Cache now 0x68 + full-screen view pages * 1.25 (264 at 4K). Not built or tested. The trace and the view dump stay
  until this is confirmed.
- Tested: 264 pages changed nothing. [verified in the export] With Direct3D the engine never uses the page cache: 0x592EA0 makes a
  pool of 512 device textures of 128x128 (0x592BD0, "push 0x200" at 0x593247) shared by the world and every GUI view tile. Now
  512 + one per full-screen tile (2552 at 4K). Not built or tested.
- Tested (2026-11-12): Review Objective fixed at 4K with the larger texture pool (2552): world visible behind it, Exit readable,
  no warnings. Removed the renderer trace, the objective view dump and the per-view tile check. panel_text_cache and gpu_blits stay.
- 2026-11-12: config.yml shortened (one-line comments, blank lines only between groups). New logs_kept (default 10, 0 = all) and
  screenshots_cleared (default on: SHOT*.PNG/BMP deleted at game start). Screenshot requests ignored for 2 s after one is taken.
  Not built or tested.
- 2026-11-13: hooking is our own engine (HookManager + InstructionDecoder: 32-bit x86 lengths, relative call/jmp/jcc/loop moved
  into the trampoline, other threads suspended and moved out of the patched bytes while they change; no AVX/VEX code at a hook's
  start). SMAA is our own morphological AA (no lookup textures; coverage worked out in the shader). VulkanShaders.h has empty SMAA arrays until compile_vulkan_shaders.bat is run; Vulkan turns SMAA off until
  then. Not built or tested.
- 2026-11-13: first run with the own hook engine crashed at start (0xC0000005, no runtime log): the jump written over a hooked
  function was made in a buffer and its distance worked out from the buffer, not the function. Fixed. Not built or tested.
- 2026-11-13: tested, own hook engine and own SMAA (Vulkan SMAA x4) run, clean exit. 5x5 Hatchery built in game had 2 pens. Placing
  walks the room's tile chain (build order) and takes the first tiles that fit, so 0x4EDFC0 is now hooked for every game (not only
  the editor) to put the chain in row order first (game-fixes.md, Room objects). Not built or tested.
- 2026-11-14: creature types. The official editor's "Modify Creature Properties" was traced: its controls (dialog resource)
  to its fields, its fields to the place its file writer gives each in a record, then each place checked as read by the game's
  own converter 0x568E30. All of Warlock's flags and values matched the user's screenshot of that window. Layout in
  creature-types.md. Dead end: the editor's in-memory copy is not the file's layout (doubles, other places), so its window code
  alone gives no offsets; the writer is what ties them. Found on the way: the game reads only 0x1549 bytes of a record
  (0x56A250), so the editor's newer flags past that (H.Portal, Immune To Lightning, Stone Knight, Emotionless) do nothing in
  this game; a stock level (Furnace_hero) carries its own creature list in a Globals file. New: EditorDialogs (Windows' own
  windows on the game's thread; the game's loop 0x5B2DD0 pumps every window, but has no IsDialogMessage, so a WH_GETMESSAGE
  hook does that) and the creature properties window, from a right click on the Creatures tab. Not built or tested.
- 2026-11-14: creature window gets Rename, Add Type (a copy, at the end) and Delete Type (only the last, only after the 46
  shared ones, refused while the level or another type uses it), and config.yml editor_dark_windows (default on). Window
  gives back the whole list now. Whether the game copes with more than 46 types is not known. Not built or tested.
- 2026-11-14: the creature window's other parts. The official editor's file reader hands each part of a record to setters
  that only its windows also call, so each window's fields were tied to file places through those (the reader's copy keeps
  the record 0x400 bytes in). Checked on stock data: Warlock's jobs and attacks, Bile Demon's gas resistance, Firefly's light.
  Flag bits 28 (camera rolls) and 29 (melee plays its animation) found. Eight windows added from a table of fields; the art
  resource windows left out (records not mapped). The Attractions frame was white because a classic group box only paints
  its border inside a window that clips its children; now drawn by the window. Not built or tested.
- 2026-11-14: screen capture tools (Lightshot, Print Screen hot keys) did nothing while the game was focused. Found: every
  DirectInput device starts out exclusive and foreground (5, 0x5DCB40); the keyboard only becomes shared on a full screen mode
  change (0x5BC280). Exclusive keyboard keeps keys from other programs. GameFixes now hooks 0x5DCBE0 to keep the keyboard
  shared. Also: editor windows got "Copy a picture of this window" on their title bar menu, and the attractions' unused
  target and amount controls are hidden instead of disabled (disabled classic controls paint in Windows' gray). Not built or
  tested.
- 2026-11-14 (tested by the user): with the shared keyboard, Windows' own Print Screen capture works while the game is
  focused, and the attractions area is no longer white. Lightshot on Numpad 9 still does nothing while the game is focused.
  Likely cause: the game runs elevated (the launcher requires administrator, Launcher/CMakeLists.txt), and Windows does not
  pass an elevated window's keys to a normal program's keyboard hook. Running Lightshot as administrator should get past it.
  [unverified]
- 2026-11-14: art resources. The official editor's art resource object keeps the file's 0x54 bytes after its own pointer, so
  its accessors gave the layout; its art window gave each type's fields and the boxes' bits; its message map gave which button
  opens which resource. Animation slots named from stock creatures' file names (the editor's button order could not be tied
  to slots: its dialog's control ids are not in the export). Added the art window and its six buttons. Also: a creature's
  kind (Unique, mercenary) is chosen in the creature window's "Places as", shown only for a type that has one; Shift + right
  click no longer does it. Not built or tested.
- 2026-11-14: placed creatures. The user asked for per-creature stats; a level only keeps a creature's position, owner, level,
  health, gold and flags, so (the user's choice) stats stay the type's, with an opt-in "own stats" that copies the type for that
  creature. Right click on a placed creature opens the creature window on it (level, health, gold, dead); the side panel's window
  sets the level and dead body placing for new ones; left press chooses and drags a creature, Delete deletes the chosen one.
  Found on the way: placing a creature of a type added in the editor would make one the game has no data for; now refused until
  the level loads again. Not built or tested.
- 2026-11-14: creature health and live drag. The game's level creature maker (0x5208B0) turns the record's whole percent into
  1 + trunc(percent x 0.01 x most - 1.0) hit points (at least 1), most being the type's Health at level 1 or Health times a
  per-level percent from the game's settings; current hit points are at creature +0x40. A level cannot hold fractions of a
  percent or exact points, so the editor keeps exact points in its own `.health` file beside the Things file and puts them on
  the creatures at load. The creature window got a percent box (two places, petals of six) and a linked points box. Dragging a
  chosen creature now makes it again on each good tile under the pointer, committing its record only on let go. Not built or
  tested.
- 2026-11-14: undo of exact hit points. Undo steps now carry the exact hit points list as it was before the step when the step
  changed it; a change of hit points alone gets a step of its own. Deleting, re-levelling or painting out a creature now drops
  its entry (undo brings it back), so an old entry cannot land on a new creature. Not built or tested.
- 2026-11-14: create-imp-price-rises. Found the seven places the game works out Create Imp's price (all gated on spell 4)
  and that the free number they subtract (0x6F28D7) is also the heart's worker minimum and is read by worker upkeep, so it
  is left alone; instead the count part of each sum is made 1 when the option is off. A session option (flag 0x20), so in
  multiplayer the host's value is used. Not built or tested.
- 2026-11-15: dead body removal. The dead body maker (0x5212D0) goes through the creature collection's 0x4B7FC0, which makes a
  thing with its own vtable (0x66D904). That vtable's +0x30, the remove slot objects use, calls 0x4B81A0 with the body's tag,
  which unlinks, destroys and pools it. Its position is the usual +0x1A/+0x1E (0x4B5020 from its init). The editor now removes
  a body on Delete and on undoing its placing or a creature's turning dead, and makes it again on undoing a delete. Effect
  generators still have no removal. Not built or tested.
- 2026-11-15: effect generator removal. Its maker (0x521380) goes through 0x4B81E0, a twin of the dead body's: its thing has
  vtable 0x66D814, whose +0x30 calls 0x4B8250 (unlink, destroy, pool, count +0x3A down). Its type (record +0x1D) is at +0x44 and
  its tile at the usual +0x1A/+0x1E. The editor now removes and remakes effect generators on Delete, undo and redo, as it does
  dead bodies. Not built or tested.
- 2026-11-15: four reported game bugs looked at (game-fixes.md, todo). Backstab and the North/South advantage are the same
  code: the bonus damage angle at 0x4990C4 is taken from the attacker's position only, never the target's facing, so the
  +y side gets up to +51%. The computer's threat total (0x4F9B90, 0x4FA000) is 32 bit but stored in a 16 bit slot, so a big
  army wraps. Portals need a pool entry (Variables kind 1) and trigger action 6 only sets availability; survey of the stock
  levels: Temple made available in Level15a/15b/17 with no Dark Angels in any keeper pool. Dead end: checking pool creatures
  against their room's availability gave noise (start rooms such as Lair and Library are not in Variables), dropped.
- 2026-11-15: threat clamp and backstab written (GameFixes.cpp). The creature facing is + 0xF4 in our build, not Flame's
  +0xF0 (their struct offsets are 4 lower: position +0x16 there, +0x1A here); confirmed by 0x4B2C10, which turns + 0xF4
  toward an angle made by 0x65FB54(target - self). Threat: five 16 bit stores of a total jump to clamping stubs. Not built
  or tested.
- 2026-11-15: portal pool fix written (CreatureFixes, called from MercenaryPortal's portal hook). Found: the pool has an
  entry for every creature type (a left-out type is 0, so "no entry" never happens), 0x506460 only adds to an existing
  entry, and the room availability setter stores the level's raw values (4/3/1). Kept to Dark Angels (Temple) and Black
  Knights (Combat Pit): a rule for every creature would also hand out Salamanders and Rogues from the early levels on,
  which is the campaign's pacing. Not built or tested.
- 2026-11-16: second bug list. Written (not built or tested): sleeping possession (0x45A93F), Wooden Bridge burning
  (0x4507B5 and the build call), room limit crash (sale and build calls 0x4C5E3B / 0x4C5CC5: a split with no rooms left
  leaves tiles with no room), drop rules checked when the drop runs (0x4C0F70, after CoopHands' own hook of 0x4C0EE0).
  Already done before: workshop, threat response, making a home, chat box crash. Dead end for now: the Heart's tick
  0x4D9A60 is raw bytes in the export, and there is no disassembler here, so the 50% warning is not found yet.
- 2026-11-16: Heart warning found by decoding the Heart tick's raw bytes by hand (0x4D9A60..0x4D9B02): exact 50% / 10%
  checks every tick, so healing through 50% warned too; fixed to warn only while health is going down. Movie icons: the
  menu table (0x6B0DD0) and how picture, film and title are chosen are known, but not which two clips (Text.str is
  compressed past its code page). Heart icon flashing after a load: not found; event icons and the event tick ruled out.
- 2026-11-16: sound log added (log_sounds, SoundLog.cpp) to find the Library's endless paper sound (#13) by watching it
  happen: the sound system's vtable 0x66F7FC, category lookup 0x567790, play 0x5678F0 / 0x567810, command 0x567B30.
  Static search was a dead end: 133 places play sounds and nothing names the paper sound's id.
- 2026-11-16: Imps per face. Slots are 3 per face x 4 faces; the search takes a free slot or steals from a live,
  working, farther holder; release code clears the right slot for dig and gold. Found no wrong count in the code itself;
  the weak point is a slot whose holder is no longer working it (never free, never takeable). Search reads now clear
  such slots. Whether this is the reported bug is not confirmed. Dead ends: face order (always visits all 4) and
  release (0x4647D0, correct for dig and gold).
- 2026-11-16: skirmish Dark Angels / Black Knights: dead end in the stock data. Both are portal creatures in
  Creatures.kwd and in Furnace_hero's own list, and every stock skirmish/multiplayer map lets every keeper have the
  Temple and Combat Pit. Where the lobby's starting counts come from was not found (0x549D10 sets other options).
- 2026-11-16: neutral creatures (#8): chosen behavior (b), they may be picked up and dropped but not into a Torture
  Chamber or Combat Pit. The drop rules 0x40D8F0 are hooked (after the drop-action fix has checked its bytes), so the
  interface and the drop action both refuse it. Not built or tested.
- 2026-11-16: chat over-read (#19) and keys after alt-tab (#21) written. The tab-out crash itself was the old
  DirectDraw display losing its surfaces, which our renderer replaces; only the stuck keys part was left to do.
- 2026-11-16: neutral creatures: Prisons added to the refused rooms (the user's choice).
- 2026-11-16: bump mapping of water and lava. Flame did not write a shader: the game has its own (setting
  0x759B10, textures BumpWater*/BumpLava*, stages set at 0x5895xx with an identity matrix) and needs a 16-bit bump
  texture format. Our layer now offers the formats and caps, keeps bump surfaces as 16-bit texels and the shaders do the
  bump step; bump_mapping in config.yml (on by default). Not built or tested; Vulkan needs its shaders made again.
- 2026-11-17: neutral creatures: the drop rule (Prison, Torture Chamber, Combat Pit) was taken out again at the user's
  choice; wanted instead is neutral creatures fighting back in the Combat Pit. Pit facts found (fighter bit 0x8000 at
  + 0x3E6, holder tag + 0x3CB, state 0x8B with handler 0x457CF0), but not where pit fights are paired. Dead ends: the
  flee checks (0x45EC80, 0x45EDA0) and the Prison's search (0x46E9xx).
- 2026-11-17: debug logs follow log_level now (the user's choice): the sound log has no setting of its own and writes
  at Debug level; a Combat Pit log was added the same way, to see what a neutral creature does in a pit.
- 2026-11-17: log clean-up (the user's request: small logs for players). 96 Info messages moved to Debug and 2 to
  Trace (the game window's messages, up to 300 a session; centered menu callers, up to 64); 67 stay Info. The rule is in
  docs/api/logging-and-config.md.
- 2026-11-17: creature API, first steps. Flame has no creature API: its "alternative resources" are folders put in
  front of the game's file lists, and creature mods are data packs. Written: ModData (mod data\ folders and Flame packs
  in front of the game's file lists at 0x55C700, and a read redirect for files opened by path at 0x5B3980, which is how
  Creatures.kwd and levels are read). Sweep for 46-sized tables: the player's portal table (+0xB57, 46 entries) is the
  one that matters; CreatureUsed (+0xFCE, 47) is never counted into. Not built or tested.
- 2026-11-17: portal table fix (types past 46) written in CreatureFixes: slots go to portal-capable types first and the
  per-tick pass reads each slot's own type. Sweep finished as far as the export goes: no other fixed per-type table
  limits new types (CreatureOnLevel is 256, CreatureUsed is never counted into, the other dump entries are not per
  type). Left for testing in game: the Creatures panel with many types.
- 2026-11-17: creature registry written (ModContent.h for mods, CreatureRegistry.cpp): mods export
  KeeperForge2_RegisterContent, add types by copy or record, and the game reads a merged shared list from
  KeeperForge2\cache through ModData's read redirect. Not built or tested. Left: levels' own creature lists, the
  identical-mods check for multiplayer, our editor.
- 2026-11-17: mods check for multiplayer (ModCheck.cpp): a fingerprint of KeeperForge2's DLL, mod DLLs and mods' and
  Flame packs' editor data, sent by the host in the lobby's creature list (rows 1-2) and compared by joining players;
  warning in the log on a mismatch. Also: the received-chat copy now always keeps the line's sender (a 16-bit tag before
  the text). Not built or tested.
- 2026-11-17: mods check sends KeeperForge2's version and build hash too, and shows a notice in the game's chat at level
  start (0x409EC0, sender 0x8181) when they or the mods differ. Sending mods to players was ruled out (code from any
  host, no file channel in TCP/IP games, mods cannot be swapped in a running game). Not built or tested.
- 2026-11-17: the host sees the mods notice too, in Steam games: every player's version, build and mods go into Steam
  lobby member data, and each machine compares with all other members at level start. Without Steam only joining
  players compare (lobby rows). Not built or tested.
- 2026-11-16: creator's name in rainbow colors (CreatorName.cpp), by Steam account: in-game player list (0x40A1FB) and
  chat lines starting with the name (0x40A00C, redraw through 0x409F30). Chat colors are { B, G, R, A, palette } (table
  0x756F34). Lobby rows colored by redirecting the color set at 0x544C1D (slot
  number in the caller's [esp + 0x10]). Not built or tested.
- 2026-11-16: bump_mapping on crashed the game about 20 s after the menu with "Out of memory allocating 8192 bytes"
  (Widescreen's engine heap); off was fine [verified, in game]. Cause [verified in the export]: the game's bump-map maker
  (0x591820) writes one row past its 16-bit buffer (width * height * 2, made by 0x590580) and reads one row past the
  height texture. The game's own heap hid that; our Windows heap's records got overwritten and later allocations
  failed. Fix: the allocation at 0x5905D8 asks for one more row (row size in EDI). Not yet built or tested.
- 2026-11-16: tested: the creator's name did not change color in the multiplayer lobby's chat (the in-game chat patch
  does not cover it). The lobby chat is button 0x1CD, drawn by 0x544FA0 (not disassembled in the export; read from its
  bytes): line = sender's slot digit + text, color from the sender's player color set at 0x5451D1; now redirected.
  Correction: 0x540220 and 0x540600 (the "%s : %s" lines with game text 0x5D4, records at 0x740350/0x740354, index
  0x740344) are not the lobby chat but a list entry's details box. Not built or tested.
- 2026-11-16: crash after switching back to the game (access violation reading 0x17090000 at 0x5FCB80, ESI 3 bytes
  before the end of a page) [verified, crash log]. 0x5FCB80 is in the pixel converter 0x5FC460 (called through 0x5FB6D0
  from 0x5D1EA9 while the game remade its textures for the new screen): it reads each pixel as a dword, so a 3-byte
  pixel at a block's end reads one byte past it. Widescreen's engine heap now adds 16 spare bytes to every block (the
  game's own heap always had memory after a block). Not built or tested.
- 2026-11-16: the Temple's pool had no bump mapping. It is a room liquid (engine method 0x5A2F10, texture "Water" for
  room type 13), which always makes a plain surface, while water tiles (0x5A2570) use the BumpWater maps. The room
  liquid's surface call (0x5A31E0) now gets the water tiles' bump values when bump is in use. Not built or tested.
- 2026-11-16: bump_mapping now takes all, water (water tiles and the Temple's pool) or lava (and off); luminance moved
  to its own bump_luminance setting. Water and lava each have one short jz past their bump values (0x5A2B0D, 0x5A2E54);
  the one not wanted is made a jmp. Old values on and luminance are turned into the new ones. Not built or tested.
- 2026-11-16: lobby chat notices for the creator and friends. The lobby chat's lines are added by the handler 0x542BF0
  (the network chat callback 0x7581C0 while the lobby is up; message = digit n, n digits equal to the word at 0x759C35,
  text; line = slot digit, slot name, ": ", text, ended by 0xFFFF in front end + 0x30CAB). Notices are added through it
  as slot 0 named "SYSTEM" for the call, from a hook on the chat drawing 0x544FA0. Friends' names in gold. Public
  Steam lobbies (steam_lobby, lobby data "keeperforge2" = "1", app 480). Not built or tested.
- 2026-11-16: lobby password (steam_lobby_password). A salted SHA-256 tag as lobby and member data "kf2pw"; public
  searches filter on it, a lobby with another tag is left, members with another tag are left out of the member list
  (no packets either way). Not built or tested.
- 2026-11-17: the public lobby matching was replaced by listing: each hosted game's Steam lobby is public with
  "kf2host"; the session list's broadcasts go to every listed host (Steam lobby list every 15 s while the list is open,
  F5 at once with a 5 s cooldown, since the game's window 11 has no refresh button); joining moves into the host's lobby
  first. Packet header now has a kind and the password tag. Not built or tested.
- 2026-11-17: Steam multiplayer now builds against the Steamworks SDK 1.65 (ThirdParty/, left out by .gitignore) and
  uses it as Steam games do: SteamAPI_InitEx, SteamAPI_RunCallbacks with STEAM_CALLBACK and CCallResult, the SDK's
  interface classes, instead of steam_api.dll's flat C functions looked up by hand with manual dispatch. steam_api.dll
  is delay-loaded (preloaded from KeeperForge2 or the game folder, exports checked). Added: rich presence
  steam_player_group / steam_player_group_size / status, member refresh on LobbyChatUpdate_t and LobbyDataUpdate_t,
  session failure logging, SteamAPI_ReleaseCurrentThreadMemory on game threads. Without the SDK the runtime builds with
  Steam multiplayer off. Not built or tested.
- 2026-11-17: invite key F6 on the multiplayer screens (session list or lobby): Steam's invite window for our lobby
  through the overlay, or Steam's friends list (steam://open/friends) when the overlay is not on. The lobby screen is
  known from CreatorName's lobby chat draw hook (SteamMultiplayer::OnLobbyDrawn). Not built or tested.
- 2026-11-17: mods' creature types now also reach levels with their own creature list (files ending Creatures.kld or
  Globals.kld with section 0xAA, opened through ModData's open hook: a copy in KeeperForge2\cache\levels with the
  types after the level's own, skipping names already there) and our level editor (shared list from mods' data
  folders, then CreatureRegistry::AddTo; mod copies show their stock type's icon; mod types count as shared, so not
  deletable). Also: bump_temple (off keeps the Temple's pool flat while water is bumped) and the creator/friend lobby
  notices drawn white. Not built or tested.
- 2026-11-17: mod API (ModApi.h, KeeperForge2_UseApi export, ModApiHost.cpp): log (mod's Error does not close the game:
  Logger::ErrorAndContinue), hooks through HookManager (FindPattern must match exactly one place:
  PatternScanner::FindUnique; only the game's exe; all taken back before the mod's DLL is unloaded), events (level
  start through SessionOptions, game turn from CoopCampaign's world tick hook), read-only game state (players, map,
  tiles, creatures), and a shared random number. Found for it: the simulation's seed is world + 0x15A0 [verified:
  0x5558D0's callers pass 0x6E5054's world + 0x15A0; Flame names it the game seed]; the mods' random number restarts
  from it and the turn each turn and never steps it. Not built or tested.
- 2026-11-17: console commands (ConsoleCommands.cpp, PerformanceStats.cpp): cpu, ram, gpu, fps, stats, reset, help,
  read from the shared console's CONIN$ on a thread of our own. Figures are sampled once a second:
  - cpu: GetProcessTimes over the time passed;
  - ram: working set, plus private memory and MemoryWatch's address space. GlobalMemoryStatusEx is used, since
    MemoryWatch only caps GlobalMemoryStatus;
  - gpu: VRAM from IDXGIAdapter3::QueryVideoMemoryInfo (per process, whatever renderer), load from the PDH
    "GPU Engine(*)\Utilization Percentage" instances named pid_<ours>_..., busiest engine;
  - fps: a frame counter in FrameRate's present hook, leaving out seconds while unseen (BackgroundRunning::IsSeen) or
    without frames.
  [unverified] Whether a wildcard PDH counter picks up engine instances made after it was added: the sampler opens
  the query again after 10 seconds with none of ours. Not built or tested.
- 2026-11-17: the crash at 0x5FCB80 came back (ESI 0x17096FFD, EDI 0x40, after a screen mode change, ~1 hour into a
  multiplayer game). The engine heap padding of 2026-11-16 did not cover it: this source buffer is the PNG body
  reader's row buffer, width * 3 bytes from the game's allocator 0x5B75F0 (malloc), not the engine heap [verified:
  0x5FC460 sets ESI = source + (count - 1) * step and walks back; the call is 0x5B656A's FUN_005d2860(surface row,
  row buffer, width)]. The 2026-11-16 note naming 0x5D1EA9 was a stale return address on the stack. Fix: GameFixes
  hooks 0x5B75F0 to add 16 bytes to every block. Not built or tested.
- 2026-11-17: the fps command also gives frame time (ms): present-to-present times in FrameRate's present hook
  (shortest and longest per second, taken by the stats thread; frames of 1 s or more left out). `ms` and `frametime`
  are other names for fps. Not built or tested.
- 2026-11-17: player ranks (PlayerRanks.cpp): None, Friend, Admin, Creator by Steam account; the creator and friend
  ids moved there from CreatorName.cpp, and an empty admin list was added. Admins are drawn dark red (#8B0000) wherever
  friends are drawn gold. CanModerate: moderators (Admin, Creator) act only on lower ranks, never on themselves. No
  moderation commands yet. Not built or tested.
- 2026-11-17: lobby chat join/leave notices for admins too ("Admin of KeeperForge2 has joined/left the lobby!"),
  white. Not built or tested.
- 2026-11-17: first moderation command, rank <SteamID64> <rank> (console). Ranks set by it travel as Steam lobby chat
  messages "kf2rank 1 <id> <rank>": the receiver gets the sender's account from Steam (GetLobbyChatEntry), checks it
  with PlayerRanks::CanSetRank and applies it; moderators send the session's ranks again 3 s after someone joins.
  Session only. The Creator's account keeps the Creator's rights whatever rank it shows. Not built or tested.
- 2026-11-17: ranks are saved (KeeperForge2\ranks.txt, loaded at start). Each change carries its time; the newest wins
  per player (same second: the higher rank). Changes back to the built-in rank are kept for their time. Moderators
  send their saved ranks 3 s after anyone joins the lobby, this machine included. Message is now "kf2rank 2 <id>
  <rank> <time>"; dated over 5 min ahead is refused. Not built or tested.
- 2026-11-17: saved ranks moved to ranks.dat, sealed with DPAPI (CryptProtectData, current user, extra entropy text), so
  the file cannot be read or edited by hand; one that does not open is ignored with a warning. Real protection stays
  every machine checking each change itself. Not built or tested.
- 2026-11-17: notify <system|alert> <message> (Notices.cpp). Shown once as "SYSTEM: ..." (white) or "ALERT: ..." (red)
  in the lobby chat (slot 0 renamed for the call) or the in-game chat (0x409EC0, sender 0x8181), colored by matching
  shown lines. Reaches every KeeperForge2 player through SteamMultiplayer::PostToEveryone: an invisible lobby naming
  the poster and message ids (no text), searched every 30 s; the text comes from the poster over Steam networking
  channel 1, so the sender is proven. Joining found lobbies was ruled out: searches return public lobbies too, and
  joining a fake regular one could pull a player out of theirs. Not built or tested.
- 2026-11-17: kick <SteamID64> (ModerationCommands.cpp). Rule PlayerRanks::CanKick: Admins kick None, Friends and
  themselves; the Creator also kicks Admins; nobody else kicks the Creator. Lobby message "kf2kick 1 <id>", checked by
  every machine: the target leaves the Steam lobby and makes its own; the others shut it out of their member list
  until Steam reports it gone. SteamMultiplayer::NameOf does not free Steam's text on the Steam thread (mid-callback).
  Not built or tested.
- 2026-11-17: ban, tempban, unban, bans (Bans.cpp). Times on Steam's server clock (GetServerRealTime; nullopt when not
  BLoggedOn): a tempban ends at a moment on it, and counts while it cannot be read. Records "kf2ban 1 <id> <until>
  <time>" (0 for good, -1 lifted), newest wins, saved in bans.dat (DPAPI moved to SealedFile.cpp, same key text for
  ranks.dat), passed on at every join and posted to everyone. Banned players are left out by SteamMultiplayer's
  member filter; their own machine leaves when told. Member-joined handlers became a list. Rank changes and notices
  are now dated by Steam's clock when it can be read. Not built or tested.
- 2026-11-17: a banned account cannot play online at all: Bans' tick (every second, SteamMultiplayer::AddTickHandler)
  sets SteamMultiplayer::SetSelfBlocked while its own ban lasts. That means no lobby (CreateLobby refuses), joins and
  hosting refused (the game's join or host gets false; a red alert, at most every 10 s), and packets neither sent nor
  taken. Unblocked when the ban ends. Moderators keep a standing ban list on the notice board (SetStandingPost,
  "kf2banlist"), which banned machines read every 30 s, so unbans reach them without a lobby. Not built or tested.
- 2026-11-17: online command (anyone). Each machine with Steam multiplayer (not banned) keeps an invisible presence
  lobby ("kf2online", "kf2pfrom", "kf2pstate" = game/lobby/menu, from OnGameTurn and the lobby screen's draw).
  RequestOnlineCount runs one worldwide search (result count filter 500) and counts accounts. App 480's player count
  (GetNumberOfCurrentPlayers) was ruled out: it counts every game using the test app. [unverified] The largest result
  count Steam allows. Not built or tested.
- 2026-11-17: steamid64 <userid> (moderators). Custom URL names are unique on Steam; resolved from the public profile
  XML (steamcommunity.com/id/<name>/?xml=1, steamID64 element) with WinHTTP, no Web API key (a key could not be
  shipped). Also lists lobby members whose Steam name matches (names are not unique). [unverified] That the XML
  profile page still answers this way. Not built or tested.
- 2026-11-17: ping command (anyone). Uses Steam's relay network: CheckPingDataUpToDate(0) measures again, the Steam
  thread waits until GetRelayNetworkStatus says the measurement is done (at most 15 s), then GetPingToDataCenter for
  each data center (nearest 5), plus GetSessionConnectionInfo's ping for each lobby member. Not built or tested.
- 2026-11-17: middle click camera reset. Camera fields found in our export: + 0xD80 zoom, + 0xD84/0xD86/0xD88 turn,
  tilt, lean (0x800 a turn, matrix built by 0x44D6D0), the smooth view move's targets at + 0xDB6..0xDD4 and its
  start/finish at 0x44BD70 (camera, ms). The reset sets the move's targets (same point, starting zoom and angles) and
  starts a 500 ms move: no game action, this machine's camera only. Dead end: the camera's zoom and turn functions at
  0x449BD0..0x44A0D0 are not disassembled in our export, so where the zoom (type 7) and turn (type 8) actions are
  applied was not found. Not built or tested.
- 2026-11-18: middle click camera reset built and works in-game. Still open: whether loading a saved game counts as a
  level start for it.
