# Possession

What a possessed creature's actions do in the game, and what PossessionActions (Runtime) adds on top. Everything below
is from our export of DKII-DX.exe unless tagged otherwise.

## The possessed creature's attack
- [verified] 0x514830 (thiscall (world, action), ret 4) runs the possessed creature's attacks for a player's action. The
  action's player tag is at + 0x10; the player's possessed creature at player + 0x134. Data1 to data3 pack the
  creature's position (low 20, 20 and 14 bits) and its view angles (bits 20 and up of data1 and data2, bits 14 and up of
  data3, 11 bits each); data3's top 4 bits are the ability slot.
- [verified] Data3 bit 0x8000000 is a melee hit. Otherwise slots 0 to 2 cast the creature's abilities and slot 3 is
  Group: a creature in front starts following (0x461980, below) and the possessed creature's follower count
  (+ 0x488, 32 bit) goes up, to at most the global at 0x6F29D0.
- [verified] A melee hit happens only when the melee is ready (0x48C9B0, thiscall (creature, creature data) ret 4: the
  time since the last hit, creature + 0x427 in game turns, against its speed). It then stores the turn at + 0x427 and
  asks the world what is in front: world vtable + 0x334 (0x515420), thiscall (world, creature, position, angles), ret
  0xC, a tag. That lookup only searches the nine tiles around the creature, with no side effects.
