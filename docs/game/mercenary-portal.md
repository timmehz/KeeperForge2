# Mercenary Portal

A room that, once claimed, brings creatures into your dungeon the way a
Portal does. Placed with the level editor. Runtime/Source/MercenaryPortal.cpp
sets who comes.

## Room types
- 0x18 is "MercenaryGate" in the game's room dump strings (0x4E7BF2): no
  fields of its own, and no attraction code was found for it. [verified]
- 0x19 is "HeroPortal" in the dump. It is treated like the keeper Portal
  (room type 3) in its entrance tile setup (0x4E4560 area), the portal
  count (0x4DCE30) and the creature choice below. [verified]
- Assumed to be the editor's "Mercenary Portal". [unverified naming]

## The portal room tick (0x4DBBE0)
- It is not disassembled in our export (raw bytes; decoded by hand). Two
  room-type entries in the game's data point at it (0x6A1F2F and
  0x6A22B5, 0x386 bytes apart), likely Portal (3) and Mercenary Portal
  (0x19). [verified bytes; which types inferred] Ember's reference map
  lists its two calls into 0x4DBF90 (0x4DBC6C, 0x4DBCC2) [reference];
  the bytes match.
- It skips portals owned by the neutral player (not claimed) and portals
  whose owner is defeated (+0x132).
- The next arrival is due at room +0x49 (last arrival turn) + (n * n +
  DAT_006F2550) * DAT_006F2590, with n = room +0x4D (arrivals so far).
  When due it calls 0x4DBF90 with flag 1 (make the creature); otherwise
  with flag 0. [verified bytes]
- The room size used by requirements is room +0xA7, summed over the
  player's rooms (list from player +0x36, next at room +8; 0x4C6EB0).
  [verified]

## The game's creature choice (0x4DBF90, thiscall (portal room, flag) ret 4)
Run for each Portal and Mercenary Portal. The owner is the room's player
(room +0x1D tag, room type at +0x18).

For each creature type (world vtable +0xB8 by index, data +0xC0):
1. It must be in the owner's creature pool (player +0x1109, 0x506400:
   5-byte entries {type, int}, as the map sets them), and data flags
   +0x686 must not have 0x8000000. [verified]
2. Keeper Portal: data +0x686 & 0x400. Mercenary Portal: data +0x68A & 8,
   tested at 0x4DC064 (`test byte [eax+0x68A],8; jz` → 0x4DC474 next
   type; on to 0x4DC071, the room type 0x19 check). [verified]
3. Its requirements are checked by 0x4DC810 [verified]:
   - the attraction list is data +0x5DA, entries of 3 bytes {value,
     amount, kind}, count at data +0x788, room for 2 entries [reference];
   - kind 1: room type `value` with total size >= `amount` (0x4C6EB0);
   - kind 2: gold >= value; kind 3: mana >= value.
   No entries means always met.
4. There is a cap on portal arrivals (creatures with +0x3E6 bit 2) of
   (portals - 1) * DAT_006F28B4 high byte + its low byte. [verified]
5. Weight:
   - if the first attraction entry's room type is 2 (Lair), a fixed weight
     (0x1000, or 0x3000 when data +0x771 == 0x16);
   - otherwise the total size of the player's rooms of that type, minus
     one per creature of this type the player already has;
   - then scaled by the player's per-type entrance values (player +0xB57
     table: 46 entries of 0x12 bytes, multipliers at +6 and +0xE).
   [verified]
6. One type is picked at random by weight (the game's own synced RNG,
   0x5558D0) and made by 0x4DC9A0. [verified]

Why "nothing came out" (tested on a custom map, setting mercenary): heroes
have no attraction entries, so their weight is 0 (no room type). The game's
mercenaries also need to be in the player's creature pool. [verified
mechanism, cause inferred]

## What KeeperForge2 changes (Mercenary Portal only)
- `mercenary-portal-spawns: off`: the portal's tick runs untouched (the game's
  own rules), and the lobby does not list the heroes.
- `mercenary-portal-stone-knights: off` and `mercenary-portal-royals: off` stop
  those from coming. Stone Knights also leave the lobby's list.
