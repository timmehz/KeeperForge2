# In-game level editor (LevelEditor.cpp)

The editor opened from the Single Player menu. Plan: run the game's real world and
3D view with the simulation held, change tiles through the game's own code, and save
the map to the game's level files.

Stages: 1 open a map (first test done, fixes not tested), 2 read and save level files,
3 terrain, 4 things, 5 level rules and triggers, 6 test play. Stage 2 is written, not tested:
see level-files.md. A map with a copy in `KeeperForge2\maps` opens from there.

## Stage 1: what it does
- Editor in the Single Player menu opens the skirmish screen, where the map is chosen
  and started as usual. The next skirmish started is then a level to edit. Pressing
  the Skirmish button clears this, and so does starting any other kind of game.
- Leaving a level returns to the skirmish screen without passing through the Editor button [log: the lobby's session options
  line comes right after the level ends], so leaving a session sets this again. A saved level is otherwise only opened by
  backing out and pressing Editor. A normal Skirmish always plays the stock map, not the saved copy.
- It always starts as player 3, ignoring the lobby's Swap Keeper and Good player
  choices and the co-op rows.
- Level setup's six play mode reads get campaign for that level, so it keeps what its
  file says (all keepers, no lobby lists, fog, walls or AI). See coop-campaign.md for
  those reads. The fog is lifted once, in the first tick, by 0x451EF0.
- Only the actions listed under "Holding the simulation" are carried out. Each other
  kind is logged once as "Editor: ignoring game action N".
- Tested once: the button and opening a level worked. Things bobbed and vanished, and
  Quit did nothing (both explained below; the fixes are not tested).

## Stage 3: painting terrain
- Tested once (Bottleneck, 2026-11-03), from the log: many tiles painted over about 3 minutes, rooms included, drags of
  up to 6 tiles, then undone all the way back, with no crash. Survived minimize and restore. What it looked like is not
  in the log. Saving was not tried, so a saved map opening again is still untested.
- Left click or drag paints the chosen terrain and owner, right click picks them from a tile, `[` `]` change
  terrain, `,` `.` change owner, Ctrl+Z undoes a stroke, F5 saves.

### The side panel (EditorPanel.cpp, written, not tested)
- Tested, second run (Spiral): the sheets loaded and the heart stepped the owner in the right order both ways (log). Then a
  crash at 0x452D8C, reading terrain data for id 0, called from 0x4500C8 in the tile change 0x44FC40. Registers gave tile
  x 24, y -1. [verified] Its first neighbor loop (0x4500AA, for a new terrain without flag bit 1) has no bounds check, so
  painting the map's outer ring reads a tile off the map. The ring is now refused. Not tested yet.
- Sheets, 64x64 cells, read by eye from the user's PNG exports [unverified against the editor's own numbers, which sit
  behind virtual +0x24 of its item objects, not found in the raw records]. Tables in LevelEditor.cpp.
  - RoomIcons (9x2): cells 0 to 16 are Rooms.kwd in order, Treasury to Stone Bridge. 17 is "?", for all the others.
  - TerrainIcons (5x5): row 0 water, lava, rock, impenetrable rock (the last two are a guess at which is which), gold. Row 1
    gems, dirt path, then an unowned wall, something dark and black, all unused. Row 2 vault stars: white, red, purple,
    green, yellow. Row 3 reinforced wall, row 4 claimed path: red, purple, green, yellow, white. Those colors are taken to
    be keepers 1 to 4, then anyone else, and these tiles' icons follow the chosen owner.
  - TrapIcons (13): gas, boulder, a bell-like one taken as alarm, a skull taken as sentry, fireburst, lightning, freeze,
    fear, trigger, spike, flag (guard post), jack in the box, "?". Sentry and alarm are the likeliest to be swapped.
  - DoorIcons (7): wooden, steel, braced, magic, secret, barricade, "?". Hero barricade shares barricade.
  - CreatureIcons (31 used of 39): 0 Imp, 1 Horny (devil face), 2 to 24 Creatures.kwd in order, 25 Maiden Of The Nest (spider),
    26 to 28 taken as Stone Knight, Lord Of The Land, Royal Guard (a guess between similar helmets), 29 small crown for the
    Princes, 30 "?" for Jack. The Unique ones share their base creature's.
  - ObjectIcons (4 by 4, not used yet): crate, gold bag, gold pile, spell book, bed, gem, a crate-like box, a table, then two more.
- The user's exports are in `docs/game/tab images`. They are the game's own art, so that folder is kept out of the repository (.gitignore).
- Tested, first run: nothing drew, and the tabs seemed dead. The draw hook was on 0x409F30, which the game only calls in multiplayer
  (0x404012 skips it otherwise) [verified]. So the first palette never showed either. Drawing is now after 0x419DF0, the
  in-game interface's own draw, with one caller (0x404D1E) in the per-frame function. Not tested yet.
- User's design: the four game tabs stay, with the editor's content, using the icons in `Data\editor\Graphics`. Spells
  are not on a map, so the second tab (the game's Keeper Spells button, wand icon still) holds terrain.
- The Creatures tab is in the user's order: the 16 keeper creatures on the first row (Imp to Maiden Of The Nest), the 16
  heroes on the second (Dwarf to Prince Balder), then anything else (Jack, types added in the editor) from the third. An item
  can start a new row of the grid (`EditorPanel::Item::startsRow`); even at 4:3 a row holds 17. Written, not tested.
- Rooms and Terrain choose what to paint. Workshop (traps then doors) and Creatures show their icons and can be chosen, but
  nothing is placed yet (stage 4).
- The owner: left click on the top bar's heart steps keeper 1, 2, 3, 4, heroes, neutral, and round; right click goes back.
  `,` and `.` do the same. The click is taken in the mouse handler, ahead of the heart's own right click (0x416680).
- On a screen wider than 4:3 the grid now runs to 2240 plus the layout's spare width (the right-hand windows move by as
  much), so 16:9 gets 27 columns instead of 17 [unverified in game].
- Grid at layout y 1600 to 1900, 17 columns by 3 rows. The tab buttons reach about y 1587 (a screenshot), so the first
  placement at 1520 would have covered them. The name line sits to the right of the tab buttons.
- The tab buttons (0x416C20, also what the panel keys call) only set which list shows, in a session. The game's panels
  (window ids 1 to 3, 5 to 17, [reference for the ids]) are hidden each frame by clearing window +0x48.
- Icon sheets are grids of 64x64 cells. The official editor gets columns and rows from width and height >> 6, and a cell
  from each item's own icon number: column = number % columns, row = number / columns [reference, editor, verified in its
  export]. Where the number is kept in each list was not found, so an item's place in its file is used. Traps and doors
  in particular may show the wrong picture. [unverified, needs looking at in game]
- It is a view of our own (0x42C7D0), as no game view reaches the bottom of the screen: 0x2737 stops at layout y 1460. Its
  textures are released with the game's in 0x419C00, and it is made again when next drawn. Pixels are written directly,
  ARGB 32 bits or 1555 at 16. [unverified that the channels are in this order]
- More than 140 tiles needs Widescreen's larger table: at 4K it is 300. Without it, the panel is left out with a warning.
- Clicks on the palette or panel are taken in the mouse handler 0x406530, ahead of the game's GUI, whose stone background
  could swallow them when no button is hit (0x52C0A0).

### The palette (written, not tested)
- A small box at the left of the screen, layout x 0 to 620, y 190 and down: the owner, Undo and Save, and a status line.
  Save shows a * with unsaved changes.
- It is text in a view of its own, first made in the one the options menu uses (interface +0x2737), which GuiTextCache also followed.
  It is not drawn while that menu or the objective is open, as they use this part of the screen.
