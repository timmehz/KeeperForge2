# Frame timing and screenshots

Target: DKII-DX.exe v1.70 with the GOG patch.dll.

## Game turns vs frames
- [reference] The main loop runs input, the game session tick, then draws,
  with no frame cap of its own.
- [verified] The simulation only advances when the communication interface
  hands out a game turn. In the base class (0x521BA0) the turn period is
  `1000 / turnsPerSecond` ms, using the setting at `comm[1] + 8` and the ms
  timer 0x5B5E90. A turn is collected only once that time has passed. The
  single-player version is CLocalCommunication [reference name], whose
  0x522760 calls 0x521BA0 before collecting actions. So the frame rate does
  not change the game speed, as long as frames come faster than turns.
- [verified] 0x521BA0 hands out at most one turn per call (one call of
  vtable + 0x18, then it returns), so below the turn rate the game slows
  down: at 1 fps it moves about one turn per second.

## Running while unseen (BackgroundRunning.cpp)
- The game pauses itself when it loses focus. Its window procedure 0x558C70
  takes WM_ACTIVATEAPP (0x1C), WM_ACTIVATE (6) and WM_NCACTIVATE (0x86);
  with a low word of 0 it calls the 8 callbacks at 0x757D71 with event 1
  (inactive) and sets 0x757D5D = 1, 0x757D59 = 0; otherwise event 0
  (active), 0x757D59 = 1, and if 0x757D5D was set it clears it and sets
  0x757D61 = 1 [verified]. Logs showed no frames at all while minimized.
- With `run_in_background: on` and our renderers, the layer's window
  procedure gives those three messages to DefWindowProc instead of the game
  when focus is lost, so the game never pauses. The window is still
  minimized on focus loss.
- Tested: with only the "focus back" half reaching the game, the main
  menu's 3D background was black after a restore (buttons fine; the log's
  frame samples dropped from ~3500 lit to ~330). The held focus-loss
  messages are now handed to the game's procedure, in order, right before
  the first focus message that comes back, so it hears both halves as
  before, with no pause between them. Not tested yet.
- Seen = the game has focus and its window is not minimized. While unseen:
  - nothing is put on screen (a minimized window cannot show pictures);
  - campaign and skirmish (play mode 1 or 2) wait in the present hook until
    a second has passed since the last frame: 1 fps, and about one turn a
    second. Multiplayer (and the menus outside those modes) keep their frame
    rate, so the game does not slow down for everyone;
  - the process's audio session is muted on every output device (Windows'
    volume mixer), from a thread of our own; unmuted when seen. Windows keeps
    a program's mute between runs, so it is unmuted at start.
- The game's window gets its messages on its main thread, which is the one
  waiting, so coming back from 1 fps takes up to a second.
- [unverified] DirectInput: the game is no longer told to give up the
  keyboard and mouse when focus goes; Windows takes them from it anyway, and
  focus coming back runs its "active" callbacks as before. To be tested.
- [verified] The finished frame is shown by 0x5587F0 (thiscall on the game
  object, no arguments), once per frame from the main loop and from loading
  and menu loops.

## GOG patch.dll frame pacing
- [verified] patch.dll reads `%APPDATA%\GOG\DungeonKeeper2\Config.ini`
  (0x100077C0). "Vwait" (default 1) lives at patch.dll+0x24DB4. "CpuIdle"
  (default 0) is a Sleep in ms per frame, and with it set, timeBeginPeriod(1)
  is called.
- [verified] Its FakeSurface::Flip sleeps CpuIdle ms, presents, then with
  Vwait 1 calls WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN), or flag 4 for
  other values. The read is `mov eax,[0x10024DB4]` at patch.dll+0x2EEA.
- [unverified] Together with DDrawCompat's own vsync this can wait for two
  vertical blanks per frame (~30 fps on 60 Hz). We saw ~32 fps in-game.
- FrameRate.cpp: with frame_rate set, Vwait is cleared in memory (after
  checking the instruction) and frames are paced with a QueryPerformanceCounter
  limiter in a hook on 0x5587F0.

## Panel text (GUI views) and frame rate
- [verified] In-game panels (the level objective, the info bar, tooltips)
  put their text into a "view". A view is a set of 128x32 tiles sized from
  the screen (+0x27F tiles wide, +0x283 high, pixels at [view], size
  [view+0x293]).
