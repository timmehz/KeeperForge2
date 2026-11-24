# Creature behavior

## States
- [verified] A creature's state block is at creature + 0x24E. Its current
  state is at creature + 0x25E (block + 0x10) and the state it goes into when
  it reaches where it walks to at creature + 0x32D (block + 0xDF). Both are
  state numbers into the same table (0x6DA8C0, 0x19-byte entries, filled at
  run time).
- [verified] 0x48AD30 sets the state: thiscall (creature, state), ret 4, on
  the block. 0x48ECE0 works out a new path: thiscall (creature).
- [verified] State 7 walks to a room. Arrival state 0x50 (80) is "make a
  home": 0x46F390 checks the creature has none, that its type has a lair
  object (creature data + 0x771), finds a room for it and walks there with
  arrival state 0x50 (unless it already has 0x50). [reference: Flame names
  80 "making a home"]

## Creatures with no bed
- [verified] A creature type's bed is the object in its creature data + 0x771
  (Creatures.kwd record + 0xF52, an Objects.kwd id). These have none (0):
  Imp, Prince Tristran, Skeleton, King Reginald, Prince Felix, Horny, Lord of
  the Land, Prince Balder, Unique Skeleton and Jack. The heroes have the Hero
  Creature Bed (6), the Fairy the Dark Elf Creature Bed (20).
- [verified] The usual way into making a home, 0x46F390, checks for a bed
  object. Two Lair state handlers do not. Both are thiscall (state block), with
  the creature at block + 8, and return 0 when there is nothing to do.
  - 0x46F4F0: a creature in a Lair with no bed of its own walks to a free spot
    with arrival state 0x50.
  - 0x46F6B0, that arrival: it makes the bed (0x4E03F0, then 0x48AD50 with the
    object from + 0x771). If that fails, it finds another spot (0x4E0140) and
    walks there with arrival 0x50 again.
  - A creature with no bed can never make one, so it went round that for ever.
    [user: the royals got stuck trying to make a bed]
- CreatureFixes hooks both. For a creature whose type has no bed object they
  return 0 at once, so it drops the job. [unverified: in game]

## Map changes reset walking creatures
- [verified] 0x4E8D30 (fastcall, player) goes through the player's creatures
  (first at player + 6, next at creature + 0x10, tags into 0x6EE540). For
  current states 3, 4 and 7, and 6 with arrival state 1, it works out a new
  path and sets state 1 (idle). It is called from three player functions
  that change the map (0x4C361D, 0x4C39D5, 0x4F0817).
- That also cancels a walk to make a home. With workers digging and claiming
  all the time, such a creature never arrives: it stops, turns and starts
  over (seen in game with a Lord of the Land from the Mercenary Portal).
  [reference: Flame fixes the same, keeping state 7 with 0x50]
- CreatureFixes.cpp replaces 0x4E8D30 with the same choice, except that a
  walk with arrival state 0x50 keeps going. [unverified: in game]

## Workers and untagged tiles
- [verified in the export] A player's worker task list is at player + 0x1105:
  0x24-byte records { x, y (shorts), kind byte at + 4 (1 dig, 2 gold), up to 12
  assigned creatures + 5..+ 0x1C, next + 0x1F, own index + 0x21, in use + 0x23 }.
  0x508070 (thiscall (list, index, kind) ret 8) says whether a task is still
  there; 0x507EF0 removes one (zeroes the record, frees it).
- [verified] Tag and untag: 0x5129A0 / 0x5129E0 call 0x512A20 with flag 1 / 0.
  Untagging removes the dig or gold task of every diggable tile (terrain flag
  + 0x17B & 8; gold when terrain + 0x185 is set) in the rectangle. Workers are
  not told.
- [verified] A worker's job: + 8 the creature, + 0x8A its task index, + 0x8C /
  + 0x90 the tile. The job search 0x469AF0 sends it walking with state 0xD
  (dig) or 0x19 (gold). Flame names the states [reference], the code agrees:
  0x4654F0 state 0xD, 0x4655D0 state 0xE (digging), 0x465790 0x19, 0x465870 0x1A
  (mining gold).
- [verified] The digging states return at once while the creature's action
  runs (0x496D00: creature + 0x3A9 set and not 0x15) and check the task only
  after it, so a worker already swinging kept digging an untagged tile.
  0x4647D0 (leaving a task) stops the action with 0x496D20 first.
- Fix (CreatureFixes): before states 0xE and 0x1A run, a running action whose
  task is gone is stopped; the state then sends the worker to idle (0xA)
  [unverified in game].

