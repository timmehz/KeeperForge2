# Game fixes (bugs in DK2 itself)

The Flame and Ember projects fix many of the game's own bugs. Each one is
taken here as a fact, verified in our export of DKII-DX.exe, and written from
scratch (GameFixes.cpp, CreatureFixes.cpp). Status per fix:
- **done**: implemented (each still needs an in-game test unless marked);
- **todo**: fact noted, not yet verified/implemented;
- **covered**: already handled by another KeeperForge2 feature;
- **skip**: a feature or option rather than a bug fix, or not applicable.

## Done
- **Crash loading art (pixel converter reads past a row)** [verified code and
  crash log] The game's pixel converter 0x5FC460 (thiscall (destination,
  source, count) ret 0xC, called through 0x5D2860) walks a row from its last
  pixel back to its first and reads each source pixel as 4 bytes. For a
  3-byte-per-pixel PNG the last pixel's read takes one byte past the row.
  The PNG body reader (0x5B6560 area, called from the art loader 0x5540B0)
  converts each decoded row from a buffer of exactly width * 3 bytes, made by
  the game's allocator 0x5B75F0 (cdecl (size), 0 for size 0, else the C
  runtime's malloc 0x637BB0). When that block ends at the last byte of a
  memory page the read faults. Seen as an access violation at 0x5FCB80 with
  ESI 3 bytes before a page end, count 64 (a 64-pixel-wide picture), right
  after a screen mode change. 0x5B75F0 is hooked to ask for 16 more bytes
  for every block; its blocks are freed with free (0x6341B0), which does not
  care. Widescreen's engine heap already pads its own blocks for the same
  converter.
- **Screen capture keys (shared keyboard)** [verified code, not tested] Every
  DirectInput device's wanted cooperative flags start at 5 (exclusive and
  foreground, 0x5DCB40); only a full screen mode change (0x5BC280, event 7)
  asks for 6 (shared) for the keyboard (input system + 0x58) and 5 for the
  mouse (+ 0x5C), or 10 for both windowed. An exclusive keyboard keeps keys
  from other programs while the game is in front, which fits the user's report:
  Lightshot and other capture tools did nothing while the game was focused, and
  worked when it was not. 0x5DCBE0 (thiscall (device part, int* result) ret 4,
  called only by 0x5DCCD0, which then acquires) hands the flags to DirectInput;
  it is hooked to turn exclusive into shared for the keyboard only. The mouse is
  left as it is. [unverified] That this was the whole cause.
- **Stuck modifier keys** [verified code] 0x5B2A50 returns the modifiers held
  (1 Shift, 2 Ctrl, 4 Alt) from the game's own keyboard state. It now only
  counts a modifier Windows also says is held. The log says the first time this
  drops one. Replaces the Shift, Ctrl and Alt part of the todo "Keyboard state
  after alt-tab" below.
- **Side panel at level start** [verified code] The tab click 0x416C20 (cdecl
  (button id, panel, interface)): with Shift held it pins the panel (count at
  interface + 0x5F, one place per panel at + 0x60..+ 0x63), and 0x417310 then
  gives every open panel an equal share of the side panel's width. Level setup
  (0x419B15) calls it with button id 0 for Creatures (panel 4), so Shift seen at
  that moment pinned Creatures for the level. Setup now clears the pins and
  ignores the modifiers. Shift + click still pins by choice.
- **Workshop build time** [verified] 0x506B50 (done?) and 0x506B70
  (percent) read the build time of item 0 of the manufacture list instead of
  the item being made: the list is a ring of 0x80 items of 0x10 bytes,
  current index at list + 0x801 (0x506930 reads that item, 0x506970 moves
  on), count at + 0x803, build time a word at item + 0. Both are replaced to
  use the current item. [reference: Flame workshop_manufacture_build_time_fix]
- **Computer player threat response** [verified] In 0x4FE0B0 the computer
  drops a creature at the threat only when the threat's tile is not its own
  (`jz` at 0x4FE5AA past the drop when the owner equals the player), but a
  keeper can only drop on its own tiles, so it never defended. The jump is
  inverted. [reference: Flame response_to_threat_fix; Flame says 1.5.1 has
  it the right way]
