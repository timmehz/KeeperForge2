# Playing the Good player in a game

What the game does differently for the Good player (player 1, the heroes),
and what Runtime/Source/GoodPlayer.cpp changes so a human can play it. How
the human gets there is in skirmish-lobby.md (Swap to Good Player).

## Player tick
- 0x4B9EC0 (thiscall (player)) is each player's tick. It returns early:
  - for the neutral player (tag at 0x6F2548), after 0x4BB130;
  - for the Good player (tag at 0x6F254C), after setting its mana
    (+0x82) to 0x7FFFFFFF. So the heroes never run out of mana. [verified]
- Everything after that is the keeper part (0x4BB2A0): imp generation at
  the heart, the player's timed action (0x4C0AA0), and more. [verified]
- The hand's pick up and drop are timed actions, so the Good player's hand
  never finished one (details in skirmish-lobby.md). GoodPlayer runs
  0x4C0AA0 after the tick for a Good player with an action set. [verified]

## Workers
- Player +0x1105 is the worker task list (digging, claiming, and so on;
  500 tasks of 0x24 bytes) [reference]. Player init skips it for players 1
  and 2 [verified]. Hero Dwarfs read it (a crash when it was missing showed
  this), so with a task list they use the worker jobs. [verified crash,
  behavior unverified]
- Creature type ids: 1 Imp, 14 Dwarf [reference]. The game uses 14 for
  the first member of a random hero party (0x51A24B). [verified]
- The creature's type is at creature +0x36F [reference]. Much creature
  code checks for type 1 directly, so Dwarfs may still act unlike Imps in
  some ways (combat, fleeing, pay). [verified: the checks exist]
- The Workers row of the creatures panel shows the type in interface
  +0x123. Interface setup (0x419180, thiscall (interface)) picks the first
  creature type whose data flags (+0x686) have both 0x80 and 1. The panel
  list (0x4185E0) puts that type in the Workers row and leaves it out of the
  other rows. [verified]
- GoodPlayer sets interface +0x123 to 14 after setup when the interface's
  player (tag at interface +8) is the Good player.

### Why Dwarfs did not dig (and the fix)
- Tagging works for the Good player: the tag action calls 0x512A20
  (thiscall (world, player, x1, y1, x2, y2, flag) ret 0x18) with no player
  check, and it adds dig (1) or gold (2) tasks to that player's +0x1105
  list. [verified]
- Creature states are in a table at 0x6DA8C0 (0x19-byte entries, set up
  around 0x47CD00). [verified]
- State 0x4773D0 chooses what a creature does next:
  - Good owner → state 0x5A (hero behavior);
  - neutral owner → state 5;
  - creature data flag 1 (worker) → state 0xA, the Imp idle (0x463DF0),
    which looks for jobs with 0x468520 (kind 1/2 = dig, 0x469AF0);
  - flag 0x4000 → state 0xEE;
  - otherwise state 0x59.
  So the Good player's workers never reached the worker check. [verified]
- Hero workers (flag 1) owned by Good tunnel through when they cannot find
  a path (0x485B50, from the movement helpers around 0x471890). [verified]
- Fix: the 9 bytes at 0x47759E (`cmp cx,[0x6F254C]; jnz`) jump to a stub.
  A Good-owned creature with flag 1, whose player this machine controls,
  continues at the worker check (0x4775EC). Other Good creatures go to
  0x4775A7, and everything else to 0x4775BE, as before.

## Create Imp -> Create Dwarf
- Keeper spell 4 is Create Imp: imp generation at the heart casts spell 4
  for free when there are too few imps. [verified]
- Spell data comes from world vtable +0x110 (thiscall (world, spell)). The
  world object is the pointer at 0x6E5054. [verified]
- Spell data [verified unless marked]:
  - +0x00 and +0x1B: two 0x1B-byte art entries, likely its icons
    [reference].
  - +0x42: the effect it makes.
  - +0x43 and +0x47: two values handed to the effect. For Create Imp, +0x43
    is the creature type to make. The cost check also counts creatures of
    type +0x43.
  - +0x64: name text id (the tooltip uses it). +0x66: likely the
    description id [unverified].
