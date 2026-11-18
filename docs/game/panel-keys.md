# Side panel keys (PanelKeys.cpp)

Keys that open the side panel's tabs, set in config.yml. Not tested in game yet.

## The tabs
- [verified] The tab buttons are 0x50-byte entries from 0x690C58. Each has click
  0x416C20 and passes (button id, panel, interface):

  | Button | Panel | x in the panel | Tab |
  |---|---|---|---|
  | 0xF8 | 4 | 0 | Creatures |
  | 0xFB | 1 | 136 | Rooms |
  | 0xFA | 2 | 272 | Spells |
  | 0xF9 | 3 | 408 | Workshop |

- Panel 4 being Creatures is from level setup (0x419B15). The other three names are
  their left to right order in the game. [unverified in game]
- 0x416C20 opens the side panel if it is closed, and with Shift held pins the panel,
  so a key does what a click does.

## Sell
- [verified] The Sell button (id 255) has click 0x4163F0, which the export leaves as raw
  bytes, read by hand: it sends the click action (0x422600 with 0 and the id), sets
  interface + 0x13DB to 4 and + 0x13F0 to 0, then clears a tutorial highlight on this button
  if a script set one (world vtable + 0x2F8, + 0x2FC, + 0x300).
- It reads the id from the object at interface + 0x1166. The writers we can see only set that
  for the chat box or to 0, so it is not called from the key. PanelKeys does the first two
  steps itself, with the id. Not the highlight, so a tutorial's flashing Sell button would
  keep flashing after the key. [unverified: whether any level does this]
- Key 0x29 (` and ~) has no action in the game's defaults. [reference]
- [verified] Pressed again it lets go of Sell the way 0x407580 and a right click do: + 0x13DB,
  + 0x13F0 and + 0x13E4 to 0, then 0x40ABC0 on the cursor at interface + 0x12F6 with (0, 0). The
  Sell button still works as before: it only ever sets the tool.

## Escape and the options menu
- [verified in code] Action 24 (default key 1, Escape, set by the 0x323 write in the key defaults)
  calls 0x428710, the Options button's own click, from the key event 0x405010 at 0x4057F9.
- Before that the event returns on: Escape during a camera path or scripted move or turn (it
  skips them), the interface + 0x10C6 object's + 0x10 or + 0x14 set (0x52C520), + 0x111, + 0xFD,
  camera state 10, and a camera refused by 0x44D590.
- Reported in game: Escape does not open it. Cause not found: nothing in the code above explains it
  on its own. So `options_key`: after the game's key event, if + 0x2722 (menu open) did not change,
  PanelKeys calls 0x428710 itself, unless typing or a path or move was running. It reads the flag
  after the event, so the menu cannot be toggled twice.
- The first 8 presses log what the event did and the values of those checks. No such line at all
  would mean the key event never gets Escape, and something before it takes the key.

## The key event
- [verified] 0x405010, cdecl (key, pressed, modifiers, interface), stored at interface
  + 0x134B. It gets every key of the view. Hooked, and the game's own handling still
  runs afterwards.
- [verified] Keys 1 to 4 are the game's actions 6 to 9, read only while a creature is
  possessed (keeper + 0x134). So the tab keys do nothing then. With Alt they are
  actions 59 to 62, so nothing with Ctrl or Alt held opens a tab.
- Also off when interface + 0x10E2 is set (the key event itself then only takes Escape),
  for a spectating player (+ 0x76 bit 0x80000), and during a camera path.
- In a co-op game one keeper is shared, so while the other player possesses a creature
  the keys do nothing here either.
- [unverified] Whether the key event sees keys while typing in multiplayer chat. If so,
  typing 1 to 4 there changes the tab. Same open question as WASD (camera-controls.md).
