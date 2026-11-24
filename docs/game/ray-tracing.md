# Ray tracing (ray_tracing)

Ray traced lighting for the game's 3D view: shadows, ambient occlusion, reflections and bounce light, drawn on top of
what the game draws. `ray_tracing: on` or `off` in config.yml (off by default). Being built; this page is the plan and
what is known.

## What the renderer has to work with [verified, renderer.md]
- The game draws through our DirectDraw/Direct3D 6 layer. Its 3D triangles come already in screen space: each vertex
  has x and y in pixels, z (0 to 1) and rhw (1 / w), a colour (the game's own lighting is in it) and texture
  coordinates. There are no world positions, normals, lights or materials in the draws.
- Culling is done on the CPU, so every triangle drawn is one the camera sees (front faces only). Geometry the camera
  does not see (behind it, off screen, back faces) never reaches the renderer.
- DirectX 11 has no ray tracing. DirectX 12 has DXR (tier 1.1 adds inline ray queries); Vulkan has
  VK_KHR_acceleration_structure with VK_KHR_ray_query.

## The 32-bit question [unverified: the first step checks it]
- The game is a 32-bit program, and KeeperForge2 runs inside it. Ray tracing drivers are made and tested for 64-bit
  programs; whether a card's driver offers DXR or Vulkan ray queries to a 32-bit process is not known here, and may
  differ between makers.
- First step (written): with `ray_tracing: on`, each backend asks its driver and logs the answer once at start:
  - Vulkan: the device's Vulkan version and whether it lists VK_KHR_acceleration_structure, VK_KHR_ray_query,
    VK_KHR_ray_tracing_pipeline, VK_KHR_deferred_host_operations, VK_KHR_buffer_device_address, VK_KHR_spirv_1_4 and
    VK_EXT_descriptor_indexing. Usable when the first, second and fourth are there.
  - DirectX 12: the adapter's name, the DXR tier from CheckFeatureSupport (OPTIONS5) and whether ID3D12Device5 is there.
    Usable at tier 1.1.
  - DirectX 11: says it has none.
- Result, Vulkan [verified, 2026-11-19]: on an NVIDIA RTX A4000 (a card with ray tracing), Vulkan 1.4 in this 32-bit
  process lists 256 device extensions, among them VK_KHR_buffer_device_address, VK_KHR_spirv_1_4 and
  VK_EXT_descriptor_indexing, but none of VK_KHR_acceleration_structure, VK_KHR_ray_query,
  VK_KHR_ray_tracing_pipeline or VK_KHR_deferred_host_operations. So NVIDIA's driver offers no hardware ray tracing
  to 32-bit Vulkan programs. DirectX 12 not checked yet.
- If neither backend offers it to a 32-bit process, ray tracing would need a separate 64-bit process to do the GPU work
  (sharing the frame with the game's), which is a much larger design. The log decides which way this goes.

## The plan (if the driver offers it)
1. Geometry: every 3D triangle of a frame is kept (screen x, y, rhw), turned back into camera space with the game's
   projection (the focal length Widescreen sets, the screen's middle), and built into an acceleration structure each
   frame (a few tens of thousands of triangles). Its texture and colour go with it, for bounce light and reflections.
2. Per pixel: position from the depth buffer the same way, normal from the triangle it belongs to.
3. Effects, each its own setting later: ambient occlusion (needs no lights), shadows (needs the game's lights: their
   places and colours, to be found in the game's memory), reflections (water, lava and shiny floors, picked by
   texture), one bounce of diffuse light.
4. Our own denoiser (no third-party code): reuse of the last frames' results where the picture has not changed, then
   an edge-aware blur.
5. Shaders: Vulkan GLSL with ray queries compiled to SPIR-V like the others (Runtime/Shaders), and HLSL for DXR
   compiled to DXIL ahead of time and embedded, as the d3dcompiler_47 the DirectX backends use now cannot make DXIL.

## Known limits
- Only what the camera sees can cast shadows or show in reflections (off-screen walls and the ceiling are not drawn by
  the game). Rays that leave the visible geometry use a fallback (no shadow, the sky colour).
- The game's own lighting stays in the vertex colours; ray traced light is added on top or darkens it, not replacing it.

## Decision (owner, 2026-11-19): our own tracer in compute shaders
No hardware ray tracing reaches the 32-bit game (Vulkan checked above), so KeeperForge2 traces rays itself in ordinary
compute shaders, which 32-bit programs do get: our own bounding volume tree, ray tests and denoiser, no third-party
code. It does not use the cards' ray tracing cores, so the effects run at half the frame's size and are smoothed. It
works on any card with compute shaders, NVIDIA or AMD.

## Stage 1: ambient occlusion on Vulkan [written; not built or tested]
- Scene (Runtime/Source/Renderer/RayScene.cpp): every draw that is depth tested and written and not blended (the
  world, not effects or the 2D panels) adds its triangles. Each corner goes back into camera space: w = 1/rhw,
  x = (sx - width/2) * w / focal, y = (sy - height/2) * w / focal, z = w; the focal length is WidescreenCamera's (320 *
  height / 480 * 100 / field_of_view; the game's own 320 without it). The units are the game's own up to one scale, so
  distances are measured against the scene's typical edge (the median of the triangles' longest edges, about a tile).
- Tree: a bounding volume tree by the binned surface area heuristic (12 bins, leaves of 2 to 16 triangles), built on
  the CPU every scene; nodes of 32 bytes (two vec4), triangles of 48 (a corner and two edges, three vec4).
- When: at the game's EndScene, if the scene has at least 64 triangles; a frame shown with its scene never ended drops
  it (logged once). The trace's result is laid over the frame there, before the game draws its 2D panels.
- Vulkan (BackendVulkan.cpp): the tree and triangles go into the frame's upload ring (now also a storage buffer); two
  compute passes, ray_ao.comp and ray_blur.comp (Runtime/Shaders, compiled into VulkanShaders.h by
  compile_vulkan_shaders.ps1), work at half the frame's size:
  - ray_ao: a ray from the camera through each pixel finds the surface it sees (the nearest hit in the tree); 6 rays
    over the half sphere above it (more straight out than sideways) look for anything within one typical edge, nearer
    hits closing it in more. A fixed noise pattern per pixel (no change from frame to frame, so nothing crawls).
  - ray_blur: a 7x7 blur that only mixes pixels at nearly the same depth (2% of the depth), so edges stay sharp.
  - The result (gray BGRA) is copied into an ordinary texture, which the layer draws over the whole frame with
    multiply blending (zero, source color) and linear sampling.
- Until compile_vulkan_shaders.ps1 runs, VulkanShaders.h has placeholders for the two compute shaders and the tracer
  stays off (CanRayTrace is false). DirectX 12 and 11: not yet.
- Log (debug): the first traces and then one in 1800: size, triangles, tree nodes, build time, focal and reach.

- Tested (2026-11-19, Vulkan, RTX A4000, 3840x2160): runs at 60 fps; 1,600 to 4,400 triangles a scene in a level,
  the tree built in 1.3 to 3.9 ms; reach (the typical edge) 0.46 to 1.0 in levels, 0.19 in the front end [verified,
  log]. One "could not trace a frame" at the move from the front end to a level (cause not known; the tracer's
  descriptor sets per frame were 16, now 64 with a retry).

## Stage 2: shadows from the level's lights [written; not built or tested]
- The engine's camera [verified in the export]: 0x575A00 sets it each time the world is drawn: position at 0x760AB8
  (3 floats, tiles), rotation at 0x760AC4 (9 floats, row major, view to world: the screen-to-world code 0x572920
  turns a view direction into a world one with 0x594E70, out[i] = sum of M[i][j] * v[j]), focal length 0x760AE8,
  viewport 0x760AEC-0x760AF8 and its middle 0x760AFC, 0x760B00. Its projection (0x575F10): screen = middle + focal *
  view.xy / view.z. The scene is now rebuilt with this focal length and middle (before: WidescreenCamera's and the
  frame's middle); world to camera is view = Mᵀ (world - position).
- The lights [verified in the export; Flame calls them CLight]: the bridge (world → session + 0x10, vtable 0x66CC94)
  keeps them in a list from + 0x33A, each one's next at + 0x13C (made by 0x4397E0, which links the new one first;
  0x43A1E0 sets it up). A light is a bridge thing: position + 0xD6 (x, y, z, 4096 per tile), color now + 0x111 (three
  16-bit 0-255; the one it was made with at + 0x11F), reach + 0x117 (4096 per tile, at most 0x2780), strength + 0x145
  (a byte, 128 by default), flags + 0x134 (not understood).
- Each scene, the lights that can reach what the camera sees (in front of it, near the picture) go to the tracer, the
  64 nearest at most (RayTracing.cpp). In the trace pass, for the point each pixel sees, every light in reach that
  faces it counts by color, strength, (1 - distance / reach)² and how squarely it falls; a ray to it (stopping a
  quarter tile short, as a torch sits inside its own model) says whether it arrives. The game has already lit the
  point with all of them, without shadows, so the point keeps (ambient + lights that arrive) / (ambient + all of them),
  ambient 0.35. This is multiplied with the ambient occlusion and blurred with it.
- Settings: ray_tracing_occlusion and ray_tracing_shadows (both on).
- To check in the log (debug): "middle ... (engine)" and the nearest light's place, which should be in front (z > 0)
  and on screen near a torch or room light. If the lights are not where they should be, the world axes or units
  differ from this reading (the engine's world taken as tiles with z up, positions / 4096).

- Tested (2026-11-19, 4K, Vulkan): the engine's camera is read (focal 2879.6 and middle 1920.5, 906.5 in a level at
  3840x2160: not WidescreenCamera's 1440 nor the screen's middle, so stage 1 had the scene twice as wide as it is);
  15 to 24 lights a scene, 10 to 14 tiles ahead, the nearest on screen where the Dungeon Heart is (its light cyan)
  [verified, log and screenshot]. Cost: 60 fps fell to 43-51 (traced at half the screen).

## Stage 3: quality setting and reflections [written; not built or tested]
- ray_tracing_quality: high, medium (the default) or low trace at a half, a third or a quarter of the screen's size;
  the blur and the linear stretch over the frame hide most of the difference. Lights too faint at a point to show a
  shadow there (share under 0.03) get no shadow ray.
- Water and lava are the draws with a bump mapping stage (BUMPENVMAP or BUMPENVMAPLUMINANCE): their triangles go
  into the scene as mirrors (RayTriangle corner.w = 1) even when blended. Ambient occlusion, shadow and reflection
  rays pass through mirrors; the camera ray stops at them. A mirror is not darkened; its reflection ray (up to 12
  typical edges) finds what it shows, and that point's color is read from the frame (the compute pass samples the
  frame, resolved first with MSAA): the scene holds only what the camera sees, so what a reflection meets is almost
  always on screen; a ray that leaves the picture shows nothing. How much shows: 0.6 * (0.3 + 0.7 * Schlick's
  Fresnel), fading toward the reach and the picture's edges. The result is a second texture, laid over the frame by
  its alpha after the lighting's multiply.
- ray_tracing_reflections: on or off.

## Stage 4: one bounce of light [written; not built or tested]
- From each point the camera sees (not water or lava), half as many rays as ambient occlusion's (at least 2), with
  their own pattern, go over the half sphere up to 5 typical edges (about tiles). Where one meets a surface, that
  point's color is read from the frame (as reflections do), counting less the farther it is; the average is the
  light bounced onto the point. It is blurred with the lighting (same depth-keeping weights), scaled by its strength
  (0.5) and laid over the frame as frame * (1 + bounce) (blend dest color, one), so each surface takes the bounced
  light in its own color. Order over the frame: lighting (multiply), bounce, reflections.
- ray_tracing_bounce: on or off. Two more buffers on the card (before and after smoothing) and a third texture.

## Stage 5: DirectX 12 and DirectX 11 [written; not built or tested]
- The same two passes in HLSL (Runtime/Source/Renderer/RayHlsl.h, cs_5_0, compiled at run time with d3dcompiler_47
  like the other DirectX shaders), kept in step with ray_ao.comp and ray_blur.comp; names HLSL keeps for itself
  (point, triangle, lit, noise) are avoided. Bindings: b0 the settings (80 bytes); t0-t2 the tree, triangles and lights
  (structured buffers of float4), t3 the frame, s0 linear clamped; u0 the trace's result and depth, u1 reflections, u2
  bounced light before smoothing, u3 the smoothed result, u4 the smoothed bounced light.
- Neither DirectX copies a buffer into a texture the way Vulkan does (DirectX 11 not at all, DirectX 12 only with rows
  of 256 bytes), so the three result textures are turned, the first time, into RGBA pictures the compute pass can
  write (typed UAV store of R8G8B8A8_UNORM is required at feature level 11), in the same handle and view; drawing them
  is unchanged. A result not wanted is written into a scratch picture.
- DirectX 12: a compute root signature (root CBV b0; one table of 4 SRVs and 5 UAVs; static sampler s0), the scene and
  settings in the frame's upload buffer (as structured buffer views), the views made in the frame's descriptor heap
  (9 per trace), the frame moved to non-pixel shader resource and the pictures to unordered access around the passes,
  a UAV barrier between them; replaced pictures are released with the frame (FrameSlot doomedResources).
- DirectX 11: dynamic structured buffers for the scene (grown when needed), a dynamic constant buffer, the frame's
  render target unbound while the frame is read, and every compute binding cleared after.

## Lighter on the computer [written; not built or tested]
- [user, 2026-11-23: it works the computer too hard] Where the work went: each pixel of the result sent a ray from the
  camera, 6 ambient occlusion rays, 3 bounce rays and one long shadow ray to every light in reach (15 to 24 a scene in
  a level, up to 64), and the tree was rebuilt on the CPU for every frame shown (1.3 to 3.9 ms each).
- ray_tracing_rate (5 to 240, 0 every frame; 30 by default): the scene is collected, built and traced at most that many
  times a second. A scene in between is not collected at all (no triangles kept, no tree, no trace); at its end the last
  result is laid over the frame again if it is under a quarter second old and the frame's size is the same. At 60 frames
  a second this halves the CPU's and the card's work; the lighting then follows a moving view a frame late.
- Shadows: every light counts as arriving, and only the 4 strongest at a pixel get a shadow ray (a blocked one is taken
  back out). The faint rest hardly showed a shadow; their rays were most of the tracer's work.
- The traversal stack is 32 entries instead of 64 (private memory on the card; the tree is far shallower).
- ray_ao.comp changed: VulkanShaders.h must be made again (compile_vulkan_shaders.ps1, needs the Vulkan SDK) before
  Vulkan uses it; RayHlsl.h (DirectX) is compiled when the game starts.
- Checkerboard: a trace does every other pixel of the result (settings.frame bit 0 picks the half, the halves
  taking turns), and the rest keep what the trace before wrote (the trace's buffers stay for the same size in every
  backend); the blur pass mixes the two. Every pixel is traced (bit 1) when the buffers are new, the frame's size changed
  or the last trace is over a quarter second old. About half the card's tracing work; edges of things moving across the
  view may trail by one trace.
- Rays by quality: high 6 ambient occlusion rays (3 bounce), medium 4 (2), low 3 (2).
- Not done (owner's choice, 2026-11-23): reading the game's depth buffer instead of the camera ray. The camera ray is 1
  of about 14 a pixel and the cheapest (nearest hit, front to back), so it would save some 10 to 15% of the card's work
  but needs every backend's depth readable by the trace pass (with MSAA its own shader), the game's depth turned back
  into distance, normals from neighbouring pixels, and water and lava found another way.
