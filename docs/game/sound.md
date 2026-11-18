# Sound

## The sound system
- The game's sound system is one object, pointer at 0x75B468, vtable 0x66F7FC [verified]. Methods used here (all
  thiscall):
  - + 0x1C (0x567790): (bank name, category name) ret 8, returns a category id. Things look their categories up when
    their types are made, e.g. a creature type's two (creature data + 0x6E3, + 0x6E7) under bank "GLOBAL" [verified].
  - + 0x20 (0x5678F0): play at a position, (old handle, category, sound, int position[3]) ret 0x10, returns the new
    handle. A thing keeps its handle (a creature at + 0x2E) and passes it back on its next sound [verified].
  - + 0x24 (0x567810): the same without a position, (old handle, category, sound) ret 0xC [verified].
  - + 0x38 (0x567B30): four values ret 0x10, copied into a command (vtable 0x6085C0) for the sound thread
    [verified]; what the values mean is not known yet. Some callers pass a thing's handle in it, likely to stop or
    move its sound [unverified].
- Library papers: the "Book Explosion" effect (Effects.kwd) has elements named Page1 and Page2, the likely source of
  the flying paper sound [unverified].

## Sound log (log_level debug)
- SoundLog.cpp hooks the four methods above only when config.yml's `log_level` is `debug` or `trace` (nothing is hooked
  at other levels; there is no setting of its own). It writes, at Debug level (the log file and the console), each
  category lookup (id and "bank/name"), and each play and command with the game address that asked for it (as an
  address in our export) and the game turn.
- Each (play or command, caller, category, sound) is written out three times, then only counted; the counts are
  written every 30 seconds.
- Use: set log_level to debug, make the bug happen, note the game time, and look for a play near that time whose sound
  never stops.