- `mercenary-portal-<hero>-room-ids` and `mercenary-portal-royal-room-ids` set the
  rooms (one or two ids from rooms.md, joined with +). The first room is what the
  weight below uses, so it sets how often the hero comes; Lair first gives the
  large fixed weight.
- Around each Mercenary Portal's choice, our creatures' attraction lists are
  replaced with their rooms (any size, kind 1, amount 1), then put back. The
  defaults:

  | Creature (type) | Rooms |
  |---|---|
  | Wizard (16) | Library (6) |
  | Guard (25) | Training Room (7) |
  | Giant (15) | Workshop (10) |
  | Thief (18) | Casino (15) |
  | Knight (13) | Combat Pit (16) |
  | Elven Archer (17) | Guard Room (9) |
  | Monk (19) | Graveyard (14) |
  | Fairy (20) | Temple (13) |
  | Royal Guard (30) | Torture Chamber (12) + Training Room (7) |
  | Stone Knight (28) | Torture Chamber (12) + Combat Pit (16) |
  | Lord of the Land (29), King Reginald (21), Princes (2, 26, 31) | Lair (2) + Hatchery (4) |

  Ids: creatures.md and rooms.md. If a creature id has no creature data, a
  warning is logged at level start.
- The test at 0x4DC064 (13 bytes) jumps to a stub. During a Mercenary
  Portal's choice it allows:
  - our heroes;
  - the royals, only while the owner has not had one this game;
  - the game's own mercenaries (+0x68A & 8), only with
    `mercenary-portal-spawns: mercenary`.
  Outside one it does the game's own test.
- Royals: the Lair as first room gives each the same weight, so the game's
  weighted pick chooses one at random. After each portal choice, if the
  owner has any royal type it is marked as having had its royal; the marks
  are cleared at level start. Not saved with the game: after loading, a
  dead royal could be replaced.
- The map's creature pool still applies (the user's choice): a hero must
  be available to the player on the map.
- Mercenary forms: the game's data has no hero copies (types 32-44 are
  copies of keeper creatures) [reference]. The mercenary part is how a
  creature arrives, see below.
- Arrival (0x4DC9A0, the portal's creature maker) [verified]: after the
  creature is made, `cmp al, 0x19` at 0x4DCBF3 splits it:
  - Mercenary Portal: arrival state 0x116, 0x4B5570 with its position,
    creature +0xF3 bit 0 set, then 0x48E0D0(1);
  - any other portal: state 0xC1 (walks out of the Portal), +0x99 = 0x1000,
    then a check of the player's count (0x4BA7D0) with message 0x59.
  Both set +0x4EB to the room's tag.
- 0x48E0D0 (thiscall (creature, on)) sets creature +0x3EA bit 9 and +0xF0.
  The game's creature debug dump names +0x3EA bit 9 "IsMercenary".
  [verified] +0xF3 bit 0 is tested at the start of state handlers
  (0x48E84B, 0x4A6E29), so it is a general state flag, not the mercenary
  mark. [verified use]
- Tests:
  - with the game alone, heroes from a Mercenary Portal came as mercenaries;
  - with the Portal's arrival (state 0xC1) for them, they appeared in the
    air over the room's center, dropped there and were stuck among the
    pillars, with no arrival effect. That arrival is made for the Portal's
    open middle. The Mercenary Portal's own (state 0x116) brings them out
    between the pillars, as in the campaign's Mercenary Portals.
- KeeperForge2 keeps the Mercenary Portal's arrival and patches the
  `push 1; xor al, 1; mov ecx, esi` at 0x4DCC4F (the 1 is the IsMercenary
  value for 0x48E0D0): with `mercenary-portal-spawns: normal` it pushes 0,
  so heroes come as normal heroes. With `mercenary` it stays 1.
- 0x48E0D0 also sets creature + 0xF0 to the same value: render info
  (+ 0x44) + 0xAC, the texture bank [reference: Flame names it]. The
  drawing code passes that byte to the mesh lookup 0x4B44E0 (0x59B459,
  0x59B4AE) [verified]. Bank 1 is the mercenary textures. King, Lord of the
  Land, the three Princes, Stone Knight and Dwarf have none and were drawn
  white (model and animations fine) [verified in game]. KeeperForge2 hooks
  0x48E0D0 and puts the bank back to 0 for those types; IsMercenary stays.
- The Hand of Evil keeps its own render info for the creature it holds
  (hand + 0xFB8, bank at + 0x1064). On pick-up, 0x40D7D5..0x40D7F5 sets that
  bank to 1 when the type's data flags (+ 0x686) have 0x8000000 or the
  creature has IsMercenary, else 0 [verified]. So those types were white
  again in the hand (tested). The IsMercenary test (0x40D7E1, 11 bytes) now
  jumps to a stub that also needs a type with mercenary textures.
- Both settings are a session option (see session-options.md).

## Creature pool and the lobby's creature list
- The player's creature pool (player +0x1109: count +0xC, entries +0x10 of
  {type, int number}) is how many more of each type may come through a
  portal. 0x506400 (thiscall (pool, type) ret 4) is "number != 0", and each
  arrival takes one off (0x5064B0, called from 0x4DC9A0). [verified]
