# KeeperForge2

KeeperForge2 makes **Dungeon Keeper 2** work better on today's computers. It also lets people make **mods** for the game.

You start the game with the KeeperForge2 launcher instead of the normal way. The launcher starts Dungeon Keeper 2 and adds KeeperForge2 to it while it runs. Your game files are never changed.

> **Note:** KeeperForge2 is a free, non-commercial fan project. It is not made by, approved by, or connected to Electronic Arts, Bullfrog Productions or Valve. Dungeon Keeper is a trademark of Electronic Arts; Steam is a trademark of Valve. All trademarks belong to their owners and are used here only to say which game KeeperForge2 works with.
>
> You need your own copy of the game. KeeperForge2 does not include any of the game's files, art, sounds, text or code, and it does not get around any copy protection. It changes the game only in memory while it runs; your game files are never changed.

---

## What it does

### Looks better
- Plays at your screen's full size, even on wide and 4K screens.
- Menus and side panels keep their shape instead of looking stretched.
- New graphics engines: **Vulkan**, **DirectX 11** and **DirectX 12**.
- Smooths jagged edges (anti-aliasing): FXAA, SMAA or MSAA.
- Lets you see more of your dungeon with a wider view.
- Plays videos full screen.
- Plays in a window with no border, so switching to other programs is easy.
- Turns on the game's own rippling water and lava effect (bump mapping), including the Temple's pool. You can pick water, lava or both.
- **Ray traced lighting** (off by default): soft shadows in corners, shadows from torches and room lights, reflections on water and lava, and light bouncing off nearby surfaces. It uses KeeperForge2's own ray tracer, so it works with every graphics engine on any graphics card with compute shaders. You can choose how often it is worked out, to go easier on your computer.
- Creatures and objects are drawn with all their detail at any distance, and with rounder shapes.
- Sharper floors and walls far away (mipmaps and anisotropic filtering).

### Runs smoother
- Choose your own frame rate, like 60, 120 or 144. The game itself does not speed up.
- Keeps running when you click away from it. While you can't see it, it goes quiet and slows down to save power.
- Lets the game use up to 4 GB of memory instead of 2 GB (still being tested).
- The mouse comes from Windows: it moves at your Windows pointer speed, without lag, at any screen size.
- The title screen at start is short, and a key or click skips it.

### Easier to play
- Move the camera with **W, A, S and D**.
- Zoom in and out with the **mouse wheel**, much further out than before.
- Keys **1 to 4** open the side panel tabs.
- One key turns Sell on and off.
- Screenshots are saved as PNG pictures in their own folder.
- **Autosave** in single player every few minutes of play, keeping the last few saves.

### More ways to play
- **Play online through Steam.** Every hosted game shows up in the game's own list, so you can join anyone's game, not just friends'. Games can have a password, and you can still invite friends from Steam.
- **Swap Keeper** in skirmish and in the multiplayer lobby.
- **Play as the Good player** (the heroes) in skirmish.
- **Mercenary Portal:** heroes can join your side once you build the rooms they like.
- **Dwarf workers:** your workers can be Dwarfs instead of Imps.
- **Type in the creature numbers** in the lobby instead of dragging the sliders.
- **Custom campaigns:** play campaigns other people made, from the new Custom Campaign button in the Single Player menu, with their own briefings and pictures.
- **Keeper colors:** pick each keeper's color on the skirmish screen with a color wheel. It shows on claimed floors, wall tops and the minimap.
- **More to do when you possess a creature:**
  - Hit a Training Dummy or Training Target to train (it costs the same gold as the Training Room).
  - Hit your creature's bed to send it straight to sleep.
  - Hit a chicken to eat it.
  - Open your Prison with the bolt by its door and walk in: your creature and its followers become prisoners.
  - Jump into your Combat Pit: your creature and its followers fight everyone, to the death.
- **Co-op campaign** and an **in-game level editor** (both still being worked on).

### Bug fixes
KeeperForge2 also fixes bugs in the game itself. For example:
- A Hatchery now always gets the right number of chicken coops, no matter how you built the room.
- The Workshop now finishes each item at the right time.
- The chat box no longer crashes when Steam is open.
- Keys like Shift no longer get "stuck" after you switch windows.
- Chickens stay in the Hatchery, and hatch inside it.
- Creatures walk to the Treasury, Heart or room that is the shortest walk away, not the one closest through a wall.
- Hero parties no longer crash the game when the map is full of creatures.