## Dig and gold task slots
- [verified] A dig or gold task's 12 creature slots are 3 on each of its 4 faces: 0x508110 (thiscall (list, task,
  face, slot) ret 0xC) reads one, 0x508160 (thiscall (list, task, face, slot, tag) ret 0x10) sets one. A worker's
  job (its state block, creature + 0x24E) keeps the task at + 0x8A, the face at + 0x94 and the slot at + 0x96.
- [verified] The job search 0x469AF0 starts at the face nearest the worker (0x4805F0 gives the face and a step of +1
  or -1) and goes round all 4. A face counts if the tile beside it is open (terrain flags + 0x17B bit 1 clear) and
  reachable (0x471630). On it the worker takes the first free slot (tag 0) at once, or else a slot whose holder is a
  live creature with a job (+ 0x252), not in state 0x33 or 0x34, that 0x477310(10) allows, and farther from that
  face than the worker is from the task's tile. Before taking a held slot the holder is sent to idle (state 10).
  Leaving a task (0x4647D0) clears the worker's own slot for dig (0xD, 0xE) and gold (0x19, 0x1A).
- So a slot whose holder is gone, has other work, or holds another place is neither free nor takeable, and that face
  keeps one worker fewer while the task lasts [unverified in game: how such a slot is left behind].
- Fix (CreatureFixes): the search's two slot reads (0x469D34 dig, 0x46A1B6 gold) go to our function, which clears a
  slot whose holder is not a creature whose job is that task, face and slot, and reports it free. The read just
  before taking a slot (0x46A38D) then sees 0, so no other creature is sent to idle [unverified in game].

## Payday: which Treasury or Heart
- [verified] 0x472390 (thiscall (state block), creature at block + 8) picks where a creature goes for its wage. It
  walks the owner's rooms (player + 0x36, next room + 8) for a Treasury (type 1) or a Dungeon Heart (type 5, room +
  0x9A bit 1 clear) with gold in it (room + 0x49), each at its middle tile (room + 0x2B, + 0x2C). A room counts when
  0x471630 says the creature can get there (flags 0x80: 0x4D0E60, the path search), and the nearest wins, measured
  by 0x43C260: the straight-line distance squared (/ 4096). Walls do not count, so a Treasury just behind a wall
  beat a Heart a short walk away. [user: creatures went to a Treasury rather than the closer Heart]
- [verified] The path search 0x4D0E10 (cdecl (result, start x, start y, end x, end y, -2), in 1/256 tile) fills a
  0x814-byte result: start and end, the number of waypoints at + 0x10 and the waypoints from + 0x14 as x, y pairs
  (0x4D0320, 0x4CEF40). 0x4D0E60 sets the creature's walking rules as globals and then calls it; the last search is
  kept and handed back for the same start and end.
- CreatureFixes patches the choice in place: the call to 0x43C260 (0x472446) calls our own measure, the length of
  the walk (the same path search, added up along its waypoints with an integer square root), and the game's second
  straight-line sum (0x472455) is replaced by keeping that length as the best. Which rooms count is unchanged.
  [unverified: in game]
- [verified] The other 19 calls of 0x43C260, one by one:
  - Choosing a room to walk to, right after the creature's own reachability check for that room passed (0x471630
    or 0x471740, flags 0x80, both through 0x4D0E60, so the path search's walking rules are that creature's):
    0x457010 (a Treasury or Heart holding more gold than the creature's + 0x3F4; 0x457107, 0x45711B), 0x464400 (a
    Treasury or Heart with room for more gold, for workers bringing gold; 0x46450C, then the same sum again inline at
    0x464515 like payday), and the rooms' own "nearest room of mine for this creature" methods 0x4D8870, 0x4DED00
    (Hatchery), 0x4E0460, 0x4E2500, 0x4F1A20, 0x4F2B80, 0x4F3560, 0x4F6200 (0x4D896C, 0x4DED9A, 0x4E0528, 0x4E25F0,
    0x4F1AEC, 0x4F2C42, 0x4F362C, 0x4F62E0). The calls in between (0x4E8210, 0x4D8430, 0x4F6140, 0x4E53B0) do not
    search paths themselves.
  - Left alone: 0x46C2D4 and 0x46C387 (0x46BD70, a Workshop: measured before any reachability check, so the search's
    rules could be another creature's), 0x4689B2 (0x468910, the search comes after), 0x46DBEF and 0x46DBFA (two
    things compared), 0x48C302 (an "already there" test), 0x43BB3E (a light) and 0x516BB2 (geometry).
- CreatureFixes points those 11 calls at its own measure: the same path search from the call's two places (no
  creature needed: the rules were just set), its length put on the game's scale (length² / 16 for 1/256 tile, the
  squared distance / 4096 of 12.12), the straight line when no path is found. 0x464515 keeps that measure as the best,
  as payday does. All or none: every call's bytes are checked before any is changed. [unverified: in game]
