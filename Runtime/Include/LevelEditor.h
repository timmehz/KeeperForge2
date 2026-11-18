#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// The in-game level editor (docs/game/level-editor.md). Adds an Editor button to the Single Player menu. A level
	// started through it runs the game's own world and 3D view with the simulation held: nothing moves, and only camera
	// actions are carried out. Set in the game, not in config.yml.
	class LevelEditor {
		public:
			// Also needs CoopCampaign's hooks on the world tick, the level start and the level setup's play mode reads.
			static void Install();
			// The level being played is being edited. Single player only, so simulation code may use it.
			static bool InSession();
			// Each run of the game component, before anything is set up. playing: a level is about to start, not the 3D
			// menu. True if that level is to be edited, in which case other features leave its settings alone.
			static bool OnGameComponentStart(bool playing);
			// Around the world tick while in a session. The first returns the action list to give the tick in place of
			// the one it got.
			static const std::uint8_t* BeforeWorldTick(std::uint8_t* world, const std::uint8_t* actions);
			static void AfterWorldTick(std::uint8_t* world);
			// Testing the level being edited: it plays for real, and Ctrl+T or the palette goes back to editing it.
			static bool Testing();
			// After each world tick while testing.
			static void AfterTestTick(std::uint8_t* world);
			// After each world tick of a level played for real, neither edited nor tested: puts the editor's mercenary marks
			// on the level's creatures, which the game's level load leaves out. Deterministic (the level's own files).
			static void AfterPlayTick();
			// A side panel tab being pressed, before the game's own handling. True if the editor took it.
			static bool OnTabClick(std::int32_t button, std::int32_t panel, std::uint8_t* playerInterface);
			// A key in a level, before the game's own handling. True if the editor took it.
			static bool OnKey(std::int32_t key, std::int32_t pressed, std::uint32_t modifiers, const std::uint8_t* playerInterface);
	};
}
