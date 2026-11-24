add_library(KeeperForge2 SHARED)
target_sources(KeeperForge2 PRIVATE
	Source/Autosave.cpp
	Source/BackgroundRunning.cpp
	Source/Bans.cpp
	Source/CameraControls.cpp
	Source/ConsoleCommands.cpp
	Source/CpuThreads.cpp
	Source/CustomCampaign.cpp
	Source/ColorPicker.cpp
	Source/KeeperColors.cpp
	Source/PanelKeys.cpp
	Source/DllMain.cpp
	Source/EditorDialogs.cpp
	Source/EditorPanel.cpp
	Source/Memory.cpp
	Source/PatternScanner.cpp
	Source/PerformanceStats.cpp
	Source/PlayerRanks.cpp
	Source/Runtime.cpp
	Source/RuntimeStartup.cpp
	Source/HookManager.cpp
	Source/InstructionDecoder.cpp
	Source/MercenaryPortal.cpp
	Source/ModApiHost.cpp
	Source/ModCheck.cpp
	Source/ModData.cpp
	Source/ModManager.cpp
	Source/ModerationCommands.cpp
	Source/Notices.cpp
	Source/CombatPitLog.cpp
	Source/CoopCampaign.cpp
	Source/CoopHands.cpp
	Source/CrashHandler.cpp
	Source/CreatureCountInput.cpp
	Source/CreatorName.cpp
	Source/CreatureFixes.cpp
	Source/CreatureRegistry.cpp
	Source/FrameRate.cpp
	Source/FrontEndTexts.cpp
	Source/GameFixes.cpp
	Source/GoodPlayer.cpp
	Source/PossessionActions.cpp
	Source/GuiTextCache.cpp
	Source/LevelEditor.cpp
	Source/LevelFiles.cpp
	Source/MemoryWatch.cpp
	Source/MenuVersionText.cpp
	Source/ModelDetail.cpp
	Source/Renderer.cpp
	Source/Renderer/Backend.cpp
	Source/Renderer/BackendD3D11.cpp
	Source/Renderer/BackendD3D12.cpp
	Source/Renderer/BackendVulkan.cpp
	Source/Renderer/DirectDrawLayer.cpp
	Source/Renderer/ModelSmoothing.cpp
	Source/Renderer/RayScene.cpp
	Source/RayTracing.cpp
	Source/Screenshots.cpp
	Source/SealedFile.cpp
	Source/SessionOptions.cpp
	Source/SkirmishSwapKeeper.cpp
	Source/SoundLog.cpp
	Source/SteamMultiplayer.cpp
	Source/Widescreen.cpp
	Source/WidescreenCamera.cpp
	Source/WidescreenFonts.cpp
	Source/WidescreenGui.cpp
	Source/WidescreenLoadingScreen.cpp
	Source/WindowsMouse.cpp
)
 target_link_libraries(KeeperForge2 PRIVATE
	KFShared
	comctl32
	Psapi
	dbghelp
	ole32
	windowscodecs
	winmm
	bcrypt
	# GPU load for the console's gpu command (Windows' performance counters).
	pdh
	# Sealing the saved ranks (ranks.dat) with Windows' data protection.
	crypt32
	# The steamid64 command reads Steam's public profile pages.
	winhttp
	# Renderers. DirectX 12, the shader compiler and Vulkan are loaded at run time, so the game still starts
	# where they are missing.
	d3d11
	dxgi
	dxguid
 )
# Steamworks SDK (Steam multiplayer). It is not part of the shared project (Valve's license: ThirdParty/ is in
# .gitignore); without it the runtime is built with Steam multiplayer off. steam_api.dll is delay-loaded, so the game
# still starts without it: SteamMultiplayer.cpp loads it from the KeeperForge2 folder or the game folder first.
set(KEEPERFORGE2_STEAMWORKS_SDK "${CMAKE_SOURCE_DIR}/ThirdParty/steamworks_sdk_165/sdk" CACHE PATH "Steamworks SDK folder (the one holding public/ and redistributable_bin/)")
if(EXISTS "${KEEPERFORGE2_STEAMWORKS_SDK}/public/steam/steam_api.h" AND EXISTS "${KEEPERFORGE2_STEAMWORKS_SDK}/redistributable_bin/steam_api.lib")
	target_include_directories(KeeperForge2 PRIVATE "${KEEPERFORGE2_STEAMWORKS_SDK}/public")
	# 32-bit: steam_api.lib straight in redistributable_bin (win64/ is the 64-bit one).
	target_link_libraries(KeeperForge2 PRIVATE "${KEEPERFORGE2_STEAMWORKS_SDK}/redistributable_bin/steam_api.lib" delayimp)
	target_link_options(KeeperForge2 PRIVATE "/DELAYLOAD:steam_api.dll")
	target_compile_definitions(KeeperForge2 PRIVATE KEEPERFORGE2_STEAMWORKS=1)
	message(STATUS "KeeperForge2: Steamworks SDK found, Steam multiplayer is built in.")
else()
	target_compile_definitions(KeeperForge2 PRIVATE KEEPERFORGE2_STEAMWORKS=0)
	message(WARNING "KeeperForge2: no Steamworks SDK at ${KEEPERFORGE2_STEAMWORKS_SDK}; building without Steam multiplayer.")
endif()
# The version shown in the main menu comes from project(... VERSION) in the root CMakeLists.txt.
target_compile_definitions(KeeperForge2 PRIVATE KEEPERFORGE2_VERSION="${PROJECT_VERSION}")
target_include_directories(KeeperForge2 PUBLIC
	Include
	../Shared/Include
)
