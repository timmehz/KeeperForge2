# Creatures (creature types)

Every creature is a creature type, a byte id. In our code they are
`Game::Creature::<Name>` in Runtime/Include/GameAddresses.h, named as in
the game with "_" for a space (`Dark_Elf`, `Lord_Of_The_Land`).

## Ids
The list is in the order DK2 Editor pro.exe lists the creatures: Imp to Maiden
Of The Nest are the evil creatures, Dwarf to Prince Balder the good ones.
Every id below is the one the level editor shows (checked by the user)
[verified in the editor]; all but the Princes and the Maiden also match
Flame's creature type list.

| Creature | Code name | Id | Side | Notes |
|---|---|---|---|---|
| Imp | `Imp` | 1 | evil | also in the game's Create Imp and worker code; Dwarf workers swap 1 for 14 (tested) |
| Goblin | `Goblin` | 7 | evil | |
| Warlock | `Warlock` | 5 | evil | |
| Firefly | `Firefly` | 12 | evil | |
| Troll | `Troll` | 10 | evil | |
| Dark Elf | `Dark_Elf` | 6 | evil | |
| Skeleton | `Skeleton` | 9 | evil | |
| Mistress | `Mistress` | 4 | evil | |
| Salamander | `Salamander` | 11 | evil | |
| Rogue | `Rogue` | 24 | evil | |
| Bile Demon | `Bile_Demon` | 3 | evil | |
| Vampire | `Vampire` | 8 | evil | |
| Black Knight | `Black_Knight` | 22 | evil | |
| Dark Angel | `Dark_Angel` | 23 | evil | |
| Horny | `Horny` | 27 | evil | Flame calls it Reaper; the game's own strings say "Horny" (HornyTick, HasPlayedHornyAvailable) |
| Maiden Of The Nest | `Maiden_Of_The_Nest` | 45 | evil | not in Flame's list, which stops at 44 |
| Dwarf | `Dwarf` | 14 | good | also in the game's code and in game (Dwarf workers) |
| Knight | `Knight` | 13 | good | |
| Thief | `Thief` | 18 | good | |
| Guard | `Guard` | 25 | good | |
| Wizard | `Wizard` | 16 | good | |
| Giant | `Giant` | 15 | good | |
| Elven Archer | `Elven_Archer` | 17 | good | Flame calls it Elf |
| Monk | `Monk` | 19 | good | |
| Fairy | `Fairy` | 20 | good | |
| Royal Guard | `Royal_Guard` | 30 | good | |
| Stone Knight | `Stone_Knight` | 28 | good | 0x48E110 treats 28 like the King and Lord, see below |
| Lord Of The Land | `Lord_Of_The_Land` | 29 | good | came through our Mercenary Portal as type 29 (tested) |
| King Reginald | `King_Reginald` | 21 | good | |
| Prince Tristran | `Prince_Tristran` | 2 | good | Flame lists 2, 26 and 31 only as "Prince" |
| Prince Felix | `Prince_Felix` | 26 | good | |
| Prince Balder | `Prince_Balder` | 31 | good | |

- Ids 32-44 are copies of the keeper creatures (Firefly, Goblin, Warlock,
  Troll, Dark Elf, Skeleton, Mistress, Salamander, Rogue, Bile Demon,
  Vampire, Black Knight, Dark Angel, in that order) [reference]. They are not
  hero forms (see mercenary-portal.md).
- Mercenary textures: King, Lord of the Land, the Princes, Stone Knight and
  Dwarf have none (drawn white as mercenaries; we draw them normally).
  [verified in game] See mercenary-portal.md.

## Addresses
Creature types have no fixed address each: the game loads their data from its
data files at level start, so it lives in memory the game allocates. It is
reached like this [verified]:
- A creature type's data: the world's virtual function at vtable + 0xC0,
  thiscall (world, type) (`WorldCreatureData`). The other ones: first type
  + 0xAC, next type + 0xB0, how many + 0xC4. The world pointer is
  `WorldPointer` in GameAddresses.h.
- From a creature: + 0x36F its type id, + 0x370 its type's data pointer,
  + 0x28 its owner's tag, + 0x10 the next creature of the same owner.
  A creature's tag finds it in the thing table 0x6EE540.
- In a type's data: + 0x686 flags (1 = worker, 0x80 = Imp drop rules),
  + 0x68A portal flags (8 = may come as a mercenary), + 0x5DA the
  attraction list with its count at + 0x788.
- In a creature record (the data file's form, as the lobby reads it):
  + 0x133B type id, + 0x1314 name text id.
- Making a creature of a type: 0x4B78D0 (see GameAddresses.h).

Code that checks particular ids [verified]:
- 0x48E110 (thiscall (creature)): 1 for types 29, 21 and 28 (Lord Of The
  Land, King Reginald, Stone Knight), else creature + 0x3EA bit 11.
- 0x48E0D0 makes a creature a mercenary and sets its texture bank
  (+ 0xF0).
- Create Imp's spell data + 0x43 / + 0x4C hold the type it makes
  (1, or 14 with Dwarf workers).
