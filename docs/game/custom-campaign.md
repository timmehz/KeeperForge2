# Custom campaigns (design, not built)

Status: design approved 2026-11-18. Phases 1 and 2 written (CustomCampaign.cpp), not built or tested.

## Owner's decisions (2026-11-18)
- Names and folders as proposed.
- After a win the next level starts straight away.
- Reuse the skirmish screen's list, but as a list of **campaign names** first (a screen of its own built on that list).
  Picking a campaign plays its levels. A campaign never continues into a level that is not in its own list: after its
  last level it ends (back to the campaign list).
- The game's **briefing screen** is shown before each level, with the text from campaign.yml.
- campaign.yml gives each level's **main objective** and **sub objectives**; the in-game objectives screen (the "O"
  button) shows them too.

## Phases
1. Campaign list, starting levels, chaining within the campaign, progress.yml.
2. Briefing screen and the "O" objectives with campaign.yml's text.

Flame has a "Custom Campaigns" button in the Single Player menu, but it is switched off in Flame and unfinished: it
opens an empty copy of the skirmish map list and loads nothing [reference: Flame custom_campaign]. So this is our own
design, built mostly from parts KeeperForge2 already has.

## What a player sees
1. Single Player menu: a new **Custom Campaign** button (next to our level editor's button).
2. It opens a list of the installed campaigns: name, how many levels, how far the player got.
3. Picking one (tick) lists its levels in the same window ("N. title", "Won" for those won), the first not won
   chosen. Any level can be picked; none is forced. The cross goes back to the campaigns.
4. A level's tick shows its briefing, whose tick starts it with campaign rules (the level's own keepers, fog, walls,
   triggers, no skirmish lobby lists) and the campaign's ending (Horny comes for the Portal Gem).
5. After a level (won or lost) and the game's results screen, the campaign's level list shows again, with the next
   level chosen after a win, the same one after a loss. Nothing starts by itself (owner's decision, 2026-11-19).
6. Saving and loading work as in any game (the save holds the level; progress is kept apart, below).

## A campaign on disk
```
KeeperForge2\campaigns\<folder>\
    campaign.yml         name, author, description, the levels' texts and pictures
    1.kwd, 2.kwd, 3.kwd ...   the levels, played in this order (numbered from 1, no gaps)
    <name>Map.kld ...    each level's .kld files under the names its .kwd lists (as the level editor saved them)
    briefing.png         optional: the briefing's picture (any name; PNG, JPEG or BMP)
    progress.yml         written by KeeperForge2: levels won (per player profile not needed: one per install)
```
Every level must be in the campaign's folder. Its .kwd is renamed to its number; its .kld files keep their names,
because the .kwd lists them by path (such as `Data\Editor\Maps\RiverMap.kld`) and the game's reads of them are sent to
the file of that name in the campaign's folder. A numbered .kwd after a gap (1, 2, 4) is left out with a warning, and
a `levels` line in campaign.yml is not used (a warning says so).

campaign.yml (same simple format as config.yml; a level's lines are prefixed with its number):
```
name: The Keeper's Revenge
author: someone
description: Six levels after the end of the main campaign.
image: briefing.png
1.title: The Underground River
1.briefing: The heroes have found a way in by water. Stop them before they reach your heart.
1.objective: Destroy the Lord of the Land.
1.subobjectives: Claim the Temple | Keep your Dungeon Heart alive
1.image: river.png
```
Keys are the level's number, then `.title`, `.briefing`, `.objective`, `.subobjectives` (split by `|`),
`.image`. `image` without a level is the picture for every level that has no `.image` of its own. Pictures are files
in the campaign's folder, at most 4096 pixels a side; they are shrunk to fit the space left of the briefing's boxes,
keeping their shape, so a tall picture (about 9 wide by 16 high) fills it best.
- Levels are read from the campaign's own folder (ModData's redirect), so a campaign never touches the game's Data
  folder.
- A campaign may carry its own creature list and other shared lists the way any level can (level-files.md).
- Mods can ship campaigns: a mod's data folder may hold `campaigns\<folder>` too [to decide: see questions].

## How it works (game side)
- Button: added to the Single Player window like the level editor's (LevelEditor.cpp, "Single Player menu buttons").
- The list: first version reuses the skirmish screen's list, filled with campaigns instead of maps (as the editor
  reuses that screen), so no new window art is needed. A window of its own can come later.
