#pragma once
#include <cstdint>
namespace KeeperForge2 {
	// Multiplayer: every player must have the same KeeperForge2 and mods, or the game goes out of step. The host sends its
	// KeeperForge2 version, a hash of its KeeperForge2 DLL and a fingerprint of its mods (mod DLLs, and the editor data of
	// mods and Flame packs) in the lobby's creature list with the session options. A player who joins compares them with
	// its own: the log says what differs, and when the level starts the game's chat shows it (with the host's version).
	// See docs/api/mods.md.
	class ModCheck {
		public:
			// After SessionOptions (it uses its lobby callbacks).
			static void Install();
			// This machine's mods fingerprint (made the first time it is asked for).
			static std::uint32_t LocalFingerprint();
			// The in-game player interface was set up (called by GoodPlayer, which hooks that set-up): shows the notice.
			static void OnInterfaceReady(void* playerInterface);
	};
}