- The view is sent to the screen only when a row changes (hover included), as that is slow at 4K (frame-timing doc).
- Text goes through 0x42CAB0, with a font copy made by 0x62C2D0 passed by value, which the function destroys. After the
  text, each still-empty pixel in the column gets the see-through black the options menu uses (0x42C9B0's value). It runs
  after the text so GuiTextCache's held-back clear cannot wipe it.
- The mouse position is 0x5B2BA0, screen pixels [verified by its use in 0x406DF0]. Painting and picking skip the column.
- Left out of the lists: Fog Of War, Edge of Map, Claim Scan, Hero Gate [ FrontEnd ], and everything the file marks
  `<not used>`.
- Tried first: status text through the chat lines (0x409E30). It sat where the palette goes, so the status line replaced
  it. Whether it showed was not reported.
- The tile comes from what the game already works out for the hand: interface +0x1068, tile x and y at +8 and
  +0xC, kept inside the map (filled by 0x5992B0) [verified]. 0x4075E0 and 0x408EE0 are the world's clicks, reached
  only when not over a panel or menu (0x406530) [reference for the caller's logic, the signatures checked: both
  ret 8]. In a session they run none of the game's own handling.
- A tile changes as level scripts do it, case 0x16 of 0x518AD0 [verified]. If the tile is in a room: 0x4F0840, the
  owner's 0x4C5DB0, 0x4F09B0. For a room's terrain (terrain flags bit 0x10): set the tile with the room's own
  terrain, or with water or lava for a bridge (room data flags at +0x43D), with the neutral player's tag, then
  0x4F0390, the player's 0x4C5C30, 0x4F04F0. For any other: 0x44FC40. The script also makes an effect and a sound,
  left out as a held world would never play them out.
- Changes go into the level's own map as well, which is what is saved. Only the Map file changes. A first save makes
  the level's copy in `KeeperForge2\maps`.
- All changes happen after the world tick, the one place the game changes the world.
- 0x451EF0 runs again after each stroke, as a change may lose a tile's "seen" mark. [unverified that it is needed]
- [unverified] Whether a tile's look updates at once in a held world, which may need a world tick, and whether
  dragging works: the hand's tile may only update on some ticks.
- Done since (see "Tools and fixes, batch 1"): things on a painted tile, the Dungeon Heart painted over, the warning on
  quitting, and New map.

### Windows' pointer instead of the hand (written, not tested)
Editor sessions only; a normal game keeps the hand. If any of the four hooks below does not fit, the editor keeps the hand.
- [verified] The hand is the game's software pointer, not a model in the world. 0x40ACD0 picks its picture from the state
  table at 0x68F658 and hands it to 0x5B2BD0, cdecl (result, image, source rect, hot spot). A null image shows no pointer,
  which the game does itself in some states. Hooking 0x5B2BD0 and always passing null removes the hand. 0x40ACD0's other
  work (the tile highlight) still runs.
- [verified] The icons beside the hand are separate: 0x40CAE0, thiscall (interface, x, y) ret 8, skipped in a session.
- [verified] The mouse device is input system (0x79CF90) +0x5C, device part +0x10. Its flags are the usual DirectInput ones:
  1 exclusive, 2 shared, 4 foreground, 8 background. 0x5DCE50 sets them and re-acquires. 0x5BC280 asks for 5 (exclusive) full
  screen and 10 windowed. Exclusive hides Windows' pointer and stops it moving, so a session sets 6, checked every frame as
  the game sets it again on a screen mode change, and puts 5 back at the end.
- [verified] The window class has no pointer of its own, and 0x5BC280 calls ShowCursor(FALSE) on activation. Our hook on the
  class's procedure 0x5B5070 answers WM_SETCURSOR over the client area, raising the show count on that thread. The arrow is
  IDC_ARROW, so it follows the player's Windows pointer scheme.
- [verified] The position object is input system +0x64: floats x and y at +0x20 and +0x24, movement at +0x28 and +0x2C,
  multiplier at +0x30, whole numbers at +0x10 and +0x14 (what 0x5B2BA0 returns), 0x5DD630 clamps and converts. The motion
  handler 0x5DD4A0 runs with the multiplier at 0 in a session, after the floats are set from Windows' position. It still runs
  as it passes the event on, and the wheel goes through the same event.
- Tested: the hand came back on leaving a level but Windows' arrow stayed on top. ShowCursor counts every call, and the first version
  raised the count on every mouse movement. The window gets no mouse messages once the mouse is exclusive, so one request to hide it
  was all it got. It now raises only when hidden, goes back by what was raised, and each mouse movement (motion handler, still called)
  checks GetCursorInfo while full screen and asks the window to hide it. [unverified which of these was the whole cause]
- Windows' position is scaled from the window to the game's screen (a 1280x720 window over a 3840x2160 screen in the logs),
  allowing for bars at the sides when the picture keeps its shape (`Renderer::WindowPointToScreen`).
- Full screen only, the pointer is clipped to the window: a click on a second monitor would take focus and minimize the game.
- [unverified] That everything reads the position rather than DirectInput directly, that clicks land under the arrow in the
  options menu too, and that the hand comes back in the menu after leaving. The front end sets its own image (0x536E20), and
  the session has ended by then.
- Screenshots leave out the pointer in a session, as Windows' arrow is not part of the picture.

## Stage 4: creatures (written, not tested)
- Choosing on the Creatures tab makes a left click place one, with the owner from the heart. Choosing terrain or a room goes back
  to painting. Solid terrain (flag 0x01 of terrain +0x17B) refuses, water and lava do not. Other things in stage 4 are not done.
- Made through level load's own steps for that kind of record: init, read, make (see GameAddresses.h, [verified: the reader
  0x56FA00 and table at 0x51E11C]). So what is placed is what the saved file makes. The make functions end up in 0x4B78D0, then
  0x4B7900, which refuses at 0x1C0 creatures. They also add to a sum at world +0x72E7, which only appears to matter for
  checks between players. [unverified what reads it]
