# Co-op campaign (CoopCampaign.cpp)

Campaign levels played over the game's own multiplayer. The host chooses the
level and each player's part in the lobby; nothing is in config.yml. Not tested
in game yet. Facts marked [reference] come from the dk2-coop project (a fork of
Flame) in the reference folder. Every address it uses here was checked against
our export of DKII-DX.exe; what is untested is how they behave.

## The idea
- [verified] Game actions carry the tag of the player they are for (action
  + 0x10), and nothing ties a network slot to one player. So two machines can
  have the same local player number (0x75AD53) and send commands for one keeper
  over the ordinary lockstep. [reference: played that way by dk2-coop]
- [verified] Native multiplayer already runs with a different local player
  (world + 0x7243, read through vtable + 0x264, 70+ sites) on each machine and
  stays in step, so machines may also play different players.
- [verified] The game component's main routine (its call to 0x525D30 is at
  0x5253EA) runs once for the 3D menu (0x75AEAF set; 0x52F140 only starts the
  menu) and once per game (0x75AEAF clear, the play mode real). When a menu world
  ends the mode goes to 5, with the real one kept at + 8. First version hooked
  0x52F140: it ran before anything was chosen, so the chosen level never loaded
  (reported: "does not load"). Now the call at 0x5253EA sets the level name
  0x75AD57 (game settings + 0x10) and 0x75AD53, before the world is made. The
  lobby's map only carries the session. The lobby's lists of creatures, rooms and
  the rest get count 0.

## Campaign rules on the multiplayer transport
- Play mode (0x75AD4B) stays 3, so the network code is unchanged. These reads
  get 1 instead while in a session [reference for what each one is, bytes
  verified]:
  - level setup 0x50C800: 0x50C89D, 0x50C8FE, 0x50C982, 0x50C9C2, 0x50CB71,
    0x50CBF6 (fog, walls, gold, time limit, availability, AI, alliances);
  - 0x50A67A: a time limit defeats every keeper in multiplayer. Without this it
    still counts, for events a level times by it;
  - 0x4BB368, 0x4BB39A: a destroyed heart and the last keeper standing.
- The seventh level setup read, 0x50C803, is inside SessionOptions' hook, whose
  first bytes were moved. The mode really is 1 from the last level start callback
  until the next patched read puts it back.
- 0x4BEF14 reads the mode to spread a win or loss to the other keepers: 0 while
  everyone shares one keeper (no others), else the real mode, as a machine that
  did not win needs it to end the level.

## Parts
- Shared keeper: player 3. Good player: player 1 (GoodPlayer.cpp). Second keeper:
  player 4.
- [verified] GoodPlayer's "this machine controls" changes how the world runs (the
  keeper part of the tick, creature behavior, mana). In a session it asks whether
  anyone plays that player, the same on every machine.
- A keeper a person plays has no computer player: 0x4BB800 and 0x4BB850 do nothing
  for it, and the active flag (player + 0x10FA) is cleared in the first turns.
  [unverified: that levels start theirs only through these]
- A level with no player 4: 0x508C60 finds the local tag 0 and gives the main
  keeper instead, with a warning.
- A player 4 with no heart: at level start (0x50C800), its heart tile (+ 0xB43,
  + 0xB45, default 1,1, set by 0x4C5F60) still at the default means no dungeon.
  Then it counts as not played on every machine, and whoever chose it gets the
  main keeper (world + 0x7243 and the interface's tag). [unverified: that the
  heart has set its tile by then; if not, a real second keeper would be refused
  and the warning in the log will say so]

## Scripted camera paths
- [verified in code, reference for the problem] Script trigger type 72 (0x4C7B20)
  waits for the camera, which runs on each machine's clock. In a session:
  - 0x44A370 loading a path skips this camera's own state checks by going on at
    0x44A3B5, as a machine in another state must still take it. A move in progress
    is finished first (0x44BD70). Path target + 0xDEC is kept through the load.
  - The path's end is a game turn: (2 * (points - 1 - first) - 1) half samples at
    30 samples a second. 0x44D8F0 only ends it from the world tick 0x50A450 hook;
    drawing and skip keys hold the last sample.
  - Action 4 (0x50E5C0) counts 500 ms of movement and its data3 of rotation in
    turns, paused during a path. Trigger 72 holds when these are done.
- The game's rolling saves (0x50FD10, every 30 s) keep a copy of these counters
  with their turn. If the turn goes back to a saved one they are restored, else
  cleared.

## Hands (CoopHands.cpp)
- [verified] One keeper has one list: + 0xB2 (16-bit tags), count + 0x1BF; a drop
  takes the last. [reference for the approach, addresses checked]
- 0x527130 (sending an action) marks the machine, 1 + lobby slot: pick-ups (kinds
  57 to 60) in data1 bits 12-15 (a tag has 12 bits), drops (61) in data3 bits
  28-31. The handlers 0x513480, 0x5134E0, 0x5135B0, 0x513600 strip the mark before
  the game reads the tag, on every machine.
- Timed pick-ups keep the machine in the action's unused data2 (0x4C09F0, 0x192 in
  the player). 0x4BC500 records an owner only when the thing really went in.
- 0x4C0EE0 (timed drop) first moves this machine's last thing to the end of the list.
- The interface sees only its own things, by return address: 0x5094B0, 0x5094D0,
  0x5094F0 from ten sites around 0x40C000-0x40E459, 0x4BCAB0 from 0x40AF59 and
  0x40E4B3, and 0x4BCAE0 from the hand tick, so a thing the other player won is
  dropped from the local list without being dropped in the world. Everything else
  (world, computer players) sees the whole list.