- Cast: 0x4BBD30, thiscall (player, spell, position, flag) ret 0xC. It
  creates the effect and copies +0x43/+0x47 into it (effect +0x13D/+0x141).
- An upgraded spell (spell entry +0xD set) uses +0x4B, +0x4C and +0x50
  instead of +0x42, +0x43 and +0x47 (0x4BBD30). [verified] The upgraded
  Create Imp made a level 4 Imp for the heroes (tested), so +0x4C is its
  creature type, and +0x50 is very likely the level.
- Price of Create Imp [verified]: base cost (spell data +0x58) times
  (the player's count of creature type +0x43, + 1, - the free number at
  0x6F28D7), at least 1. Worked out in the cast (0x4BBD97), in its
  affordability check (0x4BB8F9) and the same way in the other users of
  0x6F28D7 (the spell panel among them). All read +0x43, even for the
  upgraded spell.
- First version: GoodPlayer set +0x43/+0x4C to 14 for the whole cast. So the
  cast counted the player's Dwarfs while the panel counted Imps (none):
  Create Dwarf cost more than shown (tested: "costs the upgraded version's
  mana").
- Now: the cast is left alone until the effect is made. At 0x4BBE4B ("test
  al, al; jz", EDI = the type read from +0x43 or +0x4C) a stub puts 14 in
  EDI's low byte while a Good player or Dwarf-worker keeper casts spell 4.
  The price stays the one the panel shows: base cost while the player has
  no Imps. (The creature hook at 0x4B78D0 also turns their Imps into Dwarfs.)
- Tested: with a flat price, 150+ Dwarfs were made. Then the portals
  stopped, Create Dwarf took mana without making a Dwarf, and the Horny
  talisman did nothing. Cause [verified]: the creature maker 0x4B7900
  refuses once the world's live creature count (creature list +0x1E, up on
  each creature made and down at 0x4B7A20) reaches 0x1C0 = 448, across all
  players and the map's heroes (`cmp [edi+0x1E], 0x1C0` at 0x4B7945). The
  cast takes the mana before its effect (0x4AFE50) tries to make the
  creature.
- The game has no other creature limit for a keeper here. Player +0x10F6
  ("MaxCreatures" in the debug dump) is cleared and never read. The portal
  cap counts only creatures with +0x3E6 bit 1 ("ContributesToPortalLimit",
  set only for portal arrivals). [verified]
- Now, with keeper-dwarf-workers on, GoodPlayer sets spell 4's +0x43 and
  +0x4C to the Dwarf for the whole game (checked every player tick, put
  back when the option is off). All seven price sites (the panel, the
  affordability check 0x4BB8A0 and the cast among them) then count Dwarfs:
  Create Dwarf's price rises with each Dwarf, as Create Imp's does with each
  Imp. The cast stub above still covers a human playing Good without the
  option (unlimited mana).
- The seven price sums [verified, each behind "spell is 4" and the effect
  check]: 0x4BB8A0 (affordability), two more just after it, and 0x4BE560
  read the count from the player's per-type list (entry +9) and do "sub eax, ecx;
  inc eax" at 0x4BB930, 0x4BBAD0, 0x4BBC50, 0x4BE608; the cast (0x4BBD30)
  and two more call 0x4BA7D0 for the count and do "sub edx, ecx; add eax,
  edx" (EDX was 1) at 0x4BBDAF, 0x4BC01A, 0x4BC318. ECX is the free number
  0x6F28D7 in all. Then "cmp eax, 1; jge" / "mov eax, 1" and the multiply by
  +0x58. 0x6F28D7 is also the heart's free worker minimum (0x4BB47E, the
  top-up) and is read by the worker upkeep (0x4C15FE, 0x4C1C17), so it is not
  changed.