- [unverified] Whether a creature made after the level loaded draws and stands right in a held world, with no tick to run it.
- Traps and doors (written, not tested): choosing on the Workshop tab places one with a left click, the same way as creatures, from
  their own make functions 0x520E50 and 0x520F40 [verified: table at 0x51E150]. They need ground with neither low flag 0x01 nor 0x02
  and one to a tile. A door works out its way round from its neighbors. The editor wants a wall on both sides of a door (left and
  right, or above and below), any terrain with solid flag 0x01, from the level's map as the editor holds it. Barricades, by name, are
  left free, as their data flag 0x40 skips the way-round test in 0x4A5070. Right click finds them
  by tile after any creature. They have no level, so the menu shows only Delete. Removal is vtable slot +0x30, as creatures, which runs
  0x4A5290 for doors (clears the tile's door bit) and 0x4A7B20 for traps. Those may play a sound. The trap list's Magic Door is left off.
- Selling (written, not tested): deleting a door, trap or room tile uses the game's sale, for 0 gold. The Sell tool's actions 0x33, 0x35 and
  0x31 go to player functions (table built at 0x511A80: 0x513240 to 0x4C3670 for doors, 0x5132A0 to 0x4C3A20 for traps, 0x5128F0 to 0x4C70A0
  queuing room tiles for 0x4C5DB0, the same one painting over a room calls) [verified]. Every price is cost * byte 0x6F28EA / 100, so that
  byte is 0 for the call and put back. The money text 0x50FB70 is skipped in a session. A sale of a door or trap only sets health to 0 for the
  thing's next tick (vtable +0x20 is 0x4B2AD0), and a held world has none, so it is removed straight after. Painting over a room is now free too.
- Rooms: a right click on one opens a menu with Delete (one tile) and Paint with this, which is what a right click on it did before. The
  Dungeon Heart and hero gates have no Delete, as they are 0xCC things in the file too. [unverified] Whether removing a tile leaves the
  rest of a room looking right, as nothing ticks.
- Variants: a right click on a Workshop item cycles it (a creature's is chosen in its window since 2026-11-14). The color behind the icon and its name show which. Trap blueprint is 0 at
  record +0xC; door blueprint is bit 1 of +0x10, which goes to door +0x109 (0x4A66C0) [verified]. Unique creatures are off the panel. Choosing
  one's normal kind and cycling gives the Unique's id in the record. Heroes with mercenary textures (the same list as mercenary-portal.md, less those
  without) cycle to mercenary through 0x48E0D0. Nothing in the file says mercenary, so the editor uses bit 1 of the creature's flags at +0xF, which
  no make function reads and no stock level sets [verified by a script over all 67]. It applies it on placing and on opening a level. Playing a
  saved level for real ignores it until stage 6 reads it.
- Keeper 1's heart looked unfinished (user's screenshots): the arches were missing from it alone. Level setup (0x4DBAAF, 0x4DBB29) makes the local
  player's Arches (object 86) and three rings (88) with draw alpha +0x8F at 0 and bit 0 of +0x9F set, and the heart's tick (0x4D9CB8) fades them in,
  ending at alpha 255 with the bit clear [verified]. A held world never ticks, so a session sets that end state on the first tick. Not tested yet.
  The fade's map call 0x4522A0 is left out. [unverified whether it matters]
- The Terrain tab's rock icon did not show. The log gave its place as right, with the panel out and the sheet loaded, so the game's button (0x412260) must
  draw over the view. The hook skips that one button in a session (sprite 6 at live +0x2C, text 175 at +0x30), which loses its hover glow. [verified: the offsets
  at 0x412xxx use and config data; not tested]
- Tested: a saved copy reopens (Olympia, "opening the saved copy", 44 things), so the level name path works. Whether painted tiles were there was not reported.
- Tested: still black with only the turn fix, and filled in before with only the second reveal. Both are now done: the turn each tick,
  and the reveal again 8 ticks after the first. [unverified why the first reveal alone is not enough]
- Tested: the creature menu's level rows and Delete were hidden behind the bottom bar (user's screenshots), not failing to draw. The bar and
  panel draw over any view, as the tab button did. The menu now stays above layout y 1450.
- Tested: mercenaries did not survive a save and reopen. Cause not found. The first tick now logs how many records are marked, and 4 ticks
  later each marked one is checked: made a mercenary again if it is not, and given texture bank 1 if it lost it. The log line says which happened.
- The minimap was black in a run's later sessions. 0x41A9B0 only draws when the session's turn differs from 0x6D394C, and the held turn does not change, so a
  session starting on the turn the last one stopped on never drew [verified in code]. It is now cleared each world tick. Revealing again was the wrong idea and is gone.
- Right click: the game's under-hand struct says kind 2 or 3 and the tag at +0x1C and +0x20 [verified in 0x5992B0: the thing's +0x12
  is 0, its owner compared]. If that gives none, every tag is checked for a creature on the tile (in use at 0x6ED540, vtable 0x66D524).
- Delete sets flag 0x40 of creature +0x3E6, clears 0x20 and 0x80, and calls 0x4B7A20 (ret 4 on world +8), which runs 0x487C70. Every
  death effect in 0x487C70 (sound, effect, body, counts, score) checks flag 0x40. [verified: the checks. Nothing in the export sets
  the bit by that name, so it is used for what the checks show] The game's own removal without a death is 0x4DFD10.
- Tested: placing and deleting worked, then a crash at 0x48836D reading null + 0x110D, in 0x487C70, called through 0x4B7A3B. It read
  creature +0x2A, the player, and that was 0. Cause: the tile scan found a creature already removed. Removed ones go to a pool, keeping
  their tag, vtable, in-use mark and position, so "Deleted Horny" in the log, after all Hornies had gone, was already a second removal
  of the same one, and the third crashed. Their list at +0x12 is 9 once removed (0x4B4AB0) and 0 for a live creature [verified in
  0x5992B0's use of it], which IsCreature now checks. Undo used the same scan. Not tested yet.
- A deleted creature's record is found by tile, owner, then type and level. One with none (level setup made it) goes from the world only.
  In the world a Unique creature has its base type (the log showed Unique Goblin deleted as Goblin), so names come from the record.
- The menu's level (1 to 10, [verified: 0x48B120 stops at 10]) opens out in the menu. Choosing one remakes the creature from its
  changed record, so level load's own code raises it, then the record is replaced. It is one undo step. [unverified] What a high
  level looks like straight after, in a held world, as raising a level can start effects.
- Undo covers both. Hero parties are not on the map until a trigger makes them, so they cannot be deleted here.
- The menu, the palette and the side panel each have a view of their own that they clear themselves. GuiTextCache keeps a clear back
  until text differs, and it leaves out text matching the last run first, so the late clear wipes what it left out: hovering Undo or
  Save could take away the Owner line, which only came back at the next change. [verified in the code, fits what the user saw]
- Level numbers in the menu sometimes not showing: not explained. Text starting with spaces was the suspect, so they now sit further in by
  position, with no spaces. Each row is checked after drawing and the log says if one drew nothing (up to 20 times).
- The second tab: `TextById` gives "Terrain" for 2157 (its config +0x30) and 175 (+0x40) in a session [verified in the data at 0x690CF8;
  which one shows is not known, so both]. The icon is the Rock cell, drawn in the panel's view over the button, which stays lit
  behind it. Its place is measured from a screenshot, so it would be off if the tabs were moved. It is not drawn with the panel closed.

## Saving under a name, thumbnails, testing (written, not tested)
- Palette: Undo, Save, Save as on one row, Test the level on the next. Ctrl+S saves (F5 still does), Ctrl+T tests.
- Save as: the status row becomes "Name: ..._", typed with letters, digits, space, - and _ (Shift for capitals and _), DirectInput key
  numbers. Enter saves, Esc stops, Backspace deletes. Keys go to the name only while typing, but WASD still moves the camera, as the camera
  reads held keys elsewhere. [unverified that Shift is modifier bit 1; Ctrl is 2]
- A named level is written to Data\editor\maps as <name>.kwd with its own files <name>Map.kld, <name>Things.kld and so on (the kind is what
  follows the old level's name in each file's name), and its list's paths point at those. The description's first field, the wide file
  name (`<name>.kwd`), is replaced: stock levels have nothing else in its first 0x1180 bytes, so 64 wide characters fit. Names are kept to 40.
- Stock levels are never written: a name already used by a level the editor did not make is refused. Levels it made are listed in
  KeeperForge2\maps\editor-levels.txt, and Save writes those in place. Saving a stock level still makes its copy in KeeperForge2\maps.
- After Save as the game's level name is the new one, so a test or a restart loads it. [unverified] Whether the skirmish list shows a new
  level without restarting the game, and whether it needs more than the file name in the description.
- Thumbnail: Data\editor\maps\Thumbnails\<name>144.bmp, as the game reads them ("Maps\Thumbnails\%s144" at 0x53F7F0, from the level's wide
  name). Made from the map's tiles, 144x144, 24 bit, the whole map in its shape on black: rock, gold, water and the rest in fixed colors,
  owned tiles in the owner's color (red, blue, green, yellow, white for heroes). Only for the editor's own levels. [unverified: the game's
  size and depth, as those files are not opened here]
- Test: saves, leaves the session (Windows' pointer off, the hand back, the game's panels back) and calls action 0x66's handler 0x513870
  (thiscall world, action; ret 4) from after a world tick. It loads the level again from the level name as a restart does [verified in code:
  it formats `%sMaps\%s.kwd` from 0x75AD57]. With the session off, the level plays with the skirmish's rules. The palette shows only "Back to
  editing (Ctrl+T)", which loads the level again from its saved files the same way, with the session on, and reads the document again. Leaving
  the level while testing goes back to the skirmish screen ready to edit. [unverified] Everything about how this goes.

- Tested (Patrol, 2026-11-07): placed creatures and painted tiles were gone in Test and after Back to editing, while the editor's
  list had them (152, 182, 197 things: the log). [verified in our code] A stock level opened without a saved copy kept the game's
  level name on the stock files; the first Save made the copy, but Test and Back reload by that name. Save now points the name at
  the copy when it has one (PointGameAtSavedCopy) [unverified in game].
- Tested: Save as made testmap with all six files and its thumbnail, and the test played. Going back to editing crashed at 0x417366: an IDIV
  by the count of shown panel windows 1 to 4 in 0x417310, which the restart's panel layout (0x416FD0) reached with all of them hidden by the
  editor [verified in the assembly]. 0x417310 is now skipped in a session.
- Tested: the saved level marks 3 creatures as mercenaries (log), so they are saved; the user still saw normal ones. The 4 tick recheck had
  not run before the crash. Tests now apply the mark after the game's load too, as the game ignores it. Playing a saved level outside the
  editor does too since batch 2 (below).

## Room objects and wall props (written, not tested)
- The user's report: a 5x5 Hatchery painted in the editor had 3 pens, not 4 (they think the game does the same when a room is
  built a tile at a time), and walls beside rooms kept props that should have gone or lacked ones that should be there.