- **Computer threat total overflow** [verified code] (written, not tested in
  game) 0x4F9B90 and 0x4FA000 (computer player events) work out threat =
  enemy threat * cp +0x4DA / 100 - own threat, as 32 bit, then store it with a
  16 bit write into the response slot (cp + slot * 8 + 0x5A). The sums come
  from 0x4FAC40: per creature, type data +0x698 (threat; Creatures.kwd +0xF12,
  e.g. Black Knight 213, Dark Angel 280, Horny 560) times the level's
  multiplier (DAT_006E4154 + 0x105 + level * 2) / 100. Above 32767 the slot
  wraps: negative means "no threat, pull back" in the response code 0x4FE0B0,
  a small positive means drop one or two creatures. Fits "huge armies treated
  as a few". Fixed: each of the five 16 bit stores of a total (0x4F9F1E,
  0x4F9F85, 0x4FA37A, 0x4FA3B6, and 0x4FEF82, an attack task's own army
  threat / 2) jumps to a stub that clamps to -32768..32767 and keeps registers
  and flags. Stores of 0 (0x4F9F54, 0x4F9FB6, 0x4FA38B, 0x4FA3BD) are left.
  0x4FE0B0 adds or subtracts one creature's threat at a time as 16 bit, which
  cannot wrap from a clamped value. [unverified: the level multipliers' values,
  i.e. how big an army must be]
