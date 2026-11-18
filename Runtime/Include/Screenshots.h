#pragma once
namespace KeeperForge2 {
	// The game saves screenshots as SCRSHOTS\SHOTnnnn.BMP. This makes it name them SHOTnnnn.PNG (so its search for
	// the next free number looks at PNG files) and re-encodes the file it wrote as a real PNG.
	class Screenshots {
		public:
			static void Initialize();
	};
}
