#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Lets the game draw at a chosen frame rate. The simulation is not affected: the game only runs a game turn when
	// its own turn timer says one is due, whatever the frame rate. The GOG patch.dll waits for a vertical blank on
	// every frame on top of the display's own vsync, which can halve the frame rate; with a frame_rate set, that wait
	// is turned off and frames are paced here instead.
	// Controlled by "frame_rate" in config.yml: a number of frames per second, 0 for no limit, or vsync for the
	// game's original behavior.
	class FrameRate {
		public:
			static void Initialize();
			// Whether the per-frame present hook is in place (it is even with frame_rate: vsync).
			static bool IsPresentHooked();
			// Called every frame right before it is shown, when everything for it has been drawn.
			static void SetBeforePresent(void (*callback)());
			// Called every frame right after it was shown, on the game's thread, outside the world's tick (any number of
			// callbacks, in the order added). Only while the present hook is in place.
			static void AddAfterPresent(void (*callback)());
			// Frames shown since the game started (wraps around); counted only while the present hook is in place.
			static std::uint32_t FrameCount();
			// The shortest and longest time from one frame shown to the next (microseconds) since the last call, and
			// starts again. False when no frame was timed. Frames of a second or more (loading, a pause) are not
			// counted.
			static bool TakeFrameTimes(std::uint32_t& shortestUs, std::uint32_t& longestUs);
	};
}
