# Mouse (WindowsMouse.cpp)

How the game reads its mouse, and `windows_mouse`, which takes the mouse from
Windows' messages instead of DirectInput. Ported from Flame's
`replace_mouse_dinput_to_user32` (on there too).

## Why
- The game's DirectInput mouse adds DirectInput's raw movement counts, times
  the mouse's multiplier (mouse + 0x30), to its position in screen pixels
  (0x5DD4A0). A count is a fixed number of pixels whatever the resolution, so
  at 3840x2160 the pointer crawls compared with 1024x768, and Windows' pointer
  speed and acceleration do not apply. [verified in the assembly]
- With the mouse from Windows' messages the game's mouse is put where Windows'
  pointer is, so it moves exactly as on the desktop. [reference: Flame]

## The game's mouse [verified]
- Input system 0x79CF90: keyboard (DirectInput) + 0x58, mouse (DirectInput)
  + 0x5C, the control keys object + 0x60, the mouse's position object + 0x64.
  A DirectInput device's own part is 0x10 into it.
- The game sets up its input in 0x557CB0: it lists the display devices
  (DirectDrawEnumerate), then the keyboard (0x5B2B20), then the mouse
  (0x5B2B60 -> 0x5BB1C0 -> 0x5BC060), and only later makes its DirectDraw. So
  whether our renderer is used has to be known from its install, not from the
  game having made our DirectDraw.
- 0x5BC060, the mouse's set-up, thiscall (input system, int* result) ret 4:
  makes the DirectInput mouse into + 0x5C (an error if one is there), gives it
  the control keys object (+ 0x60) and acquires it. 0x5BB1C0 then hands the
  pointer to 0x5DB6A0, which is fine with none.
- 0x5BC210, window (de)activation, thiscall (input system, int* result,
  active) ret 8: stores "active" in the keyboard's and the mouse's device parts
  (0x5DCCA0, which writes + 0x10) without checking for a missing mouse, then
  0x5BC3A0 (cooperative levels; checks both). The other users of + 0x5C
  (0x5BC0F0 release, 0x5BC280 event 7, 0x5BC3A0, the destructor 0x5BBEA0)
  check for none.
- DirectInput data handler 0x5DDA90, thiscall (mouse, count) ret 4, makes an
  input event per entry and hands it to the control keys object (mouse + 0xC):
  - movement (vtable 0x672900, 0x14 bytes): time + 4, "not handled" + 8 (1),
    axis + 0xC (0 x, 4 y, 8 wheel), amount + 0x10; through vtable + 0x14.
  - button (vtable 0x6728F8, 0x24 bytes): key + 0x10 (0xF0 left, 0xF1 right,
    0xF2 middle, 0xF3 the fourth), a point + 0x14 (0, 0), data byte + 0x1C
    (0x80 pressed), flags + 0x20 (8 pressed); through vtable + 0x18.
  - Each event's one function (0x5DD8B0, 0x5DD8C0) hands it to a listener's
    + 0x14 or + 0x18. The game queues the events and clears "not handled" once
    taken, so an event must live until then. [reference: Flame frees them in
    the game loop once handled]
- Control keys object (vtable 0x672888): + 0x14 (0x5BB660) passes a movement
  on to its next listener (+ 8); + 0x18 (0x5DD010) keeps Shift, Ctrl and Alt
  from the keyboard's keys (its + 0x20) and puts them into a button's flags
  (bits 1, 2, 4) before passing it on.
- Position object (vtable 0x6724A8): whole numbers + 0x10 and + 0x14, float x
  and y + 0x20 and + 0x24, movement not yet read + 0x28 and + 0x2C, multiplier
  + 0x30, bounds + 0x34 .. + 0x40 (min x, min y, max x, max y; set by
  0x5DD600). 0x5DD630 keeps the floats in bounds and works out the whole
  numbers. 0x5DD6A0 (from 0x5B2BB0) reads the movement and clears it.