- In skirmish and multiplayer, world setup fills every keeper's pool from
  the game settings' creature list, GameCfg +0x174 (pointer, 0x75AEBB) and
  +0x188 (count, 0x75AECF). It uses 5-byte entries {type, count, ...}:
  0x506520 then 0x506460 (type, count). [verified]
- The lobby's options screen edits the front end's copy:
  - entries at +0x30CB3, with the count byte at +1 (slider at 0x5496B0);
  - types at +0x30CCB (bytes) and name ids at +0x30CC7 (shorts);
  - the number of rows at +0x30CEF;
  - tab +0x603B (0 = creatures), 8 rows visible, scroll bar.
  [verified]
- The rows come from the creature data loader. Its record callback
  0x548B70 (cdecl (record, front end)) adds a record's type (+0x133B) and
  name id (+0x1314) when its flags (+0x12EC) have 0x400 and lack 1 and
  0x8004100: the keeper Portal creatures. [verified]
- 0x55DA50 (thiscall (GameCfg, front end list out, count) ret 8) makes the
  GameCfg list and the front end's copy, all zero. It is called from
  0x548xxx (+0x30CB3) and from the saved-game reader (null). [verified]
- The host's lobby message to the other players (type 199, built around
  0x5489xx) holds the map name, the packed options +0x6026..+0x602A, then
  all five lists. [verified] So the lists and our session option bits
  reach every player.
- KeeperForge2:
  - adds the ten heroes to the lobby's list (after the record callback,
    when the game did not add them);
  - sets their count to 16 when the list is made;
  - for heroes, uses the pool's number as "how many alive at a time".
    The first time a keeper's portal runs in a level, its numbers are
    kept as limits. For each portal choice, the pool is set to the limit
    minus how many of that type the keeper has, then put back to the
    limit.
  - Royals are not in the list. While a royal is allowed, the pool check
    (0x506400, hooked) says yes for it during a Mercenary Portal's choice.
- Hiding rows [verified code, not tested]: a joining player ignores the host's
  lobby message unless its size matches its own lists (0x5432BD), and takes the
  row types from its own list (0x549F60), so all machines must agree on the rows.
  So every machine keeps the rows sorted (the game's, the heroes, Stone Knight
  last), makes the lists with room for all of them, and sets the count to hide
  rows at the end, using its own settings. The size check is patched: a joining
  player takes its count from the size of the host's message. 0x75AECF, which
  level setup reads, is kept equal to the count.
- Rooms in multiplayer: each hero's row holds its rooms in bytes +2 and +3, and
  the first row holds the royals' in +3 and +4 (see session-options.md).
- Saved games store the lists without a count (0x55B8xx reads GameCfg's
  count of entries). A skirmish saved before the heroes were added to the
  list may not load right. [unverified]

## Diagnosing (log lines)
- "Mercenary Portal: player N has a working Mercenary Portal." appears the
  first time a claimed Mercenary Portal of that player runs its choice in
  a level. If it never appears, the room is not type 0x19, or it is not
  claimed.
- "Mercenary Portal (player N): ready: ...; waiting for rooms: ...; not in
  the map's creature list: ...; royal: ..." is written whenever it
  changes. A hero under "not in the map's creature list" needs to be made
  available to that player in the editor.
- Second test (custom map, normal, Dwarf workers on): nothing came out
  again, and no log lines yet existed to tell why. The diagnostics were
  added for the next test.
