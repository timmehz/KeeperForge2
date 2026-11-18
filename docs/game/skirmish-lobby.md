# Skirmish lobby and keeper slots

How the skirmish screen stores its players, and what the "Swap Keeper" button
(Runtime/Source/SkirmishSwapKeeper.cpp) changes.

## Player slots
- 8 slots of 63 bytes at 0x73F5C8. Byte +0x3A holds flags; its low 3 bits
  are the slot type: 0 empty, 1 computer, 2 or 3 human. A computer slot keeps
  its AI type as an int at +0. A human slot keeps its name there. [verified]
- +0x2A and +0x32 are two 8-byte tables, one byte per other slot. Kicking a
  computer player clears that slot's column in every slot's tables. Only the
  multiplayer start copies +0x32 into the game settings (as alliances). [verified]
- Each slot's list text is at 0x73F858 + slot*0x66. The list renderer rebuilds
  it every frame. [verified]
- The front end's byte at +0x5AAC is the map's slot count. Its byte at
  +0x30344 is the menu state: 7 means skirmish, 0x10 means multiplayer. [verified]
- The list shows only the slots in use, in slot order. The selected row is
  the int at 0x740340, with -1 meaning nothing is selected. The renderer and the kick
  handler map row to slot the same way. [verified]
- Keeper color and player number follow the slot: player number = 3 + slot.
  [verified]

## Opening the screen (0x538AC0, cdecl (front end))
Copies the player's name into slot 0 and marks it human. Sets the human
count to 1 and the AI count to 0. Then adds one computer player through the
Add Computer Player handler (0x545FC0), which fills the first empty slot.
[verified]

## Starting a skirmish (menu action 0x5A in the big front end handler)
- Writes one dword per slot into 0x75AF4B: the AI type for computer slots,
  0xFF for human slots, and 9 for empty slots. [verified]
- Stores a fixed 3 into the local player number at 0x75AD53, using the
  instruction at 0x531B3E (`C7 05 53 AD 75 00 03 00 00 00`). [verified]
  Multiplayer instead stores 3 + its own network slot (0x5430E7).
- World setup (0x50C800 area) gives the matching AI to each keeper, or
  removes keepers whose slot is empty. Its check at 0x50CB97
  (`cmp al,3; jb; jbe`) skips player 3 (keeper 1), because the original
  lobby always puts the human there. [verified]
- When a player is created with the number at 0x75AD53, the game remembers
  it as the local player (it stores its id and start position in the world;
  around 0x51F1C5). [verified]

## Text buttons
- A front end window config is 0x44 bytes. Its pointer to the button list is
  at +0x3E. The skirmish window (id 9) is at 0x6AD6A0, and its buttons are at
  0x6ADD10. [verified]
- Each button config is 0x50 bytes:
  - +0x1: id
  - +0x6: click handler, cdecl (int, int, front end)
  - +0x20: x, y, w, h (in the 2560x1920 layout)
  - +0x38: renderer
  - +0x40: text id
  - +0x44: pre-rendered text index
  - +0x48: (slot << 16) | alignment
  The list ends with an entry whose id is -1. [verified]
- "Add Computer Player" is id 528, at 64,1180 with size 1360x116, with text
  index 0xB. Kick Player is id 464, text index 0xA. [verified]
- The text button renderer (0x532670) does not draw text itself. It copies
  pre-rendered images, picked by the live button's +0x34 (from config +0x44)
  and slot (+0x38 >> 16). [verified, layout of the renderer from Flame,
  [reference]]
