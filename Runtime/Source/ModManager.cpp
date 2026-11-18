#include "ModManager.h"
#include "ConfigFile.h"
#include "CreatureRegistry.h"
#include "ModApiHost.h"
#include "ModContent.h"
#include "Logger.h"
#include "Paths.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace fs = std::filesystem;
namespace { bool initialized = false; }
namespace KeeperForge2 {
	namespace {
		// A mod's config.yml, handed to the mod as its IModConfig.
		class ModConfig final : public IModConfig {
			public:
				ConfigFile file;
				const char* Get(const char* key, const char* defaultValue) const override {
					if (key == nullptr) { return defaultValue; }
					const auto it = values.find(key);
					return it != values.end() ? it->second.c_str() : defaultValue;
				}
				// The values are copied out once after loading, so the pointers Get returns stay valid.
				void Snapshot() { values = file.Values(); }
			private:
				std::unordered_map<std::string, std::string> values;
		};
		std::string Join(const std::vector<std::string>& parts) {
			return parts | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
		}
	}

	struct ModManager::LoadedMod {
		// Destroyed in reverse: the mod, then its api (its hooks, which point into its DLL, are taken back), its config, then
		// its DLL.
		HMODULE module = nullptr;
		std::string dllName;
		ModConfig config;
		IModApi* api = nullptr;
		std::unique_ptr<IMod> mod;
		~LoadedMod() {
			mod.reset();
			if (api != nullptr) { ModApiHost::Release(*api); }
			if (module != nullptr) { FreeLibrary(module); }
		}
	};
	std::vector<std::unique_ptr<ModManager::LoadedMod>> ModManager::mods;

	bool ModManager::Initialize() {
		if (initialized) return true;
		initialized = true;
		Logger::Debug("ModManager initialized.");
		LoadModFolders();
		return true;
	}
	void ModManager::Shutdown() {
		if (!initialized) return;
		for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
			if (*it && (*it)->mod) (*it)->mod->Shutdown();
		}
		while (!mods.empty()) mods.pop_back();
		initialized = false;
		Logger::Debug("ModManager shutdown.");
	}
	bool ModManager::IsInitialized() { return initialized; }
	std::size_t ModManager::GetModCount() { return mods.size(); }

	bool ModManager::Start(std::unique_ptr<LoadedMod> loaded) {
		IMod& mod = *loaded->mod;
		// The runtime's log, hooks, events and game state, before Initialize so the mod can use them there.
		if (const auto useApi = reinterpret_cast<UseApiFn>(GetProcAddress(loaded->module, UseApiExport))) {
			loaded->api = &ModApiHost::Create(mod.GetName());
			useApi(*loaded->api);
		}
		if (!mod.Initialize(loaded->config)) { Logger::Error(std::string("Failed to initialize mod: ") + mod.GetName()); }
		Logger::Info(std::string("Registered mod: ") + mod.GetName());
		// Content (creature types...), in load order so every machine gives the same ids.
		if (const auto registerContent = reinterpret_cast<RegisterContentFn>(GetProcAddress(loaded->module, RegisterContentExport))) {
			CreatureRegistry::SetCurrentMod(mod.GetName());
			registerContent(CreatureRegistry::Content());
		}
		mods.push_back(std::move(loaded));
		return true;
	}

	// Each folder in KeeperForge2\mods is one mod: the DLLs in it that export KeeperForge2_CreateMod are loaded (other
	// DLLs there are the mod's own libraries), and its config.yml is brought up to date and handed to it. The folder's
	// name is only for the player; a mod is known by its DLL's name.
	void ModManager::LoadModFolders() {
		std::error_code error;
		const fs::path root = fs::absolute(Paths::Mods(), error);
		if (!fs::is_directory(root, error)) { return; }
		std::vector<fs::path> folders;
		for (const auto& entry : fs::directory_iterator(root, error)) {
			if (entry.is_directory(error)) { folders.push_back(entry.path()); }
		}
		// The same order on every machine.
		std::sort(folders.begin(), folders.end());
		for (const auto& folder : folders) {
			std::vector<fs::path> dlls;
			for (const auto& entry : fs::directory_iterator(folder, error)) {
				if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".dll") == 0) { dlls.push_back(entry.path()); }
			}
			std::sort(dlls.begin(), dlls.end());
			for (const auto& dll : dlls) {
				const std::string dllName = dll.stem().string();
				const bool duplicate = std::ranges::any_of(mods, [&](const auto& loaded) { return _stricmp(loaded->dllName.c_str(), dllName.c_str()) == 0; });
				// The mod's own libraries next to it are found from its folder.
				const HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
				if (module == nullptr) {
					Logger::Warning("Mods: could not load " + dll.string() + " (error " + std::to_string(GetLastError()) + ").");
					continue;
				}
				const auto create = reinterpret_cast<CreateModFn>(GetProcAddress(module, CreateModExport));
				if (create == nullptr) {
					// A library of the mod rather than the mod itself; it stays loaded for the mod that uses it.
					Logger::Debug("Mods: " + dll.string() + " has no " + CreateModExport + ", not a mod.");
					continue;
				}
				if (duplicate) {
					FreeLibrary(module);
					Logger::Warning("Mods: " + dll.string() + " was skipped, a mod named " + dllName + " is already loaded.");
					continue;
				}
				auto loaded = std::make_unique<LoadedMod>();
				loaded->module = module;
				loaded->dllName = dllName;
				loaded->mod.reset(create());
				if (!loaded->mod) {
					Logger::Warning("Mods: " + dll.string() + " did not create its mod.");
					continue;
				}
				// The config.yml next to it, kept in step with the settings the mod lists.
				std::size_t count = 0;
				const ModSetting* listed = loaded->mod->GetSettings(count);
				std::vector<ConfigSetting> settings;
				for (std::size_t i = 0; listed != nullptr && i < count; ++i) {
					if (listed[i].key == nullptr) { continue; }
					settings.push_back({ listed[i].key, listed[i].defaultValue ? listed[i].defaultValue : "", listed[i].comment ? listed[i].comment : "" });
				}
				const fs::path configPath = folder / "config.yml";
				const auto header = ConfigFile::MakeHeader(loaded->mod->GetAuthor() ? loaded->mod->GetAuthor() : "", loaded->mod->GetDescription() ? loaded->mod->GetDescription() : "");
				if (!loaded->config.file.Load(configPath, settings, header)) { Logger::Warning("Mods: could not read or write " + configPath.string() + ", the mod gets its default settings."); }
				loaded->config.Snapshot();
				const auto& fileHeader = loaded->config.file.Header();
				std::string about = "Mods: loaded " + dllName + " (" + loaded->mod->GetName() + ") from " + folder.filename().string();
				if (fileHeader.present && !fileHeader.authors.empty()) { about += ", by " + Join(fileHeader.authors); }
				Logger::Info(about + ".");
				Start(std::move(loaded));
			}
		}
	}
}
