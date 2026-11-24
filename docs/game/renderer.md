# Renderer (DirectX 11, DirectX 12, Vulkan)

KeeperForge2 can draw the game with a modern graphics API instead of the game's
own Direct3D 6. It does this by giving the game its own DirectDraw and Direct3D
(Runtime/Source/Renderer/DirectDrawLayer.cpp), which hand the work to a backend
(BackendD3D11/D3D12/Vulkan.cpp). Set with `renderer:` in config.yml: `vulkan` by
default, `dx11` or `dx12`. The game's own Direct3D through patch.dll (`original`)
is no longer offered (user's choice, 2026-11-05): that word or an unknown one
gives Vulkan, which is written back to config.yml.

## How the game reaches DirectDraw
- The game calls `DirectDrawCreate` and `DirectDrawEnumerateA` through its import
  table: 0x66C020 and 0x66C024, which point at PATCH.DLL's exports (GOG/Steam
  release). The thunks are 0x5B29F6 and 0x5B29F0. Calls: 0x5574CF and 0x5579B4
  (device and mode lists at startup), 0x5B4DEC (the real one). [verified]
- The real one (0x5B4DE0, called from the screen setup) runs only while the
  game's DirectDraw pointer 0x79D3D0 is 0; 0x5B4050 hands it out with AddRef.
  So the game makes its DirectDraw once and keeps it. [verified]
- KeeperForge2 writes its own functions into the two import slots when a new
  renderer is chosen, before 0x79D3D0 is set. If it is already set (the runtime
  started late, not through the launcher), that session draws with patch.dll's
  Direct3D, as nothing else can take over. [verified mechanism]

## DDrawCompat (the game folder's ddraw.dll) [verified]
- The Steam release's ddraw.dll is DDrawCompat, a third-party DirectDraw
  compatibility layer (its strings in our export, its license and its log
  DDrawCompat-DKII-DX.log in the game folder). patch.dll imports it, so it
  is loaded at start even when the game uses our DirectDraw.
- At load it hooks display modes, the registry and the graphics driver (the
  NVIDIA user mode driver). On the first call of any of its exports it also
  installs DirectDraw, Direct3D and GDI hooks (each export calls the install
  routine first). [unverified: that it is what blacks out our renderers]
- With those hooks in, D3DIM700/D3DIM are loaded and our DirectX 11 picture
  never reached the window (its pixels read back black) [verified].
- It installs them on its own while loading, before a late-injected runtime
  starts (its "installed" byte was already set when the runtime looked;
  setting it first was tried and was too late) [verified]. So the launcher
  loads KeeperForge2 with the game's DLLs (see
  [api/launcher.md](../api/launcher.md)), and when one of our renderers is
  chosen Renderer::BeforeDllsStart turns DDrawCompat's start routine into
  "return TRUE" before it runs (only if it really is DDrawCompat, found by
  its "DDrawCompat loaded successfully" string). Its two TLS callbacks are
  the compiler's thread-local init/cleanup with nothing to do then
  [verified in the export]. patch.dll's ddraw.dll imports are then pointed
  at Windows' own ddraw.dll, so nothing reaches the DDrawCompat that never
  started. A start through the launcher always keeps it out now.
  Tested: DirectX 11, DirectX 12 and Vulkan all show the game with this.

## What the game uses (as patch.dll's wrapper answers it) [reference]
Flame rebuilt patch.dll's DirectDraw/Direct3D wrapper; which of its methods do
real work tells what the game calls. In plain terms:
- DirectDraw 1: CreateSurface, EnumDisplayModes, GetCaps, SetCooperativeLevel,
  SetDisplayMode, RestoreDisplayMode, QueryInterface for DirectDraw 2/4 and
  Direct3D 1/2. DirectDraw 2: CreateSurface (FourCC video surfaces only), GetCaps,
  GetFourCCCodes, GetAvailableVidMem. DirectDraw 4: CreateSurface (textures),
  GetCaps, GetDeviceIdentifier, QueryInterface for Direct3D 3.
