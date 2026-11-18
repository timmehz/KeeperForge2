#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace KeeperForge2 {
	// Messages from Admins and the Creator to every KeeperForge2 player on Steam (the console's notify command): to
	// their own Steam lobby at once and to every other lobby (public, friends only, invited) within about 30 s
	// (SteamMultiplayer::PostToEveryone). Every machine checks itself, with its own ranks (PlayerRanks), that the sender
	// may send them. Shown once in the multiplayer lobby's chat or the in-game chat, whichever is on screen first, as
	// "SYSTEM: <message>" in white or "ALERT: <message>" in red (CreatorName draws them), and printed in the console.
	// See docs/api/console-commands.md.
	class Notices {
		public:
			// Registers for the messages over Steam. Before SteamMultiplayer::Install.
			static void Install();
			// The console's command: notify <system|alert> <message>. Returns the text to print.
			static std::string NotifyCommand(std::string_view type, std::string_view message);
			// A notice shown only on this machine (like one received; nothing is sent).
			static void ShowLocal(bool alert, std::string_view message);
			struct Notice {
				bool alert = false;
				// "SYSTEM" or "ALERT".
				std::wstring label;
				std::wstring text;
			};
			// The oldest notice waiting to be shown; Pop once it is shown. Game's thread.
			static std::optional<Notice> Next();
			static void Pop();
			// Whether a chat line's text ("<label>: <text>") is a notice that was shown: nullopt if not, else whether it
			// is an alert.
			static std::optional<bool> ShownLine(const wchar_t* line);
	};
}
