# Rooms (room types)

Every room is a room type, a byte id. In our code they are
`Game::Room::<Name>` in Runtime/Include/GameAddresses.h, named as in the game
with "_" for a space (`Training_Room`, `Combat_Pit`). The
`mercenary-portal-*-room-ids` settings in config.yml use these ids.

## Ids
The list and its order are the user's [verified by the user]. Lair 2 is also
in the game's own portal code, and Portal 3 and Hero Portal 25 in its room code
(mercenary-portal.md) [verified].

| Room | Code name | Id |
|---|---|---|
| Lair | `Lair` | 2 |
| Hatchery | `Hatchery` | 4 |
| Dungeon Heart | `Dungeon_Heart` | 5 |
| Portal | `Portal` | 3 |
| Library | `Library` | 6 |
| Training Room | `Training_Room` | 7 |
| Treasury | `Treasury` | 1 |
| Work Shop | `Work_Shop` | 10 |
| Casino | `Casino` | 15 |
| Guard Room | `Guard_Room` | 9 |
| Wooden Bridge | `Wooden_Bridge` | 8 |
| Prison | `Prison` | 11 |
| Torture Chamber | `Torture_Chamber` | 12 |
| Stone Bridge | `Stone_Bridge` | 17 |
| Graveyard | `Graveyard` | 14 |
| Combat Pit | `Combat_Pit` | 16 |
| Temple | `Temple` | 13 |
| Hero Gate [Final] | `Hero_Gate_Final` | 18 |
| Hero Gate [2x2] | `Hero_Gate_2x2` | 20 |
| Hero Gate [3x1] | `Hero_Gate_3x1` | 23 |
| Hero Portal [Mercenary Portal] | `Hero_Portal` | 25 |

- Id 24 is "MercenaryGate" in the game's room dump strings, not on this list
  (mercenary-portal.md) [verified].
- A room's size for a creature's room requirement: mercenary-portal.md.