- Surfaces (version 2 table): Blt, BltFast, Flip, GetAttachedSurface (back
  buffer), Lock/Unlock, SetColorKey, GetSurfaceDesc, GetPixelFormat,
  AddAttachedSurface (depth buffer), QueryInterface for version 4. Version 4:
  Lock/Unlock, BltFast, QueryInterface for IDirect3DTexture2 and the gamma
  control.
- Direct3D 1/2: EnumDevices. Direct3D 3: CreateDevice (on the back buffer, HAL
  device), CreateViewport.
- Device 3: GetCaps, EnumTextureFormats, BeginScene/EndScene, SetRenderState,
  SetTexture, SetTextureStageState (stage 0; stages 1-2 with bump mapping),
  DrawIndexedPrimitive: triangle lists only, flags 0x1C, vertex format 0x144
  (XYZRHW, diffuse, 1 texture coordinate: 28 bytes) or 0x344 (3 coordinates, 44
  bytes), z between 0 and 1.
- Everything is 32 bits: display modes, surfaces, and the texture formats
  offered (XRGB and ARGB 8888).
- Gamma ramps are ignored. Viewport SetViewport2 does nothing; the depth buffer
  is cleared at the first BeginScene after each Flip.
- The device test 0x557820 needs the HAL description's triangle texture caps
  bit 0 (perspective) and filter caps bit 1 (linear), plus three fields of the
  device's 3D info (+0x182/+0x186/+0x18A) set. [verified] What sets those three
  is not known (likely the texture format list). [unverified]

## The layer
- Every surface lives in memory, so locks and blits work as the game expects.
  The 3D render target is also the backend's "frame"; a flag says which copy
  is newer, and the other is brought up to date only when needed (a lock or
  2D blit reads the frame back; a draw after 2D work uploads it).
- The primary surface shows a surface without copying it (after Flip, or a
  whole-frame Blt onto it); the copy is taken only if the game then locks or
  blits onto the primary. Drawing onto the primary shows it at once.
- Triangle culling is done on the CPU (the vertices are already in screen
  space), so no backend culls and winding never depends on the API.
- The texture stages run in one shader (ShaderSources.h): D3DTOP operations on
  D3DTA arguments for up to three stages, then the alpha test. Direct3D 5 render
  states (TEXTUREMAPBLEND, TEXTUREHANDLE and so on) are turned into stage 0
  states as Direct3D 6 does.
- Display modes reported: 640x480 up to 1600x1200 and the desktop size, all 32
  bits; SetDisplayMode accepts any size (Widescreen adds its size as with
  patch.dll). The window is left as the game made it; the picture is scaled
  into it keeping its shape (videos stretch, as with patch.dll).
- The game calls SetCooperativeLevel with more than one window: its mode list
  (0x5B4E20) makes a hidden "ModeList" window, uses it and destroys it. The
  layer draws into the latest window and starts the backend again when it
  changes. [verified in the code; the black screen it explains is unverified]
- All calls take one lock: the game plays videos from a thread of its own.
  The lock records its owner; a watchdog thread logs where the threads are
  when the lock is held over 5 s or nothing is shown for 10 s in front.
- Flip: when both copies of the 3D frame match, the screen and the back
  buffer swap their memory, so the screen keeps the frame just shown (as a
  real flip). The lazy copy (mirror) is used otherwise.
- The game's window is taken out of always-on-top (a hung game covered
  everything).
- The game's window: the layer subclasses it. It drops WM_ERASEBKGND and
  WM_PAINT and removes the class's black brush: Windows painting that black
  covered what Vulkan showed (tested: black until this, then the picture).
  It minimizes the window when the game loses focus and shows nothing while
  the game is in the background (as patch.dll does); without that, Vulkan
  waited forever for a window image and the game froze (tested).
- DirectX 11 and 12 (flip and blit swap chains alike) showed nothing while
  DDrawCompat's hooks were in, with every call succeeding; with DDrawCompat
  kept out all three renderers show the game (tested). DirectX 11 is back to
  flip first, blit only before Windows 10.