- **Backstab and North/South combat** [verified code] (one bug; written, not
  tested in game) A melee hit's bonus damage is worked out at
  0x4990C4..0x49913F (in the creature's deal-damage function at 0x498E30, a
  vtable entry, not decompiled in our export; ESI the target creature, EDI
  the attacker). The angle comes from 0x65FB54 (cdecl (dx, dy), 0..0x7FF)
  with dx, dy = attacker +0x1A/+0x1E minus target's: 0 for -y, 0x200 for +x,
  0x400 for +y. Values above 0x3FF are folded (0x7FE - a), then / 20 gives
  0..51, and above 15 the damage gets that many percent more. The target's
  facing was never used, so an attacker on the +y side got up to +51%, one on
  the -y side none, east and west about +25% (+y taken as south [unverified:
  map orientation]). A creature's facing is + 0xF4 (Flame's +0xF0 is the same
  field; its offsets are 4 lower than ours): 0x4B2C10 turns it toward an angle
  that callers take from 0x65FB54(target - self) (e.g. 0x46F94C), so facing and
  this angle share one convention [verified]. Fixed: at 0x4990DB ("and eax,
  0xFFFF; add esp, 8", 8 bytes) a stub subtracts the target's facing, so the
  angle is 0 when the target faces the attacker and 0x400 from straight
  behind; the game's fold and bonus then run as before.
  [reference: Flame backstab_fix]
- **Network wait spin** [verified] 0x61C090 only returns [this + 0xC] & 2
  and is polled in a loop; it now yields the processor (SwitchToThread)
  first. [reference: Flame while_without_syscall_fix]
- **Exit list overflow** [verified] The C runtime start-up 0x634860
  allocates 0x80 bytes (32 entries) for exit functions; it is made 0x200
  (128). Only when the launcher loads KeeperForge2 with the game.
  [reference: Flame buffer_overrun_fix]
- **Chat box crash** [verified] The game's text input subclasses its window
  (SetWindowLongA at 0x430D0E with 0x430F40, the previous procedure kept at
  + 0x4C) and calls the previous procedure directly at 0x43033D (`call eax`,
  message 0x410) and 0x4305E4 (`call esi`). When the window was subclassed
  before by something else, Windows returns a handle (0xFFFFxxxx) meant for
  CallWindowProc. Tested: clicking the multiplayer chat box crashed with
  EIP = 0xFFFF0877, called from 0x4305E6 (with Steam in the process). Both
  calls now go through CallWindowProcA (jumps at 0x43032C, 19 bytes, and
  0x4305DD, 9 bytes, to stubs that push the same arguments). Our own find,
  not from Flame/Ember.
- **Making a home** (CreatureFixes) see creature-behavior.md.
- **Room objects depend on build order** [verified code] (written, not tested
  in game) Every change to a room clears its own objects (0x4E6BC0) and places
  them again for the whole room (0x4EDFC0, thiscall (world + 0x9BE, room)
  ret 4). Types 4, 9, 10, 11, 13, 14, 15 and 26 go to their own placer, which
  walks the room's tile chain (build order) and takes the first tiles that fit.
  A Hatchery (0x4DE220) wants size / 6 pens (at most 36), on middle tiles
  (0x454110) without one, each at least sqrt(3) tiles from the pens placed so
  far. On a 5x5 only the corners of the inner 3x3 give 4; a chain starting with
  its top middle and bottom middle gives 2 (seen in game, 2026-11-13). The rest
  of 0x4EDFC0 (one object per middle tile) is seeded by the tile's x and y, so
  order does not matter there. Fix: 0x4EDFC0 is hooked and, for those types,
  the chain is put in row order (top to bottom, left to right: what one drag
  builds) first. A room already built keeps its objects until it next changes
  (a tile added or sold).
- **Chickens hatching outside the Hatchery** [verified code] (written, not
  tested in game) The Hatchery lays an egg (object type 0x2F) on a random pen
  (0x4DE620, from the room's tick 0x4DE0A0), 0x800 above the floor, in object
  state 0x18 (0x4A21C0). That state destroys the egg if it is not on a pen
  tile (0x4DEA50). At 4 seconds old it throws the egg off the pen, at the
  pen's facing give or take a random 0x80 (speed 0x6000 across, 0x3000 up).
  Once its speed drops below 0x1000 the egg goes to state 0x20 (0x4A1CF0).
  That state makes a chicken (type 9) at the egg's position with the egg's
  owner, and nothing checks the egg is still in the room. A throw from a pen
  near the room's edge, or a bounce, could leave it outside, so the chicken
  came out there. Fix: the egg stops at the room's edge. 0x4A21C0 is hooked,
  and after each of its turns (while the egg sits and while it rolls) an egg
  that is not on a tile of a Hatchery its owner owns is moved (0x4B5560, which
  relinks it into the tile's list) to the nearest point inside such a tile
  within 5 tiles, an eighth of a tile in from its edge. It is never put below
  that tile's floor (0x4545C0). Its speed across (+ 0x104, + 0x108) is set to
  0, and its upward speed (+ 0x10C) is kept. The game's own speed check then
  hatches it there. 0x4A1CF0 is hooked too and does the same check once more
  just before the chicken is made. Ties go to the first tile in row order.
  If there is none (the room was sold meanwhile), the game's own behavior is
  kept. The fix only reads game state, so multiplayer stays in step. Our own
  find, not from Flame/Ember.
- **Chickens walking out of the Hatchery** [verified code] (written, not
  tested in game) [user: a bunch walked out of the Hatchery] An object's turn
  (0x49BD20, object vtable + 0x14) runs its state (0x49EED0) and then moves it
  (0x4B2030). A chicken's wander state (0x4A24A0) turns it at random (0x4B2C10)
  and looks at the room under it only when its walk ends (its own Hatchery:
  state 6, a Prison: 3, elsewhere: 0xA), so nothing kept it on the room's
  tiles while it walked. Fix: 0x49BD20 is hooked; a chicken that starts its
  turn on a Hatchery tile of its owner and ends it off one is put back where
  it was (0x4B5560), its speed across set to 0 and its heading (+ 0xF4) turned
  half round. Chickens already outside are left alone. Our own find.
- **Videos frozen after switching away** [verified code] (written, not tested
  in game) While a video plays, PlayMovie (0x53BCD0) adds 0x53C2E0 to the game's
  focus handlers (list at 0x757D71). 0x558C70 calls them on WM_ACTIVATE,
  WM_ACTIVATEAPP and WM_NCACTIVATE, with event 0 when focus comes (low word of
  wParam not 0) and 1 when it goes; 2 is a screen mode change. The video's
  handler swapped the player between playing and paused (0x5A8FF0: state at
  player + 0x9C, 3 and 4, and its sound buffer) on 0 and 1 alike, so only an even
  count kept it playing. Windows does not always send the same set (minimize and
  restore, clicking back in), and background running hands the held focus-loss
  messages over only as focus comes back, so a video could stay paused on its
  last frame for good. Fix: 0x53C2E0 is hooked; on 0 it plays the video (player
  at 0x73F594) and on 1 pauses it, each only when it is not so already and only
  between those two states; any other event goes to the game's handler. Our own
  find, not from Flame/Ember.
- **Portal creatures missing from a level's pool** (CreatureFixes; written,
  not tested in game) A portal only brings a type whose pool number is above
  0 (0x506400). The pool (player + 0x1109) holds one entry per creature type
  (its debug dump says NumberOfCreatureTypes), so a type the level leaves out
  is 0. Only Variables kind 1 sets it (handler 0x51C9C0; skirmish and
  multiplayer refill it from the lobby list when the play mode is 2 or 3).
  The portal choice (0x4DBF90) never reads availability. The trigger action
  "make available" (action type 6, 0x518AD0: byte 0 player, 1 category, 2
  type, 3 on) sets availability only (0x4BAA10 creatures, 0x4BAAD0 rooms,
  stored as given: 3 on, 1 off; the level's own values 4, 3 or 1) [verified].
  A survey of the stock levels [verified: level files]: Level15a, 15b and 17
  make the Temple available to player 3 by trigger, Level18, 19 and 20 have it
  in their Variables, and none gives player 3 Dark Angels; Level15a, 15b, 18,
  19 and 20 also have the Combat Pit with no Black Knights; Level17's rivals
  (4 and 5) get Dark Angels made available and the Temple, with no pool entry.
  Other gaps (Training Room without Salamanders, Casino without Rogues, from
  the early levels on) look like the campaign's pacing and are left alone.
  Fix: at the start of every portal choice (through MercenaryPortal's hook of
  0x4DBF90), in campaign play only (play mode 1, or a campaign level played
  together): at a keeper's first choice in a level, a Dark Angel or Black
  Knight number of 0 with none owned marks that type as never set; once the
  keeper's Temple or Combat Pit state is 2 or more (researchable or
  available), the type's number is set to the largest number in that
  keeper's pool (20 if none) and a line is logged. After loading a saved game
  a type whose number had run down to 0 with none alive would be given its
  number again.
- **Possessing a sleeping creature** [verified code] (CreatureFixes; written,
  not tested in game) 0x45A8B0 (fastcall (creature)) sets whether a creature
  can be attacked (+ 0x3EA bit 0x40). One test (0x45A93F) says no when the
  creature's state is a sleep-type one (state table 0x6DA8C0, 0x19-byte
  entries, flags at + 8 bit 4). Possessing a creature leaves its state as it
  was, so a possessed sleeper was ignored by enemies and most traps. A
  creature's + 0x3E6 bit 0x80 is "possessed" (the game's own debug line for it
  says so, in its creature dump). Fixed: that test jumps to a stub that skips it for a
  possessed creature. [reference: Flame sleeping_possession_fix]