- Wall props [verified in code]: when the game changes a tile's terrain itself (digging, reinforcing: 0x453221, 0x4533C0) it sets
  the tile (0x44FC40) and then calls 0x4ECDB0 (thiscall world + 0x9BE, x, y; ret 8). For each room on a tile beside it, that takes
  the room's wall pieces off (0x4F0D70), works out its walls (0x4ECEB0), clears every object the room placed itself (0x4E6BC0,
  the list at room +0xE) and places them again (0x4EF5E0, 0x4EF1F0, 0x4EDFC0), then puts the wall pieces back (0x4F0E30). The
  editor painted with 0x44FC40 alone, so rooms never heard of a wall or floor painted beside them. Painting now calls 0x4ECDB0
  after every tile, room or not.
- Pens [verified in code]: every change to a room re-places its objects for the whole room, but by walking the room's chain of
  tiles: first at room +0x2D, last at +0x2F, each tile's next at tile +4, two bytes x then y, (0, 0) ending it. A tile goes on the
  end when added (0x4ECB70), so the chain is in the order of building. A Hatchery (0x4DE220) wants one pen for every 6 tiles and
  puts one on each middle tile of the chain (a room tile with +0xC 0xFF, 0x454110) not within about 1.7 tiles of one already
  placed; an early pen in the middle blocks the rest. A build in one go adds row by row (0x4F04F0's loops), so its chain runs that
  way and 5x5 fits 4.
- So when a stroke ends (and after an undo or a room tile's Delete), each room of the world at or beside a changed tile has its
  chain put in row order, top to bottom and left to right, and is refreshed with 0x4ECDB0 from the tile right of its first. The
  Dungeon Heart and hero gates are left alone. Nothing is sold, so nothing in a room is lost and no sound plays. The level's file
  does not change. [unverified] Whether this gives 4 pens, and whether the wall props now show at once in the held world.
- Since 2026-11-13 GameFixes puts the chain in row order inside every object placing (game-fixes.md, Room objects), in play as
  well, so the editor's own sorting is no longer needed for the pens; the refresh is still what makes a stroke's rooms re-place.
- A room's tiles are bytes in that chain, so no room can be past x or y 255, whatever the map's size (see New map). Tested: a new
  1024x512 map, its heart at 512,256, crashed level load at 0x4ECF7B (from the tile loader 0x51EDA4 through 0x4C5CCA and 0x4ED2BC):
  a neighbor's tile was looked up at y -1, read past the map. So New map now puts the heart at most at 251,251, and painting a room
  tile (by any tool, and paste) past 254 either way is refused.

## Objects and action points (written, not tested)
- The Workshop tab's last item, "Objects and action points", turns it to a second page: its first item goes back, its second is
  the action point tool, then 48 objects levels place (gold, spell book, chicken, trap and door crates, keys, bones, a barrel,
  hero tables, the Specials). Room furniture, beds, pillars, the heart and front end objects are left out. ObjectIcons.bmp holds
  only ten pictures, left to right and down, four a row: crate, gold bag, gold pile, spell book, bed, gem, a Special's box, table,
  weapon rack, level indicator [verified by eye: docs/game/tab images]. Gold, gold bags, spell books, crates, the Specials, the
  hero gate table and the armory trestle use those; the chicken borrows KeeperSpellIcons.bmp's chicken (cell 10). Keys, bones and
  the barrel have none and show as a plain cell, named on hover.
- Placing an object: like a creature, on open ground, with the owner from the heart, made by level load's own functions from its
  record (0x56F6E0, 0x56F730, 0x520BB0). A right click on the panel steps gold through 50 to 5000 (both kinds share it) and a spell
  book through the keeper spells. A right click on a placed object of the level's list gives a menu with Delete, which removes it
  through its vtable's +0x30. Objects that rooms make (no record) get no menu. Undo works for both.
- Action point: press on a tile and drag; the status row shows the box, and letting go places it with the lowest free number.
  They are only in the level's list while editing (a test and the game make them from it). To see them, each has an outline (below);
  only if that hook fails does a Skull (object 49) stand on each corner instead, in the world only, never saved, made again each
  time the level is opened. A right click on a tile inside
  one (the smallest, for one inside another) gives its menu with Delete. Since batch 3 deleting one used by a trigger is warned about
  and its name can be changed; its other fields are not editable.
- Right click order: creature, trap or door, listed object, action point, room, then picking the tile. The Delete key uses the same.

### Action point outlines (written, not tested)
- [verified in the assembly] The game draws one box on the map, the hand's. 0x40ACD0 hands the interface's box (+0x132F: x1, y1,
  x2, y2, the end not included) to the engine interface (*(interface +4) +0x10) through 0x44E130 and 0x44E1C0, which keep it at
  +0x10 and +0x4E. The tile drawer's draw of one tile, 0x43CC30 (thiscall drawer, x, y, pass; ret 0xC; vtable 0x66CDC8), reads
  it on pass 0: for a tile on the border it moves the drawer's box piece (engine thing table 0x6D6458, index at drawer +0x35D)
  to the tile with 0x446C70, sets piece +0x81 to the sides (1 top, 2 bottom, 4 left, 8 right, always | 0x10, | 0x20 when drawer
  +0x36 is set), sets bit 0 of +0xD5, and calls the renderer (drawer +0x9E) slot +0xE8 (x, y, piece, 0). Each tile on the border
  moves the same piece, so the renderer must use it at once.
- The editor hooks 0x43CC30: after the game's own draw on pass 0, each tile on the border of any action point (and the one being
  dragged out) gets the piece drawn again with its sides, then the piece is put back as the game left it (sides, +0xD5, place).
  The list of boxes is remade after each world tick. The Skull markers are only made if the hook fails.
- [unverified] That the renderer draws it at once (if it only queued it, every piece would land on the last tile), that a held
  world draws it at all, and its color: it is the hand's box color. What 0x20 changes is not known (perhaps the red of a place
  that cannot be built on). Making it white or blue needs whatever draws the piece's look, not found yet.

## Tools and fixes, batch 1 (written, not tested)
- Palette rows: Owner; Undo, Redo, Save; Save as, Test, New map; Brush NxN and Mode; the status line. Clicking Brush steps 1, 3, 5,
  7 tiles square (and goes back to the brush). Clicking Mode steps Brush, Box, Fill, Copy, then Paste once something is copied;
  with a creature or other thing chosen, the first click only goes back to terrain. Ctrl+T still tests, it is no longer in the text.