- Starting a level: set the level name (0x75AD57) before the world is made, as CoopCampaign does at 0x5253EA, and give
  level setup's play mode reads campaign (the six reads listed in coop-campaign.md), as the editor and the co-op
  campaign already do. Play mode stays 2, so nothing campaign-only (the main campaign's level number 0x759B38, its
  map, its progress) is touched.
- Result: the level result writes (0x4BEACB, 0x4BEBD6) say won or lost; a win is recorded in progress.yml. The game's
  own campaign progress write is skipped, so the real campaign is never changed.
- Next level: after the end-of-level screens the game goes back to the front end; there KeeperForge2 starts the next
  level the same way as from the list. [to research: the exact place, so the debrief still shows]
- Cinematics and the campaign's 3D map are not used. A level's own intro/outro speech and triggers play as usual.

## Not in the first version
- Multiplayer (a co-op custom campaign): later, on top of CoopCampaign.
- Branching or secret levels, a campaign map, between-level movies.
- A window with its own look; the in-game save menu buttons.

- Mods ship campaigns by installing them into `KeeperForge2\campaigns` (no data folder lookup). Progress is one
  progress.yml per campaign, per install.

## Phase 1 as written (CustomCampaign.cpp) [verified code; the flow is untested]
- Button: LevelEditor's Single Player list gets "Custom Campaign" (id 9101, text id 60001, image 5) at 948, after Editor;
  Skirmish moves to 1212 and Load Game to 1392. It works like Skirmish (the camera flies to the skirmish screen) and
  tells CustomCampaign the list is wanted; the skirmish screen itself never shows (below).
- Campaigns are read from `KeeperForge2\campaigns\*\campaign.yml` (name, texts; read only, never rewritten). Its levels
  are the folder's 1.kwd, 2.kwd, ... up to the first number missing (999 at most); a folder with no 1.kwd is left
  out with a warning.
- The front end (0x70D6D8): its menu windows are a list from the pointer at + 0x30FCE, each with its id at + 0x44
  (9 the skirmish screen, 34 the map list, 7 the briefing), a shown flag at + 0x48 and the next window at + 0x62; the
  game's own "window shown" lookup (0x5322D0) takes the first shown one [verified]. + 0x6016 is not the window shown:
  it is only where the map list was opened from (9 or 32), set by actions 0x48/0x57/0x4D/0x58 [verified; the first
  test read it as the window shown, saw 0 and never opened the list]. Its chosen map name is
  + 0x6580 (0x40 wide characters). Its menu action handler 0x5306F0 is cdecl (argument, action, front end).
- Menu buttons [verified]: a button's click (0x528610) calls its action (button config + 0x16 and + 0x1A as the
  first two arguments), then hides the button's own window when config + 0x12 is set, then shows the window named at
  config + 0xE. A menu action on its own switches no windows, so code that runs actions without the button must hide and
  show the windows itself (the second test left the skirmish screen drawn over the Single Player menu for this reason).
  The Single Player menu's buttons use click 0x538000: its first argument's low word is a window to hide, its high word
  one to show, and the second's low word a camera step. The skirmish screen's map button is action 0x58 (argument 0x22),
  shows 34 and hides its own; Back is 0x18 and hides its own; Start is 0x5A and hides its own. The map list's tick
  (0x4E) and cross (0x4F) show nothing (one pair hides its own window); their actions show the skirmish screen again
  through 0x5321A0 (thiscall (front end; window to show, window to hide)). The briefing's cross (0x0A) hides its own,
  its tick (0x0C) hides nothing.
