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

## Todo (facts from the references, to verify)
- **Hero party spawn limit crash** [reference] The trigger that spawns a
  hero party: if making a creature fails (the world's 448 creature limit),
  it returned early, and when no leader was made the party finish code used
  a null leader. Flame finishes the party with what was made and skips the
  leader step when there is none. Likely more common with many Dwarfs.
  Flame: CWorld spawnWholeParty / finalizeSpawnParty.
- **Missing PNG** [reference] 0x59A4B0 can return no surface for a missing
  PNG, which crashes users; Flame substitutes a placeholder texture.
- **Host port field** [reference] The port number text input (button id
  0x212, configured in 0x52AA10) allows one character too few for a 5-digit
  port.
- **Sound thread stop spin** [reference] A sound thread destructor waits in
  a loop without yielding (Flame MySnd_ListThread::destructor).
- **Save path buffer overrun** [reference] Flame CGameComponent: building
  the save file path from a long name overflows a 64-byte buffer.
- **Closing the window** [reference] WM_CLOSE during a game should go
  through the game's exit action instead of ending abruptly.
- **Ember: uninitialized structure** [reference] Ember
  fix_usage_unitialized_structure.cpp (to read).
- **Ember: relative mouse data** [reference] Ember mouse_dinput
  fix_relative_mouse_data.cpp (to read).
- **Multiple network interfaces** [reference] LAN games pick the wrong
  local address with several network adapters (Flame multi_interface_fix).
- **Blocking threat response** [reference] Flame keeps a per-creature
  penalty so the computer does not keep picking a creature it cannot drop;
  needs extra creature data (maybe later).
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
  owner's (0x473920). Not found yet: where pit fighters are paired and why a
  neutral one does not hit back.
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
