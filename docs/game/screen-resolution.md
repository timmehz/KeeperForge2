# Screen resolution and what breaks above 1024x768

Target: DKII-DX.exe v1.70. Addresses are virtual addresses at image base 0x400000.

## How the game picks a resolution
- [verified] The video settings live inside the global resources object.
  Display width is at 0x759A7C, height at 0x759A80, bit depth at 0x759A84,
  windowed flag at 0x759A90. They are loaded from the registry
  ("Screen Width" / "Screen Height").
- [verified] The function at 0x525D30 sets the in-game screen mode. It reads
  width/height from the settings above and calls the screen setup function
  0x5581B0 (thiscall on the game object at 0x756EE8, six stack args: width,
  height, bit depth, windowed, two flags). If that fails it steps down through
  its own list (1600->1280->1024->800->640->512->400). Any width not in that
  list goes straight to 640x480x16.
- [verified] 0x525D30 is thiscall (callers set ECX) with one stack arg. It
  also runs at startup (called from 0x5253EA, before the frontend at
  0x52F140), not only when a level starts. If it returns 0 the caller
  jumps to its quit path.
- [verified] The screen setup 0x5581B0 only accepts a mode that is in the
  selected device's mode list; otherwise it reports "Screen Mode %d*%d (%d bpp)
  is not available" and returns 0. Selected device index: game object
  0x756EE8 + 0xE69. Device table pointer at 0x758050 (count at 0x758048),
  0x21A bytes per device; mode count at +0x1FE, mode array pointer at +0x206,
  0x7C bytes per mode starting with width, height, bit depth. The list is
  filled by a DirectDraw EnumDisplayModes callback (0x557A10) with no
  filtering of its own.
- [verified] In-game test: with resolution auto (3840x2160 desktop) the game
  quit with exit code 0 about 3 s after start. DDrawCompat's log shows its
  default SupportedResolutions = "native, 640x480, 800x600, 1024x768", so the
  driver-side list is what decides which sizes are possible. The runtime now
  picks the closest mode from that list and logs the list.
- [verified] The game creates DirectDraw through patch.dll's DirectDrawCreate
  (a "FakeDirectDraw" wrapper in front of the real ddraw/DDrawCompat). Its
  EnumDisplayModes ignores the real display and reports a fixed 32 bpp list:
  640x480, 800x600, 1024x768, plus 1280x1024 and 1600x1200 behind a flag,
  plus the registry "Screen Width"/"Screen Height" pair
  (HKCU\Software\Bullfrog Productions Ltd\...) if it is not one of those.
  Its SetDisplayMode stores any width/height, only asserts 32 bpp, and
  returns DD_OK. So DDrawCompat's SupportedResolutions has no effect on the
  game's list. In-game log confirmed: list was 640/800/1024 x32 only.
- [verified] The mode-list callback 0x557A10 is stdcall (DDSURFACEDESC*,
  device index), ret 8. It appends {dwWidth (+0xC), dwHeight (+0x8),
  bit count (+0x54), palettized flag (bit 5 of +0x4C)} to that device's list
  using the game's own realloc. The runtime calls it to add the wanted size
  when patch.dll is loaded.
- [verified] The main menu calls the same setup with a hardcoded 640x480
  (push 0x1E0 / push 0x280 at 0x52F21B / 0x52F220).
- [verified] The settings are only written back to the registry by 0x565FD0,
  which takes new values as arguments (the options menu). Changing the values
  in memory does not by itself save them.
- [reference] The menu may only render correctly at 640x480.

## What overflows at high resolutions (all fixed by Widescreen.cpp)
- [verified] Engine heap (alloc 0x57CEB0, free 0x57D0B0, both cdecl). It
  starts with one chunk of a startup size and grows by fixed 2 MB chunks.
  Hitting 32 chunks is fatal, and a single request larger than one chunk can
  never be served. Chunk count is at 0x765B08, total size at 0x765B14, raw
  chunk buffers (each +0x20 bytes for alignment) at 0x765DA0[32], aligned
  chunk headers at 0x765B18[32]. Only alloc/free use the internal allocator
  0x57CD30.