- The skirmish screen is shown by the step 0x538AC0 (cdecl (front end)) once the camera has flown there: it shows
  window 9 and runs action 0x15 (the screen's set up, its map list). It is the same function SkirmishSwapKeeper
  hooks as the skirmish setup, so its hook calls CustomCampaign (a second hook on it was refused, and the third test
  showed the skirmish screen for a moment); CustomCampaign hooks it only when SkirmishSwapKeeper has not. After it, when the campaign list or
  a campaign level is wanted, window 9 is hidden again in the same call and the campaign list or the level's briefing
  shows instead, so the skirmish screen is never drawn. The campaign list opens as the map button does: action 0x58
  (0x22), window 9 hidden, window 34 shown, then our rows put in.
- The map list window's widgets (table at 0x6B2F48) [verified]: 474 the title (draw 0x536700, title picture 9),
  231 the list's frame, 687 the chosen map's preview (draw 0x537C30, only while front end + 0x30C1E is 1 to 3; the map
  list's action sets it to 1), 468 the chosen map's details (0x540600), 470/471/472 the column headings (texts 1544,
  194, 1545), 477 the list itself (click 0x5416E0), then the cross and tick. While the list shows campaigns, + 0x30C1E
  is set to 0, so no preview is drawn.
- Window titles are pictures the front end makes once when it starts (0x536850): for each text id in the table at
  0x6B8468 it makes two 0x28-byte picture records (0x5B5270 sets one up, 0x5B52C0 gives it pixels) at front end
  + 0x360C and + 0x39A4 (0x28 apart per title) and draws the text into them with 0x536370 (thiscall, a packed
  settings block), writing the size at + 0x3D3C (0x10 apart). The title widget draws the picture its text id field
  names (widget + 0x30, from button config + 0x40; config + 0x44 is something else). The map list's title is 9 (text
  2800); title 6 is text 145, Load Game's.
  Our "Custom Campaigns" is made the same way into records of our own the first time the list opens, and swapped with
  title 9 ("Map Select", shared with the multiplayer map list) while the list shows campaigns, then swapped back.
- The column headings (470, 471, 472) are text widgets: their draw (0x5323B0) asks for the text of the widget's text
  id field (+ 0x30) each frame and draws nothing while it is 0. A window's widgets start at window + 0x6A, each with
  its next at + 0x7C and its id at + 0x74 [verified]. While the list shows campaigns the headings' text ids are
  60002 ("Campaign") and 60003 ("Won", over the "won / levels" column), given by the text-by-id hook LevelEditor
  already has (0x553A60), and the size heading's is 0; the game's are put back when the list closes.
- The list's background (frame widget 231, draw 0x541940) is one picture, 0x7401F0, with the size column tinted green.
  The picture draw (0x52CF90) takes its width from the picture record's + 0x1C (height + 0x20, rows stepped by the
  pitch + 0x24), so while the list shows campaigns that width is cut to the first two columns (880 of the list's 1240
  layout units) and put back when the list closes. The row highlight (picture 0x740218, drawn by the list's draw
  0x5410C0 from the same left edge for the chosen and pointed rows) has a green part over that column too, so it is
  cut to the same width.
- The chosen map's details (widget 468, draw 0x540600, cdecl (widget, front end)) [verified]: it finds the chosen row
  (0x740348) in the list at 0x740354 by counting fields (4 per row), draws the first field in a banner, then
  "Max Players : <second field>" (text 0x5D4, format "%s : %s"), then the third field; each is skipped when the list
  ends before it. The walk to the next field stops at a 0 or at 0xFFFF, and only a stop on 0xFFFF means "no field"
  (a 0 followed by 0xFFFF still gives an empty second field, and "Max Players :" is drawn) [verified, tested]. It is
  hooked: while the list shows campaigns it is given a list of 4 empty fields per row before the chosen one, then the
  campaign's name with 0xFFFF right after it, then a 0, so only the name banner shows. The text conversion (0x553B20,
  through 0x62E1F0) reads up to a 0 and writes a 0 byte for a character the game has no mapping for, such as 0xFFFF.
