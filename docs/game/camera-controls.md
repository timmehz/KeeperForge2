# Camera controls (CameraControls.cpp)

## Key actions
- Table at 0x759D8F: { key (DirectInput code), modifiers } per action, 8 bytes
  each. Modifiers: 1 Shift, 2 Ctrl, 4 Alt. [verified]
- 0x4097E0, is an action's key held: thiscall (interface, action, modifiers
  held, ignore modifiers) ret 0xC. Polled by the camera code (0x4066A0 and
  others). The interface is not used. [verified; the C export shows it as a
  plain function, the assembly shows ECX set and ret 0xC]
- 0x409740, does a pressed key match an action: thiscall (interface, key,
  action, modifiers, ignore modifiers) ret 0x10. Used for key presses (around
  0x40634A). [verified]
- 0x5B2A40, is a key held: cdecl (key). [verified]
- Actions [reference: Flame]: 0x0E zoom in (Home), 0x0F zoom out (End), 0x10 up,
  0x11 down, 0x12 left, 0x13 right, 0x24 ally window (A). In 0x4066A0, 0x0E and
  0x0F queue a camera command (0x4346E0) of -0x400 and +0x400, type 7, each
  poll. [verified use]
- Default keys [reference: Flame]: W, S, D and Y are not used alone; Ctrl + S
  and Alt + A are.

## WASD
- 0x4097E0 is hooked: an action that is not held by its own key is also held by
  W (up), S (down), A (left) or D (right), unless Ctrl or Alt is held, and only
  while the action's table key is its arrow key (checked at run time, so custom
  keys are left alone). The arrow keys keep working.
- 0x409740 is hooked: while the ally window's table key is A, pressing A no
  longer matches it and pressing Y does. The game's key options screen still
  shows A. [not changed there]
- [unverified] Whether the camera keys are polled while typing in-game chat. If
  so, typing W, A, S or D there moves the view. No chat-open flag was found.

## Wheel zoom
- 0x5DDA90, the DirectInput mouse's buffered data handler: thiscall (mouse,
  count) ret 4; data pointer at mouse + 0x30, entries of 16 bytes { offset,
  data, time, sequence }; offset 8 is the wheel. [verified] (Flame names the
  pointer + 0x2C, 4 lower, as with its button fields.)
- It is hooked to count wheel turns. Each notch (120) makes 0x4097E0 report zoom
  in or zoom out as held for 6 polls, so the game zooms through its own command.
  Turning the other way replaces what was left. Turns unused after 500 ms are
  dropped, as the zoom actions are not polled in menus.
- [unverified] Zoom per notch: 6 polls of 0x400 is about Flame's direct camera
  zoom of 50 per wheel unit, if the two use the same units.
- Windows' WM_MOUSEWHEEL is not used with the DirectInput mouse: its exclusive
  mode can hold it back. [unverified for this game; reference: Flame reads both]
  With `windows_mouse` on there is no DirectInput mouse, so WindowsMouse hands
  WM_MOUSEWHEEL and WM_MBUTTONDOWN to the same wheel zoom and middle click
  reset (game/mouse.md).

## Middle click camera reset
- Setting `middle_click_camera_reset` (default on). Pressing the middle mouse
  button eases the view's zoom and angles back to what they were when the level
  started. The spot the view looks at stays the same. [verified in-game]
- Camera fields (the bridge's camera, reached through world + 4 → session,
  + 0x10 → bridge, vtable + 0xD0 → camera) [verified]:
  - + 0xD68: the map point looked at (x, y, z).
  - + 0xD80: zoom. Its value doubled is the view distance (0x44D550).
  - + 0xD84, + 0xD86, + 0xD88: angles, 16 bits each, 0x800 a full turn. They
    are the turn, the tilt and a lean. 0x44D6D0 builds the camera's matrix from
    them. 0x44DD40 eases the lean back to 0.
  - + 0xD8A, + 0xD8E: two more view values that a move also eases.
  - + 0xDA8: the mode. The keeper views are 3, 4, 5, 9, 13, 14 and 15
    (0x44D870).
- The game's smooth view move (used when changing views, 0x44ACE0). Its
  targets are, in order: + 0xDB6 point, + 0xDC2 zoom, + 0xDC6 turn, + 0xDC8
  tilt, + 0xDCA lean, + 0xDCC and + 0xDD0 the two view values, + 0xDD4 the mode
  to end in. [verified]
  - 0x44BD70 thiscall (camera, ms), ret 4. With a time, it sets + 0xDDC (start)
    and + 0xDD8 (end) from the game's ms clock (0x5B5E90). With 0, it snaps to
    the targets and clears + 0xDD8.
  - The camera's frame update (0x44BED0, keeper views) eases each value towards
    its target until the end time, then calls 0x44BD70 (camera, 0).
  - While + 0xDD8 is set, the view's keys are not polled (0x44D590).
- How the reset works:
  - At each level start a flag is set. At the next poll of the view's keys in a
    keeper view, with no move running, the zoom and the three angles are taken.
    The keys are not polled during an opening camera path, so the values are
    taken after it.
  - The mouse data hook (0x5DDA90) sees the middle button: offset 0xE, data
    0x80 when pressed. The game passes it on as key 0xF2.
  - At the next poll (zoom out, action 0x0F, is polled once per pass of
    0x4066A0), the move's targets are set to the current point, view values and
    mode, plus the starting zoom and angles. Then 0x44BD70 (camera, 500) starts
    the move. A click unused after 500 ms is dropped.
  - Only this machine's camera fields are written. No game action is queued.
- [unverified] Whether the game's own key actions for zoom (type 7) and turning
  (type 8), which go through the turn's action list (0x4346E0), are sent to the
  other machines. The reset does not use them either way.
- [unverified] Whether the level start callback also comes when a saved game is
  loaded. If not, the reset uses the view taken at the last level start in this
  run (or does nothing before one).
- [unverified] Key 0xF2 is not bound to any action by default. If a player binds
  one to it, the middle button also does that.

Not tested yet.

## Wider zoom (wide_zoom)
- The camera's zoom by an amount is 0x449CA0, thiscall (camera, amount) ret 4 (raw bytes in our export; disassembled
  with ml + dumpbin) [verified]. It does nothing while the camera is held (+ 0xD92 bit 8, or a move under way at
  + 0xDD8). Otherwise zoom (+ 0xD80) += amount, kept from the minimum (+ 0xD50) to the maximum (+ 0xD54) - 1. The zoom
  keys and the mouse wheel (through the zoom key action) both end up there.
- With `wide_zoom: on` (default) it is hooked: the minimum is lowered by 2000 when it is above 2000, the maximum raised
  by 50000, the game's own code runs, and both are put back. Flame's amounts [reference: Flame increase_zoom_level].
  Other code that reads the limits (setting the zoom directly, the level's starting view) sees the game's own.