- Brush: a square around the tile under the pointer, dragged as before. Box: drag out a box, letting go paints it all. Fill: a click
  paints every tile joined side by side with the same terrain and owner. Box and fill stop at 16384 tiles, as each tile is the
  game's whole tile change, and both skip the map's edge. Each is one undo step.
- Copy: drag out a box; its tiles (terrain and owner) and its listed creatures, traps, doors and objects are kept, and Mode goes to
  Paste. Paste: a click puts the copy's top left there (its outline follows the pointer, a tick behind). Things that do not fit
  (ground, a trap there already, a door's walls, a player the level does not have) are left out and counted. Action points are not
  copied, as triggers know them by number. Hearts and hero gates stay as they are on both sides. The copy stays when another level
  is opened.
- Hearts and hero gates: painting over one, or painting one in, is refused. They are things in the level's list too, and level load
  builds them from those (a heart painted over kept its object).
- Painting clears things: a tile painted to solid terrain loses the listed creatures, objects, traps and doors on it; water, lava and
  others with no-build flag 0x02 lose traps and doors only. They go into the stroke's undo step. Things made by level setup, not
  listed, stay, as undo could not bring them back.
- Undo steps now hold any number of thing changes with their tiles. Undo goes back through the tiles, then the things, so a thing
  comes back after its ground. Redo (palette, Ctrl+Y or Ctrl+Shift+Z) is the step undo gives back: a placing's is a deletion, a
  deletion's a placing, a change's the record before. Any new change clears redo.
- Move: in the right click menu of a listed creature, trap, door or object, and of an action point. The next left click on a tile
  moves it there (an action point by its top left, keeping its size), checked as placing is. It is taken out of the world and the
  list and made again from its record with the new place, as one undo step. Esc, a right click or choosing on the panel stops it.
  Things not in the list (made by level setup) have no Move.
- Delete key: what a right click would give a menu for, deleted at once (creature, trap or door, listed object, action point, room
  tile). [unverified] that the game hands the key on as DirectInput's 0xD3.
- Quitting with changes not saved: the first leave action (1 or 125) is dropped with a warning on the status line, and leaving again
  goes. Saving or a new change makes it warn again. [unverified] that the game sends the leave only once, so dropping it does not
  leave the options menu waiting.

## Level settings, players and availability, batch 2 (written, not tested)
File layouts in level-files.md (Description, Players, Variables).
- The level's Players and Variables files are loaded with its map and things, and its .kwd's level section is kept. All are written
  on Save (into the level's copy for a stock level, into its own files for one the editor made) and taken along by Save as. For the
  .kwd, only the description is written, into the .kwd being saved to, so a copy's file list stays its own.
- Palette row 4 is now Brush, mode (Free paint, Box, Fill, Copy, Paste) and Level. Level opens a menu beside the palette:
  - Name, Description, Author, Email, Information: a click types it on the status line (letters, digits, space, - _ . , ' " / ? ! @),
    Enter keeps it, Esc leaves it. The names of these fields are a guess (level-files.md).
  - Each player: its starting gold (a click types it) and, for keepers, whether it is a computer player (a click toggles it, and the
    menu stays open). A keeper made a computer player with no settings of its own gets another computer player's settings.
  - Availability on the panel: on or off (stays open). "Level *" on the palette while it is on.
- Availability on the panel: each Rooms, Creatures and Workshop item reads "name: available" and so on, colored by it, for the
  keeper chosen by the heart (heroes or neutral mean every keeper, player 0). The Terrain tab holds the keeper spells instead. A
  click steps: not set, available (4), researchable (3), not available (1), not set. A creature only steps between not set and
  "comes" (3), and gets its pool record (kind 1, 20 of it) with it or loses it; creatures need a keeper chosen, as stock levels never
  give them to player 0. Right clicks on the panel do nothing then. Turning it off puts the items back as they were. The heart only
  reaches keepers 1 to 4, so a fifth keeper's availability cannot be changed yet.
- Not in undo: level texts, players and availability. They do count as unsaved changes.
- Mercenaries in a level played for real (not edited or tested): on its first world tick the level's own Things file is read from
  the level name, and each marked creature is made a mercenary, then checked again 4 ticks on (by tile, so one that has walked off is
  not found again). Not for a saved game, which holds them. [unverified] A restart of the level does not apply them again.
- [unverified] Everything above in game: whether the game reads the texts where it shows a level, whether a changed gold or computer
  player shows, and what each availability value really does.

## Triggers to look at and action point names, batch 3 (written, not tested)
File layout in level-files.md (Triggers).
- The level's Triggers file is read with the rest, and never written (Save as copies it as it is), as nothing changes it yet.
- Level menu, "Triggers (to look at)": whose trees there are (the level, players, action points, creatures, and trees nothing
  reaches, likely hero parties'), a click shows that tree: "When <trigger type> <value> [its four settings bytes]", and under it, one
  deeper, its triggers and "Do <action type> [eight settings bytes]", with "at action point N" where that is known. 16 rows a page,
  with Previous and Next page, and Back to the list. The settings bytes are shown raw, as most are not decoded.
- An action point's menu: Rename (typed on the status line, 31 characters, one undo step; it shows in its text from then on), and
  "Triggers using it (N)", which lists the actions that name it with whose tree they are in, then its own tree. Delete warns first
  when anything uses it, and a second Delete goes ahead.
- Done since: changing triggers and hero parties (batch 4).

## Changing triggers and hero parties, batch 4 (written, not tested)
File layouts in level-files.md (Triggers, Hero parties).
- Level menu, "Triggers and hero parties": every tree owner (the level, each player, each action point and hero party even with no
  triggers, "(none)"; party members and creatures that have one; trees nothing reaches). A click shows the tree; a click on a row opens
  that trigger or action. "Nothing yet: add a first trigger" starts an empty one (its first trigger's id goes into its owner).
- A trigger's page: its type (a list of all 75 to choose from), the comparison (a click steps < <= = > >= !=), byte 1 (what: the flag,
  the timer, ...), whether it compares with the value or with the flag in byte 3, the value, byte 3, the repeat count; add a trigger
  after it, add a trigger under it (if nothing is), add an action under it (if no trigger is); delete it with all under it.
- An action's page: its type (51), bytes 0 to 7, named where the game's handler shows what they are (Set Flag, Create Hero Party ...),
  the repeat count; add an action after it; delete it. Numbers are typed on the status line (the value may be below 0); the menu stays
  open and shows the change.
- New records: a trigger "Flag 0 = 0, once", an action "Set Flag 0 to 1", with the next free id (triggers count up, actions down
  from -1). Deleting points whatever pointed to it (a trigger before it, the one above it, or its owner) to what came after it.
- Hero parties: the list, New hero party (the lowest free number), and a party's page: its members (a click steps one's level 1 to 10),
  add the creature chosen on the Creatures tab, remove the last member, delete the party. Each is one undo step. A party is brought in
  by a Create Hero Party action (byte 0 the party, byte 4 the action point).
- Triggers are not in undo. They count as unsaved changes; the file is only written when they changed, so other levels keep theirs
  byte for byte.
- Dead bodies and effect generators (level-files.md, Other thing kinds): two items on the Objects page. A dead body is of the creature
  last chosen on the Creatures tab, with the owner from the heart; an effect generator copies the stock one's settings. Both are made
  by level load's own makers and need open ground. A right click (or Delete) on one gives Delete.
  - Dead body: Delete and undoing its placing take it out of the world as well as the list, and undoing its deletion makes it again.
    It is a thing of its own (vtable 0x66D904, 0x140 bytes, type at +0x128, made by 0x4B7FC0), found by its tile (+0x1A/+0x1E as
    other things, written by 0x4B5020 from its init 0x4B0F20) and its type. Its vtable +0x30, the remove slot objects use, calls
    0x4B81A0 on the creature collection (0x6E504C) with its tag: unlinked (0x4B1070), destroyed, given back to the pool and the
    collection's count (+0x36) lowered. [verified in code; not tested]
  - Effect generator: the same, through its own thing (vtable 0x66D814, made by 0x4B81E0 from 0x521380, constructor 0x4B7560,
    init 0x4B19A0, which places it through 0x4B5560 so its tile is the usual +0x1A/+0x1E). Its type, the record's +0x1D, is at
    +0x44. Its vtable +0x30 calls 0x4B8250 on the creature collection with its tag: unlinked, destroyed, given back to the pool
    and the collection's count (+0x3A) lowered. [verified in code; not tested]
- Plinths have nothing in the game to make them, and the camera record is the level's camera settings, so neither is placed.
- Outline color: the box piece's tint (engine thing +0x6E) is set from config.yml's editor_outline_color (six hex digits, default
  80C0FF, or game) while each outline is drawn, and put back. [unverified] Whether the box is drawn with its tint, and the bytes' order.
- Not done: players' own action handler (the actions shown with their bytes still work, only fewer are named).

## Creature types window (written, not tested)
Field layout in creature-types.md; saving in level-files.md (A level's creature list).
- A right click on any creature on the Creatures tab opens Windows' own "Modify Creature Properties" window for that type
  (EditorDialogs.cpp), laid out like the official editor's. A creature's kind (Unique, mercenary) is chosen there too
  ("Places as", below); a right click used to step it.
- It shows the level's creature list (its own if it has one, else the shared Creatures.kwd). It covers: the Name list (every
  type, so Unique ones and Jack too), the 43 numbers of the Attribute list, 23 checkboxes and the Evil/Good and Male/Female
  pairs, Armor Type, Fight Style, Job Class, Death Fall Direction, Lair Object, Clone Creature, the four effects, the two
  Attractions, Unique Name Text ID, Intro Camera Path Index, and both sound categories.
- Rename... asks for a new name (plain characters, up to 31) in a small window of its own; the creature window waits while it
  is open. The new name shows on the Creatures tab once given to the level.
- Add Type... asks for a name and adds a copy of the type shown at the end of the list, with the next id (its id byte at
  +0xF4B set to it), up to id 255. Its panel item has the "?" icon.
- Delete Type takes away the type shown, only if it is the last one and after the 46 of the shared Creatures.kwd (ids are
  places in the list, and the game's code knows its own types by id). It is refused while another type names it (Clone
  Creature, a creature attraction) or the level uses it: a creature on the map, a dead body, a hero party member, a
  keeper's pool or availability (Variables), or a Create Creature action. The editor checks the level again when the
  deletion comes back, and keeps the type if something used it in the meantime.
- [unverified] How the game copes with more than 46 types. Players keep their per-type counts in lists made from the
  list's size (GameAddresses.h, PlayerCreatureTypes), but fixed size tables elsewhere have not been looked for.
- Eight buttons open windows for the record's other parts, as the official editor's do: String / Speech IDs, Job
  Preferences, Available Attacks (with the melee attack), Resistances, Light, Anger String IDs, 1st Person Properties, Anim
  Offsets (layouts in creature-types.md). Each lays its fields out in rows under column headers from a table in
  EditorDialogs.cpp. The creature window waits while one is open. Its OK writes the fields that were changed into the
  working record (all or none, if one does not fit), and Apply or OK on the creature window gives that to the level.
- Art resources (written, not tested): six more buttons (Anim Resources, GUI Icon, Portrait Resource, Creature Icon, Unique
  Resource, 1st Person Resources) open one art window on that resource. Its list at the top reaches every resource of the
  creature (47 animations by name, then the single ones). It shows the type, the name, the fields and boxes that type has
  (the others are not there), and for an animation its footstep frames. OK puts the changes into the working record, as the
  part windows do. Statue Resource is left out: its place is not known (creature-types.md).
- Places as: a creature with a Unique type or mercenary textures has a "Places as" list (Normal, and Unique or Mercenary) by
  its name in the creature window; one without has no list. It is the editor's placing setting, so it takes effect at once
  (no Apply). Shift with a right click on the Creatures tab no longer changes it; a right click there always opens the window.
- Left out: the four flags past the game's record size (creature-types.md).

### Placed creatures (written, not tested)
- A level keeps very little of a placed creature's own: position, owner, level, health percent, gold held and flags
  (level-files.md, Things). Everything else is its type's, shared by every creature of the type.
- A right click on a placed creature opens the creature window on it (no longer the in-game menu; traps, doors and objects keep
  theirs). Its type's stats show as usual and change for every creature of that type. A row "This creature" holds its own Level
  (1 to 10), Gold and Dead, and a Health row. OK or Apply makes it again from its changed record (as the level will), one undo
  step; a change of hit points alone that keeps the record's percent only sets them on the creature.
- Health row: a percent box (typed, or picked from 100%, 83.33%, 66.66%, 50%, 33.33%, 16.66%) with the petals of the health
  flower it leaves (six petals, a part petal counts as one: 50% is 3 left, 50.01% is 4), and a Points box with "of N at level L".
  Each fills the other. The percent takes two places: a third place of 6 to 9 rounds the second up, 0 to 5 leaves it (36.846 is
  36.84%, 36.847 is 36.85%), anything after is not looked at. Points are whole, from 1 to the creature's most at its level (a
  value the level already had passes as it is). Changing the level keeps the percent and moves the points.
- Most hit points at a level: the type's Health (creature record +0xF0C) at level 1, else Health times the game's per-level
  percent (16-bit at settings +0xE1 + level × 2, the settings being what 0x6E4154 points at) over 100. A level made creature
  gets 1 + trunc(percent × 0.01 × most − 1.0), at least 1 [verified: 0x5208B0]. Its current hit points are the 32-bit value at
  creature +0x40 [verified: same function].
- The level only keeps a whole percent, so the record gets the nearest one (1 to 100) and, when that does not give the chosen
  points, the exact points go in the level's `.health` file (level-files.md), which the editor puts on the creature (+0x40)
  when it loads the level, after Test, and in a played level on its first tick, beside the mercenary marks. An entry is found
  by tile, owner, type and level; one that no longer matches a creature is left alone.
- Undo and redo take the exact hit points along: an undo step keeps the list as it was before it whenever the step changed it
  (a step of hit points alone too), and taking it back puts the list back and sets every creature named in either list to its
  exact points, or to what its record's percent gives. Moving a creature moves its entry; deleting it, changing its level
  from the in-game menu, painting rock over it or making it a dead body drops its entry (all brought back by undo). A change
  to a new type the world does not have yet stays out of undo, as before.
- A creature window open on a placed creature follows it through undo and redo: its row (level, gold, health percent, petals,
  points) shows the creature as it is after, found by its record (unchanged; changed in place; or moved, as the like record put
  in by the step). The type's fields keep what is being edited. If the creature is gone, the window goes back to its type.
- The chosen creature (left click) follows undo and redo the same way, keeping its outline, Delete and drag; gone, nothing is
  chosen. One not in the level's list stays chosen while it is still there. A drag under way when undo comes puts its creature
  back on its own tile first.
- The creature window's title says "[Outlined]" after the creature's place while its creature is the chosen one (checked each
  editor tick, so it follows choosing, letting go, dragging and undo). A window open on a creature being dragged keeps the mark
  through the drag and is put on the creature again (its new place) when the drag ends.
- Deleting a creature the creature window is open on, painting rock over its tile, or making it a dead body (Dead with OK or
  Apply) closes the window, as Cancel would (with any of its smaller windows):
  changes being made there are dropped; types already given back with OK or Apply stay. The editor keeps the creature's record:
  an undo (or redo) that brings a creature with that record back opens the window on it again, if no creature window is open
  then. A redo that takes the window's creature away closes the window the same way, so undoing again reopens it. Forgotten
  when any creature window is opened or another level is loaded.
- Closed this way, the window keeps what it held and opens again with it: its working copy of the list (types changed, added,
  renamed or given their own copy, not yet applied), the type and attribute it was on, the fields changed and not yet read
  into the copy (shown as they read, and still marked changed so OK or Apply takes them), the creature's own row (level,
  gold, health percent, petals, points; not Dead) and where the window was. Only if the level's list is still the one the copy
  was taken from; else it opens as new. A smaller window open over it (Rename, Add Type, a part such as Job Preferences, or Art
  Resources) is kept too and opened again over it, where it was, with its own changes: the name typed; a part's changed
  fields (still marked changed, so its OK takes them); the art window's resource, its copy of the record (with changes already
  made to other resources) and the changed fields, a changed type laying out its fields first. A window opened any other way
  drops what was kept.
  Dead takes it away and puts a dead body of its type on its tile (one undo step: undoing it takes the body out of the world and
  makes the creature again).
