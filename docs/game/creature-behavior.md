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
