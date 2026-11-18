#pragma once
#include <cstdint>
namespace KeeperForge2 {
    class HookManager {
        public:
            static bool Initialize();
            static void Shutdown();
            static bool CreateHook(std::uintptr_t target,void* detour,void** original);
            static bool EnableHook(std::uintptr_t target);
            static bool DisableHook(std::uintptr_t target);
            static bool RemoveHook(std::uintptr_t target);
            static bool IsInitialized();
    };
}
