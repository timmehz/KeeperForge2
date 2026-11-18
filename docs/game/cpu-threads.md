# CPU threads (CpuThreads.cpp)

## Why the game uses one
- [verified] patch.dll's start routine (at 0x10001570) calls
  SetProcessAffinityMask(process, 1) when `SingleCore` in the `[Misc]` part of its
  `GOG\DungeonKeeper2\Config.ini` is not 0. Its default is 1. So every thread of the
  process is kept on the first logical processor.
- [verified] The game is not single threaded itself: it makes threads with
  CreateThread (0x63683E, 0x63873C), as do QMIXER.dll (sound) and WEANETR.dll
  (network). It plays videos and draws the pointer on threads of their own. KeeperForge2
  adds Steam's, the crash reporter's and MemoryWatch's, and the graphics driver has its
  own. With the limit they all take turns on one logical processor with the game.
- [unverified for every call, but what our hooks have seen] The simulation and most drawing run on the main thread. Nothing here splits
  that up: doing so would mean rewriting the game's world code, and it has to stay in
  step between machines. So more threads help the rest, not the game's own work.
- [reference: DDrawCompat's source] DDrawCompat has its own CpuAffinity setting and
  moves the game between processors. [verified] It also hooks SetProcessAffinityMask
  (0x101B79D0), and can return success without changing anything. It does not run with
  our renderers. With `renderer: original` it can undo or block this.

## What we do
- `cpu_threads` in config.yml: a number (default 4), `all`, or `game` to leave patch.dll's
  limit.
- At Initialize, after the game's DLLs have started (the runtime starts from the exe's
  start routine), SetProcessAffinityMask with the chosen processors, then read back. The
  log says what it got.
- Chosen by GetLogicalProcessorInformationEx: whole cores, from the fastest efficiency
  class first, so a performance core before an efficiency core. Only the first 32
  logical processors, as a 32-bit process has a 32-bit mask.
- Each machine can differ: this does not touch the simulation, so it cannot cause a
  desync by itself.

## Risks
- [unverified] Every release with patch.dll ran the game on one thread, so a race between
  its threads never had a chance to show. Crashes or sound and network oddities after
  this are a reason to try `cpu_threads: game`. Our own shared state between threads
  (Steam, the renderer's lock, wheel turns) already has locks or atomics.
- Tested alone, 2026-11-03, `cpu_threads: 4` of 8, Vulkan: read back as mask 0xF, was 0x1, as
  expected. 14 and 2 minutes in levels, no errors, exit code 0, same memory use. Frames in a
  level stayed at about 42 a second at 3840x2160, the same as before the change, under the
  limit of 60. The menu holds 60. So what limits a level is the main thread or the GPU work.
  Multiplayer not tested with this yet.