- Owners are saved after each player (0x4B8D40, 0x4B9250, 68 bytes), only in a
  session, as a machine out of step gets another's save.
- A possessed creature cannot be picked up (0x4C09F0, 0x4BC500).

## Possession (CoopHands.cpp)
- Action 55 with data1 low byte 2 is Possess. Its unused data3 gets 'POS' + machine.
  0x513340 keeps that for the cast, 0x4B7CD0 for each shot made, 0x4AEEA0 for when
  it arrives. 0x4483D0 and 0x448440 (camera into a creature; data { creature, ?,
  keeper }) only run on that machine. The world's possession is unchanged.
- After each turn, if the keeper's + 0x134 no longer holds this machine's creature,
  0x442820 brings the camera out (not during a path, whose saved mode at + 0xED2
  or deferred at + 0xDB0 decide).
- [verified] 0x40E910's local movement changes 0x6EC9E4, which computer players then
  read: put back after it, or machines drift. [reference for the effect]
- Health flowers: 0x48E8E1 and 0x595416 ask if the keeper possesses. In keeper camera
  modes 3, 4, 5, 9, 13, 14, 15 they now get no.

## Dig marks
- 0x442570 and 0x4425C0 draw the mark for actions 40 and 41 made on the other machine,
  for this keeper. An action list has its count at + 0, the actions (0x12 bytes) at
  + 0x11. [verified layout in 0x55EA10]

## Winning and progress
- Only while everyone shares one keeper: 0x4BEDC0 (the choice of ending), 0x4BF41E and
  0x4BF9EE (each ending's own refusal in multiplayer) read campaign, so the cinematic
  plays. 0x474100 and 0x4741D0 (its creature's waits) use the shared counters and go
  on at 0x47413B and 0x474211, past their own camera checks.
- 0x4BEACB and 0x4BEBD6 write the level's result: campaign on the host only. For the
  session, the level number (0x759B38, secret 0x75A661) is the chosen level's, and put
  back after, so it does not move the host's current campaign level.

## Alone: the skirmish screen
- [verified, from a log] A game started from the skirmish screen has play mode 2, and the
  level chosen in the multiplayer lobby was refused there (log: "not a co-op level (level 1,
  play mode 2)"). That is one reported "does not load".
- The level row and one part row are also on window 9 (level at 64,900, part at 64,1612).
  Started from there it is a solo session: mode 2 with the local transport, the campaign
  rules above, no progress written, and only this machine's part counts as played by a
  person. Not tested in game. [unverified: that the skirmish list's computer players stay
  out, as level setup reads campaign; other reads of mode 2 are not patched and not traced]

## Lobby
- [verified] Multiplayer's start (menu action 0x2A) calls 0x549F60, which reads the list
  back, at 0x530E2E, then 0x5435E0, which sends it. 0x549F60 also runs on leaving the
  settings screen, and the list is written when a screen opens. So on the host, and in
  skirmish, our read hook writes the current choices in instead of taking them from the
  list; otherwise a click made after opening could be undone at the start.
- Three clickable rows added to window 32's button list, drawn as text, left click
  next and right click back: level at 640,900; host's part at 1480,1612; other
  player's at 1480,1696. Places checked against the button table [reference];
  not seen in game.
- Host to others, for showing: message 0xD0 'K' 'F' level, part, part, twice a
  second, through 0x55AC20 on 0x758160 to target 0xFFFF. 0x544060 is hooked in
  SkirmishSwapKeeper.
- At the start: the free byte at + 4 of creature list rows 0, 1, 2, carried by the
  host's start message with SessionOptions' values. These have the last word.

## The Good player's result
- [verified] 0x4BE630 (thiscall (player, result)) gives a result at + 0x19A: 2 won, 1 and 3
  lost. Hearts and the "last keeper standing" rule only ever reach players with number 3 and
  up (0x4BEF14's loops check + 0x10FC), so a person playing player 1 never got one, and the
  interface's end of level (the code before 0x404259 reads the local player's + 0x19A) never came.
- In a session where someone plays the Good player, after each turn the main keeper's result
  decides it, the other way round. Not tested. [unverified: that a level's script ends with
  a result for the main keeper at all. The log now says, see below]
- [verified] With parts apart the campaign's ending is not used (0x4BEDC0 needs mode 1), so
  0x4BF410, which makes Horny for the winner, did not run in the reported hang. Its
  camera path and any Horny came from the level's own script. [unverified]
- Log lines for this: each camera path loaded, each result, and every 10 s while a path runs
  or a player has a result. They stop if the world has stopped.

## Known gaps
- Reported: a King picked up as the Good player did not show in the hand. Found (2026-11-18): the stock
  creature list has no In Hand animation for the King or the Stone Knight; GameFixes now uses their Stunned pose
  (game-fixes.md, "King and Stone Knight show nothing in the hand"). CoopHands
  logs each thing taken, with its type and owner, to tell these apart.
- None of this has been played on two machines. The hand, possession and desync code
  cannot be tested alone.
- Every level can be chosen; there is no unlock check against the host's progress.
- A guest can get secret unlocks from 0x4BEDC0's branch (0x5615F0, 0x561750).
- The other player's mouse while one possesses: dk2-coop changes its own replaced mouse
  code for this. Ours is the game's DirectInput mouse, not changed. [unverified: it may
  move as if in the creature]
- The Possess button is not grayed for the other player; the game refuses a second
  possession itself. [reference]
- A key can still skip the victory cinematic locally. dk2-coop blocks it (0x405010,
  0x42CEE0's caller 0x40422E) until the ending is done.
- How a won level ends after the cinematic in mode 3 is not traced.
- The hold-off in the mouse-side possession, co-op saves and more than two parts: out.
