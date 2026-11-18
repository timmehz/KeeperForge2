#include "Paths.h"
#include <system_error>
namespace fs = std::filesystem;
namespace KeeperForge2::Paths {
	fs::path Root() { return "KeeperForge2"; }
	fs::path Config() { return Root() / "config.yml"; }
	fs::path Logs() { return Root() / "Logs"; }
	fs::path Screenshots() { return Root() / "SCRSHOTS"; }
	fs::path Mods() { return Root() / "mods"; }
	fs::path Maps() { return Root() / "maps"; }
	bool CreateFolders() {
		std::error_code error;
		bool ok = true;
		for (const auto& folder : { Root(), Logs(), Mods() }) {
			fs::create_directories(folder, error);
			ok = ok && fs::is_directory(folder, error);
		}
		return ok;
	}
}
