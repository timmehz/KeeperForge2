#include <atomic>
#include <Windows.h>
#include "Runtime.h"
#include "Config.h"
#include "Logger.h"
#include "Renderer/Backend.h"
#include "HookManager.h"
#include "ModManager.h"
#include "ModApiHost.h"
#include "BackgroundRunning.h"
#include "Bans.h"
#include "CameraControls.h"
#include "ConsoleCommands.h"
#include "CoopCampaign.h"
#include "CpuThreads.h"
#include "CrashHandler.h"
#include "CreatureCountInput.h"
#include "CreatureFixes.h"
#include "SoundLog.h"
#include "ModData.h"
#include "ModCheck.h"
#include "CreatorName.h"
#include "CombatPitLog.h"
#include "FrameRate.h"
#include "GameFixes.h"
#include "GoodPlayer.h"
#include "GuiTextCache.h"
#include "LevelEditor.h"
#include "MemoryWatch.h"
#include "MercenaryPortal.h"
#include "ModerationCommands.h"
#include "Notices.h"
#include "MenuVersionText.h"
#include "PanelKeys.h"
#include "PerformanceStats.h"
#include "PlayerRanks.h"
#include "Renderer.h"
#include "Screenshots.h"
#include "SessionOptions.h"
#include "SkirmishSwapKeeper.h"
#include "SteamMultiplayer.h"
#include "Widescreen.h"

namespace {std::atomic_bool initialized{ false };}
namespace KeeperForge2 {
    bool Initialize() {
        bool expected = false;
        if (!initialized.compare_exchange_strong(expected, true)) return true;
        // The renderer chosen in config.yml goes into the log's name (dx11, dx12 or vulkan), so config.yml is read first
        // (the logger reads it again for its level). An unknown word, such as the old "original", is Vulkan.
        Config::Load();
        const std::string renderer = Render::ApiConfigName(Render::ApiFromConfigName(Config::GetString("renderer", "vulkan")).value_or(Render::DefaultApi));
        if (!Logger::Initialize("KeeperForge2", renderer)) {
            initialized.store(false);
            MessageBoxA(nullptr, "Logger Initialize failed!", "KeeperForge2", MB_OK);
            return false;
        }
        // The game runs fullscreen, where a message box hides behind it and the game never closes.
        Logger::SetErrorBox(false);
        Logger::Info("KeeperForge2 Runtime initialized.");
        CrashHandler::Install();
        if (!HookManager::Initialize()) { Logger::Error("HookManager initialization failed."); }
        Logger::Debug("HookManager initialized.");
        // Before the game reads its command line, where it adds its own data folders.
        ModData::Install();
        MemoryWatch::Install();
        CpuThreads::Install();
        // First: it has to be in place before the game sets up its display.
        Renderer::Install();
        Widescreen::Initialize();
        FrameRate::Initialize();
        BackgroundRunning::Initialize();
        Screenshots::Initialize();
        MenuVersionText::Install();
        SkirmishSwapKeeper::Install();
        CreatureCountInput::Install();
        // Before the features that read the session options.
        SessionOptions::Install();
        ModCheck::Install();
        GoodPlayer::Install();
        MercenaryPortal::Install();
        // Has a level start callback (the middle click camera reset), so before CoopCampaign.
        CameraControls::Install();
        // Mods' level start events (ModApi.h), before CoopCampaign's.
        ModApiHost::Install();
        // After the features that run at level start: its callback has to be the last.
        CoopCampaign::Install();
        LevelEditor::Install();
        GuiTextCache::Install();
        CreatureFixes::Install();
        GameFixes::Install();
        SoundLog::Install();
        CombatPitLog::Install();
        PanelKeys::Install();
        // Before Steam starts, so no rank message is missed.
        PlayerRanks::Install();
        Notices::Install();
        ModerationCommands::Install();
        Bans::Install();
        SteamMultiplayer::Install();
        CreatorName::Install();
        PerformanceStats::Install();
        ConsoleCommands::Install();
        if (!ModManager::Initialize()) { Logger::Error("ModManager initialization failed."); }
        return true;
    }
    void Shutdown() {
        bool expected = true;
        if (!initialized.compare_exchange_strong(expected, false)) return;
        Logger::Info("KeeperForge2 Runtime shutting down.");
        ModManager::Shutdown();
        HookManager::Shutdown();
        Logger::Debug("HookManager shutdown.");
        Logger::Shutdown();
    }
    bool IsInitialized(){return initialized.load();}
}
extern "C" BOOL KeeperForge2_Initialize(){return KeeperForge2::Initialize() ? TRUE : FALSE;}
extern "C" void KeeperForge2_Shutdown(){KeeperForge2::Shutdown();}
