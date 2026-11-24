# Keeper colors

Each keeper's color can be changed (Runtime/Source/KeeperColors.cpp, ColorPicker.cpp). config.yml keeps them:
`keeper_color_red`, `keeper_color_blue`, `keeper_color_green`, `keeper_color_yellow`, `keeper_color_purple`, each six hex
digits (RRGGBB) or `game` for the game's own. Each is named by the keeper's own color (players 3 to 7).

## The game's player color table [verified in the export]
- At 0x6E5058: three shades a player number (0 to 7), 4 bytes each (red, green, blue, 255), so 12 bytes a player:
  bright, dark, middle. Filled once by the game's start-up (0x4C04C0, entries set through 0x4C06D0).
- The game's own values (bright / dark / middle): player 0 black; 1 (Good) BFD5E0 / 9AABB5 / B0BEC6; 2 (neutral)
  white; 3 red E63232 / 960000 / BE3232; 4 blue 324EE6 / 0019A0 / 3248BE; 5 green 32E633 / 00A001 / 32BE33; 6
  yellow E2E632 / 9DA000 / BBBE32; 7 purple E632D4 / A00090 / BE32B0.
- The dark and middle shades follow the bright one: per channel, dark = (c - 50) * 150 / 180 and middle = 50 + (c -
  50) * 140 / 180 for c above 50 (230 gives 150 and 190); below 50 the dark one is 0. These match every keeper's within
  2. KeeperColors makes them this way from the color chosen.
- Read by: 0x50F830 (thiscall (players; player tag, shade): the entry of the player's number, + 0x10FC of the player,
  or player 1's for a number over 7); the lobby's rows (0x544BF6, slot + 3) and chat; an engine thing's owner tint when
  its type is "player colored" (0x436338: the owner's bright shade, each channel at least 0x30, into the thing's + 0x6E).
- [tested] Claimed floors and reinforced walls do not take their colors from this table: changing it alone left them
  in the game's colors. [unverified] When things already made pick up a change. The minimap has its own colors
  (GUI\map\MapColours.png); see below.

## Claimed floors and wall tops [verified in the export]
- They are meshes of each player's own. At each game's player set-up (0x4C1E20), 0x4C2980 loads "Claimed Floor<d>_<n>"
  and 0x4C2A70 "Claimed Top<d>_<n>" (n 0 to 4) for each player object d (0 to 6). Both write the name into a buffer at
  0x740C20 before each load, and the meshes go into the player's arrays at + 0x10AA and + 0x10BE. Player object d
  is player number d + 1 (the loop before them sets + 0x10FC to d + 1), so red (3) has "Claimed Floor2_n".
- A tile's mesh is picked from its owner's arrays (the owner is the thing whose tag is in the tile's + 8, 0x455990,
  in the tile mesh building at 0x43E860). The + 0x10BE one is used while the terrain's flag 1 is set or the tile is
  being dug. The tile's tint comes from the terrain record, not from the owner. So their colors are in the meshes'
  textures.
- Meshes get their materials through the mesh loader's material callback (0x59A650), which calls 0x57C850, cdecl
  (description, add now). That function finds or makes a material by name and returns its number in the material
  list (pointer at 0x765B00, count at 0x765B04).
  - Each material holds texture handles at + 4, 0x10 bytes each.
  - With flag 0x4000 (at + 0x10), all + 0x15 textures × + 0x19 frames are filled; otherwise only the first.
  - A handle's number (+ 0x24) indexes the texture names (pointer at 0x792D44, 0xC bytes each: name, handle).
  - A handle's + 0 is its surface (null until loaded) and + 0x18 its next smaller version.
- Textures reach the game's texture cache pages through 0x590450, thiscall (destination; source, x, y) ret 0xC, which
  copies the source surface's pixels to x, y. It is also used for the scaler's work surfaces (pointers at 0x792D49 and
  0x792D4D) while a texture is resized.
  - The cached copies are usually compressed surfaces (0x591070). They are decoded only by this copy.
  - Surface layout: + 0x18 / + 0x1C in the vtable are lock (returns the pixels and sets the pitch at + 0xC) and unlock
    (takes the pixels).
  - Size at + 4 / + 8, the pixel format at + 0x10 (bytes a pixel at + 8, masks at + 0x1C to + 0x28).

## How KeeperColors recolors them
- While the two mesh loads run, every material 0x57C850 returns is noted under the loading player (from the name in
  0x740C20). So are its textures. After the wall tops have loaded, a texture that only one keeper's meshes use counts as
  that keeper's. The log names them at log_level debug.
- [tested] The textures found are named T_Claimed_Path<k> and T_Claimed_Top<k> (some with a _64 version), one set a
  keeper. k is not always the same for a keeper's floor and top (green's were Path3 and Top5).
- [tested] Texture numbers change from one game to the next. Notes kept from the last game pointed at other textures,
  so the second game found no keeper textures at all. The notes are now cleared when the floors start loading.
- The first 8 recolors of each game are logged at log_level debug.
- After each 0x590450 copy whose source is one of those textures (any size), the copied area of the destination is
  recolored. This is skipped for the scaler's work surfaces, which are copied again later, so each copy is changed
  once.
- [tested] The textures only the Good player's meshes use are T_Claimed_Good (floor) and T_Claimed_Top0 (wall tops).
  Using them as the base (white, tinted) was tried:
  - The floor never paired, because its name differs from T_Claimed_Path<k>.
  - The wall tops' white is flat and much brighter than the keepers' trim. The tinted trim came out as flat, bright
    bands, unlike the game's own shaded trim.
  - So they are only listed in the log now.