- The map list (0x540A30 gets field n, 0x540B20 counts 0s / 4, both raw bytes): the skirmish one is a wide text at
  [0x740354], 4 fields per map each ended by 0, then 0xFFFF; made by 0x53FEB0 (list, 1) from GameAlloc when the front end
  starts. While the list shows campaigns it holds our rows (name, "won / levels", empty, folder; from GameAlloc), with the
  count (0x6B442C) set to -1 so it is counted again; the game's own are put back when the list closes.
- A row click (0x5416E0, cdecl (row, ?, front end)) stores the row at 0x740348 and loads that map's preview by name; for
  our rows only the row is kept.
- The list's tick sends 0x4E (keep the clicked map), its cross 0x4F (back to the map chosen before, front end + 0x6033).
  With a campaign row chosen, the tick runs the game's own (which shows the skirmish screen again), then at once hides
  it and starts the campaign's next level (its briefing first, if it has texts); with no row chosen the tick does
  nothing. The cross runs the game's own, then leaves as the skirmish screen's Back does (window 9 hidden, action
  0x18), back to the Single Player menu.
- Starting: the level's file name goes to front end + 0x6580 and action 0x5A runs, which starts a skirmish (play mode 2,
  local player 3, level name from + 0x6580). While it starts and plays, a game file open (ModData's hook) of a .kwd or
  .kld under a "maps" folder whose file name is in the campaign's folder reads that one, so the level name can be the
  plain file name. Level setup's play mode reads get campaign (CoopCampaign's RulesMode), and the lobby's creature,
  room, door, trap and spell lists are emptied, as for the co-op campaign. The real campaign's progress is not touched.
- The result: PlayerSetStatus (0x4BE630, 2 won, 1 and 3 lost) for the local player (world + 0x7243). A win is written to
  progress.yml. Back in the menus (the game component starting for the menus), a win with a level after it starts that
  level where the skirmish screen would show; the last level won, a loss, or leaving goes back to the campaign list.
  If the skirmish screen shows some other way than its step, the next frame hides it the same way (and says so in the
  log).
- Picking a campaign whose levels were all won starts it again from the first level.

## Phase 2 as written: briefing and objectives [verified code; meanings of the ids unverified]
- The game keeps each level's briefing and objective texts in a briefing text table, loaded by level text number
  (0x553800, file names at 0x6BC100; the level's description + 0x63B8 picks the number, 40 when absent) and read by id
  through 0x553AC0 (cdecl (id), game text; an empty text when no table is loaded). A second table (0x553780, read by
  0x553A90) holds in-game texts.
- Where the ids show, from the game's code: 0 and 1 as the level's name (front end, read into 0x80-character buffers);
  2 in the briefing window's first box (under main text 1408) and first in the in-game Level Objective window (0x4207E0,
  under main text 0x580); 3 in the briefing's tall box (under main text 1409) and, with 4 and 5, under main text 0x581 in
  the Level Objective window. So 2 is taken as the main objective and 3 as the briefing [unverified: the stock texts
  are not read here].
