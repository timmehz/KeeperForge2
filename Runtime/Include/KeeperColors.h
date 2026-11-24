#pragma once
namespace KeeperForge2 {
	// Keeper colors (docs/game/keeper-colors.md): each keeper's color can be changed, kept in config.yml
	// (keeper_color_red, _blue, _green, _yellow, _purple: six hex digits or game). The game keeps three shades per player in
	// its player color table; the color chosen becomes the bright one and the other two follow it the way the game's own
	// do. Claimed floors and wall tops are per-player meshes with their color in their textures: the textures only one
	// keeper's meshes use are recolored as the game paints them into its texture cache. The skirmish screen gets a color
	// box beside each row of its player list; clicking one opens a color picker (ColorPicker) for that row's keeper.
	class KeeperColors {
		public:
			// After SkirmishSwapKeeper and CoopCampaign, which add their buttons to the same skirmish screen.
			static void Install();
	};
}
