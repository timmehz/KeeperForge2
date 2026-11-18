#pragma once
#include <cstddef>
namespace KeeperForge2 {
	// A setting a mod keeps in its config.yml: key, value written when it is missing, comment written above it
	// (lines separated by '\n'). Plain C strings, so a mod built on its own can hand them over.
	struct ModSetting {
		const char* key;
		const char* defaultValue;
		const char* comment;
	};

	// The values of a mod's config.yml, as the runtime read them.
	class IModConfig {
		public:
			// The value of key, or defaultValue if the file does not have it. Stays valid until the mod shuts down.
			virtual const char* Get(const char* key, const char* defaultValue) const = 0;
		protected:
			~IModConfig() = default;
	};

	class IMod {
		public:
			virtual ~IMod() = default;
			virtual const char* GetName() const = 0;
			// Written into the header of the mod's config.yml when the file has none.
			virtual const char* GetAuthor() const { return ""; }
			virtual const char* GetDescription() const { return ""; }
			// The settings the mod's config.yml holds. The runtime keeps the file in step with them: missing settings
			// are added with their default, settings no longer listed are removed, values already set are kept.
			// With none listed the file's values are left as they are.
			virtual const ModSetting* GetSettings(std::size_t& count) const {
				count = 0;
				return nullptr;
			}
			virtual bool Initialize(const IModConfig& config) = 0;
			virtual void Shutdown() = 0;
	};
}

// Mod DLLs live in KeeperForge2\mods\<any folder name>\<mod name>.dll, next to the mod's config.yml. Each exports
//   KEEPERFORGE2_MOD_EXPORT KeeperForge2::IMod* KeeperForge2_CreateMod() { return new MyMod(); }
// The runtime deletes the mod it returns when it shuts down. Mods must be built with the same compiler as
// KeeperForge2 (the interface is C++).
#define KEEPERFORGE2_MOD_EXPORT extern "C" __declspec(dllexport)
namespace KeeperForge2 {
	using CreateModFn = IMod* (*)();
	inline constexpr const char* CreateModExport = "KeeperForge2_CreateMod";
}
