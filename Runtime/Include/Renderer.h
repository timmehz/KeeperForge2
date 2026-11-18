#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Chooses what draws the game: the original Direct3D (through patch.dll), or DirectX 11, DirectX 12 or Vulkan
	// through our own DirectDraw (Runtime/Source/Renderer). Set by "renderer" in config.yml. See docs/game/renderer.md.
	class Renderer {
		public:
			// While Windows starts the game's DLLs, when KeeperForge2 is loaded among them (the launcher does this): every
			// DLL is in memory but none has started. Keeps DDrawCompat (the game folder's ddraw.dll) from starting when
			// one of our renderers is chosen (the launcher passes the choice in KEEPERFORGE2_RENDERER).
			static void BeforeDllsStart();
			// Before the game makes its DirectDraw (it does at its first screen setup).
			static void Install();
			// True when one of our renderers draws the game.
			static bool IsActive();
			// Videos: stretch the picture over the whole window instead of keeping its shape.
			static void SetStretch(bool stretch);
			// Videos: where the film is on the screen (in screen pixels) and the film's own shape (width / height).
			struct VideoArea {
				std::int32_t left;
				std::int32_t top;
				std::int32_t width;
				std::int32_t height;
				double shape;
			};
			// Asked each time the screen is shown; false while the film's place is not known yet.
			using VideoAreaFn = bool(*)(std::uint32_t screenWidth, std::uint32_t screenHeight, VideoArea& area);
			// While set, only the film's area of the screen is shown, scaled to the window (whole window when
			// stretching, else in the film's shape). nullptr when the video ends.
			static void SetVideoArea(VideoAreaFn area);
			// A point in the window's client area to the same point on the game's screen, whatever draws the game.
			static void WindowPointToScreen(void* window, std::int32_t screenWidth, std::int32_t screenHeight, std::int32_t& x, std::int32_t& y);
	};
}