- 0x5DD4A0, the position object's movement handler: adds the amount times
  the multiplier (x or y; anything else is left), clamps, redraws the pointer
  at once (0x5DB8E0 on + 0xC, at most so often) and passes the event on.
- Player interface 0x6CEA10: + 0x2722 in a menu (Flame's inMenu; read by the
  mouse handler 0x4065C1), + 0x13CB set to 1 while key action 0x2B or 0x2C
  (turn or zoom the view with the mouse) is held, which then moves the view by
  the mouse's movement (0x5B2BB0, called at 0x406ACE and 0x406B85).
- Mouse sensitivity of the options menu: an int at 0x759C21 (the Options
  slider 0x9C copies it to front end + 0x31176, Flame's mouseSensitivity).
- The game's window class (0x5B4FC0) has style 3 (no CS_DBLCLKS), so Windows
  sends it no double clicks; its procedure 0x5B5070 does nothing with mouse
  messages.

## windows_mouse
- 0x5BC060 is hooked: no DirectInput mouse is made (Flame's initMouse). 0x5BC210
  is hooked: without a mouse it only does the keyboard's part and 0x5BC3A0.
- Our renderer's window procedure (DirectDrawLayer, the game's window
  "_BullfrogLibScreen") hands every message to WindowsMouse first, before
  focus-loss messages may be held back for running in the background. So
  `windows_mouse` needs `dx11`, `dx12` or `vulkan`.
- Mouse movement, while one of the game's windows is in front: the pointer is
  read (GetCursorPos) and, in the normal view and menus, mapped to the game's
  screen pixels the way our renderer shows the screen (bars, scaling), then
  put into the position object's floats, with the movement kept up as
  DirectInput's would, and clamped (0x5DD630). Then a movement of nothing is
  handed through the control keys object, so the game does what follows a
  DirectInput movement (pointer redraw, listeners). Flame only sets the
  position and clamps; its own unused move_mouse makes such events.
- In a creature (the interface's player possesses one) and while + 0x13CB is
  set, unless in a menu: relative, as Flame. On entering, the pointer's place
  is kept and it goes to the window's middle; each move adds (Flame sets) the
  movement, counted as if the window were 640x480, times the sensitivity / 10,
  and the pointer goes back to the middle once 20 pixels away. On leaving, the
  pointer goes back where it was.
- Buttons: left, right, middle and the side buttons (both 0xF3, as Flame) as
  DirectInput's button events; Shift, Ctrl and Alt come from the control keys
  object. A release is only sent for a button held. Flame's own modifier
  tracking was dropped: the game overwrites those bits, and its key switch
  falls through (Shift also set Ctrl and Alt).
- The wheel: as DirectInput's movement on axis 8, and to CameraControls (wheel
  zoom). The middle button also goes to CameraControls (view reset). Their
  DirectInput hook (0x5DDA90) gets nothing then.
- Additions to Flame's: the pointer is held to the window (ClipCursor) while
  it is the foreground window, as DirectInput's mouse kept it; without that it
  would leave for another monitor, stop the game's mouse short of the edge
  (no scrolling) and let a click switch programs. When focus goes (another
  program, or one of our own windows such as the editor's) the pointer is let
  go and held buttons are released for the game.
- The editor's Windows pointer (LevelEditor) still works: its own position
  code needs the DirectInput mouse and simply does nothing without it, as the
  position already follows Windows' pointer. [unverified in game]
- [unverified] Whether Windows' arrow ever shows over the game's hand with the
  DirectInput mouse gone: the game hides it (ShowCursor in 0x5BC280) when it
  comes to the front. If it shows, Flame's hide_mouse_cursor_in_window
  (WM_SETCURSOR) is the fix.
- [unverified] The game's own Mouse Sensitivity option no longer changes the
  pointer's speed in the normal view (Windows' setting does); it still scales
  the movement in a creature.