The full list for each version is on the **Releases** page.

---

## What you need

- **Dungeon Keeper 2 from Steam.** Other versions, like GOG, are not supported yet.
- **Windows 10 or 11.**
- **For playing online:** Steam must be open. The `steam_api.dll` it needs comes with KeeperForge2 (see [Playing online](#playing-online)).

---

## How to install

1. Download the newest release from the **Releases** page.
2. Find your game folder. On Steam, right-click **Dungeon Keeper 2**, then click **Manage**, then **Browse local files**.
3. Copy `KeeperForge2Launcher.exe`, `KeeperForge2.dll` and `steam_api.dll` into that folder.
4. Start the game by opening **`KeeperForge2Launcher.exe`**.

The first time it runs, KeeperForge2 makes a new folder called `KeeperForge2` inside your game folder. It keeps everything there:

| Folder or file | What is in it |
|---|---|
| `config.yml` | Your settings |
| `Logs` | Reports that help find problems |
| `SCRSHOTS` | Your screenshots |
| `mods` | Your mods |

**To uninstall,** delete `KeeperForge2Launcher.exe`, `KeeperForge2.dll`, `steam_api.dll` and the `KeeperForge2` folder. You might also see a file called `KeeperForge2-DKII-DX.exe` in your game folder. It is a copy KeeperForge2 makes for the 4 GB memory setting, and you can delete it too.

---

## Settings

All the settings are in `KeeperForge2\config.yml`. Open it with Notepad.

Every setting has a short note above it that explains what it does. Change the word after the colon, save the file, and start the game again.

Here are some you might want to change:

| Setting | What it does | Example |
|---|---|---|
| `resolution` | The size of the picture | `auto` or `1920x1080` |
| `renderer` | Which graphics engine to use | `vulkan`, `dx11` or `dx12` |
| `anti_aliasing` | Smooths jagged edges | `smaa x4`, `fxaa x2` or `off` |
| `frame_rate` | Frames per second | `60`, `144` or `vsync` |
| `field_of_view` | How much of the dungeon you can see | `100` is normal |
| `wasd_camera` | Move the camera with W, A, S and D | `on` or `off` |
| `bump_mapping` | Rippling water and lava | `all`, `water`, `lava` or `off` |
| `steam_multiplayer` | Play online through Steam | `on` or `off` |
| `steam_lobby` | Who can see your games | `public` or `friends` |
| `steam_lobby_password` | A password for your games | empty means no password |
| `ray_tracing` | Ray traced lighting | `on` or `off` |
| `ray_tracing_quality` | How detailed the ray tracing is | `high`, `medium` or `low` |
| `ray_tracing_rate` | How many times a second it is worked out | `30`; lower is lighter |
| `texture_filtering` | Sharper floors and walls far away | `anisotropic x16`, `trilinear` or `off` |
| `model_smoothing` | Rounder creatures and objects | `low`, `medium`, `high` or `off` |
| `autosave` | Minutes between autosaves | `5` or `off` |
| `windows_mouse` | The mouse from Windows | `on` or `off` |

If you type something wrong, don't worry. KeeperForge2 fixes the file by itself the next time it starts, and it puts that setting back to normal.

---

## Playing online

1. Make sure `steam_api.dll` from the KeeperForge2 download is in your game folder, next to `KeeperForge2Launcher.exe` (or in the `KeeperForge2` folder). It is Valve's 32-bit file from the free [Steamworks SDK](https://partner.steamgames.com/doc/sdk), which Valve allows games to ship. The game does not come with one.
   - KeeperForge2 was made and tested with `steam_api.dll` version **10.24.16.75**, the one in the download. To see a file's version, right-click it, click **Properties**, then open the **Details** tab.
2. Make sure Steam is open, then start the game with the launcher.
   - If KeeperForge2 finds `steam_api.dll` the first time it makes `config.yml`, it turns `steam_multiplayer` and `steam_player_name` on for you. Without it, they start off. If you add the file later, set both to `on` in `config.yml`.
3. In the game, go to **Multiplayer**, then **TCP/IP**.
   - **To host,** click **Create**. Your game shows up for everyone running KeeperForge2.
   - **To join,** pick a game from the list and click **Join**. The list updates every 15 seconds. Press **F5** to update it right away (once every 5 seconds at most).
4. You can also invite friends from your Steam friends list or the Steam overlay (**Invite to Game**).

**Passwords:** set `steam_lobby_password` in `config.yml` to give your games a password. Only players who type the same password in their own `config.yml` can see and join them. Leave it empty for no password. If your password has a `#` in it, put it in quotes.

**Friends only:** set `steam_lobby: friends` if you only want friends you invite to see your games.

Everyone playing together needs the same version of KeeperForge2 and the same mods.

While you play online, Steam shows you as playing **Spacewar**. That's normal: KeeperForge2 uses Valve's free test app (Spacewar, app 480) for its online lobbies, so anyone can play together no matter where they bought the game.

Don't want to play online? Set `steam_multiplayer: off` in `config.yml`. Then you can start the game without Steam open.

---

## Mods

Mods go in `KeeperForge2\mods`, each one in its own folder:

```
KeeperForge2\mods\
  Cool Mod\
    cool_mod.dll
    config.yml
```

KeeperForge2 loads every mod in that folder when the game starts. Each mod gets its own `config.yml` for its settings.

> **Be careful:** a mod is a program that runs inside your game. Only install mods from people you trust. Every KeeperForge2 mod must share its source code, so you can always check what a mod does.

The tools for making mods are still early. More is coming!

---

## Something went wrong?

- Look in `KeeperForge2\Logs`. The newest file tells you what happened. Lines that say **WARNING** or **ERROR** are the important ones.
- Try another graphics engine, like `renderer: dx11`.
- Try turning off the settings marked as being tested, like `memory_4gb: off` and `cpu_threads: game`.
- Still stuck? Open an issue here on GitHub, and attach your newest log file.

---

## How the game was studied

The `docs` folder explains how Dungeon Keeper 2 works inside, so others can build on it. Everything there is written in our own words from studying the game, the way you would take notes. It holds facts, like what a part of the game does and where it is, but no code, art, sounds or text copied from the game or from other projects.

---

## Building it yourself

KeeperForge2 is written in C++. To build it you need:

- **Visual Studio** with **Desktop development with C++** installed.
- **CMake**, which comes with Visual Studio.

Open the project folder in Visual Studio, pick the **x86 Release** preset, and build. KeeperForge2 must be built as **32-bit (x86)**, because Dungeon Keeper 2 is a 32-bit game.

The Vulkan shaders are already built into the code. You only need to rebuild them if you change a shader. Then run `Runtime\Shaders\compile_vulkan_shaders.bat`, which needs the Vulkan SDK.

---

## License

KeeperForge2 is free, and it will always be free. You can read the full rules in [LICENSE.md](LICENSE.md). Here they are in short:

- KeeperForge2 and every mod made for it must be **free** to download and use.
- Every mod must share its **full source code** with everyone, all the time.
- Mods cannot have **paid features**. You can take donations, but people who donate must not get anything first or anything extra.
- You may change KeeperForge2 and share your changes, but they must stay free and open too.
- Never share the game's own files.

---

## Credits

- **Diablo** made KeeperForge2.
- **Claude** (an AI by Anthropic) helped write most of the code, including the game changes, bug fixes and graphics.
- **CodeX by Shekh Anamul Haque** (a custom GPT for ChatGPT) made the first starting pieces: the launcher that adds KeeperForge2 to the game, and the log files.
- **Gemini** (an AI by Google) made the logo for `KeeperForge2Launcher.exe`.

## Thank you

KeeperForge2 includes work from **[Flame](https://github.com/DiaLight/Flame)** by **DiaLight**, used with his permission. Flame's features and fixes are being brought into KeeperForge2. Thank you, DiaLight.

`steam_api.dll` belongs to Valve. It comes from Valve's Steamworks SDK and is included so you can play online.

Thanks to everyone in the Dungeon Keeper 2 community who has studied the game and shared what they learned. That includes DiaLight's **[Ember](https://github.com/DiaLight/Ember)** project. And thanks to Bullfrog Productions for making such a great game.
