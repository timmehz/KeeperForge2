#pragma once
#include <cstdint>
#include "Memory.h"
// Addresses inside DKII-DX.exe (v1.70, the GOG/Steam DX build). All verified against our Ghidra export.
// They are virtual addresses for the default image base; use Game::Resolve() to get the live address.
// See docs/symbols/DKII-DX-addresses.md for what each one is.
namespace KeeperForge2::Game {
	constexpr std::uintptr_t ImageBase = 0x00400000;
	inline std::uintptr_t Resolve(std::uintptr_t virtualAddress) { return Memory::GetModuleBase() + (virtualAddress - ImageBase); }
	namespace Address {
		// Video settings (inside the global resources object)
		constexpr std::uintptr_t VideoDisplayWidth = 0x00759A7C;
		constexpr std::uintptr_t VideoDisplayHeight = 0x00759A80;
		constexpr std::uintptr_t VideoBitDepth = 0x00759A84;
		// Graphics devices found at startup, each with the display modes DirectDraw reported for it
		constexpr std::uintptr_t GameObject = 0x00756EE8;
		constexpr std::uintptr_t DisplayDeviceCount = 0x00758048;
		constexpr std::uintptr_t DisplayDevices = 0x00758050;
		// EnumDisplayModes callback that appends a mode to a device's list: stdcall (DDSURFACEDESC*, device index)
		constexpr std::uintptr_t DisplayModeCallback = 0x00557A10;
		// Sets the in-game screen mode from the video settings. One stack argument, ret 4.
		constexpr std::uintptr_t EnterGameScreen = 0x00525D30;
		// Engine heap: cdecl void* alloc(int size) and cdecl void free(void* block)
		constexpr std::uintptr_t EngineHeapAlloc = 0x0057CEB0;
		constexpr std::uintptr_t EngineHeapFree = 0x0057D0B0;
		// The engine heap allocation of a texture buffer (inside 0x590580: width * height * bytes per texel, row size in
		// EDI at the call). Bump-map making (0x591820) writes one row past such a buffer.
		constexpr std::uintptr_t TextureBufferAlloc = 0x005905D8;
		constexpr std::uintptr_t EngineHeapChunkCount = 0x00765B08;
		constexpr std::uintptr_t EngineHeapTotalSize = 0x00765B14;
		constexpr std::uintptr_t EngineHeapChunkBuffers = 0x00765DA0;
		// GUI view tile table (128x32 tiles, 140 inline slots)
		constexpr std::uintptr_t GuiViewTileWrite = 0x0042C8A2;
		constexpr std::uintptr_t GuiViewTileRead = 0x0042CE95;
		constexpr std::uintptr_t GuiViewTileReadSecond = 0x0042CF82;
		// GUI manager conversions between the 2560x1920 layout space and screen pixels (all thiscall, return out)
		constexpr std::uintptr_t GuiBoxToScreen = 0x0052C630;
		constexpr std::uintptr_t GuiPointToScreen = 0x0052C5A0;
		constexpr std::uintptr_t GuiPointToLayout = 0x0052C5F0;
		constexpr std::uintptr_t GuiMouseToLayout = 0x0052C560;
		// GUI windows: build a manager's windows from a table (thiscall (table, owner), ret 8), and a window's layout
		// rectangle and child origin (both thiscall (int* box) returning box, ret 4)
		constexpr std::uintptr_t GuiCreateElements = 0x0052B8F0;
		constexpr std::uintptr_t GuiWindowRect = 0x0052CDC0;
		constexpr std::uintptr_t GuiWindowOrigin = 0x0052CE50;
		// Draws a manager's windows: thiscall (owner), ret 4. The front end calls it from 0x5334C0 after filling in
		// its frame description; the in-game GUI from 0x419F9A.
		constexpr std::uintptr_t GuiRenderWindows = 0x0052BBD0;
		// The front end's 2D pass: fastcall (front end). Fills in the frame description, draws the windows, then
		// window 0x2F's elements (0x547C20).
		constexpr std::uintptr_t FrontEndDraw2D = 0x005334C0;
		// The front end's mouse handler: cdecl (int, int, x, y, front end), screen pixels. It tests slider-type
		// buttons (+0x73 == 8) against their converted boxes itself, and passes the position on to GuiClick.
		constexpr std::uintptr_t FrontEndMouseEvent = 0x0052FBF0;
		// Menu button handlers that test the stored mouse (front end + 0x30FF0, the menu manager's +0xAC) against
		// converted boxes themselves: cdecl (button, front end). List box, horizontal slider, vertical slider.
		constexpr std::uintptr_t MenuListHover = 0x00530440;
		constexpr std::uintptr_t MenuSliderHover = 0x005501F0;
		constexpr std::uintptr_t MenuScrollBarHover = 0x005507E0;
		// DirectDraw copy of one surface onto another: cdecl (int* result, destination, x, y, source, source rect,
		// flags). The loading and title screens use it, and so do menu sliders, previews and small-font text.
		constexpr std::uintptr_t BlitSurface = 0x005B57F0;
		// Mouse pointer drawer (its own thread draws the pointer straight onto the screen): the instance
		// (0x5DBE10 returns it). Its draw-to-surface code is 0x5DC860.
		constexpr std::uintptr_t CursorDrawer = 0x0079F638;
		// The full-screen back buffer surface (what the screenshot reads), and the window's surface in windowed mode:
		// thiscall () on game object + 0xEC9
		constexpr std::uintptr_t BackBufferSurface = 0x0079D364;
		constexpr std::uintptr_t WindowScreenSurface = 0x005565E0;
		// Binds the surface 2D drawing and text go to: cdecl (frame description*). It stores the description at
		// CurrentDrawSurface and caches its pixel pointer.
		constexpr std::uintptr_t SetDrawSurface = 0x005B3710;
		constexpr std::uintptr_t CurrentDrawSurface = 0x0079D060;
		// The in-game window table passed to GuiCreateElements (the main menu uses 0x6AE4E0)
		constexpr std::uintptr_t GameWindowTable = 0x00693EF8;
		// Minimap "N" (north) sprite, drawn at a fixed pixel size: "mov eax, 8" (width and height), "sub ecx, 6" (y nudge)
		// and "sub eax, 3" (x nudge)
		constexpr std::uintptr_t MinimapNorthSize = 0x0041EE7D;
		constexpr std::uintptr_t MinimapNorthNudgeY = 0x0041EF2C;
		constexpr std::uintptr_t MinimapNorthNudgeX = 0x0041EF39;
		// Icons of the things held in the hand, drawn next to the cursor (0x40D450, from 0x40CFE0): "mov eax, 0x20"
		// size (32 = 1:1) for each icon and for the "more" icon, and their offsets from the cursor in pixels, 9
		// (x, y) int pairs filled in at startup by 0x40AA40 (two rows of four on a 32 pixel grid, then the "more" icon).
		constexpr std::uintptr_t HandIconSize = 0x0040D4FA;
		constexpr std::uintptr_t HandMoreIconSize = 0x0040D206;
		constexpr std::uintptr_t HandIconOffsets = 0x006CE910;
		// The icon of what the cursor will place or cast (room, spell, trap...), drawn next to the cursor
		// (0x40CB?? and 0x40D310): more "mov eax, 0x20" icon sizes, and "lea edi,[edx+0x1e]" / "lea ebx,[ecx+0xa]"
		// placing the number drawn beside it (8-bit offsets)
		constexpr std::uintptr_t CursorIconSize = 0x0040CE27;
		constexpr std::uintptr_t CursorIconSizeOther = 0x0040CF44;
		constexpr std::uintptr_t CursorIconSizeThird = 0x0040D3D2;
		constexpr std::uintptr_t CursorIconTextOffsetX = 0x0040CE67;
		constexpr std::uintptr_t CursorIconTextOffsetY = 0x0040CE6A;
		// The in-game Define Keys list (0x42A9F0) draws each row's columns at fixed pixels whatever the screen size: the
		// action at 0, the modifier at "push 200", the key at "push 300".
		constexpr std::uintptr_t DefineKeysModifierColumn = 0x0042ACDD;
		constexpr std::uintptr_t DefineKeysKeyColumn = 0x0042AD40;
		// Draws (and updates) one icon that flies to the notification bar with a sparkle trail, thiscall (icon,
		// engine, x, y) ret 0xC. Called from 0x42C390 for the up to four in the player interface (the first at
		// interface + 0x4C6E). Size is a float at icon + 0x18 (0.75 or 0, set by 0x42BC70 / 0x42BE90), blending to
		// + 0x1C; the mesh is drawn at size * 512 (0x579A10).
		constexpr std::uintptr_t FlyingIconDraw = 0x0042F040;
		// GUI hit tests that convert the mouse inline (x*2560/width, y*1920/height with the game object's size):
		// thiscall int () using the manager's stored mouse, and thiscall int (x, y, a, b), ret 0x10
		constexpr std::uintptr_t GuiHoverTest = 0x0052BFC0;
		constexpr std::uintptr_t GuiClick = 0x0052C0A0;
		// Main menu screen mode: "push 480" then "push 640"
		constexpr std::uintptr_t MenuScreenHeightPush = 0x0052F21B;
		constexpr std::uintptr_t MenuScreenWidthPush = 0x0052F220;
		// Game object screen size (game object + 0x4 / + 0x8)
		constexpr std::uintptr_t GameScreenWidth = 0x00756EEC;
		constexpr std::uintptr_t GameScreenHeight = 0x00756EF0;
		// In-game font setup: cdecl int (int screenWidth), and its size table: cdecl id (int size, int screenWidth)
		constexpr std::uintptr_t InGameFontSetup = 0x005534D0;
		constexpr std::uintptr_t InGameFontId = 0x00553690;
		// Font lookup by id: thiscall (int* status, font** slot, int id) on the language object, ret 0xC
		constexpr std::uintptr_t ResolveFont = 0x0062DFF0;
		constexpr std::uintptr_t LanguageObject = 0x007410C0;
		// Global font slots filled once at startup by fixed id (0x44A = 16 px, 0x44B = 18 px, 0x44E = 24 px)
		constexpr std::uintptr_t FontSlotInGame16 = 0x00741578;
		constexpr std::uintptr_t FontSlotInGame18 = 0x0074157C;
		constexpr std::uintptr_t FontSlotInGame24 = 0x00741584;
		// Loading screen: thiscall void (world), loads "LoadingScreen\LoadingScreen%dx%d" and BltFasts it at 0,0
		constexpr std::uintptr_t ShowLoadingScreen = 0x00511020;
		constexpr std::uintptr_t LoadingScreenBlitFront = 0x00511118;
		constexpr std::uintptr_t LoadingScreenBlitBack = 0x00511139;
		// Main menu version text renderer "V%lu.%lu": cdecl (button, menu); version numbers it prints
		constexpr std::uintptr_t MenuRenderVersion = 0x00537E10;
		constexpr std::uintptr_t GameVersionMajor = 0x006BC430;
		constexpr std::uintptr_t GameVersionMinor = 0x006BC434;
		// A button's layout box: thiscall (int* box), ret 4
		constexpr std::uintptr_t GuiButtonRect = 0x00528570;
		// Text drawing as the version renderer does it: a text drawer object (thiscall constructor/destructor), its
		// alignment setters (thiscall (int* status, int align)), its draw (thiscall (int* status, int* box, text,
		// font, flags), ret 0x14), the text conversion (cdecl (const wchar_t*, out, size)) and the font it uses
		constexpr std::uintptr_t TextDrawerConstruct = 0x0062C8F0;
		constexpr std::uintptr_t TextDrawerDestruct = 0x0062C9B0;
		constexpr std::uintptr_t TextDrawerAlignHorizontal = 0x0062CA70;
		constexpr std::uintptr_t TextDrawerAlignVertical = 0x0062CA00;
		constexpr std::uintptr_t TextDrawerDraw = 0x0062CAE0;
		constexpr std::uintptr_t ConvertText = 0x00553B20;
		constexpr std::uintptr_t VersionFont = 0x007413C0;
		// Startup title screen (0x5340F0): the same BltFast-based copy of the "TitleScreen\TitleScreen" image at 0,0
		constexpr std::uintptr_t TitleScreenBlit = 0x005341A9;
		// Its wait: "call 0x5B5E90 (time in ms); mov esi, eax; add esi, 10000", then a loop calling it again until the
		// time is past (0x5341D8 to 0x5341F6, 0x1F bytes, no messages, no sleep); 0x5341F7 goes on.
		constexpr std::uintptr_t TitleScreenWait = 0x005341D8;
		constexpr std::uintptr_t TitleScreenWaitDone = 0x005341F7;
		// The game's clock in milliseconds, cdecl (no arguments).
		constexpr std::uintptr_t GameTimeMs = 0x005B5E90;
		// Shows the finished frame (thiscall on the game object, no arguments); called once per frame
		constexpr std::uintptr_t PresentFrame = 0x005587F0;
		// GUI views (panels' text surfaces, made of 128x32 tiles): put text into a view, thiscall (view, x, y, text,
		// color*, alignment, clear first, font by value, upload), ret 0x70; and draw a view onto the screen,
		// thiscall (view, x, y, flags), ret 0x10
		constexpr std::uintptr_t GuiViewText = 0x0042CB60;
		constexpr std::uintptr_t GuiViewDraw = 0x0042CEE0;
		// Uploads all of a GUI view's 128x32 tiles: thiscall (view), ret 4 (called by GuiViewText when asked to)
		constexpr std::uintptr_t GuiViewUpload = 0x0042CDF0;
		// Clears a whole GUI view: thiscall (view, patterned), ret 8 (patterned: 0x42C9B0's fill, else zeros)
		constexpr std::uintptr_t GuiViewClear = 0x0042CA70;
		// Destructor of the font object GuiViewText takes by value: thiscall ()
		constexpr std::uintptr_t TextFontDestroy = 0x0062C2B0;
		// Screenshot: "%s\SHOT%04d.BMP" name pattern, and the BMP writer it is saved with:
		// thiscall (int* status, const char* fileName, surface description, palette), ret 0x10
		constexpr std::uintptr_t ScreenshotNamePattern = 0x006BC66C;
		// Pointer to the screenshot folder's name, and the "SCRSHOTS" text it points at. The screenshot code
		// (0x558A93 on) reads the name through the pointer every time: folder check, create, file name.
		constexpr std::uintptr_t ScreenshotFolderName = 0x0066F338;
		constexpr std::uintptr_t ScreenshotFolderText = 0x006BC5C8;
		// Takes the screenshot: thiscall int () on the game object, called from the main loop (0x52594B, 0x52596B)
		constexpr std::uintptr_t TakeScreenshot = 0x00558A40;
		// Plays a .tgq video: thiscall void (const char* path), ret 4
		constexpr std::uintptr_t PlayMovie = 0x0053BCD0;
		// The video player's settings block, filled by PlayMovie (flags) and by the player from the film's header (size)
		constexpr std::uintptr_t VideoPlayerConfig = 0x0073EDC8;
		// Focus events while a video plays. The game's window messages go through 0x558C70, which on WM_ACTIVATE,
		// WM_ACTIVATEAPP and WM_NCACTIVATE calls every handler in its list (0x757D71, 8 entries, contexts 0x20 after) as
		// cdecl (event, 0, 0, context): event 0 when the low word of wParam is not 0 (focus coming), 1 when it is (focus
		// lost). A screen mode change calls them with 2 (0x5582FA). PlayMovie adds VideoFocusHandler for as long as it
		// plays: on 0 and on 1 alike it calls VideoTogglePause (cdecl (player), the player at VideoPlayer), which swaps
		// the player's state (+ 0x9C, InterlockedExchange) between 3 playing and 4 paused and stops or plays its sound
		// buffer (+ 0x5C) [verified in the assembly].
		constexpr std::uintptr_t VideoFocusHandler = 0x0053C2E0;
		constexpr std::uintptr_t VideoTogglePause = 0x005A8FF0;
		constexpr std::uintptr_t VideoPlayer = 0x0073F594;
		constexpr std::uintptr_t WriteBitmapFile = 0x005BA650;
		// Screen setup: thiscall int (width, height, bit depth, windowed, two flags) on the game object, ret 0x18
		constexpr std::uintptr_t SetScreenMode = 0x005581B0;
		// "mov [esp+x], 0x140000": the camera focal length (320.0 in 20.12 fixed point) for the player camera and for
		// camera paths. The immediate is the last 4 bytes of each 8-byte instruction.
		constexpr std::uintptr_t PlayerCameraFocal = 0x00598D26;
		constexpr std::uintptr_t PathCameraFocal = 0x0044DA05;
		// "mov eax, [screen width]": the player camera's distance is divided by the screen width, so it moves closer
		// (with a wider angle) the wider the screen is.
		constexpr std::uintptr_t PlayerCameraDistanceWidth = 0x00598CDD;
		// "push 0x18" texture page count passed to the surface cache setup
		constexpr std::uintptr_t SurfaceCachePageCount = 0x00592FAC;
		// The surface cache setup that push feeds (its only caller, at 0x592FB9): thiscall (cache, 0x792D98, page count)
		// ret 8. It makes the 256x256 pages one by one into a linked list; nothing in it is sized by the count.
		constexpr std::uintptr_t SurfaceCacheSetup = 0x00592DB0;
		// That count and setup are only for software rendering. With Direct3D the engine instead makes a pool of 128x128
		// device textures with 0x592BD0 (thiscall (pool, format, minimum, count)), called here with "push 0x200" (512, a
		// 32-bit operand) and "push 0x20": the world's textures and every GUI view tile share it.
		constexpr std::uintptr_t DeviceTextureCountPush = 0x00593247;
		// Skirmish screen (front end window 9): its window config (button list pointer at +0x3E) and button list
		// (0x50 byte entries ending with id -1)
		constexpr std::uintptr_t SkirmishWindowConfig = 0x006AD6A0;
		// The game's import table entries for ddraw.dll's DirectDrawCreate and DirectDrawEnumerateA (patch.dll's on the
		// GOG/Steam release); every call to them goes through these.
		constexpr std::uintptr_t DirectDrawCreateImport = 0x0066C020;
		constexpr std::uintptr_t DirectDrawEnumerateImport = 0x0066C024;
		// The game's DirectDraw (IDirectDraw*), made once at the first screen setup; 0 until then
		constexpr std::uintptr_t GameDirectDraw = 0x0079D3D0;
		constexpr std::uintptr_t SkirmishButtons = 0x006ADD10;
		// Single Player menu (front end window 2): its window config and button list
		constexpr std::uintptr_t SinglePlayerWindowConfig = 0x006AD658;
		constexpr std::uintptr_t SinglePlayerButtons = 0x006ADAE0;
		// Lifts the fog from the whole map and refreshes it, fastcall (map). Level setup calls it for a map revealed in the
		// lobby.
		constexpr std::uintptr_t MapRevealAll = 0x00451EF0;
		// A game text by id, cdecl (id). Returns the text in the game's own drawing format.
		constexpr std::uintptr_t TextById = 0x00553A60;
		// Milliseconds into the current game turn, thiscall (communication). Drawing's blend between turns comes from it.
		constexpr std::uintptr_t TimeIntoTurn = 0x00521C80;
		// The player interface's left and right clicks on the world, thiscall (interface, pressed, what is under the hand),
		// ret 8. The mouse handler 0x406530 only calls them when the click is not on a panel or a menu.
		constexpr std::uintptr_t InterfaceWorldLeftClick = 0x004075E0;
		constexpr std::uintptr_t InterfaceWorldRightClick = 0x00408EE0;
		// A view's making, thiscall (interface, width, height, 0x20, name, view), ret 0x14, and the freeing of its pixels, thiscall
		// (interface, view), ret 4. The game frees all of its views in 0x419C00, fastcall (interface), when the screen changes or the level ends.
		constexpr std::uintptr_t GuiViewCreate = 0x0042C7D0;
		constexpr std::uintptr_t GuiViewFree = 0x0042C980;
		constexpr std::uintptr_t InterfaceReleaseViews = 0x00419C00;
		// A window by id, thiscall (manager, id), ret 4. Opens or closes the side panel, cdecl (0, 0, interface).
		constexpr std::uintptr_t GuiWindowById = 0x0052C540;
		constexpr std::uintptr_t PanelOpenOrClose = 0x004167D0;
		// The interface's mouse button handler, cdecl (button 0xF0 left 0xF1 right, pressed, x, y, interface).
		constexpr std::uintptr_t InterfaceMouseEvent = 0x00406530;
		// Draws the in-game interface, its windows and text, once each frame in every play mode, fastcall (interface). (The
		// chat lines' 0x409F30 is only called in multiplayer, from 0x404034.)
		constexpr std::uintptr_t InterfaceDraw = 0x00419DF0;
		// Wide text into a view: the same arguments as GuiViewText, which it calls. The font the chat lines and options
		// menu use, and a font's copy, thiscall (to, from), ret 4. Where the mouse is on the screen, cdecl () returning
		// { x, y }.
		constexpr std::uintptr_t GuiViewTextWide = 0x0042CAB0;
		constexpr std::uintptr_t ViewTextFont = 0x00741320;
		constexpr std::uintptr_t TextFontCopy = 0x0062C2D0;
		constexpr std::uintptr_t MouseOnScreen = 0x005B2BA0;
		// The neutral player's tag.
		constexpr std::uintptr_t NeutralPlayerTag = 0x006F2548;
		// Changing a tile, as a level script does (case 0x16 of 0x518AD0). Map: set a tile, thiscall (map, x, y, terrain,
		// owner's tag, 1, 1). World's rooms (world + 0x9BE): before and after taking a tile out of its room, thiscall (x, y,
		// x, y); before and after adding one, thiscall (room type, owner's tag, x, y, x, y). Player: take a tile out of its
		// room, thiscall (x, y, 0); add one to a room, thiscall (x, y, room type, game turn, 1, 1, 8). World: the room type a
		// room's terrain is for, thiscall (terrain).
		constexpr std::uintptr_t MapSetTile = 0x0044FC40;
		constexpr std::uintptr_t RoomsBeforeRemoveTile = 0x004F0840;
		constexpr std::uintptr_t RoomsAfterRemoveTile = 0x004F09B0;
		constexpr std::uintptr_t RoomsBeforeAddTile = 0x004F0390;
		constexpr std::uintptr_t RoomsAfterAddTile = 0x004F04F0;
		constexpr std::uintptr_t PlayerRemoveRoomTile = 0x004C5DB0;
		constexpr std::uintptr_t PlayerAddRoomTile = 0x004C5C30;
		// What the game runs after it changes a tile's terrain (digging, reinforcing: 0x453221, 0x4533C0), thiscall (world +
		// WorldRooms, x, y) ret 8. For each room on a tile beside it: takes its wall pieces off (0x4F0D70), works out its walls
		// (0x4ECEB0), clears every object it placed itself (0x4E6BC0) and places them again (0x4EF5E0, 0x4EF1F0, 0x4EDFC0), then puts
		// the wall pieces back (0x4F0E30). MapSetTile alone does none of this.
		constexpr std::uintptr_t RoomsRefreshAround = 0x004ECDB0;
		// Places a room's own objects (Hatchery pens and the like) for the whole room, thiscall (world + WorldRooms, room) ret 4,
		// after 0x4E6BC0 has cleared them; every caller does both. Rooms of type 4, 9, 10, 11, 13, 14, 15 and 26 go to their own
		// placer (0x4DE220 for a Hatchery), which takes the first tiles of the room's chain that fit; then objects one per middle
		// tile, seeded by the tile's x and y. *(this + 6) is the object whose vtable + 0x1D8 gives a tile by x, y [verified].
		constexpr std::uintptr_t RoomsPlaceObjects = 0x004EDFC0;
		// An egg's hatching (object state 0x20, set in the object state table at 0x6E4DBC), fastcall (egg) returning int. It
		// makes a chicken (object type 9) at the egg's position with the egg's owner (+ 0x28), wherever that is. The egg got
		// there from its pen: at 4 seconds old its own state (0x18, 0x4A21C0) throws it off the pen at the pen's facing plus
		// or minus 0x80 (speed 0x6000 across, 0x3000 up), and once it slows below 0x1000 it is put in state 0x20. Nothing
		// keeps that throw inside the room [verified in the assembly].
		constexpr std::uintptr_t EggHatch = 0x004A1CF0;
		// The egg's own state (0x18), fastcall (egg) returning 0 when it removed the egg (not on a pen while young), run
		// every turn while it sits and while it rolls after the throw [verified in the assembly].
		constexpr std::uintptr_t EggTick = 0x004A21C0;
		// An object's turn (object vtable + 0x14), thiscall (object): its state (0x49EED0, the object state table at
		// 0x6E4B3C), then its move (0x4B2030). A chicken's wander state (0x4A24A0) turns it at random (0x4B2C10) and only
		// looks at the room it is in when its walk ends, so chickens walked out of the Hatchery [verified in the assembly].
		constexpr std::uintptr_t ObjectTurn = 0x0049BD20;
		// A thing's move to a position (x, y, z, 12.12 fixed point), thiscall (thing, position) ret 4; it relinks the thing
		// into the new tile's list when the tile changes (0x4B5020) [verified].
		constexpr std::uintptr_t ThingSetPosition = 0x004B5560;
		// A tile's floor height at a position on it, thiscall (tile, position) ret 4 (the egg laying, 0x4DE620, puts eggs
		// 0x800 above it) [verified].
		constexpr std::uintptr_t TileFloorHeight = 0x004545C0;
		constexpr std::uintptr_t WorldRoomTypeOfTerrain = 0x0050B550;
		// The bar text boxes are drawn with (0x411B30) has its sizes fixed in pixels: end pieces 16 wide, all pieces 32 high.
		constexpr std::uintptr_t BarLeftEndWidth = 0x00411BD2;
		constexpr std::uintptr_t BarLeftEndHeight = 0x00411BDA;
		constexpr std::uintptr_t BarMiddleStart = 0x00411C2B;
		constexpr std::uintptr_t BarMiddleShorter = 0x00411C2E;
		constexpr std::uintptr_t BarMiddleHeight = 0x00411C47;
		constexpr std::uintptr_t BarRightEndWidth = 0x00411C95;
		constexpr std::uintptr_t BarRightEndHeight = 0x00411C9D;
		constexpr std::uintptr_t BarRightEndStart = 0x00411CBA;
		// Lobby game options (front end window 0x23): its window config and button list. Creatures tab rows: sliders
		// 0x27E..0x285 (range 0-32, each with its own tick that writes the row's count), number labels 0x286..0x28D
		// (drawn by CreatureCountLabelRender from the text at button + 0x80).
		constexpr std::uintptr_t LobbyOptionsWindowConfig = 0x006B4438;
		constexpr std::uintptr_t LobbyOptionsButtons = 0x006B4480;
		constexpr std::uintptr_t CreatureCountLabelRender = 0x0054A710;
		// Key actions: the table of { key (DirectInput code), modifiers } per action, 8 bytes each. Is an action's key
		// held, thiscall (interface, action, modifiers held, ignore modifiers) ret 0xC (polled for the camera); does a key
		// press match an action, thiscall (interface, key, action, modifiers, ignore modifiers) ret 0x10; neither uses
		// the interface. Is a key held, cdecl (key). Modifiers: 1 Shift, 2 Ctrl, 4 Alt.
		constexpr std::uintptr_t ActionKeyTable = 0x00759D8F;
		constexpr std::uintptr_t IsActionKeyHeld = 0x004097E0;
		constexpr std::uintptr_t ActionKeyMatches = 0x00409740;
		constexpr std::uintptr_t IsKeyHeld = 0x005B2A40;
		// The modifiers held now, cdecl, no arguments: 1 Shift, 2 Ctrl, 4 Alt.
		constexpr std::uintptr_t ModifiersHeld = 0x005B2A50;
		// The game's debug log, cdecl (window, format, ...).
		constexpr std::uintptr_t GameLog = 0x005595C0;
		// A click on a side panel tab, cdecl (button id, panel 1 to 4, interface). With Shift held it pins that panel, so
		// it shares the side panel with each panel chosen after it. Level setup calls it for Creatures with button id 0.
		constexpr std::uintptr_t PanelTabClick = 0x00416C20;
		// The interface's key event, cdecl (key, pressed, modifiers, interface). Keys 1 to 4 are possession's own.
		constexpr std::uintptr_t InterfaceKeyEvent = 0x00405010;
		// Sends the action for a click on a button, thiscall (interface, 0, button id), ret 8.
		constexpr std::uintptr_t InterfaceClickAction = 0x00422600;
		// The Options button's click, cdecl (0, 0, interface): opens or closes the options menu.
		constexpr std::uintptr_t InterfaceToggleOptions = 0x00428710;
		// Sets the state of the hand's cursor, thiscall (cursor, state, now), ret 8.
		constexpr std::uintptr_t CursorSetState = 0x0040ABC0;
		// The DirectInput mouse's buffered data handler, thiscall (mouse, count) ret 4: entries of 16 bytes
		// { offset, data, time, sequence } at the pointer in mouse + 0x30; offset 8 is the wheel.
		constexpr std::uintptr_t MouseHandleData = 0x005DDA90;
		// The input system (0x5B2BA0 loads it). The mouse's motion handler, thiscall (mouse, event) ret 4, event: axis at
		// +0xC (0 x, 4 y, 8 wheel), amount at +0x10. Keeps the mouse inside its bounds and works out its whole numbers,
		// fastcall (mouse). Sets a DirectInput device's cooperative flags, thiscall (device, int* result, flags) ret 8.
		constexpr std::uintptr_t InputSystem = 0x0079CF90;
		constexpr std::uintptr_t MouseMotion = 0x005DD4A0;
		constexpr std::uintptr_t MouseClamp = 0x005DD630;
		constexpr std::uintptr_t InputDeviceSetFlags = 0x005DCE50;
		// Hands a device's wanted cooperative flags (device part +4) to DirectInput, thiscall (device part, int* result) ret 4.
		// Its one caller, 0x5DCCD0, acquires the device straight after [verified].
		constexpr std::uintptr_t InputDeviceApply = 0x005DCBE0;
		// Mouse from Windows messages (WindowsMouse) [verified]. The input system's DirectInput mouse set-up, thiscall
		// (input system, int* result) ret 4: makes the mouse at + 0x5C unless one is there (called from 0x5BB1C0, which
		// hands the possibly empty pointer to 0x5DB6A0; that checks for none). Window (de)activation, thiscall (input
		// system, int* result, active) ret 8: tells the keyboard's and the mouse's device parts (0x5DCCA0, thiscall
		// (device part, active) ret 4, stores it at + 0x10) with no check for a missing mouse, then 0x5BC3A0, thiscall
		// (input system, int* result) ret 4, which does check. The game sets up its mouse (0x557CB0 -> 0x5B2B60) before
		// it makes its DirectDraw.
		constexpr std::uintptr_t InputInitMouse = 0x005BC060;
		constexpr std::uintptr_t InputWindowActivated = 0x005BC210;
		constexpr std::uintptr_t InputDeviceSetActive = 0x005DCCA0;
		constexpr std::uintptr_t InputApplyCooperation = 0x005BC3A0;
		// The vtables of the DirectInput mouse's two input events (0x5DDA90 makes them): movement (0x14 bytes) and a
		// button (0x24 bytes). Their one function hands the event to a listener (its + 0x14 or + 0x18). The control keys
		// object's vtable: + 0x14 passes a movement on (0x5BB660), + 0x18 a button (0x5DD010, which puts its own Shift,
		// Ctrl and Alt bits into the button's flags first).
		constexpr std::uintptr_t MouseMoveEventVtable = 0x00672900;
		constexpr std::uintptr_t MouseButtonEventVtable = 0x006728F8;
		constexpr std::uintptr_t ControlKeysVtable = 0x00672888;
		// What those vtables hold (checked before the mouse is replaced).
		constexpr std::uintptr_t MouseMoveEventSend = 0x005DD8B0;
		constexpr std::uintptr_t MouseButtonEventSend = 0x005DD8C0;
		constexpr std::uintptr_t ControlKeysMouseMove = 0x005BB660;
		constexpr std::uintptr_t ControlKeysMouseButton = 0x005DD010;
		// The options menu's mouse sensitivity (an int; the Options slider 0x9C copies it to front end + 0x31176).
		constexpr std::uintptr_t MouseSensitivity = 0x00759C21;
		// Sets the pointer's image, cdecl (int* result, image, source rect, hot spot); a null image shows no pointer.
		constexpr std::uintptr_t SetPointerImage = 0x005B2BD0;
		// Draws the icons beside the hand (held things, what a click will do), thiscall (interface, x, y) ret 8.
		constexpr std::uintptr_t InterfaceDrawHandIcons = 0x0040CAE0;
		// Things in use, a byte for each tag (ThingTable holds their pointers). A creature's vtable, which tells them apart.
		constexpr std::uintptr_t ThingInUse = 0x006ED540;
		constexpr std::uintptr_t CreatureVtable = 0x0066D524;
		// What level load does with a creature's record in the things file, by kind: neutral, hero, keeper. Sets up the
		// decoded record, fastcall (decoded). Fills it from the file's bytes, thiscall (decoded, bytes) ret 4. Makes the
		// creature from it, cdecl (decoded, world), 0 if it could not.
		constexpr std::uintptr_t NeutralCreatureInit = 0x0056F2D0;
		constexpr std::uintptr_t NeutralCreatureRead = 0x0056F330;
		constexpr std::uintptr_t NeutralCreatureMake = 0x00520180;
		constexpr std::uintptr_t HeroCreatureInit = 0x0056F3A0;
		constexpr std::uintptr_t HeroCreatureRead = 0x0056F430;
		constexpr std::uintptr_t HeroCreatureMake = 0x00520460;
		constexpr std::uintptr_t KeeperCreatureInit = 0x0056F4D0;
		constexpr std::uintptr_t KeeperCreatureRead = 0x0056F540;
		constexpr std::uintptr_t KeeperCreatureMake = 0x005208B0;
		// Pointer to the game's global settings; at +0xE1 + level * 2, a creature's most health at that level as a 16 bit percent of
		// its type's health (level 1 uses the type's health as it is). The creature makers set health to that most health times the
		// record's percent / 100, at least 1 (0x5208B0 from 0x52099B: 1 + trunc(percent * 0.01 * most - 1.0)) [verified].
		constexpr std::uintptr_t GlobalSettingsPointer = 0x006E4154;
		constexpr std::uintptr_t TrapInit = 0x0056F650;
		constexpr std::uintptr_t TrapRead = 0x0056F690;
		constexpr std::uintptr_t TrapMake = 0x00520E50;
		constexpr std::uintptr_t DoorInit = 0x0056F5C0;
		constexpr std::uintptr_t DoorRead = 0x0056F600;
		constexpr std::uintptr_t DoorMake = 0x00520F40;
		// Objects (thing kind 0xC2) the same way: the reader's case 0xC2 in 0x56FA00 runs these two, then the level load's object
		// maker, put in its table at 0x51E13A. The maker makes the object with 0x4B7A90 and returns 0 when the world has no room.
		constexpr std::uintptr_t ObjectInit = 0x0056F6E0;
		constexpr std::uintptr_t ObjectRead = 0x0056F730;
		constexpr std::uintptr_t ObjectMake = 0x00520BB0;
		// Draws one map tile, thiscall (tile drawer, x, y, pass) ret 0xC, 1 when it went well (vtable 0x66CDC8). On pass 0 it also
		// draws the hand's box: if the tile is on the box's border it moves the drawer's box piece there, sets which sides show
		// and has the renderer draw it. The box is the engine interface's (*(drawer +4) +0x10) +0x10 or +0x4E, x1 y1 x2 y2 with
		// x2 and y2 past the end [verified in the assembly at 0x43D4E3 to 0x43D6A2].
		constexpr std::uintptr_t TileDraw = 0x0043CC30;
		// Level load's makers for a dead body (Things kind 0xCA) and an effect generator (0xCB), cdecl (decoded, world), as its table
		// sets them (0x51E0A0) and the things reader calls them (0x570500, 0x570630) [verified]. The dead body's returns 1 when made,
		// the effect generator's 0 when not.
		constexpr std::uintptr_t DeadBodyMake = 0x005212D0;
		constexpr std::uintptr_t EffectGeneratorMake = 0x00521380;
		// The engine's own things (not ThingTable), pointers by a 16 bit index.
		constexpr std::uintptr_t EngineThingTable = 0x006D6458;
		// Moves an engine thing, thiscall (thing, position) ret 4: x, y, z, 4096 to a tile. It moves it to the new tile's list
		// of things and marks it placed (+0x100 bit 0) [verified in code].
		constexpr std::uintptr_t EngineThingMove = 0x00446C70;
		// Selling what is on a tile, thiscall (player, x, y) ret 8: the player's own door or trap. They price it, pay it, play
		// its sounds and set its health to 0, for its next tick to remove it. Room tiles sell through PlayerRemoveRoomTile.
		constexpr std::uintptr_t PlayerSellDoor = 0x004C3670;
		constexpr std::uintptr_t PlayerSellTrap = 0x004C3A20;
		// The percent of its cost that anything sells for (a byte), read by all three prices (0x4A77E0, 0x4AABB0, 0x4EBF20).
		constexpr std::uintptr_t SellPercent = 0x006F28EA;
		// Shows money floating up from a place, thiscall (world, ...4 arguments) ret 0x10.
		constexpr std::uintptr_t ShowMoneyText = 0x0050FB70;
		// Their vtables. Slot +0x30 of each removes the thing, fastcall (thing).
		constexpr std::uintptr_t TrapVtable = 0x0066D754;
		constexpr std::uintptr_t DoorVtable = 0x0066D6EC;
		// An object's (set by its constructor at 0x4B720C). Its slot +0x30 (0x49BEC0) hands it to 0x4B7B10, which destroys and frees it.
		constexpr std::uintptr_t ObjectVtable = 0x0066D654;
		// A dead body (0x140 bytes, made by 0x4B7FC0, its type at +0x128). Its vtable +0x30 (ThingRemoveSlot) calls 0x4B81A0 on
		// the creature collection (0x6E504C) with its tag: unlinked, destroyed, back to the pool [verified].
		constexpr std::uintptr_t DeadBodyVtable = 0x0066D904;
		// An effect generator (made by 0x4B81E0, constructor 0x4B7560, its type at +0x44 from the record's +0x1D). Its vtable +0x30
		// calls 0x4B8250 on the creature collection with its tag: unlinked, destroyed, back to the pool [verified].
		constexpr std::uintptr_t EffectGeneratorVtable = 0x0066D814;
		// Removes a creature and returns it to the pool, thiscall (world + WorldCreatures, tag) ret 4.
		constexpr std::uintptr_t CreatureRemove = 0x004B7A20;
		// Action 0x66's handler, thiscall (world, action) ret 4: tears the level down and loads it again from the level name
		// (GameConfigLevelName) without leaving the game, as for a restart. Refuses in multiplayer.
		constexpr std::uintptr_t ReloadLevelInPlace = 0x00513870;
		// Lays out the side panel's shown windows (ids 1 to 4), fastcall (interface): it divides the panel's width by how many
		// are shown, so with none (the editor hides them) it divides by 0. Called from 0x416FD0 when the panel is out.
		constexpr std::uintptr_t PanelWindowsLayout = 0x00417310;
		// The game turn the minimap last drew for: it only draws again when the turn differs (0x41A9B0).
		constexpr std::uintptr_t MinimapLastTurn = 0x006D394C;
		// Draws a side panel tab button, cdecl (button, interface).
		constexpr std::uintptr_t PanelButtonRender = 0x00412260;
		// The window procedure of the game's window class.
		constexpr std::uintptr_t GameWindowProc = 0x005B5070;
		// A window's live button by id, thiscall (front end, id, window id) ret 8; null when not found
		constexpr std::uintptr_t FindButton = 0x00532170;
		// Where the game's window procedure sends WM_CHAR (as a wide character) for text input: cdecl (message, char)
		constexpr std::uintptr_t GameTypedCharacter = 0x005B2C10;
		// Front end text buttons: renderer, cdecl char (button, front end), drawing text pre-rendered by
		// GuiPrerenderTexts: thiscall (window id, slot, entries, font) on the front end, ret 0x10. Each entry is
		// 0x88 bytes {text id or 0, button id, inline text}; the list ends at button id 0.
		constexpr std::uintptr_t ClickButtonRender = 0x00532670;
		constexpr std::uintptr_t GuiPrerenderTexts = 0x005329A0;
		// Opens the skirmish screen: cdecl void (front end). Puts the player in slot 0 and adds one computer player.
		constexpr std::uintptr_t SkirmishSetup = 0x00538AC0;
		// Lobby player slots (8 x 63 bytes) and their list text (8 x 0x66 bytes), and the selected row of the list
		constexpr std::uintptr_t PlayerSlots = 0x0073F5C8;
		constexpr std::uintptr_t PlayerSlotTexts = 0x0073F858;
		constexpr std::uintptr_t PlayerListSelection = 0x00740340;
		// The player color table: three shades a player number (0-7), 4 bytes each (red, green, blue, 255): bright, dark,
		// middle. Filled by the game's start-up (0x4C04C0; keepers 3-7 red, blue, green, yellow, purple); read through
		// 0x50F830 (player tag, shade) and directly (the lobby's rows, an engine thing's owner tint, 0x436361).
		constexpr std::uintptr_t PlayerColors = 0x006E5058;
		// Claimed floors and wall tops are per-player meshes, not tinted by the table above: at each game's player set-up
		// (0x4C1E20) the game loads "Claimed Floor<d>_<n>" (0x4C2980, into player + 0x10AA + n*4) and "Claimed Top<d>_<n>"
		// (0x4C2A70, into + 0x10BE), n 0-4, for each player object d (0-6, player number d + 1). Both thiscall (players;
		// base name) ret 4; the name being loaded is sprintf'd into MeshNameBuffer before each load. A tile's mesh comes from
		// its owner's arrays (0x43E860). Their colour is in their textures.
		constexpr std::uintptr_t ClaimedFloorMeshesLoad = 0x004C2980;
		constexpr std::uintptr_t ClaimedTopMeshesLoad = 0x004C2A70;
		constexpr std::uintptr_t MeshNameBuffer = 0x00740C20;
		// Engine materials: create-or-find by description, cdecl (description, add now) returning the material's number
		// (the mesh loader's material callback 0x59A650 calls it). The materials (a pointer to an array of pointers) and
		// their count. A material: + 4 its texture entries (0x10 bytes each, the first dword a texture handle), + 0x10
		// flags (0x4000: all of + 0x15 textures * + 0x19 frames are filled, else only the first), + 0x15 / + 0x19 ints.
		constexpr std::uintptr_t MaterialCreate = 0x0057C850;
		constexpr std::uintptr_t Materials = 0x00765B00;
		constexpr std::uintptr_t MaterialCount = 0x00765B04;
		// The texture names: a pointer to entries of 0xC bytes { name, handle, ? } by a handle's number (handle + 0x24).
		// A texture handle: + 0 its surface (null while not loaded), + 0x18 the next smaller version (mip level).
		constexpr std::uintptr_t TextureNames = 0x00792D44;
		// Loads a texture handle's surface when it has none, fastcall (handle): from the texture cache file (a compressed
		// surface, 0x591070), else decoded and rescaled (0x5911E0). The cache placing (0x5941B0) calls it before painting.
		// A handle's + 0x40 / + 0x41 bytes are its width and height in pixels.
		constexpr std::uintptr_t TextureHandleLoad = 0x00590EC0;
		// An engine surface's paint, thiscall (destination; source, x, y) ret 0xC: the source's pixels into the destination
		// at x, y (pixels). Texture handles are painted into the cache pages with it when placed (0x5941B0 and others),
		// and into the scaler's work surfaces (SurfaceScaler + 1 and + 5, pointers) while a texture is rescaled.
		// Surfaces: + 0 vtable (+ 0x18 lock, thiscall () returning the pixels; + 0x1C unlock, thiscall (pixels) ret 4),
		// + 4 width, + 8 height, + 0xC pitch (set by lock), + 0x10 pixel format (+ 8 bytes a pixel, + 0x1C/0x20/0x24/0x28
		// red, green, blue, alpha masks).
		constexpr std::uintptr_t EngineSurfacePaint = 0x00590450;
		constexpr std::uintptr_t SurfaceScaler = 0x00792D48;
		// The minimap's colors from GUI\map\MapColours.png, fastcall (minimap), when a level's interface is set up (called
		// from 0x402F46): into the minimap's table (minimap + 0x138A, a pointer; entries 0xFFBBGGRR, red in the low byte), three a player for
		// players 1-7 (from + 0x48, + 0x6C, + 0x90, 4 bytes a player), each also handed to the minimap's drawer
		// (minimap + 0x53) by its vtable + 0x294, thiscall (player, kind 1 / 0 / 2, color pointer) ret 0xC. The draw reads
		// the table by index (0x41ECE0; the local player's own flash through all the players' entries).
		constexpr std::uintptr_t MinimapColorsLoad = 0x0041E810;
		// Lobby counts of human and computer players (Add Computer Player checks their sum against the map's slots)
		constexpr std::uintptr_t LobbyHumanCount = 0x0073FC18;
		constexpr std::uintptr_t LobbyComputerCount = 0x0073FC10;
		// "mov [local player number], 3" when a skirmish starts (the player number is 3 + keeper slot)
		constexpr std::uintptr_t SkirmishStartLocalPlayer = 0x00531B3E;
		constexpr std::uintptr_t LocalPlayerNumber = 0x0075AD53;
		// Co-op campaign. The game's table of campaign level names (wide text, by level number); the front end's main
		// loop, thiscall (front end), which returns when a game is to start; the game settings it leaves behind.
		constexpr std::uintptr_t CampaignLevelNames = 0x006B83EC;
		constexpr std::uintptr_t GameComponentScreenCall = 0x005253EA;
		constexpr std::uintptr_t GameConfigFrontEnd3d = 0x0075AEAF;
		constexpr std::uintptr_t GameConfigNoGame = 0x0075AEAB;
		// Which level's result gets written: the level's number (byte) and a secret level's number (int).
		constexpr std::uintptr_t PlayerLevelNumber = 0x00759B38;
		constexpr std::uintptr_t PlayerSecretLevel = 0x0075A661;
		constexpr std::uintptr_t LevelLostModeRead = 0x004BEACB;
		constexpr std::uintptr_t LevelWonModeRead = 0x004BEBD6;
		constexpr std::uintptr_t LevelEndingModeRead = 0x004BEDC0;
		// The loaded level's flags byte read by the campaign win branch after 0x4BEDC0: bit 3 unlocks a secret level
		// (0x5615F0), bit 4 another (0x561750), neither: Horny comes for the Portal Gem (0x4BF410). Both in PlayerSetStatus.
		constexpr std::uintptr_t LevelEndingFlags = 0x006F2957;
		constexpr std::uintptr_t GemEndingModeRead = 0x004BF41E;
		constexpr std::uintptr_t OtherEndingModeRead = 0x004BF9EE;
		// The victory cinematic creature's waits (states), thiscall (state), and each one's code past its camera check.
		constexpr std::uintptr_t GemPathWait = 0x00474100;
		constexpr std::uintptr_t GemPathReadyBody = 0x0047413B;
		constexpr std::uintptr_t GemMoveWait = 0x004741D0;
		constexpr std::uintptr_t GemMoveReadyBody = 0x00474211;
		// Saving the world to a file, thiscall (world, name), ret 4; the game's rolling saves use it. File write and read,
		// cdecl (status out, file, data, size, done out).
		constexpr std::uintptr_t WorldSaveFile = 0x0050FD10;
		constexpr std::uintptr_t FileWrite = 0x005B3EE0;
		// A list of files in a folder (0x12 bytes: + 0 the found names, + 4 a flag, + 5 the pattern, + 9 the folder; the
		// last two are kept as pointers). Emptying it, thiscall (list); filling it, thiscall (list, folder, pattern, flag)
		// ret 0xC. The in-game save and load windows' list of saved games (its folder SavesFolder, "*.sav", 1).
		constexpr std::uintptr_t FileListReset = 0x0054F0D0;
		constexpr std::uintptr_t FileListCollect = 0x0054F0F0;
		constexpr std::uintptr_t InGameSaveList = 0x006CE980;
		constexpr std::uintptr_t FileRead = 0x005B3EB0;
		// Two machines on one keeper. Sending an action, thiscall (session, action), ret 4. Handlers run on every machine,
		// stdcall (action): the pick-ups (kinds 57 to 60) and casting a keeper spell.
		constexpr std::uintptr_t SessionPushAction = 0x00527130;
		constexpr std::uintptr_t ActionPickup1 = 0x00513480;
		constexpr std::uintptr_t ActionPickup2 = 0x005134E0;
		constexpr std::uintptr_t ActionPickup3 = 0x005135B0;
		constexpr std::uintptr_t ActionPickup4 = 0x00513600;
		constexpr std::uintptr_t ActionCastSpell = 0x00513340;
		// A player's hand: start a timed action, thiscall (player, type, 3 values), ret 0x10; a thing goes in, ret 4; the
		// timed drop; things in hand as the world asks, thiscall (world, player's tag ...); the player's own.
		constexpr std::uintptr_t PlayerTimedAction = 0x004C09F0;
		constexpr std::uintptr_t PlayerTakeThing = 0x004BC500;
		constexpr std::uintptr_t PlayerDropThing = 0x004C0EE0;
		constexpr std::uintptr_t WorldHasThingsInHand = 0x005094B0;
		constexpr std::uintptr_t WorldThingsInHandCount = 0x005094D0;
		constexpr std::uintptr_t WorldThingInHand = 0x005094F0;
		constexpr std::uintptr_t PlayerThingInHand = 0x004BCAB0;
		constexpr std::uintptr_t PlayerHasThing = 0x004BCAE0;
		// Writing a player into and reading it from a save, thiscall (player, file*), ret 4.
		constexpr std::uintptr_t PlayerSave = 0x004B8D40;
		constexpr std::uintptr_t PlayerLoad = 0x004B9250;
		// Possess: making a shot, thiscall (creature list, type, keeper, position, tag out, shot out, flags), ret 0x18; a
		// shot arriving, thiscall (shot); the two messages that put the camera in a creature, and taking it out, thiscall
		// (bridge, data), ret 4.
		constexpr std::uintptr_t CreatureListMakeShot = 0x004B7CD0;
		constexpr std::uintptr_t ShotReachedTarget = 0x004AEEA0;
		constexpr std::uintptr_t BridgeEnterCreatureA = 0x004483D0;
		constexpr std::uintptr_t BridgeEnterCreatureB = 0x00448440;
		constexpr std::uintptr_t BridgeLeaveCreature = 0x00442820;
		// Local movement in a creature (thiscall (interface)), and the global it changes.
		constexpr std::uintptr_t InterfaceLocalMovement = 0x0040E910;
		constexpr std::uintptr_t PilotNavigation = 0x006EC9E4;
		// "Is the keeper in a creature" where health flowers and a creature's mesh are drawn.
		constexpr std::uintptr_t FlowerPossessedCompare = 0x0048E8E1;
		constexpr std::uintptr_t MeshPossessedCompare = 0x00595416;
		// Possession actions (PossessionActions, docs/game/possession.md). The possessed creature's attack and use action,
		// thiscall (world, action), ret 4: data3 bit 0x8000000 is a melee hit at what is in front. Leaving a creature, an
		// action handler that reads only the action's player tag, thiscall (world, action), ret 4.
		constexpr std::uintptr_t PossessedAttackAction = 0x00514830;
		constexpr std::uintptr_t StopPossessionAction = 0x00514590;
		// Whether a creature's melee is ready, thiscall (creature, creature data), ret 4.
		constexpr std::uintptr_t CreatureMeleeReady = 0x0048C9B0;
		// The thing-in-front search (0x515420, also what the interface highlights, 0x40BE21 and 0x40FCEA) keeps a thing
		// when this says so, thiscall (world, creature, thing, flag), ret 0xC: creatures, and only objects of types 1-3
		// (gold, if the creature can carry more), 9, 14, 15, 16, 46, 64 (and 94 of another keeper), each also in reach
		// (0x5162E0, thiscall (world, creature, thing), ret 8: within 0x80 of its facing and close enough by both sizes).
		constexpr std::uintptr_t PossessedTargetFilter = 0x00516050;
		// The search itself (world vtable + 0x334), thiscall (world, creature, position, angles), ret 0xC: the tag of the
		// nearest thing kept. The game's angle of a direction, cdecl (dx, dy): 0 to 0x7FF, from a table (no floating point).
		constexpr std::uintptr_t PossessedThingInFront = 0x00515420;
		// The ranged abilities' search (world vtable + 0x338), thiscall (world, creature, position, angles, flag), ret 0x10: a tag.
		constexpr std::uintptr_t PossessedRangedInFront = 0x00515520;
		constexpr std::uintptr_t GameAngle = 0x0065FB54;
		constexpr std::uintptr_t PossessedTargetReach = 0x005162E0;
		// Experience: added up to the next level and no further, thiscall (creature, amount), ret 4.
		constexpr std::uintptr_t CreatureAddExperienceCapped = 0x004858B0;
		// A state block (creature + 0x24E): set its state, thiscall (block, state, value), ret 8; go to bed (when its
		// sleep left is not 0), thiscall (block); follow a possessed creature, thiscall (block, leader tag), ret 4.
		constexpr std::uintptr_t StateBlockSetState = 0x00476D30;
		constexpr std::uintptr_t StateBlockGoToBed = 0x0046F440;
		constexpr std::uintptr_t StateBlockFollow = 0x00461980;
		// Walk somewhere, thiscall (state block, position (x, y, z), state on arrival, walking rules), ret 0xC: state 6 (walking)
		// when a path is found, 0 when not.
		constexpr std::uintptr_t StateBlockWalkTo = 0x00471890;
		// The game's direction tables (32 bit, 16.16, by angle 0 to 0x7FF; they overlap, the second a quarter turn on): a
		// heading's step across is the first's value, its step down the second's negative (as the possessed melee, 0x514830).
		constexpr std::uintptr_t DirectionTableX = 0x006C1488;
		constexpr std::uintptr_t DirectionTableY = 0x006C1C88;
		// A creature's attack target (pit pairs use it): set, thiscall (creature, tag), ret 4; clear, thiscall (creature).
		constexpr std::uintptr_t CreatureSetAttackTarget = 0x0048F390;
		constexpr std::uintptr_t CreatureClearAttackTarget = 0x0048F3F0;
		// A Prison room: open or close its door (by the room's door state), thiscall (room). An object's animation,
		// thiscall (object, animation), ret 4.
		constexpr std::uintptr_t PrisonToggleDoor = 0x004E35C0;
		// Whether a room's tile takes in what the room holds, thiscall (tile): for a Prison, tile + 0xC is -1 (not its fence
		// ring or door tile). The room's add-a-creature (0x4E5A70) needs it before taking a prisoner.
		constexpr std::uintptr_t TileTakesPrisoners = 0x00454110;
		constexpr std::uintptr_t ObjectSetAnimation = 0x0049EF60;
		// Globals: a byte the game uses as game turns per second (the Prison door stays open twice that many room
		// ticks), and the highest level the Training Room trains to (byte).
		constexpr std::uintptr_t TurnsPerSecond = 0x006F2590;
		constexpr std::uintptr_t MaxTrainingLevel = 0x006F28FC;
		// Training costs this much gold a second (signed 16 bit, stored negative). A player's gold change, thiscall (player,
		// amount, direct, tile x, tile y), ret 0x10: a negative amount with direct 0 is taken from its Treasuries and Heart
		// (room types 1 and 5), returning what was taken. Gold shown over a place is ShowMoneyText: (owner tag, position,
		// amount, 0). A player event, thiscall (world + 0x2E54, event, tag, value, value, value), ret 0x14.
		constexpr std::uintptr_t TrainingCostPerSecond = 0x006F291D;
		constexpr std::uintptr_t PlayerTakeGold = 0x004BA420;
		constexpr std::uintptr_t PlayerEvent = 0x004C4600;
		// In a shot's flight (0x4ABxxx): a shot cast by a possessed creature looks for a Training Target near it and, when
		// it finds one, makes an effect there: this is that call, call dword ptr [edx + 0x38C] (6 bytes; the shot in ESI,
		// the world in ECX, its vtable in EDX, five arguments pushed). Nothing else (no experience) [verified].
		constexpr std::uintptr_t PossessedShotTrainingEffect = 0x004ABE54;
		constexpr std::uintptr_t PossessedShotTrainingCheck = 0x004ABE28;
		// Drawing a dig mark and taking one away, thiscall (bridge, action), ret 4.
		constexpr std::uintptr_t BridgeShowDigMark = 0x00442570;
		constexpr std::uintptr_t BridgeHideDigMark = 0x004425C0;
		constexpr std::uintptr_t GameConfigLevelName = 0x0075AD57;
		constexpr std::uintptr_t GameConfigHasSaveFile = 0x0075AE5B;
		constexpr std::uintptr_t GameConfigCreatureCount = 0x0075AECF;
		constexpr std::uintptr_t GameConfigRoomCount = 0x0075AED7;
		constexpr std::uintptr_t GameConfigDoorCount = 0x0075AEDF;
		constexpr std::uintptr_t GameConfigTrapCount = 0x0075AED3;
		constexpr std::uintptr_t GameConfigSpellCount = 0x0075AEDB;
		// Instructions that read the play mode to choose between campaign and multiplayer rules.
		constexpr std::uintptr_t WorldSetupModeRead1 = 0x0050C89D;
		constexpr std::uintptr_t WorldSetupModeRead2 = 0x0050C8FE;
		constexpr std::uintptr_t WorldSetupModeRead3 = 0x0050C982;
		constexpr std::uintptr_t WorldSetupModeRead4 = 0x0050C9C2;
		constexpr std::uintptr_t WorldSetupModeRead5 = 0x0050CB71;
		constexpr std::uintptr_t WorldSetupModeRead6 = 0x0050CBF6;
		constexpr std::uintptr_t TimeLimitModeRead = 0x0050A67A;
		constexpr std::uintptr_t HeartLostModeCompare1 = 0x004BB368;
		constexpr std::uintptr_t HeartLostModeCompare2 = 0x004BB39A;
		constexpr std::uintptr_t PlayerStatusModeRead = 0x004BEF14;
		// Gives a player its result (2 won, 1 and 3 lost), thiscall (player, result), ret 4.
		constexpr std::uintptr_t PlayerSetStatus = 0x004BE630;
		// World: the local player's tag (vtable + 0x264), one turn (thiscall (world, actions), ret 4), one action
		// (thiscall (world, action), ret 4). A player's computer player: make it, thiscall (player); its level, thiscall
		// (player, level), ret 4. Does a level's trigger hold, thiscall (player, trigger), ret 4.
		constexpr std::uintptr_t WorldGetLocalTag = 0x00508C60;
		constexpr std::uintptr_t WorldTick = 0x0050A450;
		constexpr std::uintptr_t WorldAction = 0x0050E5C0;
		constexpr std::uintptr_t PlayerInitAi = 0x004BB800;
		constexpr std::uintptr_t PlayerSetAiLevel = 0x004BB850;
		constexpr std::uintptr_t PlayerCheckTrigger = 0x004C7B20;
		// Camera: load a scripted path, thiscall (camera, path, x, y, flags), ret 0x10, and where that goes on after its
		// checks of the camera's state; end a path, thiscall (camera); end a move, thiscall (camera, 0), ret 4 (with a time
		// in ms instead of 0 it starts a move of that length, towards the camera's move targets).
		constexpr std::uintptr_t CameraLoadPath = 0x0044A370;
		constexpr std::uintptr_t CameraLoadPathBody = 0x0044A3B5;
		constexpr std::uintptr_t CameraFinishPath = 0x0044D8F0;
		constexpr std::uintptr_t CameraFinishTween = 0x0044BD70;
		// The camera's zoom by an amount, thiscall (camera, amount) ret 4 (raw bytes in our export): unless the camera is
		// held (+ 0xD92 bit 8, or a move under way at + 0xDD8), zoom (+ 0xD80) += amount, kept from the minimum
		// (+ 0xD50) to the maximum (+ 0xD54) - 1. The zoom keys and the mouse wheel go through it.
		constexpr std::uintptr_t CameraZoomBy = 0x00449CA0;
		// The network object and its data send, thiscall (network, data, size, target), ret 0xC. Target 0xFFFF is everyone.
		constexpr std::uintptr_t NetworkObject = 0x00758160;
		constexpr std::uintptr_t NetworkSendData = 0x0055AC20;
		// Multiplayer lobby (front end window 32): its window config and button list (Add Computer Player is id 528
		// there too, the player list selection is PlayerListSelection, the chat is 0x1CD / 0x1CC)
		constexpr std::uintptr_t MultiplayerWindowConfig = 0x006B39A0;
		constexpr std::uintptr_t MultiplayerButtons = 0x006B39E8;
		// This machine's place among the network's players (fastcall on the network object 0x758160), which is the
		// lobby slot the game gave its human. Only used for the local player number (3 + it) when a multiplayer game
		// starts: 0x5430D7, 0x543497 and 0x52824F.
		constexpr std::uintptr_t NetworkLocalIndex = 0x0055AE30;
		// The lobby slot this machine's player is in (int). Its slot is sent to the others as message 0x68 (0x544530)
		// when it changes (ready and so on), and they unpack it into the slot number the message carries.
		constexpr std::uintptr_t MyLobbySlot = 0x00758334;
		// Lobby network messages, cdecl (message, size, sender, front end): sender is the sending player's network slot
		// (or 0xFFFC / 0xFFFE), messages from MyLobbySlot are ignored, and per-player data (message 0x68 = that
		// player's slot, 0x66 ping) is written into the slot numbered sender. For 0x68 a computer player in that slot is
		// first moved to the first empty slot.
		constexpr std::uintptr_t LobbyMessage = 0x00544060;
		// Makes a player from the map: thiscall on world + 0x7A (number, tag out), ret 8. Keepers (3 and up) then get
		// the local player check (0x51F1C5); the Good (1) and neutral (2) players do not (0x51EF99 branches).
		constexpr std::uintptr_t WorldCreatePlayer = 0x004C2340;
		// Player init (0x4B8640, reached from WorldCreatePlayer): "cmp al, 1; jz; cmp al, 2; jz" skips making the
		// task list (player + 0x1105) for the Good and neutral players
		constexpr std::uintptr_t PlayerInitTaskSkip = 0x004B87B5;
		// Per-player game tick, thiscall (player). Returns early for the Good player (only maxes its mana), so the
		// Good player's hand actions (pick up, drop) never finish.
		constexpr std::uintptr_t PlayerTick = 0x004B9EC0;
		// Runs the player's current timed action (player + 0x186) and finishes it when its time is up, thiscall
		// (player). Picking up a creature happens when the action finishes.
		constexpr std::uintptr_t PlayerActionTick = 0x004C0AA0;
		// In the player tick: "cmp ax, [Good player tag]; jnz" (EDI = player). Equal goes to 0x4B9F43 (set mana to the
		// maximum and return); not equal to 0x4B9F57, the rest of the tick (gold count, then the keeper part 0x4BB2A0).
		constexpr std::uintptr_t PlayerTickGoodCheck = 0x004B9F3A;
		constexpr std::uintptr_t PlayerTickGoodReturn = 0x004B9F43;
		constexpr std::uintptr_t PlayerTickKeeperPart = 0x004B9F57;
		// Keeper part (0x4BB2A0): free workers at the heart while the player has fewer Imps than the minimum
		// (byte 0x6F28D7). "mov eax, [eax + 9]; xor ecx, ecx" reads the Imp count (EAX = its entry, ESI = player);
		// it goes on at WorkerTopUpResume ("mov cl, [minimum]").
		constexpr std::uintptr_t WorkerTopUpCount = 0x004BB479;
		constexpr std::uintptr_t WorkerTopUpResume = 0x004BB47E;
		// The world object (pointer), and the table of every thing (creature, player, ...) by tag
		constexpr std::uintptr_t WorldPointer = 0x006E5054;
		// The game turn of the level being played, thiscall (world): world + 4 -> + 4
		constexpr std::uintptr_t WorldGameTurn = 0x00517490;
		constexpr std::uintptr_t ThingTable = 0x006EE540;
		// After the map changes: every creature of the player that is walking (states 3, 4, 7, or 6 heading for state
		// 1) gets a new path and state 1. fastcall (player). Called from three player functions that change the map.
		constexpr std::uintptr_t ResetCreatureWalks = 0x004E8D30;
		// Payday: where a creature goes for its wage (0x472390, thiscall (state block), creature at block + 8). It walks the
		// owner's rooms (player + 0x36, next + 8) for a Treasury (type 1) or a Dungeon Heart (5, room + 0x9A bit 1 clear)
		// holding gold (room + 0x49), each at its middle tile (room + 0x2B, + 0x2C), that 0x471630 says can be reached
		// (flags 0x80), and keeps the one nearest in a straight line: at 0x472446 "mov ecx, [edi+8]; add ecx, 0x1A;
		// call 0x43C260" (11 bytes; squared distance / 4096 from the creature to the place at [ebp-0x30], pushed just
		// before), "cmp eax, ebx; jnc 0x4724A2", then at 0x472455 the same sum again into EBX (best) with the room into
		// [ebp-8], up to 0x4724A2 [verified in the assembly]. Walls are not counted, so a Treasury behind a wall won over a
		// Heart a short walk away.
		constexpr std::uintptr_t PaydayRoomDistance = 0x00472446;
		constexpr std::uintptr_t PaydayKeepBest = 0x00472455;
		constexpr std::uintptr_t PaydayLoopNext = 0x004724A2;
		// The path search, cdecl (result, start x, start y, end x, end y, -2), places in 1/256 tile (12.12 positions >> 4).
		// The result (0x814 bytes): start x, y, end x, y, then at + 0x10 the number of waypoints (0 for no path) and from
		// + 0x14 the waypoints as x, y pairs, the last the end (0x4D0320, 0x4CEF40). The creature's walking rules are
		// globals set by 0x4D0E60 (thiscall (creature, place, flags)), which calls it the same way; the last search is kept
		// and handed back for the same start and end [verified in the assembly].
		constexpr std::uintptr_t PathFind = 0x004D0E10;
		// The straight-line distance, thiscall (from, to) ret 4: squared / 4096 of two 12.12 positions.
		constexpr std::uintptr_t StraightDistance = 0x0043C260;
		// Choices of a room to walk to that measure each room with StraightDistance right after the creature's own
		// reachability check for it passed (0x471630 or 0x471740 with flags 0x80, both through 0x4D0E60, so the path
		// search's walking rules are this creature's), and keep that number as the best [verified in the assembly]:
		// 0x457010 (a Treasury or Heart with more gold than the creature's + 0x3F4; calls at 0x457107 and 0x45711B),
		// 0x464400 (a Treasury or Heart with room for gold; 0x46450C, then the same sum again inline at 0x464515 into
		// EBX and the room into [ebp-4], up to 0x464562), and rooms' own "nearest room of mine for this creature"
		// (0x4D8870, 0x4DED00 the Hatchery, 0x4E0460, 0x4E2500, 0x4F1A20, 0x4F2B80, 0x4F3560, 0x4F6200). Not here: the
		// other StraightDistance calls, which are ranges, "already there" tests or come before any reachability check.
		constexpr std::uintptr_t NearestRoomDistanceCalls[] = { 0x00457107, 0x0045711B, 0x0046450C, 0x004D896C, 0x004DED9A, 0x004E0528,
			0x004E25F0, 0x004F1AEC, 0x004F2C42, 0x004F362C, 0x004F62E0 };
		constexpr std::uintptr_t GoldDropKeepBest = 0x00464515;
		constexpr std::uintptr_t GoldDropLoopNext = 0x00464562;
		// Lair state handlers, thiscall (state block) returning 0 when there is nothing to do: in a Lair with no bed of its
		// own, walk to a free spot with arrival 0x50 ("make a home"); and that arrival, which makes the bed (0x4E03F0 ->
		// 0x48AD50, the object from creature data + 0x771) and on failure walks to another spot with arrival 0x50. Neither
		// checks that the type has a bed (0x46F390, the usual way in, does).
		constexpr std::uintptr_t LairStateInRoom = 0x0046F4F0;
		constexpr std::uintptr_t MakeHomeArrival = 0x0046F6B0;
		// Workshop: whether the item being made is done, thiscall (list, work done) ret 4, and its percent done (same).
		// Both read the build time of item 0 of the list (a ring of 0x80 items of 0x10 bytes, build time a word at
		// + 0); the item being made is at index list + 0x801 (0x506930 reads it, 0x506970 moves on), count at + 0x803.
		constexpr std::uintptr_t ManufactureDone = 0x00506B50;
		constexpr std::uintptr_t ManufacturePercent = 0x00506B70;
		// Computer player, moving creatures to a threat (0x4FE0B0): "jz" past the drop when the threat's tile is the
		// computer's own, so it only tried to drop onto tiles it cannot drop on.
		constexpr std::uintptr_t ThreatDropOwnTileJump = 0x004FE5AA;
		// The same function's pick of a creature to drop at the threat (ESI the computer player, its keeper at + 0xE with
		// the creature count at + 0x16; EBX the creature): every test that rules a creature out goes to 0x4FE50D (next);
		// the last, "test byte [eax+0x686], 1; jz 0x4FE522" (9 bytes at 0x4FE504, not a worker), picks it. Then 0x48E6A0
		// (creature, keeper tag) says whether it is the keeper's: "test eax, eax; jz 0x4FE914" (8 bytes at 0x4FE544)
		// skips the drop when it is not, and the next pass picks the same creature again.
		constexpr std::uintptr_t ThreatPickLastTest = 0x004FE504;
		constexpr std::uintptr_t ThreatPickNext = 0x004FE50D;
		constexpr std::uintptr_t ThreatPicked = 0x004FE522;
		constexpr std::uintptr_t ThreatPickOwnedTest = 0x004FE544;
		constexpr std::uintptr_t ThreatPickOwned = 0x004FE54C;
		constexpr std::uintptr_t ThreatPickNotOwned = 0x004FE914;
		// Computer player: 16 bit stores of a 32 bit threat total into its response slot (cp + slot * 8 + 0x5A), each
		// "mov word [...], reg" of 5 bytes, followed by its resume address. 0x4F9B90 and 0x4FA000 (two events): enemy
		// threat * cp + 0x4DA / 100 - own threat (DX), and minus own threat (AX after neg); 0x4FE9xx (an attack task):
		// own army's threat / 2 (AX).
		constexpr std::uintptr_t ThreatStoreA = 0x004F9F1E; // [esi + ebp*8 + 0x5A], dx
		constexpr std::uintptr_t ThreatStoreB = 0x004F9F85; // [esi + ebp*8 + 0x5A], ax
		constexpr std::uintptr_t ThreatStoreC = 0x004FA37A; // [esi + ebx*8 + 0x5A], dx
		constexpr std::uintptr_t ThreatStoreD = 0x004FA3B6; // [esi + ebx*8 + 0x5A], ax
		constexpr std::uintptr_t ThreatStoreE = 0x004FEF82; // [ebp + edi*8 + 0x5A], ax
		// Creature melee hit (0x498E30): after the angle from target to attacker (0x65FB54, cdecl (dx, dy)), "and eax,
		// 0xFFFF; add esp, 8" (8 bytes), then "and eax, 0x7FF" at the resume address. ESI is the target creature.
		constexpr std::uintptr_t BackstabAngle = 0x004990DB;
		constexpr std::uintptr_t BackstabAngleResume = 0x004990E3;
		// Whether a creature can be attacked (0x45A8B0, fastcall (creature)): "mov eax, [esi + 0x25E]" (6 bytes) starts the
		// test of its state's flags (0x6DA8C8 + state * 0x19, bit 4: a sleep-type state); the resume address follows it, and
		// the skip address is past the test's "jnz" (on to the attacker slots).
		constexpr std::uintptr_t AttackableStateTest = 0x0045A93F;
		constexpr std::uintptr_t AttackableStateResume = 0x0045A945;
		constexpr std::uintptr_t AttackableStateSkip = 0x0045A952;
		// Attaching a map tile to a room (0x450770, thiscall (map, x, y, room tag, player, flag, flag) ret 0x18):
		// "push 0; mov ecx, esi; call 0x452B70" (9 bytes) sets the tile's burn level to 0; resume after it. 0x452B70 is
		// thiscall (tile, level) ret 4: tile + 0xB bits 1-4.
		constexpr std::uintptr_t RoomAttachBurnReset = 0x004507B5;
		constexpr std::uintptr_t RoomAttachBurnResume = 0x004507BE;
		constexpr std::uintptr_t TileSetBurnLevel = 0x00452B70;
		// Player builds a room tile: the call to the rooms' add-tile event, thiscall (world + 0x9BE, x, y, room type,
		// player tag, rooms left (byte: 96 - player + 0x10F9), signed byte* rooms made, orientation) ret 0x1C.
		constexpr std::uintptr_t BuildRoomTileCall = 0x004C5CC5;
		constexpr std::uintptr_t RoomsAddTile = 0x004ED1A0;
		// Player sells a room tile: the call to the rooms' remove-tile event, thiscall (world + 0x9BE, x, y, rooms left
		// (byte), signed byte* rooms made) ret 0x10. The room's other tiles are attached again with that many new rooms
		// allowed (0x4ED7B0); a tile that then fits nowhere is left with no room.
		constexpr std::uintptr_t SellRoomTileCall = 0x004C5E3B;
		constexpr std::uintptr_t RoomsRemoveTile = 0x004ED440;
		// The drop action (0x4C0EE0): its call of the player's drop, thiscall (player, int position[3], u16* direction)
		// ret 8. The interface's drop rules, thiscall (interface, thing, tile x, tile y) ret 0xC: of the interface it only
		// reads the player tag (+ 8, 16 bit) and the world (+ 0x53).
		constexpr std::uintptr_t DropActionDropCall = 0x004C0F70;
		constexpr std::uintptr_t PlayerDropAt = 0x004BC710;
		constexpr std::uintptr_t InterfaceDropAllowed = 0x0040D8F0;
		// Dungeon Heart room tick (0x4D9A60, thiscall (room), raw bytes in our export): EAX = heart object health * 100 /
		// the maximum (ECX the health; both 0 with no heart object), then "cmp eax, 50; jnz" (5 bytes) raises event 0x27,
		// and at the next address "cmp eax, 10" raises 0x28, both through the event list (0x4C4600) at 0x4D9AF7.
		constexpr std::uintptr_t HeartWarningCompare = 0x004D9ACB;
		constexpr std::uintptr_t HeartWarningFifty = 0x004D9AD0;
		constexpr std::uintptr_t HeartWarningTen = 0x004D9AE2;
		// Hero party spawn (trigger action 0xE), thiscall (world, party index, action point, invasion flag) ret 0xC, 1 done.
		// Each of up to 16 members is made by 0x519B20 (thiscall, ret 0x10, 0 when no creature could be made); its result is
		// tested at 0x51A274 ("test eax, eax; jz 0x51A756", 8 bytes, 0x51A756 returns 0 with the party half made). After the
		// last member the finish code at 0x51A5E2 (same stack as the loop) counts the party, finds the leader (creature
		// + 0x46F bit 2; pointer kept at [esp + 0x88], tag in DI, both 0 for none), at 0x51A66A ("mov eax, [esp+0x18];
		// test eax, eax; jz 0x51A6CF", 8 bytes) plays effect 0x2B at the leader, links the followers, and at 0x51A73A
		// ("mov eax, [esp+0x88]; mov [eax+0x3E1], bl", 13 bytes) gives the leader its place in the party; 0x51A747 returns 1.
		// Loads a PNG picture by name, cdecl (name), through the bridge object (0x795700) method + 0x11C; returns a picture
		// record of 40 bytes ({ red, green, blue, alpha masks, bits per pixel, bytes-per-pixel flag, pixels, width, height,
		// row pitch }) or 0 when the file is missing. Its one caller (0x5912D3) reads width and height (+ 0x1C, + 0x20) and
		// copies the pixels (0x5914E0) without a check; the record is not freed there.
		constexpr std::uintptr_t LoadPngPicture = 0x0059A4B0;
		// Converts a creature type record as read from a creature list (0x1549 bytes) into the game's own, thiscall
		// (type, record) ret 4; its one caller is the list reader (0x56A41A). Animation n of a record is a 0x54-byte
		// resource at + 0x20 + 0x54 * n, its file name first (10 Stunned, 11 In Hand).
		constexpr std::uintptr_t CreatureRecordConvert = 0x00568E30;
		// Builds the meshes of a room tile drawn by the bridge object (called at 0x43E3E0 for room terrain, terrain flag
		// 0x10), thiscall (bridge, x, y, tile, terrain data, mesh arrays) ret 0x14. It takes the tile's room as 0x4559B0
		// does (ThingTable[(tile + 2, 16 bit) & 0xFFF]) and reads the room's data (+ 0x19) with no check.
		constexpr std::uintptr_t RoomTileMeshes = 0x00443610;
		// The sound list thread's stop (0x60D630, thiscall (thread object), from its destructor 0x60D5C0): sets + 0x39
		// (exit) and + 0x3C (wake, InterlockedExchange), then at 0x60D64A waits for + 0x38 (stopped) in a loop that never
		// gives up the processor ("mov al, [esi+0x38]; test al, al; jnz +7; mov al, [esi+0x38]; test al, al; jz -7",
		// 14 bytes, ESI the object); 0x60D658 goes on.
		// The player interface's push of an action, thiscall (interface, action) ret 4: counts it (+ 0xA) and sends it
		// through its session (+ 4) with SessionPushAction.
		constexpr std::uintptr_t InterfacePushAction = 0x004346E0;
		// A text input button's configure, thiscall (button, config) ret 4: the input's character limit (button + 0x280)
		// is the config's + 0x1A, or 11/16 by keyboard layout when that is -1. The limit counts the closing 0, so it holds
		// one character fewer. The TCP/IP menu's port box is button id 0x212 with a limit of 5 (4 digits).
		constexpr std::uintptr_t TextInputConfigure = 0x0052AA10;
		// The front end component (the menus), and its menu action handler, cdecl (argument, action, front end): 0x58 with
		// argument 0x22 opens the map list (window 34) from the skirmish screen, 0x5A starts the skirmish (play mode 2,
		// local player 3, level name from front end + 0x6580, then the game component), 0x18 leaves the skirmish screen,
		// 0x4E is the map list's tick (keep the clicked map) and 0x4F its cross (back to the map chosen before, + 0x6033).
		constexpr std::uintptr_t FrontEndComponent = 0x0070D6D8;
		constexpr std::uintptr_t FrontEndMenuAction = 0x005306F0;
		// Hides (0) or shows (1) a front end window, cdecl (show, window, front end) (Flame calls it changeGui). Window 7 is
		// the briefing: its tick sends action 0x0C (start the game component with what is set up), its cross 0x0A.
		constexpr std::uintptr_t FrontEndChangeWindow = 0x005391F0;
		// The menu step that shows the skirmish screen once the camera has flown there, cdecl (front end): shows window 9
		// and runs action 0x15 (the skirmish screen's set up, its map list). Reached from the Single Player menu's Skirmish
		// button (click 0x538000) and on the way back from a skirmish. (A menu button's click, 0x528610, calls the button's
		// action with config + 0x16 and + 0x1A, then hides the button's window when config + 0x12 is set, then shows the
		// window config + 0xE names: an action on its own switches no windows.) The same function as SkirmishSetup, which
		// SkirmishSwapKeeper hooks; it calls CustomCampaign from there.
		constexpr std::uintptr_t SkirmishScreenStep = 0x00538AC0;
		// Menu window titles are pictures made once when the front end starts (0x536850, from the text ids at 0x6B8468):
		// TitleBake is thiscall (front end; settings), the settings packed: text, picture, second picture, area {0, 0, 640,
		// 70}, size out (4 ints), colour (5 bytes), 0, byte 3, font 9 (8 when the first try is 630 or more wide), 2.
		// A picture is a 0x28-byte record: ImageCreate thiscall (record; {width, height}, pixel format, 0, 0) sets it up,
		// ImageAllocate thiscall (record; result out, 0 when done) gives it pixels (+ 0x18, + 0x20 * + 0x24 bytes).
		// Draws a picture record into a frame at x, y (frame pixels, no scaling): thiscall (front end + 0x30D3F; out,
		// frame (front end + 0x2DD65), x, y, picture, source area or null, 0), ret 0x1C. The picture's format must be the
		// game's (ImagePixelFormat), as ImageCreate makes it. ImageFree, fastcall (record), frees a record's pixels.
		constexpr std::uintptr_t PictureDraw = 0x0052CF90;
		// The 3D engine's camera as it last drew the world (set by 0x575A00 from the camera it is given): position (3
		// floats, tiles), rotation (9 floats, row major, view to world: 0x594E70 multiplies a view direction by it in the
		// screen-to-world code 0x572920), focal length (pixels), the viewport (left, right, top, bottom) and its middle.
		// The projection (0x575F10): screen = middle + focal * view.xy / view.z. Read by the ray tracer.
		constexpr std::uintptr_t EngineCameraPosition = 0x00760AB8;
		constexpr std::uintptr_t EngineCameraRotation = 0x00760AC4;
		constexpr std::uintptr_t EngineCameraFocal = 0x00760AE8;
		constexpr std::uintptr_t EngineCameraCenterX = 0x00760AFC;
		constexpr std::uintptr_t EngineCameraCenterY = 0x00760B00;
		// The bridge's vtable (Flame: CBridge), to check a bridge pointer before reading its lights.
		constexpr std::uintptr_t BridgeVtable = 0x0066CC94;
		// Builds the results screen's table after a game, fastcall (front end): front end + 0x3033C, one row per player
		// (0x370 bytes each), the count by the game type copy 0x75AD4F (2 skirmish: the chosen skirmish map record's
		// players, + 0x80 of record + 0x6608 in + 0x657C; 1 and 4 one row). The skirmish results window (its list at
		// 0x6B1868, widget 195 drawn by 0x53D550) reads the table without a check.
		constexpr std::uintptr_t SkirmishResultsTable = 0x0053C520;
		constexpr std::uintptr_t ImageFree = 0x005B5310;
		// The briefing window (7): its config (first byte the window id, its button list at + 0x3E) and the game's list.
		// The list's boxes take the right of the window from x 764 (2560-wide layout); the left is empty.
		constexpr std::uintptr_t BriefingWindowConfig = 0x006AEF38;
		constexpr std::uintptr_t BriefingButtons = 0x006AEF80;
		constexpr std::uintptr_t TitleBake = 0x00536370;
		// The skirmish map list's background picture (a picture record; its columns, the size column's tinted green),
		// drawn by the list frame widget 231 (0x541940) through the picture draw 0x52CF90, which takes the width from
		// + 0x1C and the height from + 0x20 and steps rows by the pitch (+ 0x24), so a smaller width crops its right side.
		constexpr std::uintptr_t MapListBackground = 0x007401F0;
		// The map list's row highlight picture (the chosen and pointed rows, drawn at the background's left edge by the
		// list's draw 0x5410C0); it also has a green part over the size column.
		constexpr std::uintptr_t MapListRowHighlight = 0x00740218;
		// The chosen map's details (widget 468): the name in a banner, then "Max Players : <second field>" (text 0x5D4,
		// format "%s : %s"), then the third field, each skipped when the chosen row has no such field before the list's
		// 0xFFFF. cdecl (widget, front end); reads the list at SkirmishMapList and the row at MapListChosenRow.
		constexpr std::uintptr_t MapListDetails = 0x00540600;
		constexpr std::uintptr_t ImageCreate = 0x005B5270;
		constexpr std::uintptr_t ImageAllocate = 0x005B52C0;
		constexpr std::uintptr_t ImagePixelFormat = 0x0079D830;
		// A text from the level's briefing table, cdecl (id), game text (an empty one when the table is not loaded). The table
		// is loaded by 0x553800 (level text number; names at 0x6BC100). Ids 0 and 1 are drawn as the level's name, 2 in the
		// briefing's first box and first in the in-game objectives (after main text 0x580), 3 in the briefing's tall box and,
		// with 4 and 5, after main text 0x581 in the objectives (0x4207E0) [unverified: what the stock texts say].
		constexpr std::uintptr_t BriefingText = 0x00553AC0;
		// The skirmish map list (raw bytes in our export): a wide text of 4 fields per map (name, players, size, the map's
		// file field), each ended by 0, the list by 0xFFFF; made from GameAlloc by 0x53FEB0 when the front end starts.
		// Its row count (-1 = count again; 0x540B20 counts the 0s / 4), the clicked row (0x5416E0, cdecl (row, ?, front
		// end), stores it here, then loads that map's preview by its name field).
		constexpr std::uintptr_t SkirmishMapList = 0x00740354;
		constexpr std::uintptr_t SkirmishMapListCount = 0x006B442C;
		constexpr std::uintptr_t MapListChosenRow = 0x00740348;
		constexpr std::uintptr_t MapListClick = 0x005416E0;
		// The game's start-up (0x5A5DA0, cdecl (argc, argv), called once at 0x64780B). It first makes "DKII MUTEX"
		// (0x5BAE70) and quietly stops when the mutex was already there (another copy running). Then each of these calls
		// (a "call rel32", result 0 = failed) ends it without a word: the command line (0x5A63B0, cdecl (argc, argv)),
		// fonts and texts (0x552F10, no arguments), the window (0x557CB0, thiscall on 0x756EE8), speech (0x567F90,
		// thiscall on 0x75B8E8, only with sound), the network library (0x559BE0, thiscall on 0x758160), the Windows check
		// (0x557FB0, thiscall on 0x756EE8) and the components (0x552BE0, no arguments).
		constexpr std::uintptr_t GameStartupCall = 0x0064780B;
		constexpr std::uintptr_t GameStartup = 0x005A5DA0;
		constexpr std::uintptr_t StartupCommandLineCall = 0x005A5FAC;
		constexpr std::uintptr_t StartupCommandLine = 0x005A63B0;
		constexpr std::uintptr_t StartupFontsCall = 0x005A5FBC;
		constexpr std::uintptr_t StartupFonts = 0x00552F10;
		constexpr std::uintptr_t StartupWindowCall = 0x005A60DB;
		constexpr std::uintptr_t StartupWindow = 0x00557CB0;
		constexpr std::uintptr_t StartupSpeechCall = 0x005A6187;
		constexpr std::uintptr_t StartupSpeech = 0x00567F90;
		constexpr std::uintptr_t StartupNetworkCall = 0x005A6199;
		constexpr std::uintptr_t StartupNetwork = 0x00559BE0;
		constexpr std::uintptr_t StartupWindowsCheckCall = 0x005A61D0;
		constexpr std::uintptr_t StartupWindowsCheck = 0x00557FB0;
		// Inside it: "push 0; push 0; push 6; call 0x557FD0" (Windows at least 6.0 by GetVersionExA, stdcall (major,
		// minor, service pack)), then "neg eax; sbb eax, eax; inc eax": the game only passed on Windows before Vista,
		// unless Windows reported an older version to it. The "push 6" is at 0x557FB4.
		constexpr std::uintptr_t WindowsCheckMajorPush = 0x00557FB4;
		constexpr std::uintptr_t StartupComponentsCall = 0x005A61F8;
		constexpr std::uintptr_t StartupComponents = 0x00552BE0;
		// The multiplayer session list (front end 0x54D370, thiscall (front end), raw bytes in our export; disassembled
		// with ml + dumpbin). Sessions: count 0x73FE74, array 0x73FE78 of 0xA4-byte entries (+ 0 version, major << 16 |
		// minor; + 0x28 players in it; + 0x40 map info, 12 bytes: + 0x40 bits 0..6 computer players, + 0x42 (16 bit) the
		// map's players, + 0x41 name length, + 0x44 name hash, + 0x48 file check; + 0x64 name, wide). Each entry's text is
		// name, "#players / max#", map name, "#major.minor" (built in [esp + 0x30], 64 characters, at 0x54D548 every
		// path has it) and the separator. Joinable (front end + 0x602B + index, a byte; written by 0x54D58C) when players
		// < max, the version equals ours (0x6BC430 major, 0x6BC434 minor), the file check equals ours (0x7962A0) and max
		// > 1. At 0x54D548 "mov eax, [0x6BC430]" (5 bytes), at 0x54D58C "test ebp, ebp; jnz +5; mov ebp, 4" (9 bytes).
		constexpr std::uintptr_t SessionListVersionDone = 0x0054D548;
		constexpr std::uintptr_t SessionListVersionResume = 0x0054D54D;
		constexpr std::uintptr_t SessionListJoinableDone = 0x0054D58C;
		constexpr std::uintptr_t SessionListJoinableResume = 0x0054D595;
		constexpr std::uintptr_t SessionCount = 0x0073FE74;
		constexpr std::uintptr_t SessionArray = 0x0073FE78;
		// (The game's version is GameVersionMajor / GameVersionMinor, above.)
		constexpr std::uintptr_t GameFileCheck = 0x007962A0;
		// Finds a session's map among ours, thiscall (front end, map info, wide name out) ret 8: 1 found (its name), 0 not
		// (text 0xC06). Matches by the map info's name length (+ 1) and name hash (+ 4).
		constexpr std::uintptr_t SessionMapLookup = 0x0053ED60;
		constexpr std::uintptr_t SoundThreadStopWait = 0x0060D64A;
		constexpr std::uintptr_t SoundThreadStopResume = 0x0060D658;
		// Loading a saved game (in the game component's loop 0x525370): from 0x52555E to 0x5255AF (0x51 bytes) the save's
		// name (wide, game config 0x75AD47 + 0x90, as 0x55D550 returns it) is copied into a 64-character buffer, turned
		// into the code page's text in a 64-byte one (0x5557E0, WideCharToMultiByte), then "%s%s" of the saves folder
		// (0x758754, a full path) and that name is written into a 64-byte buffer at [esp + 0x114] with no limit. At
		// 0x5255CD "lea eax, [esp+0x114]" (7 bytes) passes it to the save loader (0x50E920); nothing else reads them.
		constexpr std::uintptr_t SaveLoadPathBuild = 0x0052555E;
		constexpr std::uintptr_t SaveLoadPathResume = 0x005255AF;
		constexpr std::uintptr_t SaveLoadPathUse = 0x005255CD;
		constexpr std::uintptr_t GameConfigSaveName = 0x0075ADD7;
		constexpr std::uintptr_t SavesFolder = 0x00758754;
		constexpr std::uintptr_t HeroPartySpawnFailTest = 0x0051A274;
		constexpr std::uintptr_t HeroPartySpawnNext = 0x0051A27C;
		constexpr std::uintptr_t HeroPartyFinish = 0x0051A5E2;
		constexpr std::uintptr_t HeroPartyLeaderEffect = 0x0051A66A;
		constexpr std::uintptr_t HeroPartyLeaderEffectResume = 0x0051A672;
		constexpr std::uintptr_t HeroPartyLeaderEffectSkip = 0x0051A6CF;
		constexpr std::uintptr_t HeroPartyLeaderPlace = 0x0051A73A;
		constexpr std::uintptr_t HeroPartyDone = 0x0051A747;
		// Bump mapping: the game's setting (video settings 0x759A78 + 0x98; 0 off, 1 du/dv, 2 with luminance; written by
		// 0x566860, which also saves it as "BumpMappingConfig"). Read by "mov eax, [0x759B10]" (5 bytes) where the engine
		// loads its textures (0x59856B: when not 0, also BumpWater, BumpWaterBumpMap, BumpWaterEnvMap and the same for
		// lava) and where it starts (0x59D951: 1 gives engine flag 0x10, 2 gives 0x20).
		constexpr std::uintptr_t BumpSetting = 0x00759B10;
		constexpr std::uintptr_t BumpSettingTexturesRead = 0x0059856B;
		constexpr std::uintptr_t BumpSettingEngineRead = 0x0059D951;
		// Model detail. The models are progressive meshes. The game's "PMesh Reduction Level" setting (video settings + 0xA4,
		// 0 when not saved) is read once where the engine starts, "mov eax, [0x759B1C]" (5 bytes) at 0x59864C, and handed to
		// 0x5747C0, which keeps it at 0x760AB0; in play mode 5 the engine is given 16 instead (0x598648, "push 0x10").
		// The two mesh draw set-ups (0x580FC5 and 0x584A05) work out each mesh's detail from its depth and size,
		// v = (460 / focal * depth - t) / (10 - t) - level / 16 with t = 1 + 6 * min(radius, 0.5), kept to 0..1 and times
		// 12 (mesh + 0x70 / + 0x7C). 0 is the whole mesh: a mesh with flag 0x4000 skips the sum and gets 0. A level of 16
		// or more makes every v 0, every mesh whole at any distance [verified in the assembly].
		constexpr std::uintptr_t MeshReductionSetting = 0x00759B1C;
		constexpr std::uintptr_t MeshReductionEngineRead = 0x0059864C;
		constexpr std::uintptr_t EngineMeshReduction = 0x00760AB0;
		// The engine's draw of the world (0x58F640): its queue is grouped into batches of the same textures and states; for
		// each, 0x589D90 starts it (cdecl (batch)), each queued primitive adds its triangles through its vtable + 4
		// (thiscall (primitive, index, entry) ret 8), and 0x58A150 draws it (cdecl ()); the draw calls happen there, or
		// inside an add when the buffer is full (0x58A000) [verified in the assembly]. The models' adds: creatures'
		// meshes (CEngineAnimMesh, vtable 0x66FD64) 0x5848B0 and objects' (CEngineDynamicMesh, vtable 0x66FD1C) 0x582CE0
		// [vtables verified; class names reference: Flame's mapping]. Terrain is drawn by other primitives.
		constexpr std::uintptr_t EngineBatchBegin = 0x00589D90;
		constexpr std::uintptr_t EngineBatchDraw = 0x0058A150;
		constexpr std::uintptr_t AnimMeshAddTriangles = 0x005848B0;
		constexpr std::uintptr_t DynamicMeshAddTriangles = 0x00582CE0;
		// The engine's flags (0x10 bump du/dv in use, 0x20 with luminance). Liquid surfaces: thiscall 0x5873A0 makes one
		// (arguments ..., 4 texture, 5 bump map, 6 environment map, 7 flags (0x8000 bump), 8 opacity, ...). Water tiles
		// (engine method 0x5A2570) use the engine's BumpWater textures (engine + 0x72, + 0x76, + 0x7A) when bump is in use
		// and video setting + 0x68 (0x759AE0) is 0, adding 1 to the flags when + 0x60 (0x759AD8) is set. A room's own
		// liquid (method 0x5A2F10, the Temple's "Water") is always plain; its surface is made by the call at 0x5A31E0
		// (engine object in EBP). [unverified] + 0x60 and + 0x68 are the "Translucent Water" and "Sine Wave Water" options.
		constexpr std::uintptr_t EngineBumpFlags = 0x00764BB8;
		constexpr std::uintptr_t LiquidSurfaceCreate = 0x005873A0;
		constexpr std::uintptr_t RoomLiquidSurfaceCreate = 0x005A31E0;
		constexpr std::uintptr_t WaterSettingTranslucent = 0x00759AD8;
		constexpr std::uintptr_t WaterSettingSimple = 0x00759AE0;
		// The jumps that skip bump when engine flag 0x10 is off ("test al, 0x10 / jz", short jz): water tiles (0x5A2570)
		// at 0x5A2B0D (74 27, past BumpWater, its maps' flag and full opacity), lava tiles (0x5A2BE0) at 0x5A2E54 (74 11,
		// past flag 0x8000). Made unconditional (EB) to keep one of them flat.
		constexpr std::uintptr_t WaterBumpSkip = 0x005A2B0D;
		constexpr std::uintptr_t LavaBumpSkip = 0x005A2E54;
		// Chat: a received message (0x409E30, cdecl (message, interface)) shifts the 3-line history and copies the message
		// text (message + 4) into the last line (interface + 0x1E37) with a fixed 0x102 bytes at 0x409E8F ("lea esi, [ebp+4];
		// lea edi, [ebx+0x1E37]; mov ecx, 0x40; rep movsd; movsw", 18 bytes); the resume address follows.
		constexpr std::uintptr_t ChatReceivedCopy = 0x00409E8F;
		constexpr std::uintptr_t ChatReceivedResume = 0x00409EA1;
		// Adds a line to the in-game chat history (3 lines of 0x106 bytes from interface + 0x1C27: { expiry time, sender
		// (16 bit), text }; shown 30 s), thiscall (interface, sender, text) ret 8, copying 0x102 bytes of text. Senders
		// 0x8181, 0x8282 and 0x8383 are drawn in their own colors by the chat drawing code; others in the normal one.
		constexpr std::uintptr_t ChatAddLocalLine = 0x00409EC0;
		// The in-game chat box's drawing, fastcall (interface): it draws its 3 lines into a picture of its own only when
		// + 0x1F39 is set. At 0x40A00C each line's color ({ blue, green, red, alpha, palette index }, chosen by sender)
		// is at [esp + 0x18] with the line in EDI ("lea eax, [esp+0x20]; lea ecx, [esp+0x28]", 8 bytes).
		constexpr std::uintptr_t ChatDraw = 0x00409F30;
		constexpr std::uintptr_t ChatLineColorChosen = 0x0040A00C;
		// The in-game player list (0x40A0F0, multiplayer, while shown): each row "number: name" (player + 0xEEA) is drawn
		// with a color pushed at 0x40A1FB ("add eax, 0x14; push eax; mov eax, [esp+0x7C]", 8 bytes), the row's player in ESI.
		constexpr std::uintptr_t PlayerListColorPush = 0x0040A1FB;
		// The multiplayer lobby's player list (0x5445F0): each slot's row color { blue, green, red, alpha, palette index }
		// is set by the call at 0x544C1D to 0x62C3C0 (thiscall on 0x7413C0: out, color), the slot's number in the
		// caller's [esp + 0x10]. Slots: 0x3F bytes each at 0x73F5C8 (human player's wide name at + 0, kind in + 0x3A).
		constexpr std::uintptr_t LobbySlotColorSet = 0x00544C1D;
		// The lobby's chat (button 0x1CD, drawn by 0x544FA0): each line is the sender's slot as a digit, then the text; the
		// line's color (sender's player color + 0x20 each) is set by this call to TextColorSet, the text in ESI.
		constexpr std::uintptr_t LobbyChatColorSet = 0x005451D1;
		constexpr std::uintptr_t TextColorSet = 0x0062C3C0;
		constexpr std::uintptr_t LobbySlots = 0x0073F5C8;
		// The lobby chat handler, cdecl (sender, message, front end): adds "<sender slot><slot name>: <text>" to the chat
		// (front end + 0x30CAB, lines ended by 0xFFFF; rows per line at + 0x2E5F). Message: a digit n, n digits of the
		// sender's screen (must equal FrontEndScreen), then the text. Sender below the player count (front end + 0x5AAC).
		constexpr std::uintptr_t LobbyChatReceive = 0x00542BF0;
		// The network's chat callback (network 0x758160 + 0x60, called at 0x559F01 with (sender, message, + 0x64)), and its
		// context at + 4. The multiplayer lobby sets them to LobbyChatReceive and its front end at 0x542647.
		constexpr std::uintptr_t NetworkChatCallback = 0x007581C0;
		// A 16-bit number every lobby chat message carries; the receiver drops messages whose number is not its own.
		// [unverified] That it is the current screen or state (9 and 0x11 are among the values the game tests).
		constexpr std::uintptr_t FrontEndScreen = 0x00759C35;
		// The lobby chat's drawing, cdecl (button, front end), returns with ret.
		constexpr std::uintptr_t LobbyChatDraw = 0x00544FA0;
		// Network wait poll, thiscall: returns [this + 0xC] & 2; spun on without yielding.
		constexpr std::uintptr_t NetworkWaitPoll = 0x0061C090;
		// The game's text input window procedure (0x430F40, set with SetWindowLongA at 0x430D0E, the previous one kept
		// at + 0x4C) calls the previous procedure directly in two places; the resume address follows each.
		constexpr std::uintptr_t TextInputOldProcCall1 = 0x0043032C;
		constexpr std::uintptr_t TextInputOldProcResume1 = 0x0043033F;
		constexpr std::uintptr_t TextInputOldProcCall2 = 0x004305DD;
		constexpr std::uintptr_t TextInputOldProcResume2 = 0x004305E6;
		// C runtime start-up: "push 0x80" is the size of the exit function list (32 entries).
		constexpr std::uintptr_t ExitListSizePush = 0x00634860;
		// The game's resources (0x55C020 makes them): one file manager per kind of data, each a list of WAD files and
		// folders searched first to last; a folder or WAD added goes to the front. 0x55C700 (fastcall (resources), called
		// at the end of the command line's reading) adds the loose Data\Meshes, EngineTextures, sprite, frontend and paths
		// folders after the WADs. 0x55BF40 (cdecl (int* status, manager, format, ...)) adds a folder; the game folder (with
		// its last backslash) is at resources + 0x108.
		constexpr std::uintptr_t ResourcesAddLooseFolders = 0x0055C700;
		// A player's set-up for a level, thiscall (player, value) ret 4: fills its portal table (+ 0xB57) with the world's
		// creature types 0-45 in list order. The portal's per-tick pass over that table (0x4DBD90) takes the type for slot
		// i from the world's list (place i) at 0x4DBDC7: "mov ecx, [world]; push eax; mov edx, [ecx]; call [edx + 0xB8]"
		// (15 bytes), result in AL.
		constexpr std::uintptr_t PlayerLevelSetup = 0x004B8640;
		constexpr std::uintptr_t PortalPassTypeOfSlot = 0x004DBDC7;
		constexpr std::uintptr_t ResourcesAddFolder = 0x0055BF40;
		// Opens a file by its path (not through the lists above): cdecl (int* status, file** out, path, access) returning
		// status; access as CreateFile's (0x80000000 read, 0x40000000 write). Level files and the shared lists (editor
		// data such as Creatures.kwd) are read this way (0x56C960).
		constexpr std::uintptr_t FileOpenByPath = 0x005B3980;
		// A creature's new path, thiscall (creature); its state set, thiscall (creature, state) ret 4
		constexpr std::uintptr_t CreatureNewPath = 0x0048ECE0;
		constexpr std::uintptr_t CreatureSetState = 0x0048AD30;
		// Worker states, thiscall (worker job: + 8 the creature, + 0x8A its task) returning whether it goes on: digging a
		// tile (state 0xE, task kind 1) and mining gold (state 0x1A, kind 2). Both return at once while the creature's
		// action (a swing) runs, and only then check that the task still exists.
		constexpr std::uintptr_t WorkerDigState = 0x004655D0;
		constexpr std::uintptr_t WorkerMineGoldState = 0x00465870;
		// Whether a creature's action runs (+ 0x3A9 set and not 0x15), and stopping it, both thiscall (creature).
		constexpr std::uintptr_t CreatureActionRunning = 0x00496D00;
		constexpr std::uintptr_t CreatureStopAction = 0x00496D20;
		// Whether a task of a player's task list (Offset::PlayerTaskList) is still there with that kind, thiscall
		// (list, index, kind) ret 8: its "in use" byte (+ 0x23), own index (+ 0x21) and kind (+ 4) of the 0x24-byte record.
		constexpr std::uintptr_t TaskStillThere = 0x00508070;
		// A task's slot: who holds it, thiscall (list, task, face, slot) ret 0xC, returns the creature tag (16 bit); and
		// setting it, thiscall (list, task, face, slot, tag) ret 0x10. The worker job search (0x469AF0) reads the slots
		// of dig tasks at 0x469D34 and of gold tasks at 0x46A1B6.
		constexpr std::uintptr_t TaskSlotHolder = 0x00508110;
		constexpr std::uintptr_t TaskSetSlotHolder = 0x00508160;
		constexpr std::uintptr_t DigSearchSlotRead = 0x00469D34;
		constexpr std::uintptr_t GoldSearchSlotRead = 0x0046A1B6;
		// A player's keeper spell state, thiscall (player, spell) ret 4, and setting it, thiscall (player, spell,
		// state) ret 8. State 3 = available to cast.
		constexpr std::uintptr_t PlayerSpellState = 0x004BAFA0;
		constexpr std::uintptr_t SetPlayerSpellState = 0x004BAF40;
		// Casts a keeper spell, thiscall (player, spell, position, flag) ret 0xC. Create Imp spawns the creature type
		// in the spell's data (+0x43).
		constexpr std::uintptr_t CastKeeperSpell = 0x004BBD30;
		// In the cast, after the price is paid: "test al, al; jz <no creature>" (8 bytes) with EDI = the creature type
		// read from the spell data (+0x43, or +0x4C when upgraded). Goes on at CastCreatureMake, or CastCreatureNone.
		// The price (base cost times the player's count of that type above a free number) is worked out before, from
		// +0x43 too, and so is the spell panel's price.
		constexpr std::uintptr_t CastCreatureCheck = 0x004BBE4B;
		constexpr std::uintptr_t CastCreatureMake = 0x004BBE53;
		constexpr std::uintptr_t CastCreatureNone = 0x004BBF66;
		// Create Imp's price at each of the seven places the game works it out (each only for spell 4: the affordability
		// check 0x4BB8A0, the cast, the spell panel and the rest): count of the spell's type (+0x43) less the free number
		// (byte 0x6F28D7), plus one, at least 1, times the base cost (+0x58). Four do it as "sub eax, ecx; inc eax" (2B C1 40),
		// three as "sub edx, ecx; add eax, edx" (2B D1 03 C2, EDX having been 1), each right after "mov cl, [0x6F28D7]".
		constexpr std::uintptr_t CreateImpPriceIncSites[] = { 0x004BB930, 0x004BBAD0, 0x004BBC50, 0x004BE608 };
		constexpr std::uintptr_t CreateImpPriceAddSites[] = { 0x004BBDAF, 0x004BC01A, 0x004BC318 };
		// "Receive Imps" special, thiscall (special object, player) ret 4: a ring of the first creature type with the
		// worker flag (data +0x686 bit 1), 2 tiles around the player's heart position (+0xB43)
		constexpr std::uintptr_t ReceiveWorkersSpecial = 0x004A4980;
		// Play mode (GameCfg 0x75AD47 + 4): 1 campaign, 2 skirmish, 3 multiplayer (from what the game does with it)
		constexpr std::uintptr_t PlayMode = 0x0075AD4B;
		// Portal creature choice, thiscall (portal room, flag) ret 4, run for each Portal and Mercenary Portal. A
		// creature type comes through the keeper Portal (room type 3) when its data flags + 0x686 have 0x400, and through
		// the Mercenary Portal (room type 0x19) when + 0x68A has 8: the "test byte [eax + 0x68A], 8; jz" at
		// MercenaryPortalCreatureTest (13 bytes, EAX = creature data). Accepted goes on at PortalCreatureAccepted (the
		// room type 0x19 check), rejected to PortalCreatureRejected (next type).
		constexpr std::uintptr_t PortalAttraction = 0x004DBF90;
		// Is a creature type in a player's creature pool (player + 0x1109, as the map sets it): thiscall (pool, type) ret 4
		constexpr std::uintptr_t CreaturePoolHas = 0x00506400;
		// Makes a creature: thiscall (creature list, type, owner tag, position, out) ret 0x10. Used for map things,
		// spells, portals and so on; saved games are loaded through the inner 0x4B7900 instead.
		constexpr std::uintptr_t CreateCreature = 0x004B78D0;
		// Lobby creature list (skirmish/multiplayer setup). The creature data loader calls this for each creature record,
		// cdecl (record, front end): Portal creatures (record flags + 0x12EC with 0x400, without 1 or 0x8004100) get their
		// type and name added to the front end's lists.
		constexpr std::uintptr_t LobbyCreatureRecord = 0x00548B70;
		// Makes the game settings' creature list and the front end's copy (count entries of 5 bytes, all zero), thiscall
		// (game settings, front end list out, count) ret 8
		constexpr std::uintptr_t MakeCreatureList = 0x0055DA50;
		// Sets the lobby's lists from the level files when a map is chosen, thiscall (front end; map name, flag) ret 8:
		// flag 0 sets every creature row to 0 (0x5488A0) and reads GlobalVariables.kwd, otherwise the map's own file. Their
		// creature pool entries set the rows of their types (callback 0x53F600, setter 0x548E70: row count at + 1).
		constexpr std::uintptr_t LevelPoolLoad = 0x0053F150;
		// How many rows the game settings' creature list has (game settings + 0x188); level setup reads this many.
		constexpr std::uintptr_t GameCreatureListCount = 0x0075AECF;
		// This machine's memory in MB for the lobby's Ram column, cdecl (): GlobalMemoryStatus's total + 1. Only shown.
		constexpr std::uintptr_t LobbyPlayerMemory = 0x00543820;
		// The multiplayer name this player typed (wide text from GameAlloc, freed with GameFree), its use when hosting and
		// joining, and where it is saved to the registry.
		constexpr std::uintptr_t LobbyPlayerName = 0x0073FE68;
		// thiscall (network, session name, player name, slot out, 4) ret 0x10
		constexpr std::uintptr_t NetworkCreateSession = 0x0055A5B0;
		// thiscall (network, session, player name, slot out) ret 0xC
		constexpr std::uintptr_t NetworkJoinSession = 0x0055A650;
		// thiscall (settings, name) ret 4
		constexpr std::uintptr_t SavePlayerName = 0x00561F60;
		// cdecl (size) and cdecl (pointer)
		constexpr std::uintptr_t GameAlloc = 0x005B75F0;
		constexpr std::uintptr_t GameFree = 0x006341B0;
		// Receiving the host's lobby message (199): "cmp eax, edx; jnz" (8 bytes) compares the message's size (EAX) with
		// the size this machine's lists expect (EDX). EBX = the creature list's bytes, EBP = the front end. It goes on at
		// LobbyHostListAccepted, or ignores the message at LobbyHostListRejected.
		constexpr std::uintptr_t LobbyHostListSizeCheck = 0x005432BD;
		constexpr std::uintptr_t LobbyHostListAccepted = 0x005432C5;
		constexpr std::uintptr_t LobbyHostListRejected = 0x005435B2;
		constexpr std::uintptr_t MercenaryPortalCreatureTest = 0x004DC064;
		constexpr std::uintptr_t PortalCreatureAccepted = 0x004DC071;
		constexpr std::uintptr_t PortalCreatureRejected = 0x004DC474;
		// In the portal's creature maker (0x4DC9A0), Mercenary Portal arrival (state 0x116): "push 1; xor al, 1; mov ecx,
		// esi" (6 bytes, ESI = the new creature), then at PortalMercenaryResume the + 0xF3 store and the call to 0x48E0D0,
		// thiscall (creature, on), which sets IsMercenary (creature + 0x3EA bit 9).
		constexpr std::uintptr_t PortalMercenaryFlag = 0x004DCC4F;
		constexpr std::uintptr_t PortalMercenaryResume = 0x004DCC55;
		// thiscall (creature, on) ret 4: sets IsMercenary (+ 0x3EA bit 9) and the texture bank (+ 0xF0, 1 = mercenary).
		// Called by the portal, 0x51A567 and 0x520844.
		constexpr std::uintptr_t SetCreatureMercenary = 0x0048E0D0;
		// The Hand of Evil's copy of a picked-up creature's render info (hand + 0xFB8) gets texture bank 1 (hand + 0x1064)
		// when the creature's data flags have 0x8000000 or the creature is a mercenary: "mov ecx, [eax + 0x3EA]; test ch,
		// 2; jnz" (11 bytes, EAX = the creature), then bank 0 at HandTexturesNormal, bank 1 at HandTexturesMercenary.
		constexpr std::uintptr_t HandMercenaryTest = 0x0040D7E1;
		constexpr std::uintptr_t HandTexturesNormal = 0x0040D7EC;
		constexpr std::uintptr_t HandTexturesMercenary = 0x0040D7F5;
		// World setup at each level start, thiscall (world): applies the lobby options, AI players and so on
		constexpr std::uintptr_t WorldSetup = 0x0050C800;
		// Lobby game options packed into front end + 0x6026..0x6029: written from GameCfg (0x549D10) and read back
		// into it (0x549F60, which starts by reading 0x75AE97). Both thiscall (front end).
		constexpr std::uintptr_t LobbyWriteOptions = 0x00549D10;
		constexpr std::uintptr_t LobbyReadOptions = 0x00549F60;
		constexpr std::uintptr_t LobbyOptionsMode = 0x0075AE97;
		// Sets up the in-game player interface, thiscall (interface). Picks the creature type of the Workers row.
		constexpr std::uintptr_t PlayerInterfaceSetup = 0x00419180;
		// The local player interface itself, a fixed object (used as ECX at 0x410710; its menu view, + 0x2737, was logged at
		// 0x6D1147 in game).
		constexpr std::uintptr_t PlayerInterface = 0x006CEA10;
		// Looks up a text by id, thiscall (text table, id) ret 4. Returns the game's multibyte string, or 0.
		constexpr std::uintptr_t TextGetString = 0x0062D960;
		// Game multibyte text to wide text, cdecl (text, out, size). The reverse is ConvertText. Both return 1 on success.
		constexpr std::uintptr_t TextToWide = 0x00553B90;
		// The Good player's tag
		constexpr std::uintptr_t GoodPlayerTag = 0x006F254C;
		// Creature "what next" state (0x4773D0): "cmp cx, [Good player tag]; jnz" sends every creature of the Good
		// player to its hero behavior (state 0x5A at 0x4775A7) before the worker check (0x4775EC) is reached. Not
		// Good: the neutral check at 0x4775BE.
		constexpr std::uintptr_t CreatureNextGoodCheck = 0x0047759E;
		constexpr std::uintptr_t CreatureNextGood = 0x004775A7;
		constexpr std::uintptr_t CreatureNextNotGood = 0x004775BE;
		constexpr std::uintptr_t CreatureNextWorker = 0x004775EC;
		// World setup: "cmp al, 3" before giving lobby AI to (or removing) keepers; keeper 1 (player 3) was skipped
		constexpr std::uintptr_t WorldSetupKeeperCheck = 0x0050CB97;
	}
	// Offsets inside the GOG patch.dll (from its module base)
	namespace GogPatch {
		// "mov eax, [Vwait setting]" in its FakeSurface::Flip, and the setting itself (0 = no vertical blank wait)
		constexpr std::uintptr_t FlipReadsVwait = 0x2EEA;
		constexpr std::uintptr_t VwaitSetting = 0x24DB4;
		// "cmp [ScaleMode setting], 0" in its present rectangle setup, and the setting (0 = stretch, 1 = keep shape)
		constexpr std::uintptr_t PresentReadsScaleMode = 0x1826;
		constexpr std::uintptr_t ScaleModeSetting = 0x24DA8;
	}
	namespace Offset {
		constexpr std::uintptr_t GuiViewTiles = 0x4;
		constexpr std::uintptr_t GuiViewTilesWide = 0x27F;
		constexpr std::uintptr_t GuiViewTilesHigh = 0x283;
		constexpr std::uintptr_t GameSelectedDevice = 0xE69;
		// The main menu's GUI manager inside the menu object
		constexpr std::uintptr_t MenuGuiManager = 0x30F44;
		constexpr std::uintptr_t GuiManagerWidth = 0x4;
		// Creature data: portal flags (8 = comes through the Mercenary Portal), and its attraction list: up to 2 entries
		// of { value, amount, kind } at + 0x5DA, how many at + 0x788 (kind 1: room type value with total size >= amount;
		// 2: gold >= value; 3: mana >= value). The portal's weight for a type is the total size of its first entry's
		// room type (a Lair counts as a fixed weight).
		constexpr std::uintptr_t CreatureDataPortalFlags = 0x68A;
		constexpr std::uintptr_t CreatureDataAttraction = 0x5DA;
		constexpr std::uintptr_t CreatureDataAttractionCount = 0x788;
		// Player: first creature it owns (tag); creature: next creature of the same owner, and its type
		constexpr std::uintptr_t PlayerFirstCreature = 0x6;
		// Player: how many creatures of each type it has, count then pointer to 0xD-byte entries { type, ..., count + 9 }
		constexpr std::uintptr_t PlayerCreatureTypeCount = 0x4E;
		constexpr std::uintptr_t PlayerCreatureTypes = 0x52;
		constexpr std::uintptr_t CreatureNextOwned = 0x10;
		// Creature: its current state, and the state it goes into when it reaches where it walks to (state 7 walks to a
		// room; 0x50 there = make its home, set by 0x46F390). Both are in its state block at + 0x24E.
		constexpr std::uintptr_t CreatureState = 0x25E;
		constexpr std::uintptr_t CreatureArrivalState = 0x32D;
		// Creature: its owner (player pointer); player: its worker task list (digging, gold, claiming...); worker job:
		// its creature and its task's index in that list.
		constexpr std::uintptr_t CreaturePlayer = 0x2A;
		constexpr std::uintptr_t PlayerTaskList = 0x1105;
		constexpr std::uintptr_t WorkerJobCreature = 0x8;
		constexpr std::uintptr_t WorkerJobTask = 0x8A;
		// Worker job (the creature's state block, creature + 0x24E): the face (0-3) and slot (0-2) it holds in a dig or
		// gold task (each task has 3 slots on each of its 4 faces).
		// A thing's kind (32 bit): 0 creature, 2 object, 3 trap, 4 door, 6 dead body.
		constexpr std::uintptr_t ThingKind = 0x12;
		// The input system's key state (0x5BB280 reads it): a pointer at input system + 0x70, then one byte per DirectInput
		// key code (256) from + 0x1C.
		constexpr std::uintptr_t InputKeyState = 0x70;
		constexpr std::uintptr_t KeyStateKeys = 0x1C;
		constexpr std::uintptr_t CreatureStateBlock = 0x24E;
		constexpr std::uintptr_t WorkerJobFace = 0x94;
		constexpr std::uintptr_t WorkerJobSlot = 0x96;
		constexpr std::uintptr_t CreatureType = 0x36F;
		// Creature: the texture bank its model is drawn with (render info + 0xAC, passed to the mesh lookup 0x4B44E0 by the
		// drawing code at 0x59B459); 1 = the mercenary textures, 0 = the normal ones.
		constexpr std::uintptr_t CreatureTextureBank = 0xF0;
		// Creature: state flags (the game's debug dump names the bits: 0x200 IsMercenary, 0x800 DieInstantly, ...)
		constexpr std::uintptr_t CreatureStateFlags = 0x3EA;
		// Room: its type and its owner's tag; player's rooms (first tag), next room of the same owner, room size in tiles
		constexpr std::uintptr_t RoomType = 0x18;
		constexpr std::uintptr_t RoomOwner = 0x1D;
		constexpr std::uintptr_t PlayerFirstRoom = 0x36;
		constexpr std::uintptr_t RoomNextOwned = 0x8;
		constexpr std::uintptr_t RoomSize = 0xA7;
		// Player's creature pool (how many more of each creature type it may get, from the map or the lobby): count, and
		// a pointer to 5-byte entries { type, int number }. Each portal arrival takes one off.
		constexpr std::uintptr_t PlayerCreaturePool = 0x1109;
		constexpr std::uintptr_t CreaturePoolCount = 0xC;
		constexpr std::uintptr_t CreaturePoolEntries = 0x10;
		// Front end: the lobby's creature list (5-byte entries { type, count, ... }), types (bytes), name text ids
		// (shorts) and how many
		constexpr std::uintptr_t LobbyCreatureList = 0x30CB3;
		constexpr std::uintptr_t LobbyCreatureNames = 0x30CC7;
		constexpr std::uintptr_t LobbyCreatureTypes = 0x30CCB;
		constexpr std::uintptr_t LobbyCreatureCount = 0x30CEF;
		// Creature record (as read from the data file): type and name text id
		constexpr std::uintptr_t CreatureRecordType = 0x133B;
		constexpr std::uintptr_t CreatureRecordName = 0x1314;
		// Flying notification icon: its size and the size it blends to (floats)
		constexpr std::uintptr_t FlyingIconSize = 0x18;
		constexpr std::uintptr_t FlyingIconTargetSize = 0x1C;
		constexpr std::uintptr_t GuiManagerHeight = 0x8;
		constexpr std::uintptr_t GuiManagerMouseX = 0xAC;
		constexpr std::uintptr_t GuiManagerMouseY = 0xB0;
		// GUI window: id (from its table entry), owning manager, parent window (0 for top-level)
		constexpr std::uintptr_t WindowId = 0x44;
		constexpr std::uintptr_t WindowManager = 0x4E;
		constexpr std::uintptr_t WindowParent = 0x56;
		// World: non-zero while a loading screen image is loaded and shown
		constexpr std::uintptr_t WorldLoadingScreenShown = 0xA3BF;
		// Engine surface wrapper: its IDirectDrawSurface
		constexpr std::uintptr_t SurfaceDirectDraw = 0x24;
		constexpr std::uintptr_t DeviceModeCount = 0x1FE;
		constexpr std::uintptr_t DeviceModes = 0x206;
		// Front end: description of the frame the menu draws into (copied from the back buffer each frame), and in it
		// the pixel format's bits per pixel, the pixels, width and height
		constexpr std::uintptr_t FrontEndFrame = 0x2DD65;
		constexpr std::uintptr_t FrameBits = 0x10;
		constexpr std::uintptr_t FramePixels = 0x18;
		constexpr std::uintptr_t FrameWidth = 0x1C;
		// World: its player list (passed to WorldCreatePlayer), flags (0x80 = local player already chosen), and the
		// tag of the player you control
		constexpr std::uintptr_t WorldPlayers = 0x7A;
		constexpr std::uintptr_t WorldLocalFlags = 0x7239;
		constexpr std::uintptr_t WorldLocalPlayerTag = 0x7243;
		// The game session (at world + 4) and its game turns per second.
		constexpr std::uintptr_t WorldSession = 0x4;
		constexpr std::uintptr_t SessionTurnsPerSecond = 0x8;
		constexpr std::uintptr_t SessionInterface = 0xC;
		// Session: a menu is open over the game. The world tick stops after the actions while it is set, and the game turn
		// does not count on.
		constexpr std::uintptr_t SessionMenuOpen = 0x284;
		// Session: the game turn. Communication: its session.
		constexpr std::uintptr_t SessionGameTurn = 0x4;
		constexpr std::uintptr_t CommunicationSession = 0x4;
		// World: its map object. Tiles of 16 bytes at map + 0x14, map width at map + 0xC.
		constexpr std::uintptr_t WorldMap = 0x9DA;
		constexpr std::uintptr_t MapWidth = 0xC;
		constexpr std::uintptr_t MapHeight = 0x10;
		// World: its rooms, and its table of players (player n is at table + (n - 1) * PlayerTableEntrySize).
		constexpr std::uintptr_t WorldRooms = 0x9BE;
		constexpr std::uintptr_t WorldPlayerTable = 0x84;
		// World: the terrain ids a water or lava bridge room is built over.
		constexpr std::uintptr_t WorldWaterTerrain = 0x9BAE;
		constexpr std::uintptr_t WorldLavaTerrain = 0x9BAF;
		// World vtable: a terrain's data, a room type's data, a tile by x, y.
		constexpr std::uintptr_t WorldTerrainData = 0x70;
		constexpr std::uintptr_t WorldRoomData = 0x98;
		constexpr std::uintptr_t WorldTileAt = 0x1D8;
		// A tile: its room (low 12 bits), its owner's tag (low 12 bits), its terrain.
		constexpr std::uintptr_t TileRoom = 0x2;
		constexpr std::uintptr_t TileOwner = 0x8;
		constexpr std::uintptr_t TileTerrain = 0xA;
		// A room's tiles are a chain: the room holds its first and last as two bytes, x then y (0, 0 for none), and each tile the
		// next the same way. Their order is the order they were added, and the room's object placing walks it in that order
		// (0x4DE220 for a Hatchery, 0x4EF1F0) [verified in code].
		constexpr std::uintptr_t TileNextInRoom = 0x4;
		constexpr std::uintptr_t RoomFirstTile = 0x2D;
		constexpr std::uintptr_t RoomLastTile = 0x2F;
		// A terrain's flags: bit 0x10, this terrain is part of a room. A room type's flags: bit 1, built over water; bit 2, over
		// lava.
		constexpr std::uintptr_t TerrainFlags = 0x17B;
		constexpr std::uintptr_t RoomDataFlags = 0x43D;
		// The player interface: what is under the hand. Tile x and y, kept inside the map.
		constexpr std::uintptr_t InterfaceUnderHand = 0x1068;
		constexpr std::uintptr_t UnderHandTileX = 0x8;
		constexpr std::uintptr_t UnderHandTileY = 0xC;
		// The player interface: its GUI manager, the view the options menu draws into (made 2560x1280 in layout units, drawn
		// at layout y 180, only while the menu or the objective is open), and the objective being open.
		constexpr std::uintptr_t InterfaceGuiManager = 0x10C6;
		// A window: drawn and clickable. The interface: the side panel is out, a byte.
		constexpr std::uintptr_t WindowShown = 0x48;
		constexpr std::uintptr_t InterfacePanelOut = 0x109;
		constexpr std::uintptr_t InterfaceMenuView = 0x2737;
		constexpr std::uintptr_t InterfaceObjectiveOpen = 0x2726;
		// A view: bits per pixel, pixels, pitch, width and height in pixels.
		constexpr std::uintptr_t ViewBitsPerPixel = 0x25F;
		constexpr std::uintptr_t ViewPixels = 0x267;
		constexpr std::uintptr_t ViewPitch = 0x273;
		constexpr std::uintptr_t ViewPixelWidth = 0x287;
		constexpr std::uintptr_t ViewPixelHeight = 0x28B;
		constexpr std::uintptr_t SessionBridge = 0x10;
		// The bridge's "get the camera" (vtable), a camera's mode to go to after a path, and the one saved when it started.
		constexpr std::uintptr_t BridgeGetCamera = 0xD0;
		constexpr std::uintptr_t CameraDeferredMode = 0xDB0;
		constexpr std::uintptr_t CameraSavedMode = 0xED2;
		// A player's hand: things (16 bit tags), their count (byte). The creature it possesses (16 bits). Its timed action.
		constexpr std::uintptr_t PlayerHand = 0xB2;
		constexpr std::uintptr_t PlayerHandCount = 0x1BF;
		constexpr std::uintptr_t PlayerPossessed = 0x134;
		constexpr std::uintptr_t PlayerStatus = 0x19A;
		constexpr std::uintptr_t PlayerActionType = 0x186;
		constexpr std::uintptr_t PlayerActionData1 = 0x18E;
		constexpr std::uintptr_t PlayerActionData2 = 0x192;
		constexpr std::uintptr_t PlayerActionData3 = 0x196;
		// A turn's actions: their count (byte), then the actions of 0x12 bytes each. An action's player tag.
		constexpr std::uintptr_t ActionListCount = 0x0;
		constexpr std::uintptr_t ActionListActions = 0x11;
		constexpr std::uintptr_t ActionPlayer = 0x10;
		// How long a creature's state has run.
		constexpr std::uintptr_t StateAge = 0x1C;
		// Camera: its mode (18 = a scripted path), when its own move ends (0: none), what the path's end goes back to, the
		// path's number of points, the point reached and the blend towards the next.
		constexpr std::uintptr_t CameraMode = 0xDA8;
		constexpr std::uintptr_t CameraTweenEnd = 0xDD8;
		// Camera: the map point it looks at (x, y, z, 32 bits each), its zoom (32 bits), its angles (16 bits each, 0x800 a
		// full turn: turn, tilt, and a lean that eases back to 0), and two more view values (32 bits each).
		constexpr std::uintptr_t CameraPosition = 0xD68;
		constexpr std::uintptr_t CameraZoom = 0xD80;
		// The zoom's limits, as CameraZoomBy keeps it.
		constexpr std::uintptr_t CameraZoomMin = 0xD50;
		constexpr std::uintptr_t CameraZoomMax = 0xD54;
		constexpr std::uintptr_t CameraTurn = 0xD84;
		constexpr std::uintptr_t CameraTilt = 0xD86;
		constexpr std::uintptr_t CameraLean = 0xD88;
		constexpr std::uintptr_t CameraViewA = 0xD8A;
		constexpr std::uintptr_t CameraViewB = 0xD8E;
		// Camera: where its own smooth move (a view change; CameraFinishTween with a time starts one) goes, the same
		// values in the same order, then the mode it ends in.
		constexpr std::uintptr_t CameraTweenPosition = 0xDB6;
		constexpr std::uintptr_t CameraTweenZoom = 0xDC2;
		constexpr std::uintptr_t CameraTweenTurn = 0xDC6;
		constexpr std::uintptr_t CameraTweenTilt = 0xDC8;
		constexpr std::uintptr_t CameraTweenLean = 0xDCA;
		constexpr std::uintptr_t CameraTweenViewA = 0xDCC;
		constexpr std::uintptr_t CameraTweenViewB = 0xDD0;
		constexpr std::uintptr_t CameraTweenMode = 0xDD4;
		// When a scripted move and turn end, 0 when none runs. Escape finishes these at once.
		constexpr std::uintptr_t CameraMoveEnd = 0xDFA;
		constexpr std::uintptr_t CameraTurnEnd = 0xE20;
		constexpr std::uintptr_t CameraPathTarget = 0xDEC;
		constexpr std::uintptr_t CameraPathPoints = 0xEBE;
		constexpr std::uintptr_t CameraPathPoint = 0xEC6;
		constexpr std::uintptr_t CameraPathBlend = 0xECA;
		// A game action: data1 + 0, data2 + 4, data3 + 8, kind + 0xC, player's tag + 0x10.
		constexpr std::uintptr_t ActionData3 = 0x8;
		constexpr std::uintptr_t ActionKind = 0xC;
		// A level trigger's type (byte); the player has an active computer player (byte).
		constexpr std::uintptr_t TriggerType = 0xE;
		constexpr std::uintptr_t PlayerAiActive = 0x10FA;
		// Player: its current timed action (0 = none) and its player number (1 Good, 2 neutral, 3+ keepers)
		constexpr std::uintptr_t PlayerAction = 0x186;
		constexpr std::uintptr_t PlayerNumber = 0x10FC;
		constexpr std::uintptr_t PlayerTag = 0x4;
		// Next player in the world's list (from world + 0x7A), and mana
		constexpr std::uintptr_t PlayerNext = 0x3A;
		constexpr std::uintptr_t PlayerMana = 0x82;
		// Availability lists: count, then a pointer to 9-byte entries { id, state (int) }. Rooms, then doors and traps
		// (in the order the map script's "make available" uses: 3 = +0x5E list, 4 = +0x66 list).
		constexpr std::uintptr_t PlayerRoomCount = 0x56;
		constexpr std::uintptr_t PlayerRooms = 0x5A;
		constexpr std::uintptr_t PlayerDoorCount = 0x5E;
		constexpr std::uintptr_t PlayerDoors = 0x62;
		constexpr std::uintptr_t PlayerTrapCount = 0x66;
		constexpr std::uintptr_t PlayerTraps = 0x6A;
		// Keeper spells: count, then a pointer to 0x11-byte entries { id, state (int), times, upgraded }
		constexpr std::uintptr_t PlayerSpellCount = 0x6E;
		constexpr std::uintptr_t PlayerSpells = 0x72;
		// World vtable: keeper spell data by spell id, thiscall (world, spell)
		constexpr std::uintptr_t WorldGetSpellData = 0x110;
		// World vtable: creature types (first, next after a type, data of a type, how many), map size in tiles
		constexpr std::uintptr_t WorldFirstCreatureType = 0xAC;
		constexpr std::uintptr_t WorldNextCreatureType = 0xB0;
		constexpr std::uintptr_t WorldCreatureData = 0xC0;
		constexpr std::uintptr_t WorldCreatureTypeCount = 0xC4;
		// World vtable: the creature type at a place in the list (0 up), thiscall (world, index) ret 4, returns the type.
		constexpr std::uintptr_t WorldCreatureTypeAt = 0xB8;
		// Player: its per-type portal table, 46 entries of 0x12 bytes { type byte, + 2 attracted (int), + 6 weight
		// multiplier, + 0xA, + 0xE second multiplier (fixed point 0x1000 = 1; 0xA000 special) }.
		constexpr std::uintptr_t PlayerPortalTable = 0xB57;
		constexpr std::uintptr_t WorldMapWidth = 0x1CC;
		constexpr std::uintptr_t WorldMapHeight = 0x1D0;
// World: the simulation's random seed (32 bit), stepped by the game's random function 0x5558D0 (cdecl (range, seed
// pointer, source file, line): seed = seed * 0x19660D + 0x3C6EF35F, then a rotation of it modulo the range) and saved with
// the world [verified: its callers pass 0x6E5054's world + 0x15A0].
constexpr std::uintptr_t WorldGameSeed = 0x15A0;
		// Any thing's position (12.12 fixed point: tile = value >> 12)
		constexpr std::uintptr_t ThingPositionX = 0x1A;
		constexpr std::uintptr_t ThingPositionY = 0x1E;
		constexpr std::uintptr_t ThingPositionZ = 0x22;
		// A thing's speed, x and y (z at + 0x10C), 32 bit; 0x4B1F20 adds to all three [verified].
		// A thing's heading (16 bit, 0x800 a full turn); its turn keeps the last one at + 0x102 [verified: 0x49BD4A].
		constexpr std::uintptr_t ThingHeading = 0xF4;
		constexpr std::uintptr_t ThingVelocityX = 0x104;
		constexpr std::uintptr_t ThingVelocityY = 0x108;
		// Player's dungeon heart tile (1,1 when it has none)
		constexpr std::uintptr_t PlayerHeartX = 0xB43;
		constexpr std::uintptr_t PlayerHeartY = 0xB45;
		// Keeper spell data: creature type the spell creates, name text id, description text id
		constexpr std::uintptr_t SpellCreatureType = 0x43;
		// The upgraded spell's values (effect +0x4B, then +0x4C/+0x50, used instead of +0x42/+0x43/+0x47): for
		// Create Imp, the creature type and (likely) its level
		constexpr std::uintptr_t SpellUpgradedCreatureType = 0x4C;
		constexpr std::uintptr_t SpellNameText = 0x64;
		constexpr std::uintptr_t SpellDescriptionText = 0x66;
		// Player interface: its player's tag, and the creature type shown in the Workers row
		constexpr std::uintptr_t InterfacePlayerTag = 0x8;
		constexpr std::uintptr_t InterfaceWorkerType = 0x123;
		// Player interface: its game session (0 when no game is running), which its push (InterfacePushAction) sends through.
		constexpr std::uintptr_t InterfaceSession = 0x4;
		// Front end: its list of menu windows (a pointer to the first; the game's own "window shown" lookup 0x5322D0 takes
		// the first with its shown flag set), and in each window its id (9 the skirmish screen, 34 the map list), shown flag
		// and next window. + 0x6016 is only the window the map list was opened from (9 or 32), not the one shown.
		constexpr std::uintptr_t FrontEndWindowList = 0x30FCE;
		constexpr std::uintptr_t MenuWindowId = 0x44;
		constexpr std::uintptr_t MenuWindowShown = 0x48;
		constexpr std::uintptr_t MenuWindowNext = 0x62;
		// A menu window's first widget, and in each widget the next one, its id, and its text id (button config + 0x40;
		// read each frame by text widgets (0x5323B0, nothing drawn while 0), and the title picture's number for titles).
		constexpr std::uintptr_t MenuWindowWidgets = 0x6A;
		constexpr std::uintptr_t MenuWidgetNext = 0x7C;
		constexpr std::uintptr_t MenuWidgetId = 0x74;
		constexpr std::uintptr_t MenuWidgetText = 0x30;
		// A list widget's row height in frame pixels (its draw, 0x5445F0, starts the rows at the top of its box and steps
		// by this for each row shown).
		constexpr std::uintptr_t MenuListRowHeight = 0x8C;
		// A widget's "hide my window after the click" flag (button config + 0x12), read by the click (0x528610) after the
		// button's action.
		constexpr std::uintptr_t MenuWidgetHidesWindow = 0x5D;
		// Front end: the skirmish map records (a pointer, 0x890 bytes each, players at + 0x80), their count, and the chosen
		// one's number.
		constexpr std::uintptr_t FrontEndMapRecords = 0x657C;
		constexpr std::uintptr_t FrontEndMapRecordCount = 0x6604;
		constexpr std::uintptr_t FrontEndChosenMapRecord = 0x6608;
		// Front end: the window titles' pictures (0x28 bytes each, by the title widget's text id field, button config + 0x40; the map list's is 9, text 2800),
		// their second pictures, and their sizes (4 ints each), as the title draw (0x536700) reads them.
		constexpr std::uintptr_t FrontEndTitleImages = 0x360C;
		constexpr std::uintptr_t FrontEndTitleImages2 = 0x39A4;
		constexpr std::uintptr_t FrontEndTitleSizes = 0x3D3C;
		// Front end: a byte; while it is 1 to 3 the chosen map's preview picture is drawn (widget 687's draw, 0x537C30).
		constexpr std::uintptr_t FrontEndPreviewShown = 0x30C1E;
		// Front end: what the picture draw (0x52CF90) is called on (thiscall's object).
		constexpr std::uintptr_t FrontEndPictureDrawer = 0x30D3F;
		// Front end: two flags copied from a level's details (bits 5 and 6 of its flags word, + 0x64C0) when the front end
		// reads them (0x535C30), 0 before any level is read. While either is set the briefing window is the short one: its
		// title shows only text 1 (else "text 0" - text 1, 0x53A41D) and the sub objective box is left out (0x53ADB2).
		// After playing Secret1 one was set [verified in game].
		constexpr std::uintptr_t FrontEndShortBriefing = 0x311B6;
		constexpr std::uintptr_t FrontEndShortBriefing2 = 0x311BA;
		// Front end: the chosen map's name (wide, 0x40 characters), which starting a skirmish copies into the level name.
		constexpr std::uintptr_t FrontEndChosenMap = 0x6580;
		// Interface: while set, the key event only takes Escape. What holds the bridge (+ 0x10 in it).
		constexpr std::uintptr_t InterfaceKeysHeld = 0x10E2;
		constexpr std::uintptr_t InterfaceOwner = 0x4;
		// What the hand does now (4 sells), and whether it is dragging out an area with it.
		constexpr std::uintptr_t InterfaceTool = 0x13DB;
		constexpr std::uintptr_t InterfaceToolDragging = 0x13F0;
		constexpr std::uintptr_t InterfaceToolArea = 0x13E4;
		constexpr std::uintptr_t InterfaceCursor = 0x12F6;
		// Whether the options menu is open, and whether the side panels are shown. While typing a name, keys go elsewhere. The
		// key event also stops on the two after the focus (+ 0x10D6, + 0x10DA) and on + 0xFD.
		constexpr std::uintptr_t InterfaceOptionsOpen = 0x2722;
		constexpr std::uintptr_t InterfacePanelShown = 0xF5;
		constexpr std::uintptr_t InterfaceTyping = 0x111;
		constexpr std::uintptr_t InterfaceFocus = 0x10D6;
		constexpr std::uintptr_t InterfaceKeysOther = 0xFD;
		constexpr std::uintptr_t OwnerBridge = 0x10;
		// Creature: owner's tag, creature data pointer; creature data flags (1 = worker)
		constexpr std::uintptr_t CreatureOwner = 0x28;
		constexpr std::uintptr_t CreatureData = 0x370;
		constexpr std::uintptr_t CreatureDataFlags = 0x686;
		// Creature data: the bed object (Objects.kwd id, 0 = none: the royals, Imps, Skeletons, Horny)
		constexpr std::uintptr_t CreatureDataLairObject = 0x771;
		// A creature's state block (creature + 0x24E): the creature itself
		constexpr std::uintptr_t StateBlockCreature = 0x8;
		// Creature's hero job (0 = none; run by 0x478800 from the hero idle 0x477850) and the player it is aimed at
		constexpr std::uintptr_t CreatureGoodJob = 0x409;
		constexpr std::uintptr_t CreatureGoodJobPlayer = 0x40D;
		// Creature's party: leader's tag, and flags (4 = follows the leader, as the hero idle 0x477850 checks)
		constexpr std::uintptr_t CreaturePartyLeader = 0x3DF;
		constexpr std::uintptr_t CreaturePartyFlags = 0x46F;
		// Player: tag of the player that defeated it (0 while still in the game)
		constexpr std::uintptr_t PlayerKilledBy = 0x132;
		// Game object: windowed flag and the window object
		constexpr std::uintptr_t GameWindowed = 0x10;
		constexpr std::uintptr_t GameWindow = 0xEC9;
		// Cursor drawer: the pointer image (slot 0, a surface), its source rectangle and position on screen, and the
		// screen surface it draws on (0 = not drawing). Slots 1 and 2 (+0x5C / +0x10 apart) hold the saved background.
		constexpr std::uintptr_t CursorImage = 0x54;
		// The game's surface wrapper (as at CursorImage): its DirectDraw surface, 0 while it has none (0x5B8150 reads it
		// for GetSurfaceDesc without checking).
		constexpr std::uintptr_t SurfaceWrapperSurface = 0x24;
		constexpr std::uintptr_t CursorSourceRect = 0x194;
		constexpr std::uintptr_t CursorX = 0x1C4;
		constexpr std::uintptr_t CursorY = 0x1C8;
		// The world's creature pool. What is under the hand: its kind (2 a creature of ours, 3 anyone else's) and its tag.
		constexpr std::uintptr_t WorldCreatures = 0x8;
		constexpr std::uintptr_t UnderHandKind = 0x1C;
		constexpr std::uintptr_t UnderHandTag = 0x20;
		// A thing: which of the world's lists it is on (0 creatures, 9 none, as a removed creature waiting in the pool, which
		// keeps its tag and is still marked in use), where it is (x, y, 4096 to a tile), and its player.
		constexpr std::uintptr_t ThingList = 0x12;
		constexpr std::uintptr_t ThingX = 0x1A;
		constexpr std::uintptr_t ThingY = 0x1E;
		constexpr std::uintptr_t ThingPlayer = 0x2A;
		// A live button: its sprite number and its text id, from its config's +0x3C and +0x40.
		constexpr std::uintptr_t ButtonSprite = 0x2C;
		constexpr std::uintptr_t ButtonTextId = 0x30;
		// An object: its type, how solid it is drawn (0 to 255), and flags (bit 0: still fading in). The Dungeon Heart's Arches (86)
		// and rings (88) start at 0 with that bit for the local player, and the heart's tick fades them in (0x4D9CB8).
		constexpr std::uintptr_t ObjectType = 0x12C;
		// ThingList's value for an object (tested: the Dungeon Heart's Arches are found by it).
		constexpr std::int32_t ObjectsList = 2;
		constexpr std::uintptr_t ThingAlpha = 0x8F;
		constexpr std::uintptr_t ThingFadeFlags = 0x9F;
		// A trap or door: the tag of its player, the type, and the slot in its vtable that removes it.
		constexpr std::uintptr_t ThingOwnerTag = 0x28;
		constexpr std::uintptr_t TrapDoorType = 0xF6;
		constexpr std::uintptr_t ThingRemoveSlot = 0x30;
		// The tile drawer (TileDraw): its renderer, and the index of its box piece in EngineThingTable (0 for none). The renderer's
		// slot +0xE8 draws an engine thing on a tile, thiscall (renderer, x, y, thing, 0), 0 when it failed.
		constexpr std::uintptr_t DrawerRenderer = 0x9E;
		constexpr std::uintptr_t DrawerBoxPiece = 0x35D;
		constexpr std::uintptr_t RendererDrawThingSlot = 0xE8;
		// The box piece: which sides show (1 top, 2 bottom, 4 left, 8 right, and 0x10 always set by the game), bit 0 of +0xD5 set
		// while it is in use, its position (x, y, z), and bit 0 of +0x100 once placed.
		constexpr std::uintptr_t BoxPieceSides = 0x81;
		constexpr std::uintptr_t BoxPieceInUse = 0xD5;
		constexpr std::uintptr_t BoxPiecePosition = 0xD6;
		// Any bridge thing's position: x, y, z, 32-bit, 4096 per tile (0x446C70 takes the tile as >> 12).
		constexpr std::uintptr_t BridgeThingPosition = 0xD6;
		// The bridge's lights (Flame: CLight, 0x146 bytes, made by 0x4397E0 and set up by 0x43A1E0): the first at bridge
		// + 0x33A, each one's next at + 0x13C. A light's color now (three 16-bit 0-255; + 0x11F holds the one it was made
		// with), its reach (32-bit, 4096 per tile, at most 0x2780) and strength (a byte, 128 by default).
		constexpr std::uintptr_t BridgeLights = 0x33A;
		constexpr std::uintptr_t LightNext = 0x13C;
		constexpr std::uintptr_t LightColor = 0x111;
		constexpr std::uintptr_t LightRadius = 0x117;
		constexpr std::uintptr_t LightStrength = 0x145;
		constexpr std::uintptr_t BoxPiecePlaced = 0x100;
		// An engine thing's tint: three color bytes and 255, set white or to an owner's color as the game sets a thing up
		// (0x436317, 0x43639B) [verified: the stores; that the box piece is drawn with it is unverified].
		constexpr std::uintptr_t EngineThingTint = 0x6E;
		// A creature: its level, and flags. Flag 0x40 makes its removal leave nothing behind, with no death, sound or corpse;
		// 0x20 and 0x80 are other things its death does.
		constexpr std::uintptr_t CreatureLevel = 0x403;
		// A creature's hit points now, 32 bit (the makers set it, 0x52098C) [verified].
		constexpr std::uintptr_t CreatureHealth = 0x40;
		// In the global settings (GlobalSettingsPointer): the most health percent table, 16 bit by level.
		constexpr std::uintptr_t SettingsLevelHealthPercent = 0xE1;
		constexpr std::uintptr_t CreatureFlags = 0x3E6;
		// Possession actions (docs/game/possession.md). A creature: hunger (32 bit, state block + 0x4C), sleep left (32
		// bit, state block + 0x58), its bed object's tag, prison holder (the player whose Prison or pit holds it), attack
		// target, when its melee last hit (game turn), the possessed creature it follows, and (on the possessed one) its
		// number of followers (32 bit).
		constexpr std::uintptr_t CreatureHunger = 0x29A;
		// A player's gold (32 bit); the world's player events (passed to PlayerEvent).
		constexpr std::uintptr_t PlayerGold = 0x7E;
		constexpr std::uintptr_t WorldEvents = 0x2E54;
		// Experience toward the next level (32 bit; 0x457880 adds to it and levels up at the mark).
		constexpr std::uintptr_t CreatureExperience = 0x3FF;
		// A shot: the tag of the thing that cast it, and "cast by a possessed creature" (32 bit).
		constexpr std::uintptr_t ShotCastBy = 0x145;
		constexpr std::uintptr_t ShotCastByPossessed = 0x151;
		constexpr std::uintptr_t CreatureSleepLeft = 0x2A6;
		constexpr std::uintptr_t CreatureBed = 0x3B7;
		constexpr std::uintptr_t CreaturePrisonHolder = 0x3CB;
		constexpr std::uintptr_t CreatureAttackTarget = 0x3F9;
		constexpr std::uintptr_t CreatureFollowing = 0x482;
		constexpr std::uintptr_t CreatureFollowerCount = 0x488;
		// Creature data: its sleep length (32 bit, what sleep left is set to when it gets tired) and its Training Room
		// experience per second at level 1 (16 bit; other levels scale it by the settings' table below, in percent).
		constexpr std::uintptr_t CreatureDataSleepLength = 0x5D6;
		constexpr std::uintptr_t CreatureDataTrainingExperience = 0x6B9;
		constexpr std::uintptr_t SettingsLevelTrainingPercent = 0x1EF;
		// The world's "thing in front of the possessed creature" (vtable), thiscall (world, creature, position, angles),
		// ret 0xC: a tag.
		constexpr std::uintptr_t WorldThingInFront = 0x334;
		// A room's things (first tag; a thing's next is at + 0x12A). A Prison: its door's tile (x, y, 32 bit each), the
		// bar lifted (32 bit), the door open (32 bit), the countdown to closing it (byte, room ticks).
		constexpr std::uintptr_t RoomFirstThing = 0xE;
		constexpr std::uintptr_t ThingNextInRoom = 0x12A;
		constexpr std::uintptr_t PrisonDoorX = 0x51;
		constexpr std::uintptr_t PrisonBarLifted = 0x59;
		constexpr std::uintptr_t PrisonDoorOpen = 0x5D;
		constexpr std::uintptr_t PrisonDoorCountdown = 0x61;
		// Input system: the DirectInput mouse (its device part is 0x10 in) and the mouse's position object.
		constexpr std::uintptr_t InputMouseDevice = 0x5C;
		constexpr std::uintptr_t InputKeyboardDevice = 0x58;
		constexpr std::uintptr_t InputDevicePart = 0x10;
		constexpr std::uintptr_t InputMouse = 0x64;
		// Input system: the control keys object, which the DirectInput mouse hands its events to (0x5BC060 gives it to the mouse) [verified].
		constexpr std::uintptr_t InputControlKeys = 0x60;
		// Its vtable's listener calls for movement and button events.
		constexpr std::uintptr_t ListenerMouseMove = 0x14;
		constexpr std::uintptr_t ListenerMouseButton = 0x18;
		// An input event (0x5DDA90): its time (ms), "not handled yet" (1 until the game takes it from its queue),
		// movement: axis (0 x, 4 y, 8 wheel) and amount; button: key (0xF0 left, 0xF1 right, 0xF2 middle, 0xF3 other),
		// a point (0, 0), the DirectInput data byte (0x80 pressed) and flags (8 pressed; 1, 2, 4 Shift, Ctrl, Alt).
		constexpr std::uintptr_t InputEventTime = 0x4;
		constexpr std::uintptr_t InputEventPending = 0x8;
		constexpr std::uintptr_t MouseMoveAxis = 0xC;
		constexpr std::uintptr_t MouseMoveAmount = 0x10;
		constexpr std::uintptr_t MouseMoveEventSize = 0x14;
		constexpr std::uintptr_t MouseButtonKey = 0x10;
		constexpr std::uintptr_t MouseButtonData = 0x1C;
		constexpr std::uintptr_t MouseButtonFlags = 0x20;
		constexpr std::uintptr_t MouseButtonEventSize = 0x24;
		// Player interface: in a menu (int; Flame's inMenu, read by the mouse handler 0x4065C1), and the mouse turns or zooms the view (int, set while key
		// action 0x2B or 0x2C is held, 0x406AE4; the view then moves by the mouse's movement, 0x5B2BB0) [verified].
		constexpr std::uintptr_t InterfaceInMenu = 0x2722;
		constexpr std::uintptr_t InterfaceMouseLook = 0x13CB;
		// DirectInput device part: the cooperative flags wanted, and the window it last set them for.
		constexpr std::uintptr_t InputDeviceFlags = 0x4;
		constexpr std::uintptr_t InputDeviceWindow = 0x18;
		// Mouse position object: x and y as floats (the whole numbers at +0x10 and +0x14 come from these), the movement
		// since last asked, and what the raw movement is multiplied by.
		constexpr std::uintptr_t MouseFloatX = 0x20;
		constexpr std::uintptr_t MouseFloatY = 0x24;
		constexpr std::uintptr_t MouseMovedX = 0x28;
		constexpr std::uintptr_t MouseMovedY = 0x2C;
		constexpr std::uintptr_t MouseSpeed = 0x30;
		constexpr std::uintptr_t CursorScreen = 0x224;
		// Front end: the back buffer surface it draws onto this frame (set at the start of its 2D pass)
		constexpr std::uintptr_t FrontEndBackBuffer = 0x30340;
		// Front end: number of player slots on the chosen map (byte), and the menu state (byte, 7 = skirmish)
		constexpr std::uintptr_t FrontEndSlotCount = 0x5AAC;
		constexpr std::uintptr_t FrontEndMenuState = 0x30344;
		// Front end: 1 when this machine hosts the multiplayer game (Add Computer Player checks it, menu state 0x10)
		constexpr std::uintptr_t FrontEndIsHost = 0x311F2;
		// Player slot: a human's network id (from MLDPlay::GetPlayerDesc when the player joins; sent with the slot)
		constexpr std::uintptr_t PlayerSlotNetworkId = 0x3B;
		// Window config: its button list
		constexpr std::uintptr_t WindowConfigButtons = 0x3E;
		// Button config entry: id (int), click handler (cdecl (int, int, front end)), y, renderer, text id and
		// Video player settings block (Address::VideoPlayerConfig), as 0x5AABF0 places the film on the screen:
		// flags (0x40 doubles the shown width, 0x30 the height; 4 / 8 center it across / down; 1 / 2 add the
		// offsets below; 0x80 / 0x100 replace the shown width / height), the film's own size, offsets and sizes.
		constexpr std::uintptr_t VideoConfigFlags = 0x4;
		constexpr std::uintptr_t VideoConfigFilmHeight = 0x60;
		constexpr std::uintptr_t VideoConfigFilmWidth = 0x64;
		constexpr std::uintptr_t VideoConfigOffsetX = 0x68;
		constexpr std::uintptr_t VideoConfigOffsetY = 0x6C;
		constexpr std::uintptr_t VideoConfigShownWidth = 0x70;
		constexpr std::uintptr_t VideoConfigShownHeight = 0x74;
		// pre-rendered text index
		constexpr std::uintptr_t ButtonConfigId = 0x1;
		constexpr std::uintptr_t ButtonConfigClick = 0x6;
		// Right-click handler, and the first argument both handlers get (the second is at + 0x1A)
		constexpr std::uintptr_t ButtonConfigRightClick = 0xA;
		constexpr std::uintptr_t ButtonConfigClickArgument = 0x16;
		// Position and size (int16 each); + 0x2C/+ 0x2E repeat the size for the text area
		constexpr std::uintptr_t ButtonConfigX = 0x20;
		constexpr std::uintptr_t ButtonConfigWidth = 0x24;
		constexpr std::uintptr_t ButtonConfigHeight = 0x26;
		constexpr std::uintptr_t ButtonConfigTextWidth = 0x2C;
		constexpr std::uintptr_t ButtonConfigTextHeight = 0x2E;
		constexpr std::uintptr_t ButtonConfigY = 0x22;
		constexpr std::uintptr_t ButtonConfigRender = 0x38;
		constexpr std::uintptr_t ButtonConfigTextId = 0x40;
		constexpr std::uintptr_t ButtonConfigTextIndex = 0x44;
		// Live button: pre-rendered text index the renderer draws
		constexpr std::uintptr_t ButtonTextIndex = 0x34;
		// Button config: kind (byte; 0 click button, 3 slider, 7 text), tick (cdecl (button, front end), every frame),
		// and for sliders the range (+ 0x16 minimum, + 0x1A maximum, as the slider's configure 0x528EF0 reads them)
		constexpr std::uintptr_t ButtonConfigKind = 0x0;
		constexpr std::uintptr_t ButtonConfigTick = 0x34;
		constexpr std::uintptr_t SliderConfigMaximum = 0x1A;
		// Live button (the button setup copies these from its config): shown (int, 1 = yes), id, and for text labels
		// the text
		constexpr std::uintptr_t ButtonShown = 0x61;
		constexpr std::uintptr_t ButtonId = 0x74;
		constexpr std::uintptr_t ButtonText = 0x80;
		// Slider vtable: set value, thiscall (slider, value) ret 4, clamped to its range
		constexpr std::uintptr_t SliderSetValue = 0x14;
		// Player slot: relations to the other slots (8 + 8 bytes) and flags (low 3 bits: 0 empty, 1 AI, 2/3 human)
		constexpr std::uintptr_t PlayerSlotRelations = 0x2A;
		constexpr std::uintptr_t PlayerSlotAlliances = 0x32;
		constexpr std::uintptr_t PlayerSlotFlags = 0x3A;
	}
	constexpr std::uintptr_t DisplayDeviceSize = 0x21A;
	// Each mode starts with width, height and bit depth as 32-bit ints.
	constexpr std::uintptr_t DisplayModeSize = 0x7C;
	constexpr std::uint32_t GuiViewInlineTileCount = 140;
	constexpr std::uint32_t EngineHeapChunkSize = 0x200000;
	constexpr std::uint32_t PlayerSlotCount = 8;
	constexpr std::uintptr_t PlayerSlotSize = 63;
	constexpr std::uintptr_t PlayerSlotTextSize = 0x66;
	constexpr std::uintptr_t ButtonConfigSize = 0x50;
	constexpr std::uintptr_t PrerenderEntrySize = 0x88;
	// A frame description (pixel format, pixels, width, height, pitch)
	constexpr std::size_t FrameDescriptionSize = 0x28;
	// Front end window holding the skirmish lobby, and the player number of keeper slot 0
	constexpr std::int32_t SkirmishWindowId = 9;
	constexpr std::uint8_t SinglePlayerWindowId = 2;
	constexpr std::size_t PlayerTableEntrySize = 0x1125;
	constexpr std::uint16_t TagMask = 0xFFF;
	constexpr std::uint32_t TerrainIsRoomFlag = 0x10;
	constexpr std::uint16_t RoomOverWaterFlag = 1;
	constexpr std::uint16_t RoomOverLavaFlag = 2;
	constexpr std::int32_t MultiplayerWindowId = 32;
	constexpr std::uint32_t FirstKeeperPlayer = 3;
	constexpr std::uint8_t LocalPlayerChosenFlag = 0x80;
	constexpr std::uint8_t GoodPlayer = 1;
	// Object types (ObjectType): an egg (laid in a Hatchery, hatches into a chicken, type 9).
	constexpr std::uint8_t ObjectEgg = 0x2F;
	constexpr std::uint8_t ObjectChicken = 9;
	constexpr std::uint8_t CreateImpSpell = 4;
	constexpr std::uint32_t SpellAvailable = 3;
	// States in the room, door, trap and spell availability lists
	constexpr std::uint32_t StateResearchable = 2;
	constexpr std::uint32_t StateAvailable = 3;
	constexpr std::size_t AvailabilityEntrySize = 9;
	constexpr std::size_t SpellEntrySize = 0x11;
	// The Good player's mana, as the game sets it every tick
	constexpr std::int32_t UnlimitedMana = 0x7FFFFFFF;
	constexpr std::uint32_t CreatureWorkerFlag = 1;
	// Creature data flags + 0x686 bit 0x80, with the worker flag: the Imp's drop rules (0x450910). It may be dropped into
	// the heart's middle 3x3 (where 0x459180 takes a worker that lands there away), and not onto a Portal or
	// Mercenary Portal. Also read when the interface picks "the" Imp type (first type with both) and by 0x4E8C60.
	constexpr std::uint32_t CreatureImpDropFlag = 0x80;
	constexpr std::uint8_t PartyFollowerFlag = 4;
	// The action kind of "exit to Windows" (the in-game confirmation's yes, 0x4205B0, sends it with this player's tag).
	constexpr std::int32_t ActionExitToWindows = 0x7D;
	constexpr std::int32_t PlayModeCampaign = 1;
	constexpr std::int32_t PlayModeSkirmish = 2;
	constexpr std::int32_t PlayModeMultiplayer = 3;
	// Wide characters in the game settings' level name.
	constexpr std::size_t LevelNameLength = 64;
	constexpr std::uint32_t ActionListCapacity = 16;
	constexpr std::uint8_t PortalMercenaryFlag = 8;
	constexpr std::size_t CreatureCountEntrySize = 0xD;
	constexpr std::size_t CreatureCountValue = 9;
	constexpr std::size_t CreatureListEntrySize = 5;
	// A list row is { type, count, 3 bytes }. The game copies and sends the last 3 but level setup only reads type and
	// count, so they can carry our session values.
	constexpr std::size_t CreatureListFreeByte = 2;
	// The heroes' starting number in the lobby's creature list
	constexpr std::uint8_t HeroLobbyDefault = 16;
	constexpr std::size_t CreatureAttractionBytes = 6;
	constexpr std::size_t CreatureAttractionEntrySize = 3;
	constexpr std::uint8_t AttractionKindRoom = 1;
	// Room type ids (Lair 2 and Mercenary Portal 0x19 seen in the game's own code, the rest [reference] Flame)
	// Room type ids, named as in the game (a space is "_"). See docs/game/rooms.md.
	namespace Room {
		constexpr std::uint8_t Lair = 2;
		constexpr std::uint8_t Hatchery = 4;
		constexpr std::uint8_t Dungeon_Heart = 5;
		constexpr std::uint8_t Portal = 3;
		constexpr std::uint8_t Library = 6;
		constexpr std::uint8_t Training_Room = 7;
		constexpr std::uint8_t Treasury = 1;
		constexpr std::uint8_t Work_Shop = 10;
		constexpr std::uint8_t Casino = 15;
		constexpr std::uint8_t Guard_Room = 9;
		constexpr std::uint8_t Wooden_Bridge = 8;
		constexpr std::uint8_t Prison = 11;
		constexpr std::uint8_t Torture_Chamber = 12;
		constexpr std::uint8_t Stone_Bridge = 17;
		constexpr std::uint8_t Graveyard = 14;
		constexpr std::uint8_t Combat_Pit = 16;
		constexpr std::uint8_t Temple = 13;
		constexpr std::uint8_t Hero_Gate_Final = 18;
		constexpr std::uint8_t Hero_Gate_2x2 = 20;
		constexpr std::uint8_t Hero_Gate_3x1 = 23;
		constexpr std::uint8_t Hero_Portal = 25;
	}
	// Creature (unit) type ids, named as in the game (a space is "_"), as the level editor shows them. See
	// docs/game/creatures.md.
	namespace Creature {
		// Evil creatures
		constexpr std::uint8_t Imp = 1;
		constexpr std::uint8_t Bile_Demon = 3;
		constexpr std::uint8_t Mistress = 4;
		constexpr std::uint8_t Warlock = 5;
		constexpr std::uint8_t Dark_Elf = 6;
		constexpr std::uint8_t Goblin = 7;
		constexpr std::uint8_t Vampire = 8;
		constexpr std::uint8_t Skeleton = 9;
		constexpr std::uint8_t Troll = 10;
		constexpr std::uint8_t Salamander = 11;
		constexpr std::uint8_t Firefly = 12;
		constexpr std::uint8_t Black_Knight = 22;
		constexpr std::uint8_t Dark_Angel = 23;
		constexpr std::uint8_t Rogue = 24;
		constexpr std::uint8_t Horny = 27;
		constexpr std::uint8_t Maiden_Of_The_Nest = 45;
		// Good creatures
		constexpr std::uint8_t Prince_Tristran = 2;
		constexpr std::uint8_t Knight = 13;
		constexpr std::uint8_t Dwarf = 14;
		constexpr std::uint8_t Giant = 15;
		constexpr std::uint8_t Wizard = 16;
		constexpr std::uint8_t Elven_Archer = 17;
		constexpr std::uint8_t Thief = 18;
		constexpr std::uint8_t Monk = 19;
		constexpr std::uint8_t Fairy = 20;
		constexpr std::uint8_t King_Reginald = 21;
		constexpr std::uint8_t Guard = 25;
		constexpr std::uint8_t Prince_Felix = 26;
		constexpr std::uint8_t Stone_Knight = 28;
		constexpr std::uint8_t Lord_Of_The_Land = 29;
		constexpr std::uint8_t Royal_Guard = 30;
		constexpr std::uint8_t Prince_Balder = 31;
	}
}