- DirectX 12 and Vulkan send each frame's uploads (textures, the CPU copy
  of the frame, vertices) through a buffer per frame, 32 MB to start. When it
  is full, the work so far is sent and waited for in the middle of the frame.
  A 4K frame alone is 33 MB, and busy scenes upload many textures, so with
  DirectX 12 this happened in busy moments: fps drops for a few seconds,
  never while idle (tested: DirectX 11, which never waits there, did not
  stutter). DirectX 12's buffer now doubles when it fills, up to 128 MB per
  frame (the game is 32-bit), and the log says when it grows, when it still
  waits mid-frame, and when a new kind of draw takes 20 ms or more to make
  its pipeline. Vulkan is unchanged.
- `frame_rate: vsync` makes presents wait for the vertical blank; otherwise
  FrameRate paces the game and presents do not wait.

## Videos (the game's .tgq player) [verified in the export]
- 0x53BCD0 plays a video. Its settings block is at 0x73EDC8 (flags at +4,
  DirectDraw at +0xC, DirectDraw 2 at +0x10, the screen and back buffer as
  IDirectDrawSurface2 at +0x14/+0x18).
- 0x5AA0B0 picks how frames are shown from DirectDraw 2's GetCaps: blit FourCC
  + stretch (caps 0x100 and 0x200) gives 4, overlay FourCC + stretch gives 2,
  otherwise 1. With 1 and the flags the game uses, each frame is decoded
  straight into the locked back buffer (0x5AA670) and the screen is flipped
  (0x5A9050), on the player's own thread.
- 0x5AABF0 places the film on the screen: shown size = the film's size from
  its header (+0x64 width, +0x60 height), doubled across with flag 0x40 and
  down with 0x30; centered across / down with flags 4 / 8; +0x68 / +0x6C added
  with flags 1 / 2; +0x70 / +0x74 replace the size with 0x80 / 0x100. The
  game plays with flags 0xB05064C (| 0x800000): width doubled, height not,
  centered, so the film is a band of the 640x480 screen. [verified in the
  export]
- So with our DirectDraw the film covered only part of the stretched screen
  (tested: Bullfrog logo and intro not full screen). While a video plays,
  Widescreen gives the renderer the film's area worked out as above, and only
  that area is shown: over the whole window with fullscreen_videos: stretch,
  in its shape with bars.
- The width doubling (flag 0x40) is done by writing each film pixel as 4
  bytes, two 16-bit pixels; on our 32-bit screen that is one pixel, so the
  film covers only its own width (320 for the intro and Bullfrog logo,
  tested: they showed in the left half). The renderer shows that width in
  the doubled (4:3) shape. [unverified: in game]
- 0x53C2E0 handles the game's activate events while a video plays: on each one
  0x5A8FF0 toggles the player between playing (3) and paused (4), stopping or
  starting its sound buffer. It is a toggle, so the events must alternate.
  They come from 0x558C70 (WM_ACTIVATE, WM_ACTIVATEAPP and WM_NCACTIVATE: event 0
  for focus coming, 1 for focus lost), three each way per switch but not always,
  so a video could stay paused. GameFixes now plays or pauses it as the event
  says (see game-fixes.md). [verified in the export]
- patch.dll minimizes the game's window when it loses focus, and sends the
  window WM_ACTIVATE when the first DirectDraw 2 surface is made. [reference]

## Switching
- Among DirectX 11, DirectX 12 and Vulkan, at once: the layer reads
  the frame back into memory, drops the old backend, starts the new one on the
  same window and gives it the frame; textures are uploaded again as they are
  used. If the new one cannot start, the old one comes back.
- If the chosen API cannot run (tried on a hidden window at startup), the others
  are tried there in turn (DirectX 11, Vulkan, DirectX 12). If it fails later on
  the game's window, they are tried again in that order. With none, the log has an
  error and the game shows nothing: there is no fall back to patch.dll's Direct3D.