- [verified] The functions involved (all thiscall on the player interface):
  - 0x42CA70 (view, patterned), ret 8: clears a view (zeros, or 0x42C9B0's
    fill).
  - 0x42CB60 (view, x, y, text, color*, alignment, clear first, font by
    value, upload), ret 0x70: draws text into a view. With upload set it
    calls 0x42CDF0, which uploads every tile one by one. It destroys the
    font copy with 0x62C2B0.
  - 0x42CAB0 converts wide text and calls 0x42CB60.
  - 0x42CEE0 (view, x, y, flags), ret 0x10: draws the view on the screen.
- [verified] Panels redo this every frame. The level objective clears
  with 0x42CA70, then draws each line with upload on, so the whole view is
  uploaded about 6 times per frame. At 3840x2160 that took up to 190 ms
  per frame (4 fps), measured with a temporary profiler.
- GuiTextCache.cpp:
  - remembers each view's last run of calls (from one clear to the next),
    comparing everything but the font copy;
  - leaves out a repeated run, including its clear, which is held back and
    done late if a call differs;
  - turns off the per-line uploads and uploads a changed view once, right
    before 0x42CEE0 draws it;
  - redraws each view for real every 5 s;
  - compares only the 5 color bytes the game reads (0x62C3C0): the ally
    window's callers keep them on the stack, so later bytes are left over from
    other calls and made runs look different [verified in the export];
  - draws a view in full for 0.5 s after its text changes, as a run that
    changes partway only draws from the change on.
- [verified] The ally window (window 42) draws its text into the view at
  interface + 0x1984 (0x4250F0), through the same functions.
  Tested: the objective panel stays at the frame limit.

## Mouse pointer in screenshots
- [verified] The pointer image is a game surface wrapper whose DirectDraw
  surface is at +0x24; the copy 0x5B57F0 reads it without a check (via
  0x5B8150), and it has been seen empty (crash after a minimize and
  restore). Screenshots.cpp skips the copy when the frame or the image has
  no surface, and logs when the image loses or regains it.
- [verified] The pointer is a software cursor that its own thread
  (0x5DB8E0 loop) draws straight onto the screen. The drawer is at
  0x79F638: image +0x54, rect +0x194, position +0x1C4/+0x1C8, screen
  +0x224, transparent copy with flags 1 (0x5DC530). Screenshots.cpp copies
  it onto the frame the same way before the screenshot reads it. Tested.

## Screenshots
- [verified] The screenshot routine (around 0x558A40) creates SCRSHOTS, finds
  the first free name from "%s\SHOT%04d.BMP" (0x6BC66C; counter cached at
  0x75AE77), locks the screen surface and saves it with the BMP writer
  0x5BA650: thiscall (int* status, const char* fileName, surface description,
  palette), ret 0x10.
- [verified] The folder name comes through the pointer at 0x66F338 (to
  "SCRSHOTS" at 0x6BC5C8) every time: exists check 0x5B3BF0, create
  0x5B3900 (one level only), then the file name. KeeperForge2 points it at
  "KeeperForge2\SCRSHOTS"; the launcher has already created KeeperForge2.
- [verified] The screenshot itself is 0x558A40 (thiscall on the game object,
  no args, returns 1), called from the main loop at 0x52594B/0x52596B.
  [reference] In the main menu it is called before the menu's 2D layer is
  drawn, so menu screenshots had only the 3D scene (confirmed by an in-game
  capture). Screenshots.cpp now holds the request and runs it from the
  present hook, right before the frame is shown.
- Screenshots.cpp changes the pattern to ".PNG" (same length), so the
  free-name search looks at PNG files, and after the writer returns it
  re-encodes that file as PNG with WIC.
- Cooldown: after a screenshot is taken, further requests are ignored for 2
  seconds (real clock, UI only), so holding Print Screen does not save one per
  frame. Only with the present hook (the delayed path); untested.
- `screenshots_cleared` (config.yml, default on): at every game start the
  runtime deletes the SHOT*.PNG/BMP files in KeeperForge2\SCRSHOTS before any
  screenshot is taken, so numbering starts again at SHOT0000. Other files in
  the folder are left alone. Untested.