- "Give It Its Own Stats..." (asked first) adds a copy of its type at the end of the list and points its record at the copy, so
  its stats change alone. The game knows some types by id (Imp digging, the King, Lord and Stone Knight, the Mercenary Portal,
  creature pools, availability, triggers counting a type), and a copy is none of those. A type past the ones the world was made
  with is not in the game's data until the level loads again: nothing of it is made in the world before then (the creature
  keeps its old look and stats in the editor, and that change is not in undo), and placing one is refused with a message.
- From the side panel, the row is "New ones placed": the level new creatures of the type go down at, and "Placed as a dead
  body". Editor settings, at once, as "Places as" is. Creatures already placed keep what they have.
- A left press on a creature chooses it (outlined) instead of painting or placing there; holding the button drags it. It follows
  the pointer: on each new tile it can stand on, it is taken out and made again there from a copy of its record (keeping its hit
  points); a tile it cannot stand on is outlined and it waits on the last good one. Its record stays as it was until the button
  is let go. Let go over a good tile, the record moves there (one undo step, as Move); over a bad one, it goes back to where it
  was. Delete deletes the chosen creature (put back on its own tile first if mid drag; otherwise what is under the pointer, as
  before); Esc lets go of it (mid drag, it goes back first), as does a press elsewhere. Move keeps hit points too.
