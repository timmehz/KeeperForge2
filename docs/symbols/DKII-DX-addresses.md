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
