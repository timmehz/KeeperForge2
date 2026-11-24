# DKII-DX.exe addresses used by the runtime

Code copy: Runtime/Include/GameAddresses.h. Every patch site there also has its
expected bytes in the module that patches it, so a different build is detected
and the patch is skipped instead of corrupting code.

| Name | Address | What | Tag |
|---|---|---|---|
| VideoDisplayWidth | 0x759A7C | settings: display width | [verified] |
| VideoDisplayHeight | 0x759A80 | settings: display height | [verified] |
| PlayMovie | 0x53BCD0 | plays a .tgq video, thiscall (path), ret 4 | [verified] |
| VideoPlayerConfig | 0x73EDC8 | video player settings: +4 flags, +0x60 film height, +0x64 film width, +0x68/+0x6C offsets, +0x70/+0x74 sizes | [verified] |
| EnterGameScreen | 0x525D30 | sets in-game screen mode, 1 stack arg, ret 4 | [verified] |
| EngineHeapAlloc | 0x57CEB0 | cdecl void* (int size) | [verified] |
| EngineHeapFree | 0x57D0B0 | cdecl void (void* block) | [verified] |
| TextureBufferAlloc | 0x5905D8 | inside 0x590580: the engine heap allocation of a texture buffer (width * height * bytes per texel), row size in EDI; padded by one row because the bump-map maker 0x591820 writes one row past it | [verified] |
| EngineBumpFlags | 0x764BB8 | the engine's flags: 0x10 bump du/dv in use, 0x20 with luminance | [verified] |
| LiquidSurfaceCreate | 0x5873A0 | thiscall: makes a liquid surface (args 4 texture, 5 bump map, 6 environment map, 7 flags, 8 opacity) | [verified] |
| RoomLiquidSurfaceCreate | 0x5A31E0 | in the room liquid maker 0x5A2F10 (the Temple's pool): its call to 0x5873A0, engine object in EBP; Renderer redirects it to add bump | [verified] |
| WaterSettingTranslucent | 0x759AD8 | video settings + 0x60; water tiles add 1 to their surface flags when set ([unverified] "Translucent Water") | [verified] |
| WaterSettingSimple | 0x759AE0 | video settings + 0x68; when not 0, water tiles are made without bump ([unverified] "Sine Wave Water") | [verified] |
| WaterBumpSkip | 0x5A2B0D | water tiles' short jz past their bump values when engine flag 0x10 is off (74 27); made jmp for bump_mapping lava | [verified] |
| WorldGameSeed (offset) | world + 0x15A0 | the simulation's random seed (32 bit), saved with the world; stepped by 0x5558D0, cdecl (range, seed pointer, source file, line): seed * 0x19660D + 0x3C6EF35F, then (seed >> 13 or seed << 19) modulo the range. Read (never stepped) for the mod API's shared random number | [verified] |
| LavaBumpSkip | 0x5A2E54 | lava tiles' short jz past flag 0x8000 when engine flag 0x10 is off (74 11); made jmp for bump_mapping water | [verified] |
| EngineHeapChunkCount | 0x765B08 | int, number of heap chunks (max 32) | [verified] |
| EngineHeapTotalSize | 0x765B14 | int, total heap size in bytes | [verified] |
| EngineHeapChunkBuffers | 0x765DA0 | void*[32], raw chunk buffers | [verified] |
| GuiViewTileWrite | 0x42C8A2 | tile slot address, view EBX, index EAX | [verified] |
| GuiViewTileRead | 0x42CE95 | tile read, view ESI, index EAX | [verified] |
| GuiViewTileReadSecond | 0x42CF82 | tile read, view EDI, index EDX | [verified] |
| SurfaceCachePageCount | 0x592FAC | `push 0x18` page count | [verified] |
| CampaignLevelNames | 0x6B83EC | wide text pointers by campaign level number | [verified] |
| GameComponentScreenCall | 0x5253EA | call to 0x525D30 at the start of the game component's routine (menu and each game) | [verified] |
| PlayerTakeThing | 0x4BC500 | a thing goes into a player's hand, ret 4 | [verified] |
| PlayerDropThing | 0x4C0EE0 | timed drop, thiscall (player) | [verified] |
| SessionPushAction | 0x527130 | sends a game action, ret 4 | [verified] |
| ShotReachedTarget | 0x4AEEA0 | a shot arriving, thiscall (shot) | [verified] |
| WorldTick | 0x50A450 | one game turn, thiscall (world, actions), ret 4 | [verified] |
| WorldGetLocalTag | 0x508C60 | world vtable + 0x264, reads + 0x7243 | [verified] |
| CameraLoadPath | 0x44A370 | scripted camera path, ret 0x10; past its checks at 0x44A3B5 | [verified] |
| PlayerCheckTrigger | 0x4C7B20 | does a level trigger hold, ret 4; type at trigger + 0xE | [verified] |
| ModifiersHeld | 0x5B2A50 | cdecl, modifiers held: 1 Shift, 2 Ctrl, 4 Alt | [verified] |
| PanelTabClick | 0x416C20 | cdecl (button id, panel 1 to 4, interface), side panel tab click | [verified] |
| InterfaceKeyEvent | 0x405010 | cdecl (key, pressed, modifiers, interface), the view's key event | [verified] |
| InterfaceClickAction | 0x422600 | thiscall (interface, 0, button id), sends the action for a button click | [verified] |
| InterfaceToggleOptions | 0x428710 | cdecl (0, 0, interface), the Options button's click | [verified] |
| CursorSetState | 0x40ABC0 | thiscall (cursor, state, now), the hand cursor's state | [verified] |
| SetPointerImage | 0x5B2BD0 | cdecl (result, image, source rect, hot spot), null image shows no pointer | [verified] |
| InterfaceDrawHandIcons | 0x40CAE0 | thiscall (interface, x, y), icons beside the hand | [verified] |
| InputSystem | 0x79CF90 | +0x5C DirectInput mouse, +0x64 mouse position object | [verified] |
| MouseMotion | 0x5DD4A0 | thiscall (mouse, event), adds a movement to the position | [verified] |
| MouseClamp | 0x5DD630 | fastcall (mouse), keeps it in bounds, sets the whole numbers | [verified] |
| InputDeviceSetFlags | 0x5DCE50 | thiscall (device, result, flags), DirectInput cooperative flags | [verified] |
| GameWindowProc | 0x5B5070 | window procedure of the game's window class | [verified] |
| Screen setup | 0x5581B0 | thiscall on game object 0x756EE8 | [verified] |
| Menu screen size | 0x52F21B | `push 480; push 640` for the main menu | [verified] |
| SkirmishWindowConfig | 0x6AD6A0 | window 9 config, button list pointer at +0x3E | [verified] |
| DirectDrawCreateImport / DirectDrawEnumerateImport | 0x66C020 / 0x66C024 | import slots of DirectDrawCreate / DirectDrawEnumerateA (patch.dll) | [verified] |
| GameDirectDraw | 0x79D3D0 | the game's IDirectDraw*, 0 until its first screen setup | [verified] |
| SkirmishButtons | 0x6ADD10 | skirmish button configs (0x50 each, end id -1) | [verified] |
| SinglePlayerWindowConfig | 0x6AD658 | window 2 config, button list pointer at +0x3E | [verified] |
| SinglePlayerButtons | 0x6ADAE0 | Single Player button configs (0x50 each, end id -1) | [verified] |
| MapRevealAll | 0x451EF0 | fastcall (map), lifts all fog; the map is world +0x9DA | [verified] |
| TextById | 0x553A60 | cdecl (id), a game text by id | [verified] |
| TimeIntoTurn | 0x521C80 | thiscall (communication), ms into the current turn | [verified] |
| ClickButtonRender | 0x532670 | cdecl char (button, front end), draws pre-rendered text | [verified] |
| GuiPrerenderTexts | 0x5329A0 | thiscall (window, slot, entries, font) on front end, ret 0x10 | [verified] |
| SkirmishSetup | 0x538AC0 | cdecl (front end), opens the skirmish lobby | [verified] |
| PlayerSlots | 0x73F5C8 | 8 x 63-byte lobby slots | [verified] |
| PlayerSlotTexts | 0x73F858 | 8 x 0x66-byte list texts | [verified] |
| PlayerListSelection | 0x740340 | int, selected lobby row (-1 none) | [verified] |
| SkirmishStartLocalPlayer | 0x531B3E | `mov [0x75AD53], 3` at skirmish start | [verified] |
| LocalPlayerNumber | 0x75AD53 | player number the local player controls | [verified] |
| WorldSetupKeeperCheck | 0x50CB97 | `cmp al,3` that skips keeper 1 in world setup | [verified] |
| WorldCreatePlayer | 0x4C2340 | makes a map player: thiscall on world+0x7A (number, tag out), ret 8 | [verified] |
| PlayerTick | 0x4B9EC0 | per-player tick, thiscall; returns early for neutral and Good | [verified] |
| PlayerActionTick | 0x4C0AA0 | runs/finishes the player's timed action (+0x186), thiscall | [verified] |
| WorldPointer | 0x6E5054 | pointer to the world object | [verified] |
| ThingTable | 0x6EE540 | thing pointers by tag | [verified] |
| EggTick | 0x4A21C0 | egg's object state 0x18 (sits, thrown at 4 s, rolls): fastcall (egg), 0 when it removed the egg | [verified] |
| EggHatch | 0x4A1CF0 | egg's object state 0x20: makes a chicken at the egg, fastcall (egg) | [verified] |
| ThingSetPosition | 0x4B5560 | moves a thing, relinking its tile list: thiscall (thing, position) ret 4 | [verified] |
| TileFloorHeight | 0x4545C0 | floor height at a position on a tile: thiscall (tile, position) ret 4 | [verified] |
| PlayerSpellState / SetPlayerSpellState | 0x4BAFA0 / 0x4BAF40 | per-player keeper spell state (3 = available) | [verified] |
| CastKeeperSpell | 0x4BBD30 | thiscall (player, spell, position, flag) ret 0xC | [verified] |
| PlayerInterfaceSetup | 0x419180 | thiscall (interface); picks the Workers row type (+0x123) | [verified] |
| TextGetString | 0x62D960 | thiscall (table, id) ret 4, multibyte text | [verified] |
| TextToWide | 0x553B90 | cdecl (text, out, size), multibyte to wide | [verified] |
| CreatureNextGoodCheck | 0x47759E | `cmp cx,[Good tag]; jnz` in the "what next" state; targets 0x4775A7 / 0x4775BE / 0x4775EC | [verified] |
| GoodPlayerTag | 0x6F254C | the Good player's tag | [verified] |
| FlyingIconDraw | 0x42F040 | flying notification icon update + draw, thiscall (icon, engine, x, y) ret 0xC; size float at +0x18/+0x1C | [verified] |
| PlayerTickGoodCheck | 0x4B9F3A | `cmp ax,[Good tag]; jnz` in the player tick; Good path 0x4B9F43, rest of the tick 0x4B9F57 | [verified] |
| CreateCreature | 0x4B78D0 | makes a creature, thiscall (list, type, owner, position, out) ret 0x10; fails at 448 live creatures in the world (0x4B7945) | [verified] |
| CreaturePoolHas | 0x506400 | is a type in a player's creature pool (player +0x1109), thiscall (pool, type) ret 4 | [verified] |
| PortalAttraction | 0x4DBF90 | portal creature choice, thiscall (portal room, flag) ret 4 | [verified] |
| MercenaryPortalCreatureTest | 0x4DC064 | `test byte [eax+0x68A],8; jz` (13 bytes); accepted 0x4DC071, rejected 0x4DC474 | [verified] |
| CastCreatureCheck | 0x4BBE4B | `test al,al; jz` in the cast, EDI = creature type; on to 0x4BBE53, none 0x4BBF66 | [verified] |
| PortalMercenaryFlag | 0x4DCC4F | `push 1; xor al,1; mov ecx,esi` (6 bytes) before 0x48E0D0 (IsMercenary); resumes 0x4DCC55 | [verified] |
| WorkerTopUpCount | 0x4BB479 | heart top-up reads the Imp count; resumes at 0x4BB47E | [verified] |
| WorldSetup | 0x50C800 | level start world setup, thiscall (world) | [verified] |
| LobbyWriteOptions / LobbyReadOptions | 0x549D10 / 0x549F60 | lobby options packed into front end +0x6026..+0x6029, thiscall (front end) | [verified] |
| DefineKeysModifierColumn / DefineKeysKeyColumn | 0x42ACDD / 0x42AD40 | "push 200" and "push 300", the in-game Define Keys list's column pixels | [verified] |
| LobbyPlayerMemory | 0x543820 | this machine's memory in MB for the lobby's Ram column | [verified] |
| GameCreatureListCount | 0x75AECF | rows in the game settings' creature list, read by level setup | [verified] |
| LobbyHostListSizeCheck / Accepted / Rejected | 0x5432BD / 0x5432C5 / 0x5435B2 | size check on the host's lobby message (199) | [verified] |
| PlayMode | 0x75AD4B | 1 campaign, 2 skirmish, 3 multiplayer | [verified use] |
| PlayerInitTaskSkip | 0x4B87B5 | `cmp al,1; jz; cmp al,2; jz` in player init: no task list for Good/neutral | [verified] |
| GuiRenderWindows | 0x52BBD0 | thiscall (owner) ret 4, draws a manager's windows | [verified] |
| SetDrawSurface | 0x5B3710 | cdecl (description*), binds 2D/text target (0x79D060) | [verified] |
| FrontEndDraw2D | 0x5334C0 | fastcall (front end), the menu's 2D pass | [verified] |
| BlitSurface | 0x5B57F0 | cdecl (result, dest, x, y, src, rect, flags), DirectDraw copy | [verified] |
| ObjectInit | 0x56F6E0 | fastcall (decoded), clears an object record's decoded form (0x30 bytes) | [verified] |
| ObjectRead | 0x56F730 | thiscall (decoded, record bytes), case 0xC2 of the things reader 0x56FA00 | [verified] |
| ObjectMake | 0x520BB0 | cdecl (decoded, world), level load's object maker (table at 0x51E13A); 0 when no room | [verified] |
| TileDraw | 0x43CC30 | thiscall (tile drawer, x, y, pass) ret 0xC, draws a map tile and on pass 0 the hand's box piece | [verified] |
| EngineThingTable | 0x6D6458 | engine things, pointers by 16 bit index | [verified] |
| EngineThingMove | 0x446C70 | thiscall (thing, x y z position) ret 4, moves an engine thing to its tile's list | [verified] |
| ObjectVtable | 0x66D654 | set at 0x4B720C; slot +0x30 is 0x49BEC0, which calls 0x4B7B10 to destroy and free it | [verified] |
| RoomsRefreshAround | 0x4ECDB0 | thiscall (world + 0x9BE, x, y) ret 8, refreshes the rooms beside a tile (walls, own objects) | [verified] |
| ThreatStoreA..E | 0x4F9F1E, 0x4F9F85, 0x4FA37A, 0x4FA3B6, 0x4FEF82 | 16 bit stores of a 32 bit computer threat total into its response slot (cp + slot * 8 + 0x5A); each now jumps to a stub that clamps the value | [verified] |
| BackstabAngle | 0x4990DB | in the melee hit 0x498E30, after the target-to-attacker angle (0x65FB54); resume 0x4990E3; ESI the target creature | [verified] |
| AttackableStateTest | 0x45A93F | in 0x45A8B0 (can a creature be attacked): its sleep-state test; resume 0x45A945, skip 0x45A952 | [verified] |
| RoomAttachBurnReset | 0x4507B5 | in 0x450770 (attach a tile to a room): "push 0; mov ecx, esi; call 0x452B70" sets the tile's burn level to 0; resume 0x4507BE | [verified] |
| BuildRoomTileCall / RoomsAddTile | 0x4C5CC5 / 0x4ED1A0 | building a room tile, thiscall (world + 0x9BE, x, y, type, player tag, rooms left, signed byte* made, orientation) ret 0x1C | [verified] |
| SellRoomTileCall / RoomsRemoveTile | 0x4C5E3B / 0x4ED440 | selling a room tile, thiscall (world + 0x9BE, x, y, rooms left, signed byte* made) ret 0x10 | [verified] |
| DropActionDropCall / PlayerDropAt | 0x4C0F70 / 0x4BC710 | the drop action's call of the player's drop, thiscall (player, int position[3], u16* direction) ret 8 | [verified] |
| InterfaceDropAllowed | 0x40D8F0 | drop rules, thiscall (interface, thing, tile x, tile y) ret 0xC; reads only interface + 8 (player tag) and + 0x53 (world) | [verified] |
| HeartWarningCompare | 0x4D9ACB | in the Heart room tick 0x4D9A60 (raw bytes): "cmp eax, 50; jnz" on health percent, 50% event 0x27 at 0x4D9AD0, 10% check at 0x4D9AE2 (event 0x28) | [verified] |
| LoadPngPicture | 0x59A4B0 | cdecl (name): loads a PNG, returns a 40-byte picture record (masks, bits per pixel, pixels, width + 0x1C, height + 0x20, pitch) or 0 when missing; one caller, 0x5912D3 | [verified] |
| RoomTileMeshes | 0x443610 | thiscall (bridge, x, y, tile, terrain data, mesh arrays) ret 0x14: a room tile's meshes, room from ThingTable[(tile + 2) & 0xFFF] (as 0x4559B0), unchecked; called at 0x43E3E0 | [verified] |
| SaveLoadPathBuild / SaveLoadPathUse | 0x52555E / 0x5255CD | loading a saved game: the path built into 64-byte stack buffers (0x51 bytes, resume 0x5255AF), and "lea eax, [esp+0x114]" handing it to the loader 0x50E920 | [verified] |
| GameConfigSaveName / SavesFolder | 0x75ADD7 / 0x758754 | the save to load (wide; game config 0x75AD47 + 0x90, 0x55D550 returns it) and the saves folder (full path, text) | [verified] |
| InterfacePushAction | 0x4346E0 | the player interface's push of an action, thiscall (interface, action) ret 4; session at interface + 4 (0 with no game) | [verified] |
| ActionExitToWindows | 0x7D | action kind sent by the in-game "exit to Windows" confirmation (0x4205B0), player tag at + 0x10 | [verified] |
| TextInputConfigure | 0x52AA10 | a text input's configure, thiscall (button, config) ret 4: limit (+ 0x280) from config + 0x1A; typing (0x52C40E) allows limit - 1 characters | [verified] |
| SessionListVersionDone / SessionListJoinableDone | 0x54D548 / 0x54D58C | in the session list 0x54D370 (raw bytes): version text done ([esp + 0x30]); joinable byte written (via [esp + 0x10]); resume 0x54D54D / 0x54D595 | [verified] |
| SessionCount / SessionArray | 0x73FE74 / 0x73FE78 | found multiplayer sessions, 0xA4 bytes each (see game-fixes.md) | [verified] |
| GameVersionMajor / GameVersionMinor / GameFileCheck | 0x6BC430 / 0x6BC434 / 0x7962A0 | this game's version and file check, compared with each session's | [verified] |
| SessionMapLookup | 0x53ED60 | thiscall (front end, map info, wide name out) ret 8: 1 when the session's map is ours | [verified] |
| GameStartupCall / GameStartup | 0x64780B / 0x5A5DA0 | the game's start-up, cdecl (argc, argv); mutex "DKII MUTEX" via 0x5BAE70 | [verified] |
| Startup...Call | 0x5A5FAC, 0x5A5FBC, 0x5A60DB, 0x5A6187, 0x5A6199, 0x5A61D0, 0x5A61F8 | the start-up's calls of command line 0x5A63B0, fonts 0x552F10, window 0x557CB0, speech 0x567F90, network 0x559BE0, Windows check 0x557FB0, components 0x552BE0 (0 = failed) | [verified] |
| WindowsCheckMajorPush | 0x557FB4 | in the Windows check 0x557FB0: "push 6", the Windows major version the game refuses from (0x557FD0 compares with GetVersionExA) | [verified] |
| ThreatPickLastTest / ThreatPickOwnedTest | 0x4FE504 / 0x4FE544 | in the threat response 0x4FE0B0: the pick loop's last test (next 0x4FE50D, picked 0x4FE522) and the "is it the keeper's" test (0x48E6A0; owned 0x4FE54C, not 0x4FE914) | [verified] |
| CreatureRecordConvert | 0x568E30 | thiscall (type, record) ret 4: a creature list record (0x1549 bytes) into the game's own type; called by the reader at 0x56A41A | [verified] |
| TitleScreenWait / GameTimeMs | 0x5341D8 / 0x5B5E90 | the title screen's 10 s wait loop (to 0x5341F6, resume 0x5341F7); the game's millisecond clock, cdecl | [verified] |
| FrontEndWindowList (offset) | + 0x30FCE | the front end's menu window list; per window: id + 0x44, shown flag + 0x48, next + 0x62 (as the lookups 0x5322D0 and 0x52C540 walk it); + 0x6016 is only where the map list was opened from | [verified] |
| FrontEndComponent / FrontEndMenuAction | 0x70D6D8 / 0x5306F0 | the menus; their action handler, cdecl (argument, action, front end): 0x58 map list, 0x5A start skirmish, 0x18 leave skirmish, 0x4E/0x4F map list tick/cross | [verified] |
| SkirmishMapList / SkirmishMapListCount / MapListChosenRow | 0x740354 / 0x6B442C / 0x740348 | the skirmish map list text (4 fields per map, 0-ended, 0xFFFF last), its count (-1 recount), the clicked row | [verified] |
| FrontEndChangeWindow | 0x5391F0 | cdecl (0 hide / 1 show, window, front end); window 7 is the briefing (tick 0x0C, cross 0x0A) | [verified] |
| SkirmishScreenStep | 0x538AC0 | cdecl (front end): shows window 9 and runs action 0x15, once the camera reaches the skirmish screen; the same function as SkirmishSetup, hooked by SkirmishSwapKeeper, which calls CustomCampaign | [verified] |
| MapListBackground | 0x7401F0 | the skirmish map list's background picture record (size column tinted green); width + 0x1C, height + 0x20, pitch + 0x24 as the picture draw 0x52CF90 reads them | [verified] |
| MapListRowHighlight | 0x740218 | the map list's row highlight picture record (chosen and pointed rows; green over the size column) | [verified] |
| MapListDetails | 0x540600 | the chosen map's details draw, cdecl (widget, front end): name banner, "Max Players : %s" (text 0x5D4) from field 2, field 3; hooked by CustomCampaign | [verified] |
| PictureDraw / ImageFree | 0x52CF90 / 0x5B5310 | draw a picture record into a frame, thiscall (front end + 0x30D3F; out, frame, x, y, picture, area, 0), no scaling; free a record's pixels, fastcall (record) | [verified] |
| EngineCamera* | 0x760AB8 / 0x760AC4 / 0x760AE8 / 0x760AFC / 0x760B00 | the engine's camera as it last drew the world (0x575A00): position, rotation (row major, view to world), focal length, projection middle | [verified] |
| Bridge lights (offsets) | + 0x33A, light + 0x13C / + 0xD6 / + 0x111 / + 0x117 / + 0x145 | the bridge's light list (Flame: CLight), next, position (4096 per tile), color, reach, strength; bridge vtable 0x66CC94 | [verified] |
| BriefingWindowConfig / BriefingButtons | 0x6AEF38 / 0x6AEF80 | the briefing window's (7) config and button list (title 161, boxes 162/165/168, headings 164/167, cross 169, tick 170) | [verified] |
| FrontEndShortBriefing (offsets) | + 0x311B6 / + 0x311BA | flags from the last level read (0x535C30); either set gives the short briefing (no sub objectives) | [verified] |
| TitleBake / ImageCreate / ImageAllocate | 0x536370 / 0x5B5270 / 0x5B52C0 | menu title pictures: draw a text into two picture records (thiscall (front end; packed settings)); set up a 0x28-byte picture record; give it pixels. Titles live at front end + 0x360C / + 0x39A4 (0x28 each), sizes + 0x3D3C (0x10 each); made by 0x536850 from the text ids at 0x6B8468 | [verified] |
| FrontEndPreviewShown (offset) | + 0x30C1E | byte; the chosen map's preview (widget 687, 0x537C30) draws while it is 1 to 3 | [verified] |
| (menu button click) | 0x528610 | calls the button's action (config + 0x16, + 0x1A), hides its window if config + 0x12, shows config + 0xE; the Single Player menu's click 0x538000 hides/shows by its first argument's words | [verified] |
| BriefingText | 0x553AC0 | cdecl (id): the level's briefing table text (0/1 name, 2 objective, 3 briefing, 4-5 more; meanings from layout) | [verified code] |
| MapListClick | 0x5416E0 | cdecl (row, ?, front end): stores the row, loads that map's preview | [verified] |
| SoundThreadStopWait | 0x60D64A | in the sound list thread's stop 0x60D630: busy wait for + 0x38 (stopped), 14 bytes, ESI the object; resume 0x60D658 | [verified] |
| HeroPartySpawnFailTest | 0x51A274 | in the hero party spawn 0x519F90 (thiscall (world, party, action point, invasion flag) ret 0xC): "test eax, eax; jz" after a member's spawn (0x519B20, 0 when the world is full); next member 0x51A27C | [verified] |
| HeroPartyFinish | 0x51A5E2 | the party's finish code after the member loop (same stack as the loop); leader pointer at [esp + 0x88], returns 1 at 0x51A747 | [verified] |
| HeroPartyLeaderEffect / HeroPartyLeaderPlace | 0x51A66A / 0x51A73A | the finish code's uses of the leader: effect 0x2B at it (resume 0x51A672, skip 0x51A6CF), its place in the party (+ 0x3E1) | [verified] |
| TaskSlotHolder / TaskSetSlotHolder | 0x508110 / 0x508160 | a dig or gold task's slot (3 per face, 4 faces): read, thiscall (list, task, face, slot) ret 0xC; set, (list, task, face, slot, tag) ret 0x10 | [verified] |
| DigSearchSlotRead / GoldSearchSlotRead | 0x469D34 / 0x46A1B6 | the worker job search's slot reads (calls to 0x508110) | [verified] |
| NeutralPlayerTag | 0x6F2548 | the neutral player's tag (16 bit) | [verified] |
| ChatReceivedCopy | 0x409E8F | in 0x409E30 (a received chat message): fixed 0x102-byte copy of the text into the last history line; resume 0x409EA1 | [verified] |
| InputSystem key state | 0x79CF90 + 0x70 → + 0x1C | the game's key state, one byte per DirectInput key code (0x5BB280 reads it) | [verified] |
| BumpSetting | 0x759B10 | the game's bump mapping setting (video settings 0x759A78 + 0x98): 0 off, 1 du/dv, 2 luminance | [verified] |
| BumpSettingTexturesRead / BumpSettingEngineRead | 0x59856B / 0x59D951 | the two reads of it (mov eax, [0x759B10]): bump textures loaded, engine flags 0x10 / 0x20 | [verified] |
| ResourcesAddLooseFolders | 0x55C700 | fastcall (resources): adds the loose Data folders to the file lists, at the end of the command line's reading | [verified] |
| ResourcesAddFolder | 0x55BF40 | cdecl (int* status, file list, format, ...): adds a folder to the front of a file list | [verified] |
| FileOpenByPath | 0x5B3980 | cdecl (int* status, file** out, path, access): opens a file by path (level files, shared lists) | [verified] |
| PlayerLevelSetup | 0x4B8640 | thiscall (player, value) ret 4: a player's set-up for a level; fills the portal table (+0xB57) | [verified] |
| PortalPassTypeOfSlot | 0x4DBDC7 | in the per-tick portal pass 0x4DBD90: the world's type at the slot's place (15 bytes); now the slot's own type | [verified] |
| ChatAddLocalLine | 0x409EC0 | thiscall (interface, sender, text) ret 8: adds a line to the in-game chat history (senders 0x8181/0x8282/0x8383 have their own colors) | [verified] |
| ChatDraw | 0x409F30 | fastcall (interface): draws the in-game chat box (into its own picture only when interface + 0x1F39 is set) | [verified] |
| ChatLineColorChosen | 0x40A00C | inside ChatDraw: the line's color { B, G, R, A, palette } is at [esp + 0x18], the line in EDI; CreatorName patches 8 bytes here | [verified] |
| PlayerListColorPush | 0x40A1FB | inside the in-game player list (0x40A0F0): pushes each row's color, the row's player in ESI; CreatorName patches 8 bytes here | [verified] |
| LobbySlotColorSet | 0x544C1D | in the lobby's drawing (0x5445F0): calls TextColorSet with each slot's row color (player color table 0x6E5058, by slot + 3), slot number in the caller's [esp + 0x10]; CreatorName redirects this call | [verified] |
| LobbyChatColorSet | 0x5451D1 | in the lobby chat's drawing (0x544FA0): calls TextColorSet with each line's color (sender's player color + 0x20), the line's text (after the sender's slot digit) in ESI; CreatorName redirects this call | [verified] |
| LobbyChatReceive | 0x542BF0 | lobby chat handler, cdecl (sender, message, front end): adds "<slot digit><slot name>: <text>" to front end + 0x30CAB; message = digit n, n digits equal to the word at 0x759C35, text | [verified] |
| NetworkChatCallback | 0x7581C0 | the network's chat callback (+ 4: its context); set to LobbyChatReceive and the front end at 0x542647; called at 0x559F01 | [verified] |
| FrontEndScreen | 0x759C35 | 16-bit number every lobby chat message carries; messages with another number are dropped ([unverified] the current screen) | [verified] |
| LobbyChatDraw | 0x544FA0 | lobby chat drawing, cdecl (button, front end); CreatorName hooks it to add notices | [verified] |
| TextColorSet | 0x62C3C0 | thiscall (out, color) ret 8: sets the text color { B, G, R, A, palette } of the text drawer at 0x7413C0 | [verified] |
| LobbySlots | 0x73F5C8 | lobby slots, 0x3F bytes each: a human player's wide name at + 0, kind in the low 3 bits of + 0x3A (0 empty, 1 computer) | [verified] |
| InputInitMouse | 0x5BC060 | thiscall (input system, int* result) ret 4: makes the DirectInput mouse; WindowsMouse skips it (game/mouse.md) | [verified] |
| InputWindowActivated | 0x5BC210 | thiscall (input system, int* result, active) ret 8: keyboard's and mouse's active flag (0x5DCCA0), then 0x5BC3A0; no check for a missing mouse | [verified] |
| MouseMoveEventVtable, MouseButtonEventVtable | 0x672900, 0x6728F8 | the mouse's movement (0x14 bytes) and button (0x24 bytes) input events | [verified] |
| ControlKeysVtable | 0x672888 | the control keys object (input system + 0x60): + 0x14 movement (0x5BB660), + 0x18 button (0x5DD010) | [verified] |
| MouseSensitivity | 0x759C21 | the options menu's mouse sensitivity (int) | [verified] |
| PossessedAttackAction | 0x514830 | the possessed creature's attack action, thiscall (world, action) ret 4; data3 bit 0x8000000 a melee hit (game/possession.md) | [verified] |
| StopPossessionAction | 0x514590 | leaving a creature, action handler thiscall (world, action) ret 4; reads only the player tag | [verified] |
| WorldThingInFront (vtable) | world + 0x334 (0x515420) | the thing in front of the possessed creature, thiscall (world, creature, position, angles) ret 0xC | [verified] |
| CreatureMeleeReady | 0x48C9B0 | the creature's melee is ready, thiscall (creature, creature data) ret 4 | [verified] |
| CreatureAddExperienceCapped | 0x4858B0 | adds experience up to the next level, thiscall (creature, amount) ret 4 (0x457880 adds without the cap) | [verified] |
| StateBlockSetState | 0x476D30 | sets a state, thiscall (state block, state, value) ret 8 | [verified] |
| StateBlockGoToBed | 0x46F440 | sends a creature with sleep left to its bed, thiscall (state block) | [verified] |
| StateBlockFollow | 0x461980 | starts following a possessed creature (state 0xF6), thiscall (state block, leader tag) ret 4 | [verified] |
| CreatureSetAttackTarget / CreatureClearAttackTarget | 0x48F390 / 0x48F3F0 | attack target (+ 0x3F9, bit 4 of + 0x3E6): set, thiscall (creature, tag) ret 4; clear, thiscall (creature) | [verified] |
| PrisonToggleDoor | 0x4E35C0 | opens or closes a Prison's door by its state, thiscall (room) | [verified] |
| ObjectSetAnimation | 0x49EF60 | an object's animation, thiscall (object, animation) ret 4 | [verified] |
| TurnsPerSecond | 0x6F2590 | byte the game uses as turns per second (Prison door countdown, once-a-second work) | [verified] |
| MaxTrainingLevel | 0x6F28FC | byte: the Training Room trains up to this level | [verified] |
| ObjectTurn | 0x49BD20 | an object's turn (object vtable + 0x14), thiscall (object): its state (0x49EED0), then its move (0x4B2030) | [verified] |
| PossessedShotTrainingEffect | 0x4ABE54 | in a shot's flight: the effect call (call [edx + 0x38C]) when a possessed creature's shot finds a Training Target; check at 0x4ABE28 | [verified] |
| PossessedTargetFilter / PossessedTargetReach | 0x516050 / 0x5162E0 | what the thing-in-front search keeps (thiscall (world, creature, thing, flag) ret 0xC) / the reach test (thiscall (world, creature, thing) ret 8) | [verified] |
| TrainingCostPerSecond | 0x6F291D | training's gold a second (signed 16 bit, stored negative) | [verified] |
| PlayerTakeGold | 0x4BA420 | a player's gold change, thiscall (player, amount, direct, tile x, tile y) ret 0x10; negative with direct 0 comes from Treasuries and Heart | [verified] |
| ShowMoneyText (arguments) | 0x50FB70 | its four arguments: owner tag, position, amount, 0 (the Training Room shows its gold this way); LevelEditor also hooks it | [verified] |
| PlayerEvent | 0x4C4600 | a player event, thiscall (world + 0x2E54, event, tag, value, value, value) ret 0x14; 0x37 not enough gold to train | [verified] |
| PossessedThingInFront | 0x515420 | the thing-in-front search (world vtable + 0x334), thiscall (world, creature, position, angles) ret 0xC | [verified] |
| GameAngle | 0x65FB54 | angle of a direction, cdecl (dx, dy), 0 to 0x7FF, table based | [verified] |
| TileTakesPrisoners | 0x454110 | whether a room tile takes what the room holds, thiscall (tile); a Prison wants tile + 0xC = -1 (not its edge or door tile) | [verified] |
| StateBlockWalkTo | 0x471890 | walk somewhere, thiscall (state block, position, state on arrival, walking rules) ret 0xC | [verified] |
| DirectionTableX / DirectionTableY | 0x6C1488 / 0x6C1C88 | direction tables (16.16 by angle 0 to 0x7FF): a heading's step across, and down (negated) | [verified] |
| PossessedRangedInFront | 0x515520 | the ranged abilities' target search (world vtable + 0x338), thiscall (world, creature, position, angles, flag) ret 0x10 | [verified] |