- Which pixels change is found by comparing the other keepers' versions of the same texture.
  - Versions match by name with the digits taken out, so T_Claimed_Path1 and T_Claimed_Path2 match.
  - The first time a keeper texture is copied at a given size in a game, each other keeper's version of that size is
    loaded if needed (0x590EC0) and copied to the same place in turn. Each is compared with the keeper's pixel by
    pixel, and then the keeper's own pixels are put back.
  - A pixel counts by its smallest difference from all the other versions. A colored part differs from every other
    keeper's color. A spot where the stone differs a little in one version (compression) rarely differs in them all.
  - A difference below 0.10 (largest channel, 0 to 1) is left alone. One of 0.25 or more changes fully, with a ramp
    in between.
  - [tested] Comparing with one other keeper only, at 0.06 / 0.20, left specks of the chosen color all over one
    keeper's wall tops (purple, the third keeper).
  - [tested] The colored part is about 142 of 16384 pixels of a claimed path (its gems) and about 3520 of a wall top
    (its trim).
  - The result is kept for the rest of the game and logged at debug.
- Each changed pixel becomes the chosen color, shaded as the keeper's own color is shaded there. Its brightness is
  divided by the brightness of the keeper's game color:
  - at 1 or below, the chosen color is multiplied by that ratio;
  - above 1, the chosen color is blended toward white, reaching full white at a white pixel.
- Without another version, the hue is moved instead: by the difference between the keeper's game color and the
  chosen color, with saturation and brightness scaled by theirs.
- [tested] The first version used hue alone: within 45° of the keeper's color and saturation 0.18 or more. Red's
  claimed floor stone is reddish-brown, so the whole tile changed. That is now only a fallback when no other
  version is found, and it is narrower: within 15° (fading to 30°), saturation 0.45 (full at 0.65).
- [unverified] Whether each player really has textures of its own, whether these meshes are all of the colored parts,
  and whether the texture cache is emptied between games. If it is not, a color changed after a game may only show
  on textures the cache loads again.

## The minimap [verified in the export]
- The minimap's colors come from GUI\map\MapColours.png. It is read when a level's interface is set up, by 0x41E810
  (fastcall, the minimap; called from 0x402F46).
- The picture's pixels go into the minimap's table: a pointer at minimap + 0x138A. Each entry is 0xFFBBGGRR, with
  red in the low byte (0x41ECE0 turns them round to 0xRRGGBB). [tested] Writing them as 0xFFRRGGBB swapped red and
  blue on the minimap: yellow showed as cyan, orange as blue.
  - Entries from + 0 are the minimap's other colors.
  - Then come three entries a player for players 1 to 7, at + 0x48, + 0x6C and + 0x90 (4 bytes a player).
- Each player entry is also handed to the minimap's drawer (minimap + 0x53) by its vtable + 0x294, thiscall (player,
  kind, color pointer) ret 0xC. The kinds are 1, 0 and 2 for the three tables.
- The draw's color lookup (0x41ECE0) reads the table by index. For the local player's own three, it flashes through all
  the players' entries.
- KeeperColors hooks 0x41E810. After it runs, each keeper with a color of its own gets the chosen color in its three
  entries, shaded as the game's own entry is (the same shading as the claimed terrain, by brightness against the
  keeper's game color). Each is handed to the drawer again.
- [unverified] Which minimap parts each of the three kinds colors (claimed land, rooms, walls or the like).

## The color boxes
- The skirmish screen's player list is widget 86 (kind 10): layout box x 64, y 536, w 1328, h 300. Its draw is
  0x5445F0, its row count 0x5445C0 and its row text 0x5445D0.
  - The draw starts the rows at the top of its frame box and steps by the widget's + 0x8C (row height, frame
    pixels) for each slot in use, up to the map's slot count (front end + 0x5AAC).
  - The ready tick is drawn at frame x 0x145, 3 pixels into the row (picture at front end + 0x3FD4). That is near
    the list's right edge.
- Free space right of the list: layout x 1392 to 1480 (the buttons on the right start at 1480).
- KeeperColors adds widget 9002 there: a copy of Add Computer Player at x 1394, y 536, w 84, h 300.
  - Its draw finds the list widget and works out the rows the same way. In each keeper's row it draws a box of
    that keeper's color. The box fills the row's height less a 1-pixel gap (at least 8 pixels) and the widget's
    whole width. It has a dark border, white under the mouse.
  - Its click opens the color picker for the row under the mouse.
  - The boxes are pictures in the game's pixel format, drawn with 0x52CF90, as the custom campaign briefing picture
    is drawn.

## When colors are set
- The table is filled by the game's start-up, which may run after KeeperForge2's install, so the game's own colors are
  kept, and ours written, on the first frame shown (FrameRate's after-present callback), and again after each change.
  Things set up in their owner's color take the table as it is then, so colors are chosen before a game starts.
- Only the local machine's table changes: in a multiplayer game each player sees their own choice.

## The color picker
- The picker (ColorPicker) is a Windows window on the game's thread, as EditorDialogs' windows are. It is dark
  when editor_dark_windows is on.
- It has a wheel (hue around it, saturation out from the middle) with a brightness bar beside it, the color before
  and now, red, green and blue as numbers (0-255), and the hex code (#RRGGBB). Changing any of them changes the others.
- The color is applied while it changes, so the lobby's rows and the boxes show it at once.
- OK keeps and saves it. Cancel, Esc or the close box puts back the color from before. "Game's color" goes back to
  the game's own.
- It replaced a "Keeper Color" text button (id 9002 at layout 64, 1720) that picked the keeper from the selected row.

