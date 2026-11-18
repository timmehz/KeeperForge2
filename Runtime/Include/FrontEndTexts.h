#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace KeeperForge2 {
	// Front end text buttons draw images the game makes once per window from a list of texts (0x5329A0). This adds
	// our own texts to the end of a window's list each time the game makes it, so several features can share the
	// one hook on it.
	class FrontEndTexts {
		public:
			struct Text {
				std::int32_t buttonId;
				std::wstring text;
			};
			// Hooks the text list maker. Call before the features that add texts.
			static bool Install();
			// Each time the game makes this window's texts, ours follow the game's own. *firstIndex is set to the index
			// of the first one (the rest follow in order), or -1 when the list has no room for them or the hook is
			// missing. A button shows text n when its pre-rendered text index (config +0x44, live +0x34) is n.
			static void Add(std::int32_t window, std::vector<Text> texts, std::int32_t* firstIndex);
	};
}
