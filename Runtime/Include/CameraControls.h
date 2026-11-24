#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Camera controls in a level: W, A, S and D move the view as the arrow keys do, while those actions keep their
	// default keys (the ally window moves from A to Y), and the mouse wheel zooms in and out as the zoom keys do. Both
	// act through the game's own key actions. The middle mouse button puts the view's zoom and rotation back as they
	// were at the level's start, over the same spot (the game's own smooth view move). Controlled by "wasd_camera",
	// "wheel_zoom" and "middle_click_camera_reset" in config.yml. See docs/game/camera-controls.md.
	class CameraControls {
		public:
			static void Install();
			// The wheel and the middle button when the mouse comes from Windows messages (WindowsMouse) instead of
			// DirectInput's data: a wheel turn (Windows' units, 120 a notch, positive away from you) and a middle press.
			static void OnMouseWheel(std::int32_t delta);
			static void OnMiddleButton();
	};
}