- [verified] GUI views split the 2D overlay into 128x32 tiles and keep one
  surface id per tile in a fixed table at view+0x4. Tiles wide is at
  view+0x27F, tiles high at view+0x283 (both set before any tile is
  created). Exactly three instructions touch the table:
  - 0x42C8A2 `lea ecx,[ebx+eax*4+4]`: write slot, view in EBX, index in EAX
    (in the view build function 0x42C7D0; tiles are built from index 0 up)
  - 0x42CE95 `mov eax,[esi+eax*4+4]`: read, view in ESI, index in EAX
  - 0x42CF82 `mov eax,[edi+edx*4+4]`: read, view in EDI, index in EDX
- [verified] The GOG patch.dll (loaded by the game at startup) overwrites the
  first 5 bytes at 0x42CE95 with a jmp to its own code. That code clamps the
  index to 0 when it is 0x93 or more and the view is the one at 0x6D0394,
  then runs the same two original instructions and jumps back to 0x42CE9B.
  The 6th byte (0x11) is left in place. In-game check of our runtime saw
  these bytes, not the original ones. patch.dll also patches 0x5626C0,
  0x5A5FAD, 0x588E3A, 0x58A1D0 and the pointers at 0x5B4FF4, 0x66C10C,
  0x66C090; none of those are ours.
- [reference] The table holds 140 entries (the field after it starts at
  +0x234). 1366x768 still fits, 1920x1080 does not.
- [verified] The surface cache preallocates N texture pages of 256x256 in
  0x592DB0. N comes from `push 0x18` at 0x592FAC, in 0x592EA0 (called from
  the engine setup around 0x57409F).
- [reference] 0x68 pages is known to work at 1080p and above.
- [unverified] Each page holds 16 GUI tiles, so pages needed scale with
  screen area. Widescreen.cpp used max(24 + tiles/16, 104), capped at 127
  (the push operand is a signed byte).