- The creature window is 842 layout pixels tall, more with a bigger font (scaled by the font's height over 15). When that is
  taller than the work area of the monitor the game is on, the window is cut to the work area and gets a vertical scroll bar:
  the bar, the mouse wheel (three 32-pixel steps a notch), and Tab or a refused field (scrolled into view) move it. Its
  controls move with ScrollWindowEx; the Attractions frame it paints itself is drawn with the same offset. The window is
  also kept inside the work area. A focused drop-down list still takes the wheel itself, as Windows has it.
- Every editor window's title bar menu (click its icon area or right click the title) has "Copy a picture of this window":
  PrintWindow with full content to the clipboard, for showing the window when a capture tool cannot reach it.
- An attraction's target list and amount are hidden, not disabled, when its kind has none: a disabled classic combo box or
  edit paints itself in Windows' gray whatever colors its parent gives, a light block in dark mode. [unverified that this
  was the white the user still saw after the frame below]
- The Attractions frame is drawn by the window: a classic group box paints only its border, and the window clips its
  children, so its inside was never filled (it showed white in dark mode).
- Colors: config.yml's editor_dark_windows (default on) gives the windows dark colors and, where Windows has one, a dark
  title bar (DwmSetWindowAttribute 20, or 19 on older Windows 10, loaded by name). The game has no visual styles, so the
  controls are the classic ones: labels, boxes, edits and lists take their colors from WM_CTLCOLOR..., and push buttons
  are drawn by the window. [unverified] How a classic combo box's closed field and arrow look in dark.
- OK and Apply give back the whole list when anything in it changed (types changed, even ones the Name list moved away
  from, renamed, added or deleted). Cancel and closing drop what was not applied. The editor puts what comes back into its document after the world tick. It counts as unsaved changes, and it is
  not in undo. Only fields the player touched are written, so a record that was only looked at keeps every byte.
- The world being edited does not change: the game made its creature data when the level loaded. Save, then Test (or open the
  level again) to play with the new types.
- It needs Windows' pointer (the editor's own, below); with the hand the mouse is held by the game, so it is refused then.
- [verified in the export] It runs on the game's thread. The game's loop 0x5B2DD0 takes and hands on every message of its
  thread, so it drives this window too, but it never calls IsDialogMessage. A WH_GETMESSAGE hook on the thread does that
  for this window, so Tab, Enter and Esc work. On the same thread, a click on the window does not tell the game it lost
  the foreground to another program (no WM_ACTIVATEAPP), so full screen mode does not minimize. The game's own activation
  handler (0x5BC280) only shows or hides the pointer and takes the mouse again [verified].
- [unverified] Everything in game: that the window shows and takes the mouse over the game, that the game's keyboard stays
  out of it while typing (its devices are foreground only), and that a saved level's own creature list loads and plays.

## New map (written, not tested)
- Palette row 3 is now "Test (Ctrl+T)" and "New map". New map asks in turn on the status row for a name, a width and a height in tiles
  (digits only, any size; width and height need not match). Enter goes on, Esc stops. A name any level in Data\editor\maps already has is
  refused at once. With changes not saved, the first click only warns; a second goes ahead and leaves them.