- 0x553AC0 is hooked: while a custom campaign's level is in its briefing or played, ids 0 to 5 give campaign.yml's
  texts (made into the game's text form once per level with 0x553B20). Tested: the briefing window's title is
  "<0>" - <1>, its Main Objective box is 2, and its Sub Objective box lists 3, 4 and 5 one per line (there is no
  separate box for a story text). So: 0 "Level N", 1 the title (else the level's file name), 2 the objective then the
  briefing, 3 and 4 the first two sub objectives, 5 the others.
- The short briefing: the front end copies two flags from the last level it read the details of (0x535C30; bits 5
  and 6 of the level's flags word, to front end + 0x311B6 and + 0x311BA; 0 before any level is read). While either is
  set the title shows only text 1 (0x53A41D) and the sub objective box is left out (0x53ADB2) [verified]. After
  playing Secret1 one was set and the next briefing was the short one (tested), so both are cleared when our briefing
  opens. Other ids, and levels with no texts in
  campaign.yml, get the game's own. The log (debug) says which ids the game asked for.
- The briefing window is window 7 (cross action 0x0A, tick 0x0C, which starts the game component with what is set up).
  A level with texts in campaign.yml shows it before it starts: changeGui (0x5391F0, cdecl (0 hide or 1 show, window,
  front end)) hides the skirmish screen and shows window 7. Its tick hides window 7 and runs the skirmish start (0x5A)
  instead of 0x0C; its cross opens the campaign list (the cross's button then hides window 7). A level without texts starts at once.
- The in-game objectives: [unverified] that the "O" key opens the Level Objective window drawn by 0x4207E0.
- The briefing picture [verified code; untested]: the briefing window (config 0x6AEF38, id 7, its list 0x6AEF80) has
  its boxes from x 764 of its 2560-wide layout and nothing on the left. A widget of ours (id 9102, a copy of box 165
  with our draw) is added before the list's end at x 64, y 436, 640 x 1148. Its draw gets its box in frame pixels
  (GuiButtonRect 0x528570, GuiBoxToScreen 0x52C630), and draws a picture record with 0x52CF90 (thiscall on front end
  + 0x30D3F; out, frame front end + 0x2DD65, x, y, picture, no area, 0), which does not scale: the record is made
  (0x5B5270 / 0x5B52C0, freed with 0x5B5310) at the fitted size and filled from the file, each pixel the average of
  the file's pixels under it, packed by the record's own masks (red, green, blue, alpha at + 0, + 4, + 8, + 0xC, bits
  per pixel at + 0x10). The file is read with Windows' WIC decoder (as Screenshots writes PNGs), not with game code.
  Only drawn while our briefing shows.

## Research still needed [all to verify]
- After a lost level the menus came back through the skirmish screen's step to the campaign list (tested). Still to
  see: a won level going on to the next level's briefing.
- That level setup with the campaign reads and play mode 2 runs a campaign-style level fully (triggers, objectives, the
  level's own victory conditions), as the editor's test play suggests.
- That the result writes 0x4BEACB / 0x4BEBD6 run for play mode 2 with the campaign reads.

## The level list, results and ending (2026-11-19) [verified code; untested]
- The list window (34) shows our campaign rows or one campaign's level rows (name "N. title" or "Level N", "Won",
  empty size, the level's file name). A campaign's tick and a level list's cross switch the rows in place
  (new rows from GameAlloc, the count set to -1, the chosen row set). The tick and cross buttons (467 and 529 in
  window 34) hide their window after their action (widget + 0x5D, button config + 0x12, read by the click 0x528610
  after the action); for a switch that flag is cleared for the click and put back on the next frame. The first
  heading's text (60002) reads "Campaign" or "Level" by the rows shown.
- The skirmish results screen (window list 0x6B1868: title text 18, the table widget 195 drawn by 0x53D550, its tick
  action 0x18) reads front end + 0x3033C without a check. That table is made by 0x53C520 with a row per player, for a
  skirmish (game type 2 at 0x75AD4F, a copy of 0x75AD4B) the chosen skirmish map record's player count (record
  + 0x6608 of the 0x890-byte records at + 0x657C, players + 0x80). Our levels are not among those records: after
  winning one the table was not made and the game crashed at 0x53D682 (tested). 0x53C520 is hooked: after one of
  ours, a valid record is chosen and given 1 player for the build, then both are put back.
- The ending: PlayerSetStatus (0x4BE630) chooses the campaign's ending when the game type (0x75AD4B) read at
  0x4BEDC0 is 1. CoopCampaign's ending read (EndingMode) now says campaign during a custom campaign's level (its
  progress read does not: the real campaign's progress is never written). Tested before this: the Portal Gem was
  dropped and Horny did not come. In that branch a win checks the level's flags byte 0x6F2957: bit 3 unlocks a secret
  level of the real campaign (0x5615F0), bit 4 another (0x561750), neither calls 0x4BF410 (Horny for the Portal Gem,
  an object of kind 0x1C). Around PlayerSetStatus both bits are cleared for our levels and put back after.
