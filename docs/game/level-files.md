# Level files (LevelFiles.cpp)

The game's level file format, as far as the editor uses it. Sections and blocks are from
the official editor's reader, the pro version [reference, editor], and what the bytes in
the game's own files show (Gonzalez.kwd and Traps.kwd were read) [verified by eye]. The check
below tests all of it against every stock level. [verified, 2026-11-03: 67 levels would write back
byte for byte, 0 failed. Terrain ids in the log match the game's names.] Actually saving a map and
opening its copy has not been tried yet.

## Sections
- A file is a run of sections. Each has 12 bytes: id, a second dword kept as it is, and the
  section's size, which counts these 12 bytes. The editor's reader goes on until the end of the
  file. [verified in game: taking the size for the body alone failed on every file by exactly 12
  bytes (U Turn.kwd 26919, Terrain.kwd 22688). Terrain's remainder is 41 records of 0x228 bytes.]
- Ids: Map 100, Terrain 0x6E, Room 0x78, Trap 0x82, Door 0x8C, Keeper Spell 0x96, Creature
  Spell 0xA0, Creature 0xAA, Player 0xB4, Thing 0xBE, Trigger 0xD2, Level 0xDC, Variable 0xE6,
  Object 0xF0, Effect Element 0xFA, Shot 0x104, Effect 0x10E.
- Most have this shape for section S: a block S+1 of 0x24 bytes (id, size, record count, more),
  a block header S+2 of 8 bytes (id, size), then the records. Size / count is the record
  size; old files have shorter records, which the editor reads with the rest zeroed.
  Things and Triggers differ; Things is decoded below, Triggers not yet. Players and Variables are lists of this shape.

## A level
- A level is `<name>.kwd`, with a Level section. Its blocks, in file order:
  - 0xDD of 0x24 bytes: at +8 a 16-bit count of files, at +0xA a 16-bit count of 4 byte entries
    that follow the list;
  - 0xDF: a block header, then the description, 0x6453 bytes in current files. It starts with
    the level's wide file name. Unused parts of it hold junk from the tool that saved it;
  - 0xDE: a block header, then the list of files.
- An entry of the list is 0x48 bytes: type (the section id it holds), a dword, a 64 byte path.
  Paths are relative to the game folder, such as `Data\Editor\Maps\GonzalezMap.kld` and
  `Data\Editor\Terrain` (no extension: the game adds it). [verified in Gonzalez.kwd]
- The game loads each listed file by that path [verified: 0x51DE50, 0x51EA40], so a copy
  elsewhere only needs its own paths changed.
- `<name>Map.kld` holds Map. A block 0x65 of 0x10 bytes: id, size, width, height. A block header
  0x66 of 8 bytes. Then width * height tiles of 4 bytes, row by row. First byte the terrain id, second
  the owner's player number (1 Good, 2 neutral, 3 and up keepers) [verified: the game's tile loader
  around 0x51EC79 reads them, after the loader's own 12 bytes of x and y]. The other two are not
  understood, and are kept as they are.

## Description (block 0xDF of the level section)
- 0x6453 bytes in current files; the reader 0x56C4E0 zero-fills a shorter one. 0x56C100 copies its fields out [verified]:
  - wide texts: +0 0x40 characters (the level's file name in stock levels), +0x80 0x400, +0x880 0x40, +0x900 0x40, +0x980 0x400.
    Taken to be name, description, author, email and information, from their sizes and the official editor's fields [unverified].
  - +0x1180 and +0x1182 16 bit; +0x138C 20 wide texts of 0x200 characters each (0x400 bytes apart), copied for the game's
    use only where a flag says they are there [verified: the copy; what they are is not known].
  - From +0x638C: a 16 bit flags word, a 32 character narrow name at +0x638E, and bytes and 16 bit values to +0x6433, among them
    one (+0x63B8) that goes to 0x553780 and 0x553800 (40 when absent), and a 32 character wide text at +0x6413 [verified: copied;
    meanings not known].

## Players (`<name>Players.kld`, section 0xB4)
- A list: block 0xB5 of 0x24 bytes with the count at +8, block 0xB6 with the records. 0xCD bytes each (0x56DAD0) [verified].
- +0 starting gold (0x51F0B0 hands it to 0x4BA420, which adds it to the player's money), +4 non-zero for a computer player, +8 0x9E
  bytes of its settings (0x4F8750 reads them, many fields), +0xA6 16 bit (to player +0x10DA), +0xA8 the player's number, +0xA9 and
  +0xAB 16 bit (for the local player, to world +0x7235), +0xAD a 32 character name [verified: the reader and 0x51EF80, 0x51F0B0].

## Variables (`<name>Variables.kld`, section 0xE6)
- A list: block 0xE7 with the count at +8, block 0xE8 with records of 0x10 bytes: four dwords, the first the kind (0x5716E0) [verified].
- The kind picks a handler from a table at world +0x1F24 (0x521790); the table's source list 0x6A5CD0 is empty in the export, so it
  is filled some other way.
- Kind 2 is availability: (2, player * 0x10000 + category, type, value). Player 0 is every keeper. Categories 1 rooms, 2 creatures,
  3 traps, 4 doors, 5 keeper spells, as 0x51CAE0 calls each list's setter (player +0x5A, +0x52, +0x6A, +0x62, +0x72) [verified, and
  the official editor writes them this way]. Every entry starts at 1 when the player's lists are made.
- Values, from a survey of all 67 stock levels' Variables [verified]: rooms a keeper starts with (Treasury, Lair, Hatchery, Library,
  Portal) have 4, rooms researched later (Training Room, Work Shop, Torture Chamber, the bridges ...) 3, and 1 keeps one back (a
  few levels: Portal, Treasury, Library). Traps, doors and spells alike: most 3, the advanced ones 1 in some levels, the basic ones
  4 in a few. So 4 available from the start, 3 researchable, 1 not available. Creatures always 3. 2 is in 3 records only (Guard
  Post, Jack In The Box, Barricade), meaning not known. Stock levels only use players 3 to 7 (up to five keepers), never 0.
- Kind 1 is a keeper's creature pool: (1, creature type, how many may come, player), 20 to 60 in stock levels. Every creature
  with availability 3 has one, and no other [verified: the two sets match one for one in every stock level].
- Others seen: 66 (211 records, small numbers, perhaps alliances), 74, 241 and 242 once in every level, and kinds 3 to 240 once
  each in Furnace_hero and a few in other levels (an id, a value) [verified: counts; meanings not known]. Kept as they are.
- Players in stock levels: 205 byte records in all; players 1 and 2 (heroes, neutral) are computer players in 50 of 67 levels;
  keepers mostly start with 3000 gold, some 8000 to 16000; player 3 is never a computer player [verified: the survey].

## Triggers (`<name>Triggers.kld`, section 0xD2)
- Block 0xD3 of 0x28 bytes: +8 the count of triggers, +0xC of actions. Block 0xD4 header, then that many records, each with an 8
  byte header: 0xD5 a trigger, 0xD6 an action, 16 bytes each (0x571290) [verified; in all stock levels nothing follows them].
- A trigger: +0 to +3 settings bytes, +4 a 32 bit value, +8 its id, +0xA the next trigger's id (0 at the end), +0xC its child:
  above 0 a trigger's id, below 0 an action's, +0xE its type, +0xF a byte (mostly 1). An action: +0 to +7 settings, +8 its id
  (below 0), +0xA the next action's id, +0xC 16 bit, +0xE its type, +0xF a byte [verified: the links hold together over all stock
  levels]. The game keeps them as they are in the file (0x521A60 to 0x508600, 0x521AB0 to 0x508810).
- Types by the official editor's tables (triggers at 0x50B794, actions at 0x515554 in the GOG editor), which fit how often each is
  used in stock levels (Flag, Time and Timer the most common triggers; Set Flag and Play Speech the most common actions) [reference].
  The editor's trigger table also gives each type's subject: 1 the level, 2 a player, 3 a creature, 8 an action point, 9 a party.
- Where each tree starts [verified: in stock levels each non-zero one is a trigger's id]: the level's at +0x1180 of the description
  (to world +0x7310 through 0x518080), a player's at +0xA6, an action point's at +0x14, a hero's or keeper's creature at +0x18 and a
  neutral one at +0x14, a hero party's at +0x20 and each of its members' at their +0x18 (below). +0x1182 of the description is not a trigger: the game uses it as a count of turns (multiplied by 3, 7 and 10).
- An action's action point: byte 0 for Flash (16) and Reveal (17) Action Point and byte 4 for Create Hero Party (14) [verified in
  0x518AD0]; byte 0 for Zoom To (26) and Rotate Around (27) Action Point [verified only by a survey: every stock record has one of its
  level's action points there]. A survey had also matched byte 1 of Generate Creature, Camera Follow Path and Change Room Owner, but
  0x518AD0 shows Change Room Owner's byte 1 is a player, so those were player numbers that happen to equal action points: dropped.
- A trigger's last byte (+0xF) is how many times it may fire: the game takes one off each time (0x508640) and turns it off at 0.
  Records are kept by the game as they are in the file, with a flags dword added (0x508600, 0x508810).
- The level's triggers are checked by 0x518270 and its actions run by 0x518AD0, a switch on the type [verified]. Players have their
  own (not looked into). What those two show:
  - Flag trigger (1): byte 0 the comparison (1 <, 2 <=, 3 =, 4 >, 5 >=, 6 !=; 0x519980), byte 1 the flag (128 the level's score,
    else one of the bytes at world +0x3F70), bit 0 of byte 2 compares with the value at +4, else with the flag in byte 3. Timer (2) the
    same way with a timer. Whether other count triggers (Creatures, Gold ...) use the same bytes is [unverified]; the official editor
    shows them with the same comparison and value.
  - Actions: Create Creature (1): byte 0 type, 1 player, 2 level (each above 1 raises it once), 3 flags, +4 x and +6 y (16 bit,
    tiles). Create Portal Gem (2): byte 0 the object, 1 player, +4 x, +6 y. Set Availability (6): byte 0 player, 1 category (as the
    Variables), 2 type, 3 not 0 for researchable (3), 0 for not available (1). Set Flag (7): byte 0 the flag (128 the score), byte 1
    bit 1 the value is +4 itself (else the flag numbered there), bit 3 set it, else bit 4 add, else take away. Set Timer (8): byte 0
    the timer, started from now. Create Hero Party (14): byte 0 the party, byte 1 (only if none of that party is out already when not
    0), byte 4 the action point. Flash Action Point (16): byte 0 it, byte 1 on, +4 time. Reveal Action Point (17): byte 0 it, byte 1
    0 reveal, else hide. Set Alliance (18): bytes 0 and 1 the players, byte 2 0 allies, else not. Alter Terrain Type (22): byte 0 the
    terrain, 1 player, +4 x and +6 y counted from 1. Set Level Time Limit (23): +4. Collapse Hero Gate (32), Display Slab Owner (46,
    byte 0 on) and Change Room Owner (49, byte 1 player): +4 x and +6 y from 1. Create Door (39) and Create Trap (40): byte 0 type, 1
    player, +4 x, +6 y. Toggle Effect Generator (48): byte 0 it, byte 1 on. Others are not in this handler (players' own, not looked into).
- The records block's own size (+4 of block 0xD4) does not follow the records in one way over the stock files, and the reader skips
  it; it is kept as it was when saving.

## Other thing kinds
- The things reader (0x56FA00) sends each kind to its own reader, which lays the record out for a maker from level load's table
  (0x51E0A0) [verified]: 0xCA (0x10 bytes) through 0x570500 to 0x5212D0, 0xCB (0x24) through 0x570630 to 0x521380, 0xCC (room) to
  0x521420, 0xCD (0x54, through 0x56F850 and 0x56F900 like creatures) to 0x5215F0, which gives it to the camera (engine interface
  +0xD0, then 0x449A30): the level's camera settings, not a thing on the map. Any other kind goes to a slot the level's table leaves
  empty, so the game skips it: the official editor's "plinth" has nothing in the game to make it.
- By the official editor's error strings in order (creature, object, trap, door, action point, hero party, dead body, effect generator,
  room, plinth, camera) 0xCA is a dead body and 0xCB an effect generator [reference].
- Dead body: +0 x, +4 y, +8 z, +0xC 16 bit, +0xE the creature's type, +0xF a player (0x5212D0 passes the type and makes it neutral's
  through 0x4B7FC0) [verified]. No stock level has one.
- Effect generator: +0 x, +4 y, +8 z, +0xC a dword, +0x10 and +0x12 16 bit, +0x14 four 16 bit effects, +0x1C a byte, +0x1D its type
  (0x521380 makes it through 0x4B81E0 with the type, the place and the two 16 bit values, then sets the byte and the four effects)
  [verified]. The one stock one (Level17): 8, 8, effects 3 3 3 3, 10, type 1, and junk after.

## Hero parties (Things kind 0xC9, 0x22B bytes)
- [verified: 0x570210 reads them (older files 0x108, 0x10C or 0x12C bytes), 0x5212A0 and 0x50FA70 keep each by its number; and a survey
  of all 220 stock parties] +0 a 32 character name (junk in stock levels), +0x20 the party's trigger tree (0 or a trigger in all),
  +0x22 its number (0 to 19), 8 bytes not understood (one of a few patterns, kept), +0x2B 16 members of 0x20 bytes, each a hero
  creature's record: type +0x1C (0 for an empty place), level +0xE, its own tree +0x18 (0 or a trigger in all 947 members). Stock
  parties have 1 to 16 members, at no tile (x and y 0).

## A level's creature list
- The level's file list names its creature types: `Data\Editor\Creatures` (type 0xAA) in stock levels, or a Globals file
  (type 0) holding every shared list in Furnace_hero [verified by eye]. Layout in creature-types.md.
- The editor reads whichever the level lists. Saving changed types (written, not tested):
  - from the shared list: the level gets its own `<name>Creatures.kld` holding only the creature list, next to its other
    files (KeeperForge2\maps for a stock level's copy, Data\editor\maps for an editor level), and the .kwd's type 0xAA entry
    is pointed at it. Creatures.kwd is never written.
  - from a list of the level's own (that file, or a Globals file): written back into it, records kept at their size.
  - Save as takes the creature file along under the new name, or makes it there when the changes were to the shared list.
- [unverified] That the game loads a level's own type 0xAA file in place of the shared one. Its loader is the same for every
  listed file (creature-types.md), and Furnace_hero shows a level's own creature list works.

## Exact hit points (`<name>Things.health`, KeeperForge2's own)
- Not a game file. A placed creature's record keeps only a whole health percent, so the editor keeps exact hit points beside the
  level's Things file, same name with `.health`. Text, one creature per line: `x y owner type level points`; lines starting
  with `#` are skipped. Written on save and save as when it has entries, and removed when it has none.
- The record still holds the nearest percent, so the level plays without the file (at what that percent gives).

## Shared lists
- `Data\editor\Terrain.kwd`, `Traps`, `Doors`, `Objects`, `Rooms`, `Creatures`, `KeeperSpells`. Each
  record starts with its name, 32 characters. [verified by eye in Traps.kwd; the check logs the
  rest] Ids are at their own place in a record. Terrain's records are 0x228 bytes, id at +0x1D6: the
  game's converter 0x56ED20 copies it to +0x28E of what 0x51EB50 then reads [verified in code; the
  check logs them]. Others not found yet, so the order in the file is not to be taken for the id.

## Things
`<name>Things.kld` is one section 0xBE, in all 67 stock levels. [verified: a script read them all, with counts and sizes adding up]
- Block 0xBF: 8 byte header plus 0x1C, with the record count at +8. Block 0xC0: a header whose size is all the records. Each record
  has a kind, a size, then its bytes. The game's reader is 0x56FA00.
- Kinds and sizes in stock levels: 0xC2 object 0x18, 0xC3 trap 0x10, 0xC4 door 0x14, 0xC5 action point 0x38, 0xC6 neutral creature
  0x18, 0xC7 hero creature 0x20, 0xC8 keeper creature 0x1C, 0xC9 hero party 0x22B, 0xCB effect generator 0x24, 0xCC room 0x14.
  [verified: the reader's switch and the creature handlers; the other names from the editor's error strings]
- All creature kinds: +0 x, +4 y, +8 z (tiles), +0xC 16 bit gold, +0xE level, +0xF flags (9 in most). [verified in the converters]
  - Neutral: +0x10 health percent, +0x14 16 bit, +0x16 type.
  - Hero: +0x10 dword, +0x14 health percent, +0x18 16 bit, +0x1A and +0x1B bytes (mostly 3 and 23), +0x1C type, +0x1D, +0x1E (always
    255), +0x1F. What the unnamed ones mean is not known.
  - Keeper: +0x10 health percent, +0x14 dword, +0x18 16 bit, +0x1A type, +0x1B player number.
- Object (0xC2, 0x18 bytes): +0 x, +4 y, +8 z, +0xC a dword, +0x10 a dword, +0x14 16 bit, +0x16 type, +0x17 player. [verified in
  0x56F730 and the maker 0x520BB0] The maker skips type 28 (Portal Gem). Type 7 with +0xC 2 to 18 becomes a Special (117 to 129).
  An object whose type data (+0x12D, flags at +0xF4) has 0x40 gets +0x10 passed to 0x49C760, and with 8 keeps +0xC at +0x14E: stock
  levels have their gold's amount there (50 to 5000) and their spell books' spell (1, 2, 6, 9, 11). Most objects are player 2.
  Types are their place in Objects.kwd from 1, also at +0x35A of its 0x37E byte records. [verified: all 145 records]
- Keeper spells: KeeperSpells.kwd's order from 1 (13 records of 0x196 bytes), as a spell book's +0xC. [unverified, but Create Imp is
  4 as the game casts it]
- Action point (0xC5, 0x38 bytes): +0 and +4 one corner tile, +8 and +0xC the other, both inside [verified: 0x56F790; every stock
  one is a box]. With +8 at 0 the first two are the center and +0xC how far it reaches; no stock level does that. +0x10 a dword
  (0x40000 in 716 of 1162), +0x14 16 bit and +0x17 a byte (mostly 0, up to about 50), +0x16 its number, +0x18 a 32 character name
  (empty in stock levels). Numbers are not always 1 to n (gaps in 8 levels). The maker is 0x5211A0.
- Room (0xCC): +0 x and +4 y of the room's center tile, +0x12 the room's terrain id (14 for a Dungeon Heart), +0x13 the player. Every
  stock heart also has +0xF 1 and +0x10 100, the rest 0; not understood. [verified by eye in Small and Swiss Cheese] A stock heart is 5x5
  heart tiles in a ring of the keeper's Reinforced Wall, the record at the middle tile.
- Tiles: the third byte is 1 on every stock tile; the fourth is 0 or 1, 1 on most. [verified: counted over all stock maps]
- Map size: no largest in the game (docs/game/level-editor.md, New map). Stock maps run from 30x30 to 121x66; width and height often
  differ. The level's .kwd holds no copy of the size. [verified: no stock .kwd holds its map's width and height as two 16 or 32 bit numbers]