- The images are made once by 0x5329A0, which is thiscall (window, slot,
  entries, font) with ret 0x10. Each entry is 0x88 bytes: {text id or 0,
  button id, inline text in the game's drawing format}. A list entry's index
  is its position. The list ends at button id 0. There is room for 30 per
  slot. The skirmish list (built at 0x5472DA) has 15 entries: 10 AI-type
  texts for button 681, then Kick, Add Computer, and three right-hand
  buttons. [verified]

## What Swap to Good Player does
- Adds a second button (id 9000 + 1, y 1484) with two pre-rendered texts:
  "Swap to Good Player" and "Swap Back to Keeper".
- Swap to Good Player saves the human's 63-byte slot and makes that slot a
  computer player, the way Add Computer Player does: AI type 0 at +0,
  +0x22 and +0x26 cleared, flags type 1 plus 0x18. The lobby counts are
  updated: humans -1 (0x73FC18) and computers +1 (0x73FC10). The computer
  can then be given any AI type and moved with Swap Keeper like any other.
- With no human left, the skirmish-start immediate (0x531B3E+6) is set to
  1, so the local player is the Good player. [reference] Player 1 is the
  heroes and player 2 is neutral. World setup gives every keeper slot its
  AI (slot 0 included, see below). The Good player has no lobby slot, so it
  is not touched.
- [verified] Setting the number alone did not work (tested: the player
  ended up controlling keeper 1 and fighting its AI for it). When world
  setup reads the map's players, 0x51EF99 branches on the player number:
  - 1 (Good, tag stored at 0x6F254C) and 2 (neutral, tag at 0x6F2548) are
    made directly with 0x4C2340, a thiscall on world +0x7A taking
    (number, tag out), ret 8;
  - only keepers go through 0x51F0B0, whose check at 0x51F1C5 stores the
    controlled player's tag at world +0x7243 (unless world +0x7239 has
    0x80).
  So 0x4C2340 is hooked: for player 1 or 2 equal to the local player
  number, the tag is stored at world +0x7243 the same way. The start
  camera position (+0x7235) is not set for the Good player.
- [verified] Second test crashed at 0x5082A8 (called from 0x4481FE, which
  reads player +0x1105 and got null). Player init (0x4B8640, called from
  0x4C2340 through 0x4C2460) makes a 0x469B-byte task list for the player
  (constructor 0x507A50) and stores it at +0x1105, but 0x4B87B5
  (`3C 01 74 63 3C 02 74 5F`: cmp al,1; jz; cmp al,2; jz) skips it for
  players 1 and 2. Teardown (around 0x4B8C6C) frees it with only a null
  check. So while the 0x4C2340 hook creates player 1 as the local player,
  the jz at 0x4B87B7 is set to `90 90`, and back to `74 63` right after.
  Other players and other games are unchanged (multiplayer stays the same
  for everyone). More keeper-only code may still expect things the heroes
  lack; a crash log will point at it.
- [verified] Third test: picking up a creature showed it in the hand, then
  it fell back and bounced. The hand works through timed player actions:
  - 0x4C09F0 sets one (player +0x186, plus +0x18E..+0x196 data) and
    calls its start callback.
  - The table at 0x6E50B8 has 0x36-byte entries: +0 start, +0x10 tick,
    +0x20 finish, +0x34 duration, +0x35 can be interrupted.
  - Pick up's finish (0x4C0EA0) calls 0x4BC500, which puts the creature
    in player +0xB2 (count +0x1BF, limit +0x1C0).
  - 0x4C0AA0 runs and finishes the action. It is called from the keeper
    tick 0x4BB2A0, reached from the player tick 0x4B9EC0.
  - 0x4B9EC0 returns early for the neutral player (at 0x4B9F22) and the
    Good player (0x4B9F3A; it only sets its mana to 0x7FFFFFFF).
  The screen keeps its own list and lets go after 2 s when the game-side
  player does not hold the creature [reference]. Fix: 0x4B9EC0 is hooked,
  and after it, a Good player with an action set gets 0x4C0AA0 run.
- Swap Back to Keeper puts the saved slot back into the selected computer
  row, or else into the slot that was left (if it is a computer or empty).
  It keeps that slot's relation tables and fixes the counts.
- A human back in the list, or the screen being opened again, ends Good
  mode. Expect some maps to behave oddly with the heroes as the local
  player (for example no heart, or no start position).

## What Swap Keeper does
- Adds a copy of the Add Computer Player button with id 9000, at y 1336.
  It uses our click handler and our renderer, which wraps 0x532670.
- Adds two inline texts to the window 9 pre-render list: "Swap Keeper" and
  "Pick a Keeper (Cancel)".
- When pressed, it remembers the selected row's slot. It then waits for the
  selection to move to another row.
- Then it swaps the two 63-byte slots and their list texts, and swaps the
  two columns in every slot's +0x2A/+0x32 tables.
- Each frame on the skirmish screen, it rewrites the immediate at
  0x531B3E+6 to 3 + the human's slot. It sets it back to 3 when the screen
  opens.
