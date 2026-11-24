#pragma once
#include "Backend.h"
#include "Renderer.h"
#include <Windows.h>
#include <ddraw.h>

// Our own DirectDraw and Direct3D 6, which the game uses instead of patch.dll's when a new renderer is chosen. It
// keeps every surface in memory (so the game can lock, blit and read them as it always did) and hands the 3D work
// and the picture to show to a Backend. See docs/game/renderer.md.
namespace KeeperForge2::Render::Layer {
	// Checks that the API can run here (on a hidden window) and makes it the one the layer starts with. Returns false
	// if it cannot.
	bool Select(Api api);
	// The game's DirectDrawCreate and DirectDrawEnumerateA (its import table points at these).
	HRESULT WINAPI DirectDrawCreate(GUID* guid, LPDIRECTDRAW* directDraw, IUnknown* outer);
	HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, LPVOID context);
	// True once the game has made its DirectDraw through the layer.
	bool IsInUse();
	// The API drawing now (the selected one until the game's window is known).
	Api Current();
	// Changes the API while the game runs: the picture and every texture are kept and handed to the new one. Returns
	// false (keeping the old one) if the new one cannot start.
	bool Switch(Api api);
	// Videos: stretch the picture over the whole window instead of keeping its shape.
	void SetStretch(bool stretch);
	// Videos: only the film's area of the screen is shown while this is set (see Renderer::SetVideoArea).
	void SetVideoArea(Renderer::VideoAreaFn area);
	// A point in a window's client area to the same point on a picture of this size. Where the layer shows the picture
	// when it is its window, otherwise the picture is taken to fill the window.
	void WindowPointToPicture(HWND handle, std::int32_t pictureWidth, std::int32_t pictureHeight, std::int32_t& x, std::int32_t& y);
	// Wait for the vertical blank when showing a frame (frame_rate: vsync).
	void SetVsync(bool vsync);
	// window_mode: borderless puts the game's window over its whole monitor with no frame, and leaves it there when the game
	// loses focus; full screen (false) minimizes it then, as patch.dll does.
	void SetBorderless(bool borderless);
	// anti_aliasing: what the backends started from now on use (the hidden test window never gets any).
	void SetAntiAliasing(const AntiAliasing& value);
	// texture_filtering: the game's textures get mip levels (mipmaps), and linear sampling takes up to anisotropy samples
	// along a slanted view (1 = none). For the backends started from now on.
	void SetTextureFiltering(bool mipmaps, std::uint32_t anisotropy);
	// model_smoothing: the 3D world's round shapes made rounder, each curved edge cut into segments pieces (2 to 4; 0 off).
	// See ModelSmoothing.h.
	void SetModelSmoothing(std::uint32_t segments);
	// gpu_blits: the game's blits onto and out of the frame done by the graphics card, and its whole-frame clears (off:
	// both done in memory, the frame read back; "clears": only the clears on the card).
	void SetGpuBlits(bool blits, bool clears);
	// ray_tracing: the 3D scene traced and laid over the frame, with backends that can (docs/game/ray-tracing.md);
	// occlusion, shadows, reflections and bounce: its parts (ray_tracing_occlusion, ray_tracing_shadows,
	// ray_tracing_reflections, ray_tracing_bounce); divisor: the traced pictures are the frame's size divided by this
	// (ray_tracing_quality).
	// rate: traces a second at most (0: every frame); frames between show the last result.
	void SetRayTracing(bool on, bool occlusion, bool shadows, bool reflections, bool bounce, std::uint32_t divisor, std::uint32_t rate);
	// bump_mapping: 0 off, 1 du/dv, 2 du/dv with luminance. Before the game's display starts: with it on, the bump texture
	// formats are offered and bump surfaces can be made.
	void SetBumpMapping(int mode);
}