- create-imp-price-rises: off (a session option) makes the count part 1 at
  those seven places ("xor eax, eax; inc eax", nop for the fourth byte), so
  the panel, the check and the cast all take the base cost. Patched at each
  level start from the session value, put back when it is on. Covers Create
  Dwarf too (same spell). Not tested yet.
- Per-player spell list: player +0x6E is the count and +0x72 the entries,
  0x11 bytes each: spell id, state (+1), times (+5, +9), upgraded (+0xD).
  State 3 is available (research done and the all-spells cheat set it);
  2 is researchable; 1 is the default. 0x4BAFA0 reads the state, thiscall
  (player, spell) ret 4. 0x4BAF40 sets it, thiscall (player, spell, state)
  ret 8. [verified]
- GoodPlayer sets spell 4 to available for a Good player this machine
  controls (world +0x7243 equal to player +4).

## Texts
- 0x62D960, thiscall (table, id) ret 4: text by id. Each entry is a dword
  (type 1 in the low byte, length above it), the text, then a zero dword.
  It returns the text, or 0 if the type is not 1 or the trailing dword is not
  zero. [verified]
- Texts are in the game's multibyte format. 0x553B90 (cdecl (text, out,
  size)) converts to wide text. 0x553B20 (ConvertText) converts back. Both
  return 1 on success. [verified signatures, names [reference]]
- While the interface is the Good player's, GoodPlayer replaces Create
  Imp's name and description texts: Imp becomes Dwarf (as whole words,
  with plurals, and "an Imp" becomes "a Dwarf").

## Not done yet
- A Create Dwarf icon. The spell panel's handlers (around 0x4113B0) are not
  disassembled in our export, and the art entries need research.

## Rooms, doors, traps and the keeper tick
- Room building and removal (0x4C7380 "tickRoomPlacements" [reference],
  0x4C7160), research (0x4C3000), mana income (0x4C10E0) and the gold count
  are all after the Good player's early return in 0x4B9EC0, so the heroes
  could never build. [verified: the code order]
- The check is at 0x4B9F3A: `cmp ax,[0x6F254C]; jnz 0x4B9F57`, with EDI =
  player. 0x4B9F43 is the Good path (set mana to 0x7FFFFFFF, return).
  0x4B9F57 is the rest of the tick:
  - gold = treasuries (room type 1) plus heart, while player +0x132 == 0;
  - the computer player, 0x4BA0D0 (hatchery chickens), fireworks;
  - 0x4C3340 (manufacture countdown);
  - the keeper part 0x4BB2A0. [verified]
- GoodPlayer replaces those 9 bytes with a jump to a stub. A Good player this
  machine controls gets its mana set to 0x7FFFFFFF and goes on through the
  whole tick like a keeper; the mana is set again after the tick, since
  income caps it. Other Good players are unchanged. Gold follows keeper
  rules (the user chose this): the heroes have no heart, so they start
  with none until a Treasury holds some.
- Availability lists hold 9-byte entries {id, state int}:
  - rooms: count +0x56, list +0x5A, setter 0x4BAAD0;
  - +0x5E/+0x62 (setter 0x4BAC80);
  - +0x66/+0x6A (setter 0x4BADE0);
  - creatures +0x4E/+0x52 (setter 0x4BAA10).
  The map script's "make available" (0x518B0B area) uses 1 room, 2
  creature, 3 = +0x5E list, 4 = +0x66 list, 5 spell, with state 3 or 1.
  Doors are +0x5E and traps +0x66 by that order [unverified which is which].
  [verified]
- Research (0x4C3000) walks the map's research list (count at world
  dword 0x1CC0, entry {type, id} by 0x5070A0). Types are 1 room, 2 and 3
  the +0x5E/+0x66 lists, and 4 spell. It picks the first entry that is
  researchable (state 2) for the player. Only when none is left does it
  pick a spell that is available (3), not upgraded (+0xD == 0) and has an
  upgrade (spell data +0x4B), to upgrade it. With nothing left to research
  at all, it posts message 0x18 and the current research becomes 0.
  [verified]