- [verified] The search (0x515420) keeps a thing only when 0x516050 says so (thiscall (world, creature, thing,
  flag), ret 0xC): creatures, and of the objects only types 1 to 3 (gold, when the creature can carry more), 9, 14, 15,
  16, 46 and 64 (94 too when another keeper's), each also in reach (0x5162E0, thiscall (world, creature, thing), ret 8:
  within 0x80 of the creature's facing and as close as their two sizes allow). So beds, the Training Target and the
  Prison Door are never what a possessed creature hits. [user: bed, Prison door and training did nothing in game]
- [verified] The interface asks the same search for what to highlight (0x40BE21 and 0x40FCEA, kept at interface
  + 0x10BC), so the filter decides the highlight too.
- [verified] Nothing in front: a creature that can dig hits the wall (dig or claim). An object (thing kind 2, type at
  + 0x12C): gold (types 1 to 3) is picked up; a Chicken (9) is eaten only when the creature is hungry (hunger at
  creature + 0x29A, the state block's + 0x4C, not 0) or a Bile Demon, otherwise the chicken just hops; a Training
  Dummy (14) only plays its hit animation; Anvil, Steel Bucket and Work Bench (15, 16, 46) do their own effect. Anything
  else, creatures and other objects alike (beds, the Training Target, the Prison Door), gets the melee attack action
  (0x496700 with 0x11).

## Experience
- [verified] 0x457880 (thiscall (creature, amount), ret 4) adds experience and levels the creature up when it reaches
  the next level's mark (data + 0x6B5, scaled by level). 0x4858B0 (same arguments) caps the amount at that mark first.
- [verified] The Training Room's handler (0x47B2D0, a state handler, so every game turn; first thought per second): nothing at or above the Training Room's highest level
  (byte at 0x6F28FC); else the type's training experience (creature data + 0x6B9, 16 bit) at level 1, or that times the
  global settings' percent table (settings pointer 0x6E4154, + 0x1EF + level * 2, signed 16 bit) / 100 at other levels,
  kept between 1 and 0xFFFE, through 0x4858B0.
- [verified] The same handler charges for training once a second (state block + 0xB3 counts the turns down from the
  byte at 0x6F2590): the cost is the signed 16 bit global at 0x6F291D, stored negative. With less gold than that (player
  + 0x7E) the keeper gets event 0x37 (0x4C4600 on world + 0x2E54) and training stops; otherwise 0x4BA420 (thiscall
  (player, amount, direct, tile x, tile y), ret 0x10; a negative amount with direct 0 comes out of the keeper's
  Treasuries and Heart) takes it. Every 4 seconds (state block + 0xB4) 0x50FB70 (thiscall (world, owner tag, position,
  amount, 0), ret 0x10) shows the 4 seconds' gold over the creature.
- [verified] Training creatures hit the dummy with action 0x1E and shoot at the target with 0x1F (0x4855B0).
- [verified] A shot (+ 0x145 the tag of what cast it, + 0x151 not 0 when cast by a possessed creature): in its flight
  (0x4ABxxx) one cast by a possessed creature searches the things near it for a Training Target (0x451570 with the test
  0x4B5980: an object of type 29) and, when there is one, makes an effect at the shot through world vtable + 0x38C
  (the call at 0x4ABE54). That is all: no experience.

## Sleep
- [verified] Creature data + 0x5D6 is the sleep length and + 0x5D2 the time awake (32 bit each, game turns). When the
  time awake has passed since the last sleep (state block + 0x54), the creature's sleep left (state block + 0x58,
  creature + 0x2A6) is set to the sleep length; sleeping counts it down (0x477B20).
- [verified] The bed object's tag is at creature + 0x3B7.
- [verified] 0x46F440 (thiscall (state block)) sends a creature to bed: not a prisoner (+ 0x3CB is 0), its type sleeps
  (+ 0x5D6 not 0), it has a bed and sleep left; it walks to the bed's room with arrival state 0x51. If it cannot get
  there, its keeper gets event 0x2B.

## Leaving a creature
- [verified] 0x514590 (thiscall (world, action), ret 4) is the action handler for leaving: it reads only the action's
  player tag. For a creature it lets go of a creature hypnotised through it, plays a sound, clears the possessed flag
  (0x48DEA0 with 0: creature + 0x3E6 bit 0x80, state 1 idle), puts a frozen creature in state 0xDA, then tells the
  player (0x4BE090).

## Followers
- [verified] A follower's leader (the possessed creature's tag) is at creature + 0x482. 0x461980 (thiscall (state
  block, leader tag), ret 4) starts following: state 0xF6, + 0x482 set, + 0x47E the turn; the caller adds one to the
  leader's + 0x488. A follower's death takes one off (in the creature's death code).

## Prison
- [verified] The Prison's door is an object of type 109 (Prison Door) with a bar, type 116, among the room's things
  (room + 0xE the first tag, a thing's next at + 0x12A). Room fields: + 0x51 and + 0x55 the door's tile, + 0x59 the
  bar lifted, + 0x5D the door open (32 bit each), + 0x61 the countdown to closing (byte).
- [verified] 0x4E29E0: an Imp of the room's owner on the tile just outside the door (y + 1) lifts the bar (animation
  0x2E) and opens the door (0x4E35C0, thiscall (room): opens or closes it by + 0x5D, animations 0x2C and 0x2D, sounds),
  and sets the countdown to twice the byte at 0x6F2590 (the game uses it as turns per second). The room's tick
  (0x4E2AC0) counts it down while the door is not animating and closes the door at 0.
- [verified] State 0x71 (handler 0x473130): a creature on a Prison tile inside the room (0x454110) becomes a prisoner:
  state 0x73, + 0x3CB the room owner's tag, event 0x3C.

## Combat Pit
- [verified] State 0x89 (handler 0x457A70): a creature on a Combat Pit's floor (0x4545C0 gives 0x400) gets the pit
  fighter bit (+ 0x3E6 bit 0x8000), state 0x8B, and + 0x3CB the room owner's tag.
- [verified] State 0x8B (0x457CF0) waits up to 4 seconds; state 0x8A (0x457B40) pairs: below the pit's highest level
  (the byte at 0x6F2976) and not fighting (+ 0x502 is 0), the room finds an opponent (0x4D8100) and each gets the other
  as attack target (0x48F390: + 0x3F9 the tag, + 0x3E6 bit 4 to attack a creature it otherwise would not) and the
  fighter bit. 0x48F3F0 clears the target and stops the fight (0x45EF50). Pit fighters never flee (0x45EC80, 0x45EDA0).
- [verified] At no health (0x48B770) a creature is knocked out (state 0xD8, + 0x3EA bit 0x400), unless it is King
  Reginald, Stone Knight or Lord of the Land (types 21, 28, 29) or has + 0x3EA bit 0x800: those die at once. A pit fighter's death goes
  through 0x4581F0 as well.
- [unverified] Whether the possessed creature's melee damages a pit fighter: pit fighters count as not attackable
  (0x45A8B0) for the creatures' own fights.

## What PossessionActions does
Hooks 0x516050: an object it turns down is kept when within 1.5 tiles across (not 0x5162E0, which wants the two all but
touching: [user: bed and Prison door still not hit with it]; a Prison door also within a tile of its tile's nearest
edge, as 1.5 tiles from its place was never reached in game) and it is the Training Target, the possessed
creature's own bed (a type that sleeps), or a door of its keeper's Prison; that also highlights them.
Hooks the search itself (0x515420): when it finds nothing, the bolt (116, the game calls it "Prison Bolt"; the door,
109, was never highlighted in game, so only the bolt is aimed at now) of the possessed creature's keeper's Prison is given, if within a tile of its tile or 1.5 tiles of itself and
within 0x155 of where the creature looks (angle by 0x65FB54, cdecl (dx, dy), table based), the nearest first. Needed
because the door sits in the Prison's fence: in game it came "in reach" (a tile or more away) but the search's later
facing and view-line tests (0x516950, 0x516670) never chose it [verified in game: log]. A hit on either opens the door.
Hooks 0x514830 and, for a melee hit that will happen (melee ready), asks for the thing in front first:
- Chicken: hunger 0 (and not a Bile Demon) is made 1 so the game's own eating runs. [unverified in game]
- Training Dummy or Target: after the game's hit, the keeper pays the Training Room's cost for those turns (shown over
  the creature as the room shows it; with too little gold, event 0x37 and nothing gained), then the Training Room's amount
  for the turns since the creature's last
  training hit, at least one second's and at most three seconds' worth. [verified in game: the first version gave one
  turn's worth, 3 a hit for a level 1 creature, far too little to notice]
- Its own bed (creature + 0x3B7, type that sleeps): leaves the creature (0x514590), sets sleep left to the sleep length
  and calls 0x46F440. The game's hit does not run. [verified in game]
- The Prison Bolt (or door) of its keeper's Prison: opens the door as for an Imp, or, when the room already counts it
  open, starts its closing countdown (an open door with no countdown never closes); the game's hit does not run. After each world tick,
  while the door is open, the possessed creature is inside and a follower is not, the countdown is kept at its start
  (for at most 30 seconds). When the door closes with the possessed creature inside, it is left and it and its
  followers inside get state 0x71. [unverified in game]
- Combat Pit: after each world tick, a possessed creature in its keeper's pit starts a match. Followers inside get
  state 0x89 and stop following; every fighter, the possessed one too, gets + 0x3EA bit 0x800 (its own value is put
  back afterwards). A lone fighter in state 0x8A with no target gets the possessed creature as target. When the
  possessed creature leaves the room or is left, survivors in the room have their target cleared, the fighter bit and
  + 0x3CB cleared, and follow again (or go idle). [unverified in game]
- Not kept in saves: a match or Prison visit running when the game is saved is not there after loading (fighters
  keep bit 0x800).
- Ranged: the effect call at 0x4ABE54 goes through our stub first, which gives the caster (a possessed creature) the
  same one second of experience, once a shot (shots already rewarded are remembered until they are gone).
  [unverified in game: whether the search finds the target once or on several turns of the shot's flight]
- The visit: once the possessed creature is inside the Prison and no follower is still on the way (or after 30
  seconds), the door has 3 seconds to close by the room's own countdown; if it is still open it is closed with 0x4E35C0,
  and the prisoners are taken. [unverified in game]
- Imprisoning: through state 0x71 alone nobody became a prisoner in game [verified in game: log "closed on creature
  447 and 1 follower(s)" and no prisoners]. Either 0x454110's tile test (tile + 0xC must be -1) or the state change's
  own rules (0x476D30 returns 0 when its tables refuse a switch) stopped it. Now 0x71 and then 0x73 are set and the
  holder written directly, each result logged, and each creature's state logged again a second later. [unverified]
- [verified] Why the switch to 0x73 was refused: 0x71 is a passing state (its group byte, state entry + 7, is 2), so
  0x476D30 parks the old state and moving on to 0x73 (group 0xE) runs that group's enter handler (group table
  0x699680, 0x40 a group, enter at + 0xC: 0x47B740). It asks the room under the creature to take it: 0x4E7F80 (its
  owner's room, or a Prison, Torture Chamber, Temple, Combat Pit or type 0x1A, always yes), then 0x4E5A70, which for a
  Prison wants the tile to pass 0x454110 (tile + 0xC is -1: not the fence ring or the door tile) and then room for the
  creature's size (data + 0x672, 0x4E53B0). In game the creature and its follower were just inside the door when it
  closed, so on the edge [verified in game: log, 0x73 refused].
- Now "inside" means on such a tile: the door is held open (30 seconds at most) while the possessed creature is in the
  Prison but not yet past its edge, or a follower is not; only creatures on such tiles are taken. [unverified]
- The door opened by the bolt waits up to 10 seconds for the possessed creature to walk in (the room's own countdown is
  2 seconds, which closed it first in game [verified in game: log]); each end of a visit is logged with its reason.

## Crossing the doorway and the pit's edge (second design) [unverified in game]
- In game the possessed creature could not walk into the Prison even with its door open (an invisible wall where the
  door is) nor off the Combat Pit's edge [user]. The pit's floor is its inner tiles: 0x4545C0 gives 0x400 there and
  0x1000 on normal floor (the inner test is 0x454110, as for a Prison's cells) [verified in the export].
- So creatures are put across: the game's move (0x4B5560, which relinks the thing into its tile) at the new tile's
  floor height (0x4545C0), speed across zeroed, a new path (0x48ECE0). Walking somewhere: 0x471890 (thiscall (state
  block, position, state on arrival, walking rules), ret 0xC; state 6 while walking). "Ahead" uses the game's direction
  tables (0x6C1488 and 0x6C1C88, 16.16 by angle, x the first, y the second's negative) and the creature's heading
  (+ 0xF4), so every machine works out the same.
- Prison: the bolt opens the door and arms the doorway for 30 seconds (after a second's grace). A possessed creature
  within three quarters of a tile of the door's tile (or of the door) is put on the cell tile nearest the door and made
  a prisoner (0x71, 0x73, holder), and possession ends. Its followers walk to the spot where it touched (sent again when
  they stop) and each that gets there is put in the same way; they have 30 seconds.
- Combat Pit: on the pit's edge (a pit tile that is not floor), facing the floor (the tile three quarters of a tile
  ahead) for half a second jumps the possessed creature to that floor tile's middle. A match then runs while it is on
  the floor. Followers in the pit's room (following, they stop at its edge) or at the spot it jumped from are put on
  the nearest floor tile and enter as pit fighters. On the floor, facing a non-floor tile for a second climbs it out
  there; the match ends and the survivors are put beside it and follow it.
- Combat Pit, third design (the user: the pit worked for the possessed creature, not for its followers; leaving
  possession set everyone in the pit idle; once in the pit it should no longer be possessed): jumping in now ends
  possession, and the creature is put on the floor as a pit fighter by the game's own entry (state 0x89, which pairs
  fighters, 0x457B40). Its followers walk to the spot it jumped from, as the Prison's do, and each that gets there or
  reaches the pit's room is put on the nearest floor tile the same way (30 seconds). The fight is the game's own; there
  is no climbing out and nothing is set idle. Fighters brought in die rather than being knocked out while in the pit and
  get their own setting back when lifted out. [verified in game: followers reach the spot and fight; four died, none
  knocked out]
- Ranged training (the user: melee hits on the Training Target work; ranged were not even highlighted): the ranged
  abilities aim through their own search, world vtable + 0x338 (0x515520, thiscall (world, creature, position, angles,
  flag), ret 0x10), which takes creatures and a few objects but never the Training Target; with no target the shot
  just flies along the view and never reached the target's check (0x4ABDF6). That search is hooked: when it finds
  nothing, a Training Target in one of the keeper's Training Rooms within 10 tiles and 0xAA of the view is given (the
  nearest the line of aim), so it is highlighted and the shot is aimed at it. [unverified in game]
- Confirmed in game [user]: training hits take the Training Room's gold (shown over the creature); a hungry creature
  fed chickens in possession was no longer hungry. Not seen yet: the "not enough gold" message.