- **Wooden Bridge burning** [verified code] (written, not tested in game)
  Attaching a tile to a room (0x450770) sets the tile's burn level to 0
  (0x452B70: tile + 0xB bits 1-4), and every change to a room attaches its
  tiles again, so selling or losing one tile of a burning bridge put the rest
  out. Fixed: at 0x4507B5 a stub skips that for rooms of type 8 (Wooden
  Bridge; room tag is the third argument), and a bridge tile being built
  (below, the build call) has its burn level set to 0 first.
  [reference: Flame wooden_bridge_burn_fix]
- **Room limit crash** [verified code] (written, not tested in game) A player
  may have 96 rooms (player + 0x10F9 counts them). Building (call 0x4C5CC5 to
  0x4ED1A0) and selling (call 0x4C5E3B to 0x4ED440) pass "96 - count" as a
  byte. Selling one tile splits its room: 0x4ED7B0 attaches the other tiles
  again one by one through 0x4ED320, which may make a new room only while
  rooms are left; with none left a tile that cannot join a neighboring room
  is attached to no room, and the failure is ignored. Such tiles crash the
  game later [unverified: where]. Fixed: both calls go to our functions; a
  sale is allowed at least 3 more rooms (one removed tile leaves at most 4
  pieces, the room itself counts as one), and since the count can then pass
  96 (making "96 - count" wrap to a large number), building with the count
  over 96 is given 0. The world's room array holds 96 * 7 [reference: Flame];
  going over needs every keeper near the limit. [reference: Flame raises the
  limit (override_max_room_count) and sells the whole room when under 4 are
  left; ours keeps 96]
- **Drop from hand** [verified code] (written, not tested in game) The
  interface checks the drop rules (0x40D8F0, thiscall (interface, thing, tile
  x, tile y) ret 0xC, reading only the interface's player tag + 8 and world
  + 0x53) on the machine that clicked; the drop action (0x4C0EE0) runs later
  on every machine and does not check again. Fixed: the action's call of the
  player's drop (0x4C0F70 to 0x4BC710) goes to our function, which asks the
  same rules with a stand-in interface for the last thing in the hand and, if
  they say no, keeps it in the hand. CoopHands hooks 0x4C0EE0 itself, so this
  is the call inside it. [reference: Flame drop_thing_from_hand_fix]

- **Dungeon Heart "dying" warning** [verified code] (written, not tested in
  game) The Heart room's tick (0x4D9A60, raw bytes in our export, decoded by
  hand) finds the heart object among the room's things (type 0xD at + 0x12C),
  works out health (+ 0x40) * 100 / the maximum (0x6F294B) and raises event
  0x27 at exactly 50 and 0x28 at exactly 10 (0x4D9ACB..0x4D9AFD, through the
  event list 0x4C4600), every tick, whether the Heart is being hit or healing.
  Fixed: the compare at 0x4D9ACB jumps to a stub that allows a warning only
  on a tick where the Heart's health is lower than when the tick last saw it
  (sightings older than 1000 turns, or from a later turn, do not count).

