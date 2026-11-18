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

### Runs smoother
- Choose your own frame rate, like 60, 120 or 144. The game itself does not speed up.
- Keeps running when you click away from it. While you can't see it, it goes quiet and slows down to save power.
- Lets the game use up to 4 GB of memory instead of 2 GB (still being tested).

### Easier to play
- Move the camera with **W, A, S and D**.
- Zoom in and out with the **mouse wheel**.
- Keys **1 to 4** open the side panel tabs.
- One key turns Sell on and off.
- Screenshots are saved as PNG pictures in their own folder.

### More ways to play
- **Play online through Steam.** Every hosted game shows up in the game's own list, so you can join anyone's game, not just friends'. Games can have a password, and you can still invite friends from Steam.
- **Swap Keeper** in skirmish and in the multiplayer lobby.
- **Play as the Good player** (the heroes) in skirmish.
- **Mercenary Portal:** heroes can join your side once you build the rooms they like.
- **Dwarf workers:** your workers can be Dwarfs instead of Imps.
- **Type in the creature numbers** in the lobby instead of dragging the sliders.
- **Co-op campaign** and an **in-game level editor** (both still being worked on).

### Bug fixes
KeeperForge2 also fixes bugs in the game itself. For example:
- A Hatchery now always gets the right number of chicken coops, no matter how you built the room.
- The Workshop now finishes each item at the right time.
- The chat box no longer crashes when Steam is open.
- Keys like Shift no longer get "stuck" after you switch windows.

---

## What you need

- **Dungeon Keeper 2 from Steam.** Other versions, like GOG, are not supported yet.
- **Windows 10 or 11.**
- **For playing online:** Steam must be open, and you need a 32-bit `steam_api.dll` file (see [Playing online](#playing-online)).

---

## How to install

1. Download the newest release from the **Releases** page.
2. Find your game folder. On Steam, right-click **Dungeon Keeper 2**, then click **Manage**, then **Browse local files**.
3. Copy `KeeperForge2Launcher.exe` and `KeeperForge2.dll` into that folder.
4. Start the game by opening **`KeeperForge2Launcher.exe`**.

The first time it runs, KeeperForge2 makes a new folder called `KeeperForge2` inside your game folder. It keeps everything there:

| Folder or file | What is in it |
|---|---|
| `config.yml` | Your settings |
| `Logs` | Reports that help find problems |
| `SCRSHOTS` | Your screenshots |
| `mods` | Your mods |

**To uninstall,** delete `KeeperForge2Launcher.exe`, `KeeperForge2.dll` and the `KeeperForge2` folder. You might also see a file called `KeeperForge2-DKII-DX.exe` in your game folder. It is a copy KeeperForge2 makes for the 4 GB memory setting, and you can delete it too.

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

If you type something wrong, don't worry. KeeperForge2 fixes the file by itself the next time it starts, and it puts that setting back to normal.

---

## Playing online

1. Put a **32-bit** `steam_api.dll` in your game folder or in the `KeeperForge2` folder. The game does not come with one, and KeeperForge2 does not include it either, so you need to find it yourself. It comes with Valve's free [Steamworks SDK](https://partner.steamgames.com/doc/sdk) (the 32-bit one is in `redistributable_bin`). To download the SDK you sign in with your Steam account and accept Valve's terms.
   - KeeperForge2 was made and tested with `steam_api.dll` version **10.24.16.75**. Other versions may not work. To see a file's version, right-click it, click **Properties**, then open the **Details** tab.
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

Thanks to everyone in the Dungeon Keeper 2 community who has studied the game and shared what they learned. That includes the **Flame** and **Ember** projects. And thanks to Bullfrog Productions for making such a great game.
