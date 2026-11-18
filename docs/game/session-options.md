# Session options (settings every player must share)

Some KeeperForge2 settings change the game simulation, so every player in a
game must use the same value or the game goes out of sync.
Runtime/Source/SessionOptions.cpp handles them: in multiplayer the host's
value is used, otherwise this machine's config.yml.

| Option | config.yml | Flag |
|---|---|---|
| MercenaryPortalNormal | `mercenary-portal-spawns: normal` (default mercenary) | 0x01 |
| MercenaryPortalOff | `mercenary-portal-spawns: off` | 0x02 |
| KeeperDwarfWorkers | `keeper-dwarf-workers: on` (default off) | 0x04 |
| StoneKnightsOff | `mercenary-portal-stone-knights: off` (default on) | 0x08 |
| RoyalsOff | `mercenary-portal-royals: off` (default on) | 0x10 |
| CreateImpFlatPrice | `create-imp-price-rises: off` (default on) | 0x20 |

The rooms for each hero (`mercenary-portal-*-room-ids`) are session values too. See
[mercenary-portal.md](mercenary-portal.md).

## Play mode
- GameCfg is at 0x75AD47 [reference, layout fits our export]. Its play mode
  is +4 = 0x75AD4B: 1 campaign, 2 skirmish, 3 multiplayer.
- These values are inferred from use: the mana regeneration option only
  applies in 3; fog and gold density apply in 2 and 3. [verified use]

## Lobby packed options
- The lobby packs its game options into front end +0x6026..+0x6029.
  0x549D10 (thiscall (front end)) writes them from GameCfg, and 0x549F60
  reads them back into it. [verified]
- Bits:
  - +0x6026: bit 0 = 0x75AE93, bits 1-7 = 0x75AEA9;
  - +0x6027: bits 1-7 = 0x75AEA3;
  - +0x6028: bit 0 = fog (0x75AE8B), bits 1-2 = mana regeneration
    (0x75AEA7), bits 3-7 = 0x75AE7F;
  - +0x6029: bits 0-1 = 0x75AE9F, bits 2-3 = gold density (0x75AEA5),
    bits 4-7 = the map's number of players: written on map select (0x53FC40)
    and at 0x545E40, read by Add Computer Player (0x545FC0).
  [verified]
- Our options used to ride in +0x6029 bits 4 and 5, wrongly thought free. That
  changed the map's player count, which could block Add Computer Player, and a
  map's count could read back as our options. They moved in the entry below.

## Where our values travel: the lobby's creature list [verified]
- Each row is 5 bytes, { type, count, 3 bytes }. Level setup (its loop over
  0x75AEBB) only uses the type and count. The lobby's slider writes
  only the count. The read-back (0x549F60) and the host's message copy all 5 bytes.
- The host's lobby message (type 199, built at 0x5435E0, read at 0x5432xx)
  holds the whole list, so the free bytes reach every player.
- First row, byte +2: the flags above plus 0x80 "written". The write hook
  (0x549D10) sets them from this machine's config, unless this machine has joined
  someone else's lobby. The read hook (0x549F60) keeps them as the session's values.
- Rows 1 to 4 (the game's own creature rows), bytes 2-4, hold the host's mods fingerprint, KeeperForge2 version and
  build hash, and bit 0x40 of the first row's byte +2 says they are there (ModCheck.cpp, docs/api/mods.md).
- In multiplayer, SessionOptions::Get returns the session's value once one has
  been read, otherwise this machine's. Each machine logs "Session options: ...
  (the host's)." at level start.

## Level start
- 0x50C800 (thiscall (world)) sets up the world at each level start.
  SessionOptions hooks it, logs the values for the game and runs the
  features' level-start callbacks (the Mercenary Portal clears its royal
  list there).

## Lobby options window (window 0x23) [verified]
- Window config 0x6B4438 (id byte 0x23, button list at +0x3E = 0x6B4480,
  0x50-byte entries ending with id -1). Tab +0x603B on the front end
  (0 = Creatures); 0x54A2C0 shows each tab's buttons through + 0x61.
- Creatures tab, 8 rows (y 76 apart in the 2560x1920 layout):
  - sliders 0x27E..0x285 (kind 3, x 824, w 412). Range from config + 0x16 /
    + 0x1A: 0 to 32 (the slider's configure 0x528EF0 copies them to
    + 0x84 / + 0x88; value + 0x8C; vtable + 0x14 sets it, clamped; + 0x28
    reads it). Each has its own tick (0x5496B0, 0x549770, ... 0x549C40,
    cdecl (slider, front end)): it formats the value into the 4-byte text
    at front end + 0x3137 + 4 * row, points the number's + 0x80 at it, and,
    while the number is shown and the value changed (per-row cache from
    0x6B5A10), writes it into the creature list entry (scroll bar 0x296's
    value + row) count. The game's own check of the number's + 0x61 means
    ticks also run for hidden buttons [inferred].
  - numbers 0x286..0x28D (kind 7, x 1228, w 156, h 80), drawn by 0x54A710
    (not disassembled in our export), cdecl (button, front end);
  - toggles 0x28E..0x295 (kind 0) at x 1080 for the other tabs, hidden on
    Creatures.
  - 0x54A540 (thiscall (front end, top row, rows)) fills the sliders and
    numbers from the list when the top row changes (cached at + 0x3133).
- 0x532170, thiscall (front end, id, window id) ret 8: a live button by id.
- Live button fields (copied from its config by the button setup): + 0x24
  tick (config + 0x34), + 0x28 renderer (config + 0x38), + 0x4D / + 0x51
  click handlers, + 0x61 shown, + 0x73 kind, + 0x74 id, + 0x78 / + 0x7C
  previous / next. Flame's names for these are 4 lower [reference].
- The game's window procedure sends WM_CHAR, as a wide character, to
  0x5B2C10 (cdecl (message, char)), which queues it for text input.

## Typing the creature counts (CreatureCountInput.cpp)
- The window's button list is copied: each row's slider tick is wrapped,
  each number's renderer is wrapped, and 8 click areas (ids 9100..9107,
  copies of the first toggle with the number's place and size, drawing
  nothing) are added. The wrapped slider tick shows the click area only
  while its number is shown.
- Clicking a number starts typing into it: every typed character then goes
  to it (0x5B2C10 hooked), so only digits count; up to 2 digits (a third
  starts again), Backspace removes one, Enter or Escape ends it, as does any
  mouse press or the number being hidden (tab change, window closed). Each
  change moves the slider (vtable + 0x14) to the number clamped to 0..32;
  the game's own slider tick then writes the count and shows the number.
- While typing, the number blinks (its text is swapped for an empty one
  every other 400 ms around the game's renderer).
- Not tested yet. Escape may also close the window through the game's own
  keys [unverified].