- **Imps per tile face** (CreatureFixes; written, not tested in game) see
  creature-behavior.md, "Dig and gold task slots": slots held by a creature
  no longer working them are freed when a worker looks for work.

- **Chat message read past its end** [verified code] (written, not tested in
  game) A received chat message (0x409E30, cdecl (message, interface)) is
  copied into the last of the 3 history lines with a fixed 0x102 bytes
  (0x409E8F: message + 4 to interface + 0x1E37), reading past the end of a
  shorter message. A history line is the sender (16 bits, a player's tag:
  0x409EC0, thiscall (interface, sender, text) ret 8, writes it at + 0x1E37
  and the text at + 0x1E39). Fixed: those 18 bytes jump to a stub that copies
  the sender and then the text (16-bit characters ended by 0) up to its end,
  at most 0x102 bytes, and clears the rest of the line. The local path
  (0x409EC0) copies from the game's own full-size buffer and is left. [reference: Flame
  fix_chat_buffer_invalid_memory_access]
- **Keys held after alt-tab** [verified code] (written, not tested in game)
  The game's key state is one byte per DirectInput key code from + 0x1C of the
  object at input system (0x79CF90) + 0x70 (0x5BB280 reads it). Keys let go
  while another program had the keyboard stayed down for the game. Now
  cleared when the window becomes active again (WM_ACTIVATEAPP, our window
  procedure in DirectDrawLayer). The Shift/Ctrl/Alt fix above stays.
  [reference: Flame fix_keyboard_state_on_alt_tab] The crash on switching
  away in multiplayer was the old display's lost surfaces, covered by our
  renderer (see Covered).
- **Hero party spawn at the creature limit** [verified code] (written, not
  tested in game) The hero party trigger action (type 0xE) runs 0x519F90
  (thiscall (world, party index, action point, invasion flag) ret 0xC). It
  makes up to 16 members one by one through 0x519B20, which returns 0 when no
  creature can be made (the world is full). On 0 it returned at once
  (0x51A274 to 0x51A756), so the members already made never got their leader
  links, speed or party count. The finish code after the loop (0x51A5E2) finds
  the leader (creature + 0x46F bit 2) and uses it twice: an effect at its
  position (0x51A66A, through thing 0 when there is none) and its place in the
  party (0x51A73A, writing through a null pointer when there is none). Fixed:
  a failed member jumps to the finish code, and both leader uses are skipped
  when no leader was made, so the spawn returns 1 with a smaller party.
  Followers of a leaderless party get leader tag 0, as in Flame [unverified:
  how they behave]. [reference: Flame hero_party_spawn_limit_fix]
- **Missing picture** [verified code] (written, not tested in game)
  0x59A4B0 (cdecl (name)) loads a PNG through the bridge object's method
  + 0x11C and returns a 40-byte picture record (four color masks, bits per
  pixel, a bytes-per-pixel flag, pixels, width, height, row pitch), or 0 when
  the file is missing. Its only caller (0x5912D3) reads the width and height
  from it at once and copies its pixels (0x5914E0), so a missing file crashed
  the game; the record is not freed there. Fixed: 0x59A4B0 is hooked and
  returns a shared 64x64, 32-bit placeholder (dark gray and purple squares)
  instead of 0, with a warning in the log naming the file.
  [reference: Flame null_surf_fix]
- **Room tile with no room** [verified code] (written, not tested in game)
  0x443610 (thiscall (bridge, x, y, tile, terrain data, mesh arrays) ret
  0x14, called at 0x43E3E0 for room terrain) builds a room tile's meshes. It
  takes the tile's room like 0x4559B0 does (ThingTable[(tile + 2) & 0xFFF])
  and reads the room's data (+ 0x19) without a check, so a tile marked as room
  terrain with no room crashed the game when drawn. This may be where the
  roomless tiles from the room limit (above) crashed [unverified]. Fixed:
  hooked; a tile with no room draws no room meshes. [reference: Flame
  CBridge room check]
