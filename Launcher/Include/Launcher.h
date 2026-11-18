#pragma once
#include <expected>
#include <filesystem>
#include <string>
#include <Windows.h>
#include <cstdint>
namespace KeeperForge2 {
    class Launcher {
        public:
            // With steam_multiplayer on, stops the launcher with an error if Steam is not running.
            static void CheckSteamRunning();
            static bool FindGame(std::filesystem::path& gamePath);
            // Starts the game paused (its main thread is resumed by the caller).
            static bool LaunchGame(const std::filesystem::path& gamePath,PROCESS_INFORMATION& processInfo);
            // While the game is paused: puts dllPath first in the game's list of DLLs to load, so Windows loads it with
            // the game's own DLLs and starts it first. Only for a DLL in the game's folder. If it could not, gives the
            // reason for the log, with the game left as it was (then InjectDLL after resuming, as before).
            static std::expected<void, std::string> LoadRuntimeFirst(HANDLE process,const std::filesystem::path& gamePath,const std::filesystem::path& dllPath);
            static bool InjectDLL(HANDLE process,const std::filesystem::path& dllPath,uintptr_t& remoteModule);
            static bool InitializeRuntime(HANDLE process,uintptr_t remoteModule);
        private:static HANDLE jobHandle;
    };
}