- Vulkan needs its SPIR-V: Runtime/Source/Renderer/VulkanShaders.h, written by
  Runtime/Shaders/compile_vulkan_shaders.bat (Vulkan SDK). Until then Vulkan
  reports that its shaders are missing. The anti-aliasing shaders come from the
  same script; a VulkanShaders.h without them (placeholders whose first word is
  0) runs Vulkan without anti-aliasing and says so in the log.

## Borderless window (window_mode) [unverified: written, not yet tested]
- With `window_mode: borderless` the layer restyles the game's
  `_BullfrogLibScreen` window to a plain popup (no caption, frame or topmost
  flag) over the whole monitor it is on, and puts it back there after the game
  or Windows moves it (WM_WINDOWPOSCHANGED, WM_DISPLAYCHANGE, activation).
- It is not minimized when focus goes elsewhere; the layer already shows
  nothing while the game is inactive. `fullscreen` keeps the old behavior
  (minimized on focus loss, as patch.dll does).

## DPI awareness [unverified: written, not yet tested]
- The game is not DPI-aware. On a 4K desktop at 300% scaling Windows reported
  its window and monitor as 1280x720, so the 3840x2160 frame went into a
  1280x720 swap chain and Windows stretched that up (seen in our logs) [verified].
- Renderer::Install now makes the process per-monitor DPI-aware (v2, falling
  back to the older calls) before the test window and the game's window are
  made, so window and monitor sizes are real pixels. If something already set
  the awareness (Windows' compatibility settings), it is left alone and logged.
  The borderless window is placed again on WM_DPICHANGED.

## Anti-aliasing (anti_aliasing) [verified on Vulkan, SMAA x4; others not yet tested]
- Set on the backend before Init (Backend::SetAntiAliasing); the hidden test
  window never gets any. Each backend logs what it uses, and turns the setting
  down or off when the card or the shaders cannot do it.
- MSAA x2/x4/x8: the draws go into a multisampled color and depth (all three
  APIs check the sample count for BGRA8 and the depth format and halve it until
  it is supported). The single-sample frame then only holds the frame resolved:
  it is resolved before Present and ReadFrame, only when draws happened since.
  UploadFrame fills the single-sample frame and draws it pixel for pixel into
  every sample (a copy shader, as a multisampled image cannot be copied into).
  Alpha-tested edges are not smoothed (no alpha to coverage).
- FXAA and SMAA run at Present on PresentSource::Frame only, from the frame into
  a picture of the frame's size, which is then shown instead. ReadFrame (what
  the game reads back, screenshots) is never smoothed. The game's 2D panels and
  text are in the same picture, so they are softened slightly too.
  - FXAA is our own shader (ShaderSources.h, Runtime/Shaders/fxaa.frag): finds
    the edge direction from brightness, walks along it to both ends and reads
    across it by the distance to the nearer end. x2/x4/x8 change the contrast
    thresholds, the extra softening of lone pixels and the walk length (4, 8, 12
    steps).
  - "smaa" is our own morphological anti-aliasing (SmaaHlsl in ShaderSources.h,
    Runtime/Shaders/smaa.frag), three passes drawn with PostVertex/post.vert
    that read whole pixels (Load/texelFetch; no lookup textures):
    1. Edges (R8G8): luma difference to the left and top neighbor above a
       threshold, dropped where it is less than half the strongest difference
       around it.
    2. Weights (RGBA8): each edge is followed both ways (up to 8/16/32 pixels)
       to its ends. An edge crossing an end on one side means the outline steps
       that way: it is taken to run from half a pixel to that side at the end to
       the middle of the edge. The share of the pixel's column (row) the outline
       cuts off is worked out exactly (straight lines, so no area table).
       x/y = this pixel takes from the one above / the one above takes from this
       one; z/w = the same for the left neighbor.
    3. Blend: each pixel mixes in its four neighbors by those amounts (scaled
       down when they add up to more than 1).
    x2/x4/x8 change the threshold (0.15, 0.1, 0.05) and search length; they come
    in the post constants (params), so each pass is one shader for all presets.
    [unverified: not yet seen in game]