- **Long save path** [verified code] (written, not tested in game) Loading a
  saved game (in the game component's loop 0x525370, 0x52555E..0x5255AF)
  copies the save's name (wide, game config 0x75AD47 + 0x90) into a
  64-character buffer, turns it into the code page's text in a 64-byte one
  (0x5557E0, WideCharToMultiByte), then writes "%s%s" of the saves folder
  (0x758754: the game's folder followed by data\Save\ [reference: Flame]) and
  that name into a 64-byte buffer ([esp + 0x114])
  with no limit, and hands that to the save loader (0x50E920, at 0x5255CD).
  A Steam install's saves folder alone is close to 64 characters, so the path
  ran into the buffers after it, and past about 190 characters into the
  function's exception frame. Nothing else in the function reads those
  buffers. Fixed: the block jumps to a stub that builds the path (same code
  page conversion) into a 1024-byte buffer of ours, logging when it is cut,
  and the loader is given that buffer. [reference: Flame buffer_overrun_fix,
  save path part]
- **Sound thread stop wait** [verified code] (written, not tested in game)
  Stopping the sound list thread (0x60D630, called from its destructor
  0x60D5C0; thiscall (thread object)) sets + 0x39 (exit) and + 0x3C (wake,
  InterlockedExchange), then waits for + 0x38 (stopped) at 0x60D64A in a loop
  that never gives up the processor. With the game's threads on one processor
  (patch.dll, see cpu-threads.md) that loop can keep the sound thread from
  running, hanging the game as it closes [unverified: seen]. Fixed: the 14
  bytes jump to a stub that calls SwitchToThread on each pass.
  [reference: Flame while_without_syscall_fix, sound thread part]
- **Closing the window during a game** [verified code] (written, not tested
  in game) The game's window procedure (0x5B5070) answers WM_CLOSE (Alt+F4,
  the close button) by calling 0x5B2E50(1), the quit flag, so a running game
  dropped out mid-level without its own exit. The in-game "exit to Windows"
  confirmation (0x4205B0, raw bytes in our export) instead sends an action of
  kind 0x7D with the interface's player tag (+ 8) at + 0x10, through the
  interface's push 0x4346E0 (thiscall (interface, action) ret 4: counts it at
  + 0xA, sends it through the session at + 4 with 0x527130). Fixed: our
  window procedure (DirectDrawLayer) sends that same action when the
  interface's session is set (a game is running) and does not pass WM_CLOSE
  on; in the menus the game closes as before.
  [reference: Flame fix_close_window]
- **Host port box** [verified code] (written, not tested in game) A text
  input's configure (0x52AA10, thiscall (button, config) ret 4) sets its
  limit (button + 0x280) from the config's + 0x1A (11 or 16 by keyboard layout
  when that is -1). Typing (0x52C40E) takes a character only while the length
  (+ 0x1DC of the edit state) is below limit - 1, so the limit counts the
  closing 0. The TCP/IP menu's port box (button id 0x212, + 0x74) has a limit
  of 5 [reference: Flame menu data], so a 5-digit port could not be typed.
  Fixed: 0x52AA10 is hooked; after it, the port box's limit gets one more.
  [reference: Flame max_host_port_number_fix]