- [verified] screenshots at 3840x2160: Review Objective showed only its two
  headings on black, with any gpu_blits setting. Its parchment and text are a
  full-screen view (30x68 = 2040 tiles, 128 pages) that did not fit in the
  capped 127 pages (24 of them the game's own). [verified in the export]
  0x592DB0 (thiscall (cache, 0x792D98, count) ret 8, only called from
  0x592FB9) makes the pages one by one into a linked list, with nothing sized
  by the count. Widescreen now hooks it to pass 24 + tiles/16 + 1/8 more
  (168 at 4K); the push keeps min(count, 127). Tested: 168 pages in the log,
  Review Objective still black, so this was not the (whole) cause [verified].
- [verified, renderer trace] With the objective open at 4K, a frame had 2547
  texture locks and copies (206 normally), and the world's draws used 128x128
  textures that were all black with alpha 128 (the view's gradient) instead of
  dungeon textures (brightness 8-117 in an ordinary frame). The cache holds the
  world's textures too; the full-screen view pushed them out every frame. The
  view itself was right in memory (a dump of it) and every tile had a surface.
  Now 0x68 (the size known to work for the world at 1080p) + the full-screen
  view's pages + a quarter: 264 at 4K. Tested: no change (still 2547 uploads).
- [verified in the export] The page count is only used by software rendering
  (0x592EA0 calls 0x592DB0 when [0x792D90] & 1). With Direct3D, 0x592EA0 makes
  a pool of 128x128 device textures instead: 0x592BD0 (thiscall (pool, format,
  minimum, count); 0x80 at +0xE0 is the texture size, each made with
  0x591DA0) with "push 0x200" at 0x593247 (a 32-bit operand): 512 textures for
  the world and every GUI view tile. Now 512 + one per tile of a full screen
  (2552 at 4K, 64 KB each). [verified in game, 2026-11-12: screenshot, the
  world shows behind Review Objective, "Exit" reads, 653 MB committed in a
  level] The page count change stays, harmless.

## 2D GUI scaling
- [verified] The GUI manager (screen width at +0x4, height at +0x8, mouse x/y
  at +0xAC/+0xB0) lays out everything in a 2560x1920 space and converts with
  four thiscall functions that return their output pointer:
  - 0x52C630 box {minX,minY,maxX,maxY} layout->screen (x*W/2560, y*H/1920), ret 8
  - 0x52C5A0 point layout->screen, ret 8 (used for absolute positions too,
    e.g. text positions, not only sizes)
  - 0x52C5F0 point screen->layout, ret 8
  - 0x52C560 manager's mouse position -> layout, ret 4
  About 130 call sites use the box version and 110 the point version; the
  two inverse ones have 4 and 8 callers. Since x and y are scaled
  separately, every GUI element is stretched sideways on a wide screen.
- [verified] Some in-game interface code (around 0x404000-0x407800 and
  0x598A00-0x599200) instead scales a 640x480 layout with the interface's own
  width/height (0x6CE590 / 0x6CD190), e.g. `x*W/640`. Not handled yet.
- WidescreenGui.cpp replaces all four with one continuous piecewise-linear
  x mapping (same for boxes, points and the mouse, so hit-testing stays
  consistent): outer 30% each side keeps 4:3 size and is pinned to its edge,
  middle 25% keeps size and stays centered, two 7.5% bands absorb the extra
  width. [verified] in-game screenshot: panels sit at the edges and bars
  span the width correctly.
- [verified] Two GUI hit tests skip the conversion functions and do the math
  inline with the game object's size (0x756EEC/0x756EF0): 0x52BFC0 (hover,
  thiscall, uses the manager's mouse at +0xAC/+0xB0; called from 0x52BC50)
  and 0x52C0A0 (click, thiscall (x, y, a, b), ret 0x10). Both must be fed a
  pre-converted mouse position when the mapping is replaced, or buttons stop
  responding.
- [verified] GUI windows: the config table entries are 0x48 bytes
  (id dword at +0, x/y/w/h shorts at +0xA/+0xC/+0xE/+0x10, a second rect at
  +0x12..+0x18). The in-game table is the pointer list at 0x693EF8 (first
  entry 0x693400: id 1, 580,1488, 732x468), passed to the window builder
  0x52B8F0 (thiscall (table, owner), ret 8) from 0x402DBD and 0x4098EC; the
  main menu's table is 0x6AE4E0 (from 0x534C38). Window object: id at
  +0x44, manager at +0x4E, parent at +0x56 (0 = top-level), position
  +0x14/+0x18, size +0x1C/+0x20, extra offset +0x4/+0x8 and second size
  +0xC/+0x10. 0x52CDC0 returns the window's layout box (parent origin + pos
  + offset, second size) and 0x52CE50 its origin box for children; both
  thiscall (int* box) returning box, ret 4, and 0x52CE50 recurses for
  parents.
- [reference] Window ids and their positions (Flame's tables match our
  exe's): 1 Rooms, 2 KeeperSpells, 3 WorkshopItems, 5-17 creature panels,
  18 PanelTabs (472,1360), 20 MiniMap, 21 top-left info box, 39 objectives,
  41 summon horny / keeper icon (2272,1600), 43 alarms, 44 hero portal,
  45-50 counters, 53 top info bar (1020..2560), 22-38/40/42/51/52 dialogs.
- [verified] in-game screenshot: the point conversion 0x52C5A0 is also used
  for sizes, so the in-game x mapping must be a plain `x * scale` with no
  offset. A centered mapping (offset W/2 - 1280*scale) added ~480 px at 4K to
  every converted size and made icons, digits and the minimap huge.
- In-game mapping (WidescreenGui.cpp): x * (height/1920), so the 4:3 layout
  starts at the left edge and the spare width is on the right. Top-level
  windows of the in-game manager are moved by id inside 0x52CDC0 and
  0x52CE50 (left: stay, right: + spare, top info bar: widened by spare,
  others/dialogs: + spare/2), so their buttons and hit tests follow; boxes
  ending exactly at 2560 (background stone, rods, end caps) are moved or
  widened by the spare width. The menu keeps the zone mapping.
- [verified] Button rectangles 0x528570 = window origin (0x52CE50) + the
  button's +0x14..+0x20 (with right/bottom alignment flags at +0x65), but the
  text-button renderer 0x428A30 converts the button's own +0x14..+0x20 with
  0x52C630 and never adds the window origin. Moving a dialog window therefore
  moved its icon buttons away from their labels and backing (in-game
  screenshot of the Esc options menu). Dialogs (all the full-layout x=0
  windows) are no longer moved.
- [verified] screenshot at 3840x2160: in possession the bottom rod ended at
  2880 (the 4:3 width). The possession HUD 0x420D10 [verified in the export]
  draws it as one picture at layout (0, 1792) whose size (2560, 88) comes from
  the point conversion 0x52C5A0, not the box one; the top bar tiles a 512 wide
  piece across the screen width (+0x10CA), so it reached the edge. A point at
  exactly layout x 2560 on the in-game manager now maps to the screen's right
  edge, as boxes ending there do [unverified in game]. The rod's end piece is
  then stretched by the same factor.
- Dead end: zone mapping for the in-game HUD. The bottom panel runs to about
  layout x 1200, so the zone boundary cut through the spells/workshop tabs and
  stretched them; enlarging (gui_scale) made the left zone overlap the rest.
  gui_scale was removed.
- (Removed) gui_scale enlarged the in-game GUI (screens taller than 768 lines): the
  x zones use the larger scale, and y gets its own pinned mapping (top 15%
  to the top, bottom 30% to the bottom, the middle band shrinks). Capped so
  the pinned zones fit. In-game font sizes are multiplied by the same factor.

## 3D camera field of view
- [verified] The player camera's focal length is `lens * 320` pixels: at
  0x598D26 `mov [esp+0x10], 0x140000` (320.0 in 20.12 fixed point) is
  multiplied with the lens at camera+0xD8E by the fixed-point multiply
  0x4363F0, and the result (/4096) becomes the camera state's focal length.
  The frustum scales are focal/halfWidth and focal/halfHeight (0x58A7F1..),
  so pixels stay square; but since the focal length does not grow with the
  screen, a tall screen shows a much wider angle. In-game screenshot at
  3840x2160 confirmed a strong fish-eye look.
- [verified] Camera paths (0x44DA05) set the lens to
  (screenWidth / (2*tan(fov/2))) / 320 using the same 0x140000 constant, so
  their focal length comes out as a fixed horizontal angle. Patching both
  immediates to the same value keeps paths unchanged.
- [verified] The player camera's distance is [cam+0xD80] * constants /
  screenWidth (width loaded at 0x598CDD `mov eax,[0x756EEC]`, stored to
  [esp+0x10], divided at 0x598D22). So a wider screen moves the camera closer
  while the fixed focal length widens the angle: same horizontal coverage,
  much stronger perspective. Correction to the note above: the first
  screenshot was this, not a too-wide view on its own; scaling only the
  focal length zoomed in about 4.5x.
- WidescreenCamera.cpp replaces that load with `mov eax,640` and, from a hook
  on screen setup (0x5581B0), sets both focal constants to
  320 * height/480 * 100/field_of_view for each mode. Result should be the
  original 640x480 framing with extra width.

## Fonts, loading screen, videos
- [verified] In-game fonts are set up by 0x5534D0 (cdecl, arg = GUI
  manager screen width, return ignored by its caller at 0x402DD4). It asks
  0x553690 (cdecl size, screenWidth -> font resource id) for base sizes 10,
  14 and 18. 0x553690 only adjusts for widths 400/512 (-2/-4), 800 (+2) and
  1024 (+4); every other width gets the 640x480 size. Its table: 6->0x444,
  8->0x445, 10->0x447, 12 (default)->0x448, 14->0x449, 16->0x44A, 18->0x44B,
  20->0x44C, 22->0x44D, 24->0x44E. A failed font id makes setup return 0.
- [verified] At startup the language setup fills global font slots by fixed
  id and never revisits them: 0x74156C/0x741570/0x741574/0x741580/0x741588/
  0x74158C get the menu fonts 0x454/0x455/0x456/0x457/0x458/0x459, and
  0x741578/0x74157C/0x741584 get the in-game 0x44A (16)/0x44B (18)/0x44E (24).
  The font lookup 0x62DFF0 is thiscall on the language object 0x7410C0 with
  (int* status, font** slot, int id), ret 0xC; on failure status -1 and slot
  0. The setFont/mask calls after it only configure a temporary font object.
- WidescreenFonts swaps larger fonts into the three fixed in-game slots on
  in-game screens (sized as the original at 1024x768) and the startup ones
  back for the menu. Dead end: this did not fix the minimap "N" (it is a
  sprite, below).
- [verified] The in-game Define Keys list (window 30, row drawing at 0x42A9F0)
  places its columns at fixed pixels from the row's left edge: action 0, modifier
  200 (0x42ACDD), key 300 (0x42AD40). On a 4K screen the names ran into the keys.
  WidescreenGui::OnScreenMode scales both by height / 480. The front end's own key
  list (0x54C172 area) runs at menu size and is left alone. Not tested yet.
- [verified] GUI sprite names are a table of 0x45-byte entries starting at
  0x6A7DF4 ("gui\scroll bars\v-up_arrow" = 0); the loaded sprites are at
  interface+0x12B6 in 0x1B-byte steps. "gui\map\north" (0x6A87A8) is index
  36 = offset 0x3CC, used once, in the minimap's north marker draw (0x41EDE0
  area): size from `mov eax, 8` at 0x41EE7D (width and height, fixed
  pixels), nudges `sub ecx, 6` at 0x41EF2C and `sub eax, 3` at 0x41EF39;
  only its position goes through the GUI point conversion. WidescreenGui
  scales those three by screen height / 480 on each mode change.
- [unverified] Ids 0x44F-0x453 are FONT_INGAME26/28/36/48/72 and
  0x454-0x459 are FONT_FRONTEND10/12/14/20/28/36 (inferred from file order;
  the six 0x454-0x459 ids are loaded by fixed id elsewhere). Menu fonts are
  therefore fixed in size, made for 480 lines.
- [verified] Loading screen 0x511020 (thiscall on the world, no args) builds
  "LoadingScreen\LoadingScreen%dx%d" (Japanese and "M-" variants too) from
  the game object's screen size (0x756EEC / 0x756EF0), loads it into
  world+0x1634 and copies it with 0x5B57F0 at 0,0 to the front and back
  buffers (calls at 0x511118 and 0x511139). world+0xA3BF is set to 1 once
  shown. 0x5B57F0 ends in IDirectDrawSurface::BltFast (vtable 0x1C) on the
  surface wrapper's +0x24 DirectDraw surface, so it cannot scale.
- [verified] The startup title screen 0x5340F0 (called from the menu entry at
  0x52F2C3) loads "TitleScreen\TitleScreen" (or "-Japanese", "-<language>")
  into menu+0x6711, copies it with 0x5B57F0 at 0,0 onto menu+0x30340 (call
  at 0x5341A9), presents, then waits 10 s. Same BltFast problem as the
  loading screen: a 4:3 image at the left of the 854x480 menu mode. The
  runtime redirects that call to a fit (full height, centered) draw, because
  the image has copyright lines at its bottom edge.
- [verified] Videos are .tgq files played by an embedded EA player
  (0x53BCD0 sets up a config block at 0x73EDC8). It draws into the screen
  surfaces itself; the game's callbacks (0x53C2C0, 0x53C2E0) don't draw.
- [verified] The video player 0x53BCD0 is thiscall (const char* path), ret
  4. (Ghidra shows an "unaff_ESI" input, but ESI is loaded from the stack
  inside.) The intro is played from the menu entry (0x52F336) right after
  the menu's screen mode is set (0x52F22A).
- [verified] (crash report) The video decoder runs on its own thread and
  writes 640x480 frames straight into the locked screen surface; its
  color-conversion loop is around 0x5AF310 (tables in the uva_data
  section). A screen mode shorter than 480 lines makes it write past the end
  of the surface: access violation at 0x5AF379. Dead end: cropping videos
  with a 640x360 mode.
- With one of our renderers (see renderer.md) the mode is only switched when
  it is smaller than 640x480; the renderer finds the film in the screen and
  scales it. Switching in the menu took the mouse pointer's image away
  (tested). With patch.dll, as follows:
- fullscreen_videos now switches to exactly 640x480 around each video (the
  film fills it) and, with "stretch", sets patch.dll's ScaleMode
  (patch.dll+0x24DA8, read at +0x1826; 0 = stretch to the window) for the
  duration. The saved mode (game object +4..+0x1C, as 0x5587F0 reuses them)
  and ScaleMode are restored afterwards. [unverified] that the menu
  survives the extra mode changes.
- [reference] Main menu buttons (Flame's table): the title is a box at
  628..1932 x 0..472, drawn from the box's left edge at the image's own
  size. The menu items use the same x span with centered text.
- [verified] The front end never calls the point conversion (0x52C5A0). All
  of its callers are in-game code below 0x430000. It uses the box conversion
  (0x52C630) and the sprite blit 0x52CF90, which takes screen pixels.
- [verified] Much of the menu is blitted at fixed pixel positions from the
  640x480 design, which skip every conversion. Examples: the list renderer
  0x545C80 draws at (0x10,0x4B) and (0x145,0x77), the player list at x 0x145,
  and the map list at x 0x10/0xA0. So a shift in the conversions moved the
  text but not the rows, bars, map preview or scroll bar (dead end, tested).
- [verified] The front end draws everything into a frame description at
  front end +0x2DD65. 0x5334C0 copies it from the back buffer every frame,
  just before it calls the manager's window renderer 0x52BBD0 (thiscall
  (owner), ret 4). Its fields: +0x10 bits per pixel, +0x18 pixels, +0x1C
  width, +0x20 height, +0x24 pitch. The sprite blit clips to +0x1C/+0x20.
- Menu now: conversions are a plain scale, as if the screen were 4:3. A hook
  on 0x52BBD0, for the front end only, moves the description's pixel
  pointer to the centered 4:3 area and narrows its width to 4:3. Everything
  drawn moves together. While the menu draws, the stored mouse (+0xAC) is
  moved by the same margin, because renderers compare it with what they
  drew. Outside drawing, the mouse-to-layout conversions subtract the
  margin.
- Dead end: the zone mapping in menus. A list and its pieces (the scroll
  bar, column headers, and the "Size" column on Map Select) cross the zone
  lines. Each piece was mapped on its own, so they drifted apart.
- [verified] Text box bar: 0x411B30 (save name, 0x428CD0 through 0x411A80, and others) draws three pieces in fixed
  pixels, ends 16 wide and all 32 high, whatever the screen. Reported from a 4K screenshot: the save name's bar looked
  far too thin. The numbers are immediates at 0x411BD2 and on, now scaled with the screen height like the other fixed
  sizes. The ends stop at 63 pixels, as twice that has to fit a signed byte (0x411C2E). Not tested yet.
- [verified] Main menu version text: 0x537E10, cdecl (button, menu). It takes
  the button box (0x528570), converts it with the menu's GUI manager
  (menu+0x30F44) through 0x52C630, makes a text drawer (constructor
  0x62C8F0, destructor 0x62C9B0), sets left (0x62CA70, 0) and bottom
  (0x62CA00, 2) alignment, formats L"V%lu.%lu" from 0x6BC430/0x6BC434,
  converts with 0x553B20 (cdecl (wide text, out, 0x40)) and draws with
  0x62CAE0 (thiscall (status, box, text, font, 0), ret 0x14) using the font
  object 0x7413C0. [reference] The box is 0..2560 x 1740..1900.
  MenuVersionText.cpp replaces it to add a left margin and a KeeperForge2
  line underneath.
- Fix: in-game fonts are picked by height. The loading screen loads the
  largest original size and is Blt-stretched to fit.
- The menu runs at a 480-tall widescreen size (menu_height), for example
  854x480, and patch.dll scales it up.
- [verified] 2D drawing and text go to the bound draw surface. 0x5B3710
  (cdecl (description*)) stores it at 0x79D060, caches its pixel pointer at
  0x79D07C, and resets the clip. Sprite blits (0x52CF90) instead take the
  description as an argument. Shifting only the front end's description
  moved the blits (sliders, list rows, title) but not the text (tested). The
  menu render hook therefore also binds the shifted description while the
  menu draws, then puts the old binding back.

## Not handled yet
- [unverified] Whether the 3D view stretches or widens correctly at 16:9.
  Needs in-game testing.
- [unverified] Whether the in-game HUD lays out correctly at widescreen sizes.
- [reference] Windowed mode at large sizes needs more fixes: the draw buffer
  allocators (0x5A7980, 0x5A79C0) are sized from GetClientRect, and
  0x556077 reads GetClientRect for the surface size.
- The main menu stays 640x480.

## Flying notification icon
- When a notification arrives (for example a fight), an icon flies to the
  notification bar with a sparkle trail before the still icon shows.
  [verified in code; seen in game by the user]
- Each is an object in the player interface (the first at +0x4C6E, up to
  four drawn by 0x42C390). 0x42BC70 and 0x42BE90 set them up:
  - floats at +0, +4, +8: position; +0xC, +0x10, +0x14: target;
  - start and target from the GUI's layout-to-screen conversion 0x52C5A0;
  - +0x18: size; +0x1C: target size; 0.75 (0x3F400000) or 0.
  [verified]
- 0x42F040, thiscall (icon, engine, x, y) ret 0xC, updates it (0x42EE80:
  32 frames moving to the target, size blending, then x0.95) and draws it
  as a 2D animated mesh (0x579A10, size * 0.125 * 4096), adding trail
  particles. [verified]
- The path is in screen pixels, but the size is the same at every
  resolution. WidescreenGui hooks 0x42F040 and multiplies both sizes by
  screen height / 480 for the call, dividing them back after. The base of
  480 is an assumption that matches the other GUI icon fixes. [unverified
  base]
