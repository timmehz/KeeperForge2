#pragma once
namespace KeeperForge2 {
	// Model detail (config.yml model_detail). The game's creatures and objects are progressive meshes: the engine draws
	// fewer of a model's triangles the farther and smaller it is on screen. With "full" every model is drawn whole at any
	// distance, which the game itself does in one play mode. Only the value the engine is handed when it starts is
	// changed; the game's own saved setting is not. "game" keeps the game's own way. See docs/game/renderer.md.
	//
	// It also tells the renderer which draws are models (for model_smoothing): the engine draws the world in batches,
	// and a batch that creatures' or objects' meshes added triangles to is a model batch. Terrain, effects and overlays
	// never are, so they are drawn exactly as the game gives them.
	class ModelDetail {
		public:
			// Before the engine starts (the game's entry).
			static void Install();
			// True while the engine draws a batch with creatures' or objects' triangles in it. Always false when the hooks
			// could not be made (or model_smoothing is off).
			static bool DrawingModels();
	};
}