- **Why a game cannot be joined** [verified code] (written, not tested in
  game) The multiplayer session list (0x54D370, raw bytes in our export,
  disassembled with ml + dumpbin) walks the found sessions (count 0x73FE74,
  array 0x73FE78, 0xA4 bytes each: + 0 version as major << 16 | minor, + 0x28
  players, + 0x40 map info (+ 0x40 bits 0..6 computer players, + 0x41 name
  length, + 0x42 the map's players, + 0x44 name hash, + 0x48 file check),
  + 0x64 name). Each line is name, "#players / max#", map name and
  "#major.minor", then a separator. A session is joinable (front end + 0x602B
  + index) when players < max, its version equals ours (0x6BC430, 0x6BC434),
  its file check equals ours (0x7962A0) and max > 1; the map lookup (0x53ED60,
  1 found, else the "unknown map" text) does not count. Nothing told the
  player why a game was greyed out. Fixed (two stubs in 0x54D370): after the
  version text is made (0x54D548), "#reason" is added to it (map not found,
  game is full, version x.y, different game files, one player map; the
  game's order, last wins), so it is counted in the line's size; after the
  joinable byte (0x54D58C) a session whose map is missing is made not
  joinable, as Flame does. [unverified: how the list shows the line's fifth
  field] [reference: Flame display_incompatible_reason]
- **Start-up errors** [verified code] (written, not tested in game) The
  game's start-up (0x5A5DA0, cdecl (argc, argv), called once at 0x64780B)
  makes the mutex "DKII MUTEX" (0x5BAE70: CreateMutexA; byte + 0 set on
  ERROR_ALREADY_EXISTS) and stops at once when another copy has it. Then
  each of these calls ends it with no message when it returns 0: the command
  line (0x5A63B0 at 0x5A5FAC), fonts and texts (0x552F10 at 0x5A5FBC), the
  window (0x557CB0 at 0x5A60DB), speech (0x567F90 at 0x5A6187, with sound
  only), the network library (0x559BE0 at 0x5A6199), the Windows check
  (0x557FB0 at 0x5A61D0) and the components (0x552BE0 at 0x5A61F8). Fixed:
  each of those calls goes to a wrapper that logs the first failing step
  (ErrorAndContinue); the start-up's own call goes to a wrapper that checks
  for the mutex first and, after the start-up, shows a message box: "already
  running", or which step failed. Only works when the launcher starts the
  runtime before the game's code. patch.dll sends the command line call
  (rel32 at 0x5A5FAD) to its own parser [verified in our patch.dll export
  and in game: the first build refused it as "unexpected game code"], so a
  step's call is only required to be a "call rel32" and its wrapper calls
  whatever it led to. [reference: Flame print_game_start_errors,
  notify_another_instance_is_running]
- **Windows version check** [verified code] (written, not tested in game)
  The start-up's Windows check (0x557FB0) is "push 0; push 0; push 6; call
  0x557FD0; neg eax; sbb eax, eax; inc eax": it passes only when Windows is
  NOT at least 6.0 (0x557FD0, stdcall (major, minor, service pack), compares
  with GetVersionExA). So the game only started on Windows before Vista,
  unless Windows reported an older version to it [unverified: what does that
  on current installs; patch.dll does not]. Fixed: the "push 6" (0x557FB4)
  becomes "push 11"; Windows 11 still reports 10.0, so every current Windows
  passes. [reference: Flame modern_windows_support]
- **Computer player stuck on one creature** [verified code] (written, not
  tested in game) In the threat response (0x4FE0B0), each pass picks the
  first creature of the keeper's list (keeper at computer player + 0xE,
  creature count at keeper + 0x16) that passes its tests; every test that
  fails goes to 0x4FE50D, the last one ("not a worker", 0x4FE504) picks it
  (0x4FE522). Then 0x48E6A0 (creature, keeper tag) says whether it is the
  keeper's; when not (0x4FE544 to 0x4FE914) nothing is dropped, and the next
  pass picks the same creature again, so the computer can spend its whole
  response on it. Fixed: such a creature gets a penalty of (creatures owned
  - 1); each later pick skips it and counts it down (kept to at most
  creatures owned - 1). Kept by creature tag in our own table, cleared at
  each level start (SessionOptions' level start, from the world's set-up)
  [unverified: whether that also runs when a saved game is loaded].
  [reference: Flame blocking_response_to_threat_fix]
- **King and Stone Knight show nothing in the hand** [verified: stock
  Creatures.kwd; code] (written, not tested in game; our own find) In the
  stock creature list, animation 11 (In Hand, record + 0x20 + 0x54 * 11) is
  empty for King Reginald (21) and the Stone Knight (28); every other hero
  has a "-Swing" mesh there. Nobody could pick them up before, but with the
  Mercenary Portal a keeper can own one, and the hand then showed nothing
  (seen in game with the King). Fixed: the record converter 0x568E30
  (thiscall (type, record) ret 4, its one caller the list reader at
  0x56A41A) is hooked; a record with an empty In Hand animation gets its
  Stunned pose (animation 10, "King-Stunned_pose") copied there before it
  is converted. Works for the shared list and levels' own lists.
- **Title screen wait** [verified code] (written, not tested in game) The
  title screen (0x5340F0) shows "TitleScreen\TitleScreen" and then waits
  10 seconds (0x5341D8..0x5341F6: the game's millisecond clock 0x5B5E90 + 10000,
  then a loop that only reads the clock again, no messages, no sleep).
  Changed: the loop jumps to a stub that waits `title_screen_ms`
  (config.yml, default 600, Flame's) sleeping between checks, and Space,
  Escape, Enter or a left click ends it; after a skip it waits (at most 1 s)
  for the key to be let go, so the click does not land on the main menu.
  [reference: Flame skippable_title_screen]

## Todo (facts from the references, to verify)
- **Ember: uninitialized structure** [reference] Ember
  fix_usage_unitialized_structure.cpp (to read).
- **Ember: relative mouse data** [reference] Ember mouse_dinput
  fix_relative_mouse_data.cpp (to read).
- **Multiple network interfaces** [reference] LAN games pick the wrong
  local address with several network adapters (Flame multi_interface_fix).
- **Movie clip icons (All Change, Chicken and Hyde swapped)** [verified
  mechanism] The Movies menu's clips come from a table at 0x6B0DD0: 22
  entries of 9 bytes { clip number, title text id (int), unlocked (int) }.
  0x53B750 picks each clip's picture from its number ("Mov_Shots_M_%02d_0"
  with number - 1; 0 Intro, 0x14 and 0x15 Outro), 0x53B480 shows the title
  from the text id, and 0x53BBxx plays "CutSceneLevel<number>.tgq" (0x14
  Outro). Title ids in order from clip 1: 0x589, 0x59F, 0x59B, 0x5A5, 0x594,
  0x592, 0x596, 0x598, 0x5A1, 0x597, 0x599, 0x58B, 0x586, 0x59D, 0x588, 0x58C,
  0x58D, 0x5A3, 0x58E (Intro 0x4D, Outro 0xB1B). So picture and film follow
  the number and only the title can disagree with them, or the pictures
  themselves (FrontEnd.WAD) are in the wrong place. Not fixed: which two clips
  are meant is not known yet (Text.str is compressed past its code page, so
  the titles were not read here).

- **Dungeon Heart icon flashing after a load** [unverified] Not found yet.
  Ruled out: the side alert icons (0x423BD0) flash while their event's flag
  (+ 0x27) is set, and the event tick (0x4C41E0) clears it after 200 ticks and
  ends the event at its duration, so an event alone cannot flash for ever;
  the disassembled part of the Heart tick (from 0x4D9C2A) is the Heart's beat
  and destruction. Left: which icon it is (the nav button "gui\nav\Dungeon_Heart", the "gui\windows\heart-alarm" window, or an alert icon).

- **Skirmish: Dark Angels and Black Knights not coming** [verified: stock
  data, not tested in game] Nothing found in the stock maps. In skirmish and
  multiplayer the pools come from the lobby's creature list, which is built
  from the shared Creatures.kwd (0x548B70: types with Available Via Portal,
  0x400, and without 1 or 0x8004100); Black Knight (22) and Dark Angel (23)
  have it, with the Combat Pit and Temple as their rooms. Every stock
  skirmish and multiplayer map lets each of its keepers have both rooms
  (Variables, 3 or 4), and Furnace_hero's own creature list has the same two
  records. Left: how the lobby sets each creature's starting count (its list
  is made all zero, 0x5488A0) and maps from outside the game folder.