- The research step (0x4C2E60 area) adds points, finishes the current
  subject (0x507040) and picks the next with 0x4C3000. The keeper part
  picks one when the current one is 0. [verified]
- First version (tested, wrong): every room/door/trap a keeper could have
  was set to 3 for the Good player, and spells were not copied. That left
  nothing researchable. Wizards stopped after one subject, and Create Imp
  (3, not upgraded) was upgraded straight away.
- Now: during the first 20 game turns of a level (game turn is world +4 ->
  +4, read by 0x517490), each room, door, trap and spell entry of the
  controlled Good player takes the highest state any keeper has for it
  (2 or 3), never lowering its own. That is after the map's start-up
  scripts, and too early for a keeper to have researched anything. So the
  heroes research what keepers research, in the map's order. Create Imp is
  always set to 3.
- Possible side effect: the keeper part's imp top-up casts Create Imp for
  free at the heart position (+0xB43/+0xB45) when the player has too few
  type-1 creatures. With no heart that cast should fail; if it succeeds, it
  makes a Dwarf (cast hook). [unverified]

## Hero jobs and destroyed hearts
- Creature states, using Flame's ids [reference]; my earlier table
  arithmetic was one entry off:
  - state 1 (0x4773D0) chooses what a creature does next (see the worker
    fix above);
  - Good creatures go to state 0x5A, the hero idle (0x477850);
  - other non-workers go to state 0x59.
- The hero idle (0x477850):
  - party followers go to state 0xDE (follow the leader);
  - everyone else runs its hero job: creature +0x409 through 0x478800,
    a switch over about 28 job ids;
  - a creature with no job, or whose job returns 0, goes to state 4
    (doing nothing). [verified]
- Hero jobs are set with (job +0x409, target player +0x40D, data +0x40F);
  the map's hero parties set them. [verified]
- When a Dungeon Heart object (object type 13) is destroyed, 0x4C0060 sets
  its owner's +0x132 to the killer's tag and +0xEE2 to the game turn.
  +0x132 != 0 is what the keeper tick treats as defeated. [verified]
- Tested problem: the human's Good creatures kept crowding a destroyed
  enemy heart. Fix: in the state 1 stub, a creature of the controlled Good
  player whose job targets a defeated player (+0x132 != 0) gets its job and
  target cleared, so it idles and wanders. Jobs against living players
  are kept.

## Keeper behavior for the Good player's creatures
- State 0x59 (0x4778E0) is the keeper creature idle: call to arms (player
  +0x76 bit 4), then needs and returning to the job the creature was
  dropped on. [verified, start of the routine read]
- In state 1, the 0x4775EC chain picks: flag 1 → 0xA (worker), flag
  0x4000 → 0xEE, otherwise 0x59. [verified]
- Tested problem: a Good Wizard dropped in a Library researched one subject,
  then stopped. After the job it went through state 1 to the hero idle,
  which has no job to return to, so it ended up doing nothing (state 4).
- Fix: for the controlled Good player, the state 1 stub sends to the hero
  idle only creatures with a hero order (+0x409 != 0, after dropping orders
  aimed at defeated players), or party followers (+0x46F bit 4) whose
  leader (+0x3DF) has one. Everyone else takes the keeper path (0x4775EC).
  So the heroes need lairs, food and pay like a keeper's creatures; the
  keeper tick now runs for that player, paydays included.
  [party flag meaning unverified]

## "Receive Imps" special
- 0x4A4980, thiscall (special object, player) ret 4 (from the special
  switch near 0x4A4194). [verified]
  - It picks the first creature type, in world-vtable order (+0xAC first,
    +0xB0 next, +0xC0 data), whose data flags +0x686 have bit 1 (the Imp).
  - It makes DAT_006F2972 of them with 0x4B78D0 in a 2-tile ring around
    the player's heart tile (player +0xB43/+0xB45, read through 0x4C5F50).
  [verified]