- Trap: +0 x, +4 y, +8 z, +0xC above 0 is a finished trap and 0 a blueprint (mostly 4; only tested against 0 at 0x520EDB), +0xD type,
  +0xE player, +0xF a flag byte the make function does not read. Door: +0 x, +4 y, +8 z, +0xC 16 bit, +0xE type, +0xF player, +0x10
  flags (bit 1 passed to 0x4A66C0, bit 0 to 0x4A6800), the rest junk. [verified in 0x56F690, 0x56F600, 0x520E50, 0x520F40] Their types
  are their place in Traps.kwd and Doors.kwd from 1, also at +0x20C and +0x244 of their records. [verified: stock hero traps and doors
  are mostly Hero Sentry Trap, Hero Barricade and Hero Portcullis]
- A creature's type is its place in Creatures.kwd from 1, also stored at +0xF4B of its record. [verified: stock levels read this way
  give heroes as guards, fairies, dwarves and wizards, and keepers mostly imps and vampires. Not by its other list, at +0xCF6]

## Saved copies
- Terrain.kwd records hold their cell in `TerrainIcons.bmp` as a 16-bit number at +0x196. [verified: the values fit the sheet
  (water 0, lava 1, gold 4, gems 5, dirt path 6, mana vault 10), and the editor gets a record's icon from a virtual call, then
  adds the owner for terrain ids 8 and 9 only (0x4A3D90 in rc3)] Impenetrable Rock 2, Rock 3, Claimed Path 15, Reinforced Wall
  20, Claimed Vault 11, Hero Lair 7. Rooms in that file hold nothing useful there. A search of the other files found no field
  like it, but the editor reads theirs the same way, so they probably have one. [unverified where]
- `KeeperForge2\maps`. Same file names as the original, with the paths in the `.kwd` changed.
  A copy that exists is never overwritten by the check.
- The game builds `<base>Maps\<name>.kwd` from the level name 0x75AD57, 64 wide characters. So
  `..\..\..\KeeperForge2\maps\<name>` reaches a copy with no hook. [not tested] The Editor button
  does this when the chosen map has a copy.
- The check writes a level's `.kwd` last, so one that is there has all it lists.

## The check
Runs without writing anything the first time Editor is clicked in a run. `editor_check_maps: on` in
config.yml, for one start, also makes the copies. For each `.kwd` in `Data\editor\maps`, it reads
the level and every `.kld` it lists, splits them into sections, decodes the level list and the map,
and compares what would be written with the original. The result and the shared lists go in the log,
as lines starting `Level files:`.