- **Neutral creatures in the Combat Pit** [unverified: cause] Neutral
  creatures may be dropped into a Prison, Torture Chamber or Combat Pit as in
  the original game (a drop rule refusing those rooms was written, then taken
  out at the user's choice). Wanted: in the Combat Pit they fight back, so
  they cannot be farmed for experience. Found so far [verified]: a creature
  landing in a pit's middle gets creature + 0x3E6 bit 0x8000 (pit fighter),
  + 0x3CB the tag of the player whose room holds it, and state 0x8B
  (0x457A70); state 0x8B's handler (0x457CF0, set in the state table at
  0x47E0C0) only waits and calls 0x478B80(5, ..., 9); creatures with bit
  0x8000 count as not attackable in 0x45A8B0, so pit fights are paired
  somewhere else. A neutral creature dropped in a Prison becomes the room
  owner's (0x473920). Pairing found since [verified]: state 0x8A (0x457B40)
  asks the room for an opponent (0x4D8100) and gives each the other as attack
  target (0x48F390), see possession.md. Not found yet: why a neutral one does
  not hit back.
- Combat Pit log (CombatPitLog.cpp, log_level debug or trace only): hooks the
  state setter (0x476D30, thiscall (state block, state, value) ret 8; the
  block's + 8 is its creature) and the melee hit (0x498E30, thiscall
  (creature); target tag at + 0x399), and writes at Debug level every state
  change of a creature in a Combat Pit or with the fighter bit, and every
  hit where either side is one (owner, state, fighter bit, holder tag).

## Covered
- Engine heap growth and big resolutions (Widescreen), the GUI tile table
  (Widescreen), patch.dll's display wrapper (Renderer), window focus and
  minimizing with our renderers (DirectDrawLayer).

## Skip (features or options)
- Flame: disable_bonus_damage, increase_zoom_level, use_wasd_by_default,
  skippable_title_screen, control_windowed_mode, show_wireframe,
  use_wheel_to_zoom, override_max_room_count (experimental room limit),
  welcome window, alternative resources, logging/inspect tools.