- Size: the game has no largest map [verified in code: 0x44E9E0 allocates width * height tiles of 16 bytes, 0x4D1840 a byte grid and a
  two byte grid of the same count, all from the file's width and height with no check]. Our own check had a 4096 limit; it is gone, and
  only a size whose 16 byte tiles overflow 32 bits is refused (LevelFiles::MapSizeFits). Smallest side 9, for the edge and the heart.
  Rooms do store tile coordinates in a byte [verified in code: a room's chain of tiles, 0x4ECB70], so a room past x or y 255 cannot
  work on a larger map; the rest of such a map can still hold terrain, creatures and so on. [unverified] What else breaks past 255;
  how the game copes with a very large map's memory in a 32-bit process.
- The level is made by LevelFiles::MakeBlankLevel: Swiss Cheese's .kwd and other files as the template (four keepers, no triggers, so
  nothing waits for things that are gone), a new map of Rock owned by neutral inside an edge of Impenetrable Rock, and keeper 1's Dungeon
  Heart in the middle. The things file holds only that heart's room record, Swiss Cheese's keeper 1 heart moved to the center. Written
  through Save as's code, so it is an editor level with its own files and a thumbnail.
- Then the game's level name is set to it and the level loaded in place (0x513870), the session still on, and the document read again,
  as Back to editing does. CheckDocument logs it if the world's size differs from the file's. [unverified] Whether the restart in place
  copes with a map of a different size from the one loaded.

## Holding the simulation [verified in code; first version tested, this one not]
- World tick 0x50A450 (thiscall (world, actions), ret 4) carries out the action list,
  runs some timers, then at 0x50A642 skips everything else if session +0x284 is set.
  That field means a menu is open.
- The player interface also reads session +0x284 and stops taking input (0x4066A0
  returns early), so it is set only during the tick and put back after.
- The session counts a turn (+4) after each tick unless the menu field was set both
  before and after it (0x5266DF, 0x526B94), so the turn went on in the first version.
- Drawing passes the bridge { turn (+0x278), blend (+0x27C) }. The blend is 0..256, from
  0x521C80's time into the turn, and is left alone while the menu field is set (0x526CC6).
- Tested: with both running, things bobbed and appeared and vanished. Likely cause: each
  thing is drawn at a blend that restarts every turn, and animations go by the turn.
  [the link to what was seen is inferred; what the bridge does with them was not traced]
- Now, as under a menu, both are kept still: the turn is put back before each tick, and
  0x521C80 returns one whole turn, so the blend is 256. [unverified: that 256 and not 0 is
  the right end, and that the camera stays smooth with a still blend, as its moves come
  through turns]
- An action list is a count byte at +0, then actions of 0x12 bytes from +0x11. An
  action's kind is at +0xC. Handlers are at world +0x1684 + kind * 0x10, 0x8A kinds.
- Carried out: 4 (moves the camera, coop-campaign.md), 7 and 8 (sent by the interface's
  camera code 0x4066A0), and 1 and 125, which leave the level.
- Kinds 1 and 125 go to 0x510E90 with 0 and 1. It sets player +0x76 bit 0x2000, which the
  tick returns as "stop". Tested: Quit did nothing while they were dropped. [which menu item
  sends which is inferred from the order in the log]
- Kind 92 (handler 0x527270) is the menu's pause: it sets session +0x284 and saves or
  restores the turn. Tested: with the field set for real, no button in the options menu
  worked, and the user had to kill the game (exit code 0xC000013A). Cause not found, as the
  same field works in a normal game. In the first version, with the field clear, the buttons
  did send their actions.
- So the toggle 0x428710 now runs with play mode 3 for that call. Multiplayer's branch at
  0x4287A9 sends no kind 92 and goes by interface +0x2722. Kind 92 from anything else, such
  as Review Objective, is dropped. [not tested]
- Kind 102 (0x66) is dropped too, so Restart Level does nothing yet. See below.

## Menu button [verified]
- 0x538000 is the menu's click handler, cdecl (first, second, front end), in bytes our
  export did not disassemble. first: low word this window, high word next window.
  second: low word what to do, high word the button's image number. It only goes on if
  that image was drawn hovered, unless the number is 0xFF (0x538077).
- This window's images are baked by 0x533570 (window, slot, font): window 2,
  slot 5, font 6 [reference for the call, which is in code our export does not show]. It
  goes through the window's live buttons whose renderer is 0x532670, with kind 0 and a
  text id at +0x30 (config +0x40). It asks 0x553A60 for each text, and numbers them in
  list order.
- Our button is a copy of Skirmish with image 4, placed last in the list so the others
  keep their numbers. Its text id 60000 is answered by a hook on 0x553A60. So it has the
  game's own font, glow and hover sound.
- Dead end: FrontEndTexts cannot reach this menu, as 0x5329A0 is not what bakes it. Drawing
  the text live worked but had no glow.

## Restarting a level in place [verified in part]
- Action 0x66 (handler 0x513870) tears down the current level's things and builds them
  again without leaving the game. It refuses in multiplayer. The interface sends it for key
  action 0x39 only under a condition on its stack not traced (around 0x4053F1). Flame calls
  it "editor load level" [reference]. A likely tool for stage 6 and for reloading after a
  save. It sets world +0x7239 bit 0x40, as 0x50E620 does.

## Single Player menu (front end window 2) [verified]
Window config 0x6AD658, buttons 0x6ADAE0 (0x50 each, layout in skirmish-lobby.md).
The list ends with id -1 at 0x6ADCC0.

| Button | Id | y | h |
|---|---|---|---|
| Title | 0x24F | 24 | 232 |
| New Campaign | 6 | 408 | 172 |
| Continue Campaign | 0x237 | 588 | 172 |
| Skirmish | 8 | 852 | 172 |
| Load Game | 7 | 1032 | 172 |
| Back (X) | 9 | 1688 | 192 |

The rows are 180 apart, with a larger step of 264 before Skirmish. Continue
Campaign ends at 760, which leaves 92 before Skirmish: too small for a new button.

Layout used, keeping the larger step: Editor y 768, Skirmish y 1032, Load Game y 1212.
Load Game then ends at 1384, well above Back. The list has no spare entry, so the
window gets a new copy, as Swap Keeper does for its own.

## Level files
- A level is `<name>.kwd` plus `<name>Map.kld`, `Players`, `Things`, `Triggers` and
  `Variables`, all in `Data\editor\maps`. The optional `<name>Globals.kld` appears in
  the game's strings. [verified: file names in the game folder and strings in the export]
- Shared definitions are in `Data\editor`: `Terrain.kwd`, `Rooms`, `Traps`, `Doors`,
  `Objects`, `Creatures`, `CreatureSpells`, `KeeperSpells`, `Shots`, `Effects`,
  `EffectElements`, `GlobalVariables`. So terrain, trap and door lists are game data.
  [verified: files present; their contents not read yet]
- Section ids from the pro editor's file reader [reference, editor, pro version]:
  Map 100, Terrain 0x6E, Room 0x78, Trap 0x82, Door 0x8C, Keeper Spell 0x96, Creature
  Spell 0xA0, Creature 0xAA, Player 0xB4, Thing 0xBE. Each starts with a 12-byte
  header.
- The game's terrain reader at 0x56F090 expects 0x6F and then 0x70, so a section's
  blocks are numbered from its section id. [verified for terrain only]
- The editor's error strings name the Thing kinds: creature, object, trap, door,
  action point, hero party, dead body, effect generator, room, plinth, camera.
  [reference, editor, pro version]

## Loading a level [verified]
- 0x50E620 (fastcall, world; called at 0x52565A) sets world +0x7239 bit 0x40, and
  formats `%sMaps\%s.kwd` from the base path at 0x758C68 and the level name. What the
  bit means exactly is not known: the fog code at 0x50C8AC skips when it is set.
- The level name is passed in by the caller. Whether a save outside `Data\editor` can
  be loaded through that name or needs a hook is not checked. [unverified]

## Changing a tile while the game runs
- 0x44FC40 sets a tile's terrain. It is a method of the map object (called `CMap` by
  Flame [reference]) and takes (x, y, terrain id, player, flag, flag). Map tiles are 16
  bytes at map +0x14, indexed by y * width (map +0xC) + x. The tile's terrain index is
  at +0xA and its owner at +8 (low 12 bits); its room is at +2 (low 12 bits). It has 17
  call sites. [verified: the writes; the two flags' meanings unknown]
- It checks world +0x7239 bit 0x40 and does different work with it set. [verified: the
  branch; what each side does not read yet]
- Case 0x16 of the switch at 0x518B0B changes a tile from running code. It takes a
  terrain id, a player number and x, y. If the tile has a room, it first calls 0x4F0840,
  0x4C5DB0 and 0x4F09B0. For a terrain with bit 0x10 of terrain +0x17B set, it then
  adds a room with 0x4F0390 and 0x4C5C30. Good model for the editor's paint tool.
  [verified: the calls; what each does is inferred, and that this is the "change
  terrain" trigger action is a guess]
- Terrain data flags at +0x17B: bit 0x80 water, bit 0x100 lava (level load stores those
  terrain ids at world +0x9BAE and +0x9BAF). [verified: the stores; "water" and "lava"
  names from Flame, [reference]]

## Still to find
- Creating and removing Things at run time (Flame names 0x4B7900 creature, 0x4B7A90
  object, 0x4B7B50 door, 0x4B7C10 trap; [reference], not checked in our export).
- Field layouts of the Map, Players, Things, Triggers and Variables sections.
- The terrain ids a blank map needs, and which file sections the game insists on.
- Picking a tile from the mouse in the 3D view.