- Crash (tested): the heroes have no heart, so the position is the default
  (1,1), the ring goes off the map, and a tile lookup returns nothing
  (0x45495E reading terrain data +0x17B through null).
- Fix: for the controlled Good player, the hook moves the heart tile to the
  special's own tile (thing +0x1A/+0x1E >> 12, kept 3 tiles inside the map;
  map size from world vtable +0x1CC/+0x1D0). For the call, it also hides
  the worker flag on every type but the Dwarf, so the ring is made of
  Dwarfs. Both are put back after the call.
- The keeper part's imp top-up (0x4BB2A0) also works from the heart tile;
  it has not crashed in testing. [unverified why]

## Dwarf workers for keepers (keeper-dwarf-workers)
- With `keeper-dwarf-workers: on` (a session option), every keeper (player
  number 3 and up) gets the same Dwarf worker changes as the Good player:
  - Create Imp makes Dwarfs (cast hook);
  - the Workers row shows Dwarfs, and Create Imp is renamed (screen only);
  - Receive Imps gives Dwarfs, at the heart as usual.
  The Good player keeps its own rules either way.
- Keeper-owned Dwarfs already take worker jobs: the "what next" state sends
  non-Good creatures with the worker flag to the Imp idle. [verified]
- Heart top-up: the keeper part makes free workers while the Imp count
  (the player's per-type list, player +0x4E/+0x52, 0xD-byte entries with
  the count at +9) is below the minimum (byte 0x6F28D7). The count is read
  at 0x4BB479 (`mov eax,[eax+9]; xor ecx,ecx`, ESI = player). [verified]
  A stub adds the Dwarf count for players with Dwarf workers; otherwise
  the heart would keep making Dwarfs forever.
- Creature creation: 0x4B78D0, thiscall (creature list, type, owner tag,
  position, out) ret 0x10, wraps 0x4B7900 (slot, type, owner, position,
  out) ret 0x14. 0x4B7900 also takes an alias type from data +0x5B3 when
  data +0x686 has 0x8000000, and stores the type at creature +0x36F.
  [verified] The saved-game loader (0x4B66F9, "deserializeTag"
  [reference]) calls 0x4B7900 directly.
- For a player with Dwarf workers, the 0x4B78D0 hook makes any Imp
  (type 1) a Dwarf (14). That covers the map's starting Imps and anything
  made later, and saved games are left alone. (Earlier version: map Imps
  stayed Imps.)

## Dropping Dwarf workers (heart, portals)
- [verified] The drop rule for creatures is 0x450910 (world vtable +0x230,
  via 0x509310). For a worker type (data +0x686 bit 1):
  - with bit 0x80 (the Imp): not onto Portal (room 3) or Mercenary Portal
    (0x19) tiles of kind 4, and the heart (room 5) has no extra check;
  - without it: on the heart, 0x4DA150 must pass, which refuses the middle
    3x3 of the 5x5 heart.
- [verified] 0x459180 (a state of a creature that landed): a worker (bit 1
  only) in the heart's middle, near the heart object (thing kind 13), is
  taken away, and the owner gets Create Imp's base cost (spell 4 data +0x58)
  back as mana.
- [verified] Bit 0x80 is also read when the interface picks the Imp type
  (first type with 0x80 and 1: still the Imp, type 1 comes first) and by
  0x4E8C60 (every 32 turns, a group mixing 0x80 and other creatures gets an
  order with state 5).
- So with keeper-dwarf-workers on, GoodPlayer gives the Dwarf type bit 0x80
  (checked every player tick, put back when the option is off): Dwarfs can
  be dropped into the heart to be taken away (mana back) and cannot be
  dropped into a Portal or Mercenary Portal, as Imps. [unverified: in game]
- [verified] The portal's creature cap counts the owner's creatures with
  +0x3E6 bit 2 (loops at 0x4DBE3C and 0x4DC0B8), and only the portal's
  creature maker 0x4DC9A0 sets bit 2. Dwarfs from Create Dwarf, Receive
  Imps or the heart top-up do not count toward it.