- Not done: MSAA + SMAA together, diagonal edges in SMAA (only horizontal and
  vertical outlines are followed).

## Bump mapping of water and lava (bump_mapping) [verified in the export; crash found in game, fixed, retest]
- The game has its own environment bump mapping for water and lava, off unless its setting (video settings 0x759A78 +
  0x98, "BumpMappingConfig"; 1 du/dv, 2 du/dv with luminance; the -ENABLEBUMPMAPPING and -ENABLEBUMPLUMINANCE switches
  set it) is on. [reference: Flame offers it as an option]
- With the setting on, the engine loads BumpWater, BumpWaterBumpMap, BumpWaterEnvMap, BumpLava, BumpLavaBumpMap and
  BumpLavaEnvMap (0x59856B), and starts with engine flag 0x10 (du/dv) or 0x20 (luminance) (0x59D951). It then only
  uses bump mapping if the device offers a 16-bit texture format with DDPF_BUMPDUDV (flag 0x10) or DDPF_BUMPLUMINANCE
  (0x20), keeping the last one offered [reference: Flame; our export's enumeration callback agrees].
- Drawing (0x5895xx, render flag 0x8000 du/dv, 0x10000 luminance): stage 0 texture * diffuse (color and alpha);
  stage 1 BUMPENVMAP (0x16) or BUMPENVMAPLUMINANCE (0x17) on its texture and the current color, alpha off, matrix
  1, 0, 0, 1 (BUMPENVMAT00..11, states 7-10), luminance scale 1 and offset 0 (states 0x16, 0x17); stage 2 its
  texture added to the current color (ADD), alpha off. The vertex format is 0x344 (3 texture coordinates).
- Our layer (bump_mapping: all, water, lava or off, all by default; bump_luminance: on or off, off by default; the
  old words on and luminance become all, luminance also turning bump_luminance on):
  - Renderer.cpp turns the two reads of the game's setting (mov eax, [0x759B10]) into mov eax, mode, so the game's
    saved setting is not changed.
  - [verified in the export] Water tiles (0x5A2570) and lava tiles (0x5A2BE0) each test engine flag 0x10 and jump past
    their bump values when it is off (short jz at 0x5A2B0D and 0x5A2E54). For water only or lava only, the other's jz
    is made a jmp, so it is drawn as with bump off (plain texture and its own opacity). The Temple's pool counts as
    water: its redirect (below) is only installed when water is bumped. [unverified in game]
  - The device reports D3DTEXOPCAPS_BUMPENVMAP and BUMPENVMAPLUMINANCE and offers L6V5U5 then V8U8 after the 32-bit
    formats (so du/dv mode keeps V8U8). A surface made with a bump format keeps 16-bit texels (pitch width * 2) in
    its memory; copies between bump surfaces are 16-bit (no keys); it is sent to the card as 32-bit texels: du + 128
    in red, dv + 128 in green, luminance in blue (L6V5U5's 5-bit du/dv times 8, its 6-bit luminance to 0-255).
  - The shaders (HLSL in ShaderSources.h, GLSL in Shaders/draw.frag) treat a bump stage as: color and alpha unchanged;
    offset = (du * M00 + dv * M10, du * M01 + dv * M11) added to the next stage's texture coordinates, and for the
    luminance kind the next stage's texel color times clamp(L * scale + offset). The matrix and luminance values are
    two rows added at the end of the draw constants (208 bytes).
  - [verified in the export and in game] The game's bump-map maker (0x591820) turns a height texture into 16-bit
    du/dv texels in a buffer of width * height * 2 bytes (0x590580), then writes one row past its end (and reads one
    row past the height texture). With Widescreen's engine heap (a Windows heap) that broke the heap and every later
    allocation failed ("Out of memory allocating 8192 bytes"), about 20 s after the menu. Widescreen.cpp now asks for
    one more row at that allocation (0x5905D8, row size in EDI), for every texture buffer made there.
  - [verified in the export] Which liquids get bump: terrain is drawn per tile (0x43CC30). Water terrain (flag 0x80 at
    terrain + 0x17B) goes through engine method + 0xF8 (0x5A2570), lava (0x100) through + 0xFC (0x5A2BE0), and a room's
    own liquid through + 0x100 (0x5A2F10; textures in the room's type record, filled with "Water" for room type 13,
    the Temple, at 0x4EB151). All make a liquid surface with 0x5873A0. Water uses the engine's BumpWater, bump and
    environment maps (engine + 0x72, + 0x76, + 0x7A), flag 0x8000 and full opacity when the engine's bump flag (0x10 in
    0x764BB8) is on and video setting + 0x68 is 0; lava adds 0x8000 with its own maps (+ 0x82, + 0x86). The room liquid
    never did, so the Temple's pool stayed flat. Renderer.cpp now redirects its surface call (0x5A31E0, engine in EBP)
    to give it the water tiles' bump values under the same conditions. [unverified] That only the Temple has a room
    liquid in the stock game (rooms with bit 1 at room data + 0x261).
  - bump_temple (on or off, on by default): with off, that redirect is not installed, so the Temple's pool stays flat
    (the game's own look) while water tiles are bumped. Only matters when water is bumped (all or water).
  - Vulkan needs VulkanShaders.h made again from the GLSL (compile_vulkan_shaders.bat); until then its shader passes
    the bump stage through without moving anything.

## Model detail (model_detail) [verified in the export; not tested in game]
- The game's models are progressive meshes. Its video settings (0x759A78) hold "PMesh Reduction Level" at + 0xA4
  (0x759B1C; 0 when not saved). The engine start reads it once ("mov eax, [0x759B1C]" at 0x59864C) and keeps it at
  0x760AB0 (0x5747C0); in play mode 5 it is given 16 instead.
- Each mesh's detail is worked out where it is set up to be drawn (0x580FC5 and 0x584A05, the same code):
  v = (460 / focal * depth - t) / (10 - t) - level / 16, t = 1 + 6 * min(radius, 0.5), kept to 0..1 and times 12
  (mesh + 0x70 or + 0x7C). It grows with depth and shrinks with size, and a mesh with flag 0x4000 skips the sum and
  gets 0, so 0 is the whole mesh and 12 the fewest triangles. A level of 16 or more makes every v 0.
- model_detail: full (the default) patches that one read into "mov eax, 16", so every model is whole at any distance,
  as the game does itself in play mode 5. The saved setting is not changed. game keeps the game's own way
  (ModelDetail.cpp).
- The other video settings, for the record: View Distance (+ 0x50, default 12) is only written (0x566320) and never
  read in this build, so it changes nothing. Shadow Level (+ 0x54, 0 to 3, the in-game slider, default 3) goes to
  0x764BDC; the engine only tests it below 3. High Res Textures (+ 0x78, default on) and Texture Reduction Level
  (+ 0x7C, 0 to 2, default 0) already default to the best.

## Texture filtering (texture_filtering) [written; not built or tested]
- The game makes no mip levels, so far textures shimmer and slanted floors blur or alias. With texture_filtering
  trilinear or anisotropic x2/x4/x8/x16 (x16 by default):
  - The layer gives each of the game's textures (texture surfaces, not bump maps) every mip level down to 1x1
    (MipLevelCount), made on the CPU with our own 2x2 box filter (HalveMip) each time the texture is uploaded.
    Colors are weighted by alpha so see-through texels do not bleed in. A texture whose alpha is only 0 or 255
    (cut-outs for the alpha test) keeps it so: a texel is opaque when at least half of what it covers is, so
    cut-outs do not thin out far away.
  - Backend::CreateTexture takes a level count and UpdateTexture a level (DirectX 11 UpdateSubresource of that
    subresource; DirectX 12 CopyTextureRegion into that subresource; Vulkan a copy into that level, barriers over
    every level). Other textures (the frame's copies, blit pictures, the ray tracer's) keep one level.
  - Samplers: linear ones are anisotropic at the chosen level (DirectX FILTER_ANISOTROPIC, Vulkan anisotropyEnable
    with samplerAnisotropy turned on for the device when it has it; a device with it allows at least 16). Vulkan's
    samplers now use mip levels (mipmap mode as the filter, maxLod VK_LOD_CLAMP_NONE; before, maxLod was 0).
    DirectX samplers already had MIP_LINEAR / MIP_POINT and no level limit.
  - off: one level and no anisotropy, as before.
- Watch for: textures that are pages of several pictures could bleed into each other at the smallest levels; text or
  panels drawn smaller than their texture now look smoother (they were sampled from the full picture before).

## Model smoothing (model_smoothing) [written; not built or tested]
- More triangles for round shapes, made by our own code (Source/Renderer/ModelSmoothing.*) from the game's triangles
  as they reach the layer; no game file or model is read or changed. low, medium (the default) or high cut each curved
  edge into 2, 3 or 4; off draws the game's triangles as they come.
- Which draws: only creatures' and objects' meshes. The engine draws the world in batches (0x58F640: 0x589D90 starts
  one, each queued primitive adds its triangles through its vtable + 4, 0x58A150 draws it); ModelDetail.cpp hooks the
  start and the draw and the adds of CEngineAnimMesh (0x5848B0) and CEngineDynamicMesh (0x582CE0) [class names
  reference: Flame's mapping], and a batch they added to is a model batch. Of those, only draws that are depth tested
  and written, not blended, and without a bump mapping stage.
- Tested first on every depth-written draw (2026-11-22): the terrain was curved too. Its floors are slightly uneven, so
  they bulged, and the overlays the game draws again over the same tiles (the dig selection, highlights) were cut up
  differently or not at all, and the floor showed through them in triangles. So terrain is never smoothed now.
- How (PN triangles): each corner goes back into the camera's space with the engine's projection (as the ray tracer
  reads it: 0x760AE8 focal, 0x760AFC/0x760B00 middle; WidescreenCamera's focal and the frame's middle when there is
  none): z = 1 / rhw, x and y undone from screen = middle + focal * view / z. Corners at the same screen place (x, y
  and rhw, bit for bit) are one corner, so texture seams do not split a model. A corner's normal is the size-weighted
  average of its faces; it is smooth when every face is within 60 degrees of it.
- An edge curves only when it has exactly two triangles in the draw, running opposite ways, both corners are smooth,
  and its curve's middle moves at least 0.35 pixel (and no more than half the edge's length). Its curve depends on its
  two corners and their normals alone, and its points are worked out once and shared by both triangles, so they meet
  exactly. Every other edge (a crease, a draw's border, an outline drawn by another draw) stays straight and uncut, so
  no cracks open between draws. Flat surfaces have normals square to their edges, so nothing on them curves.
- A triangle with three curved edges is cut into a grid of segments² triangles on its curved patch; one with one or
  two is a fan from the patch's middle through every point of its edges; one with none stays as it was. Colors and
  texture coordinates are mixed by the patch's weights (as the card would across the flat triangle); depth comes from
  the triangle's own z = a + b * rhw.
- It runs on every triangle before back faces are culled (the layer culls on the CPU), so an outline's far side is
  known and outlines round off too; the result is culled after. The ray tracer still gets the game's own triangles.
- More than 65535 vertices in one draw: that draw is left as it was.
- Watch for: thin dark lines or gaps along a model's edges, shapes that swell where they should not (a box's sides),
  the cost on the CPU in busy scenes (log at debug: the first smoothed draw's triangles before and after). First test
  (4K, Vulkan): 60 fps, the first smoothed draw 50 triangles into 190.

## Not done
- Gamma (ignored, as by patch.dll), flat shading (drawn smooth).
- The frame is read back whenever the game blits its 2D panels onto it, which
  costs a GPU wait each frame. If that is slow, the next step is doing those
  blits on the GPU. At 3840x2160 that is 33 MB read back and 33 MB uploaded
  per frame, with the CPU waiting for the GPU in between; about 42 fps there
  without anti-aliasing (limit 60) [verified, our logs].
- Done so far: a fill or Direct3D clear of the whole frame is done on the card
  (Backend::ClearFrame) instead of reading the frame back, filling it in memory
  and uploading it again [verified: 1 per frame in a level, 35 to 56 fps at 4K].
- Measured in a level at 3840x2160 (26 min): every read back came from a blit
  onto the frame, none from a lock, about 8 ms each, plus 3 ms to upload the
  frame again [verified]. So blits onto the frame are now drawn on the card
  (BlitOnCard) while the card has the newest frame: the source surface's memory
  picture becomes a texture (made again only when it changes or its color key
  does; keyed pixels get alpha 0 and the alpha test drops them) and is drawn as
  two triangles with point sampling, no blending and no depth. Falls back to the
  blit in memory for the screen surface, a source read outside its edges, a
  source that is the frame itself, or while a video plays [unverified].
- Tested with gpu_blits on: DirectX 11 fine; DirectX 12 "device removed" and
  Vulkan "device lost" on the first loading screen frame [verified]. Cause: a
  texture dropped between a present and the next frame's first command went on
  the slot about to start, whose list is freed when it starts, so it was freed
  while the frame just sent still used it. The loading screen makes and drops a
  picture every frame, which with gpu_blits is a texture each time. Fixed by
  starting the frame before queueing the texture [unverified]. gpu_blits: off
  in config.yml goes back to blits in memory.
- After the fix, gpu_blits on (4K, SMAA x4): Vulkan 59.6 fps on average over
  7 min, DirectX 12 58.5, no frame uploads left [verified]. One read back per
  frame remained (about 9 ms at 4K), caused by a blit that reads from the frame
  into another surface (nothing uploaded after it, so the frame was the
  source). Such a blit now reads back only the area it copies
  (Backend::ReadFrameArea) when that is less than half the frame; the memory
  copy stays marked out of date. The first 5 are logged with their size
  ("a blit read WxH at x,y out of the frame") [unverified].
- Tested (DirectX 12, 7.5 min, 4K): 59.6 fps, 2 of 42 stretches below 59 (level
  start, and a 939 ms freeze from saving two 4K screenshots) [verified]. The
  copies out of the frame were 79x38 at 423,240 (menu, 854x480) and 492x238 at
  1908,1003 (level, 4K), some minutes every frame, later none. Read back by
  area they still cost about 12 ms each on DirectX 12: the time is the wait for
  the card to finish, not the copying [verified].
- So a blit out of the frame into a whole surface (unscaled, unkeyed, the
  surface exactly the copied size) is now done on the card
  (Backend::CopyFrameToTexture into the surface's blit texture): the surface is
  then "card only". Blitted unkeyed onto the frame, it is drawn from there; any
  other use reads its texture back first (Backend::ReadTexture, counted as
  "copy" in the timing), as the frame is. Before the backend changes, such
  pictures are read into memory. Other copies out of the frame still read back
  their area [unverified].
- A flip with the frame only on the card: the old trick of swapping the two
  memory copies needs the frame in memory, so the card keeps a copy of the
  picture just shown (Backend::SaveShownFrame) and the screen surface reads it
  back only if the game uses it before the next flip (in a level it was used
  once, at the start). Those read backs count as "screen" in the frame timing
  [unverified].
- Frame timing in the log every 10 s ("Renderer: WxH, last 10 s: ..."): fps,
  slowest frame, frames over 25 ms, and per frame the read backs (time, and
  whether a lock, blit, clear or other caused them), uploads, clears on the
  card and the present's time. Use it to see what to move to the GPU next.