- Changes 0x50CB98 from 3 to 2, so that world setup also handles keeper 1.
  This changes nothing while slot 0 is human, because a human slot is 0xFF,
  which world setup leaves alone.

## Multiplayer lobby (window 32) [verified]
- Window config 0x6B39A0, buttons 0x6B39E8. Same player list (kind 10, id
  0x1CE, selection 0x740340), Kick (0x1D0), Add Computer Player (528 at
  y 1180), right-hand buttons at x 1480, y 1180/1336/1484, chat output
  0x1CD and input 0x1CC (kind 8, y 1796). Menu state +0x30344 = 0x10.
- Front end +0x311F2 == 1 on the host: Add Computer Player (0x545FC0) only
  works for the host in multiplayer (or in skirmish). It changes only the
  slot table; slots are packed per slot (0x545xxx) and unpacked by 0x545B10,
  so the host's table reaches the other players.
- A human's slot: the game puts a joining player into the slot equal to
  its network slot (around 0x5452xx: name, type 2, and + 0x3B = a value from
  MLDPlay::GetPlayerDesc, the player's network id).
- Start: 0x5430E7 `mov [0x75AD53], eax` with EAX = 3 + NetworkLocalIndex
  (0x55AE30, fastcall on 0x758160): the local player's place among the
  network's active players, i.e. the slot the game gave it.

## Swap Keeper in multiplayer (SkirmishSwapKeeper.cpp)
- A copy of Add Computer Player (id 9000) at x 64, y 900, w 560, between the
  player list and Kick Player; drawn and usable only on the host. Swaps like the
  skirmish one, but never two humans. Its second text is the shorter "Pick
  Keeper", to fit that width.
- Places tried: x 1480, y 1032 ran over the map picture; above Add Computer
  Player (x 64, y 1032, w 1360) covered a computer player's Keeper choice and
  Kick Player's box (tested).
- Button boxes in this window [verified in the data at 0x6B39E8]: player list
  x 64, y 536, w 1328, h 300; Kick x 64, y 1044, w 560, h 116; Add Computer
  Player x 64, y 1180, w 1360, h 116.
- To the left of the list there is no room inside the menu's 4:3 frame (the list
  starts at x 64), and the frame clips what is drawn outside it, so a button
  there needs WidescreenGui::BeginFullScreenMenuDraw and has no place at 4:3.
  [frame verified in our code; clipping unverified]
- Tested: after a swap, readying up never started the game. The start
  check (around 0x543038) waits for every human slot to have flag 0x10.
  A player's own slot is kept in 0x758334 (the slot the game gave it on
  joining); 0x544530 packs that slot (0x545970) and sends it as message
  0x68 whenever it changes, and receivers unpack it (0x545B10) into the slot
  number the message carries. Flag 0x10 is only ever set by that unpack (and
  0x18 for computers). So the ready went to the human's old slot.
- Now every machine remembers its human's network id (slot + 0x3B, read
  from the slot in 0x758334 the first frame it is in the lobby) and keeps
  0x758334 on the slot holding that id. 0x55AE30 (network place, only used
  for the local player number at the three start paths 0x5430D7, 0x543497,
  0x52824F) is hooked to return that slot too.
- [unverified] Whether the game's in-game network commands assume player
  number = 3 + network place; if so, a swapped human would command the
  wrong keeper. To be tested with two machines.
- Tested: swapping (host alone) made a second copy of the host. The lobby
  message handler 0x544060 (cdecl (message, size, sender, front end)) takes
  sender = the sending player's network slot, ignores it when it equals
  0x758334, and writes per-player data into that slot number; for message
  0x68 (a player's slot) a computer player there is first moved to the
  first empty slot [verified]. The host's own 0x68 comes back to it (as on
  a LAN), and with 0x758334 moved it was no longer ignored, so the host was
  written into its old slot again. Every machine now learns each network
  slot's player id the first time that slot holds a human, and the handler
  is hooked to turn sender into the slot that holds that id now.
- Tested: with the host swapped out of slot 0 (keeper 1, red), the other
  player got "host is not responding". The game ties the session host to
  its slot (network slot 0); where it checks is not found (the message is
  a game text). User's choice: the host's keeper never moves. Swap Keeper
  ignores the host's own row, as first or second pick; other humans can
  still be swapped with computer players.
