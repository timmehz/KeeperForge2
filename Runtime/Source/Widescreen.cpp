#include "Widescreen.h"
#include "Config.h"
#include "DisplayUtils.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "Renderer.h"
#include "StringUtils.h"
#include "WidescreenCamera.h"
#include "WidescreenFonts.h"
#include "WidescreenGui.h"
#include "WidescreenLoadingScreen.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace Game = KeeperForge2::Game;

// The GUI tile stubs below are inline assembly, which can only reach file-scope names.
static std::uintptr_t guiTileWriteReturn = 0;
static std::uintptr_t guiTileReadReturn = 0;
static std::uintptr_t guiTileReadSecondReturn = 0;

// A GUI view stores one surface id per 128x32 tile in a fixed table of 140 slots, which overflows above
// roughly 1366x768. Every view written through our stub gets a table of the right size here instead.
static std::unordered_map<std::uintptr_t, std::vector<std::uint32_t>> guiTileTables;

static std::uint32_t ReadField(std::uintptr_t base, std::uintptr_t offset) { return *reinterpret_cast<const std::uint32_t*>(base + offset); }

static std::uint32_t* __cdecl GuiTileSlot(std::uintptr_t view, std::uint32_t index) {
	auto& tiles = guiTileTables[view];
	// Tiles are created in order, so index 0 means the view is being (re)built.
	if (index == 0) {
		const std::uint32_t count = ReadField(view, Game::Offset::GuiViewTilesWide) * ReadField(view, Game::Offset::GuiViewTilesHigh);
		tiles.assign(std::max<std::uint32_t>(count, 1), 0);
	}
	if (index >= tiles.size()) { tiles.resize(index + 1, 0); }
	return &tiles[index];
}

static std::uint32_t __cdecl GuiTileRead(std::uintptr_t view, std::uint32_t index) {
	const auto it = guiTileTables.find(view);
	if (it != guiTileTables.end()) { return index < it->second.size() ? it->second[index] : 0; }
	// Views built before the patch was installed still use the game's own table.
	if (index < Game::GuiViewInlineTileCount) { return ReadField(view, Game::Offset::GuiViewTiles + index * 4); }
	return 0;
}

// Replaces "lea ecx,[ebx+eax*4+4]; push ecx" (ebx = view, eax = tile index).
static __declspec(naked) void GuiTileWriteStub() {
	__asm {
		push eax
		push edx
		push eax
		push ebx
		call GuiTileSlot
		add esp, 8
		mov ecx, eax
		pop edx
		pop eax
		push ecx
		jmp dword ptr [guiTileWriteReturn]
	}
}
// Replaces "mov eax,[esi+eax*4+4]; mov edx,[ecx]" (esi = view, eax = tile index).
static __declspec(naked) void GuiTileReadStub() {
	__asm {
		push ecx
		push eax
		push esi
		call GuiTileRead
		add esp, 8
		pop ecx
		mov edx, dword ptr [ecx]
		jmp dword ptr [guiTileReadReturn]
	}
}
// Replaces "mov eax,[edi+edx*4+4]; mov edx,[esp+0x28]" (edi = view, edx = tile index).
static __declspec(naked) void GuiTileReadSecondStub() {
	__asm {
		push ecx
		push edx
		push edi
		call GuiTileRead
		add esp, 8
		pop ecx
		mov edx, dword ptr [esp + 0x28]
		jmp dword ptr [guiTileReadSecondReturn]
	}
}

namespace KeeperForge2 {
	namespace {
		bool enabled = false;
		bool widescreenMenus = true;
		std::uint32_t menuHeightSetting = 480;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		HANDLE engineHeap = nullptr;

		// thiscall with one stack argument; __fastcall with an unused EDX has the same layout.
		using EnterGameScreenFn = int(__fastcall*)(void* self, void* unused, void* argument);
		using EngineHeapAllocFn = void*(__cdecl*)(int size);
		using EngineHeapFreeFn = void(__cdecl*)(void* block);
		EnterGameScreenFn originalEnterGameScreen = nullptr;
		EngineHeapAllocFn originalEngineHeapAlloc = nullptr;
		EngineHeapFreeFn originalEngineHeapFree = nullptr;

		// -1 in a pattern matches any byte.
		using BytePattern = std::vector<std::int16_t>;
		struct PatchSite {
			const char* name;
			std::uintptr_t address;
			std::vector<BytePattern> accepted;
		};
		// The bytes each patch site may contain. Anything else means a different game build.
		const std::vector<PatchSite> patchSites{
			{ "EnterGameScreen", Game::Address::EnterGameScreen, { { 0x83, 0xEC, 0x08, 0xA1, 0x84, 0x9A, 0x75, 0x00 } } },
			{ "EngineHeapAlloc", Game::Address::EngineHeapAlloc, { { 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x53, 0x55, 0x56, 0x57 } } },
			{ "EngineHeapFree", Game::Address::EngineHeapFree, { { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x0F, 0x84 } } },
			{ "GuiViewTileWrite", Game::Address::GuiViewTileWrite, { { 0x8D, 0x4C, 0x83, 0x04, 0x51, 0x68, 0x01, 0x01, 0x00, 0x00 } } },
			// The GOG patch.dll replaces the first 5 bytes here with a jmp to its own copy of these instructions
			// (plus a bounds check for one view). Ours does the same work for any index, so it is safe to overwrite.
			{ "GuiViewTileRead", Game::Address::GuiViewTileRead, {
				{ 0x8B, 0x44, 0x86, 0x04, 0x8B, 0x11, 0x50, 0xFF, 0x52, 0x68 },
				{ 0xE9, -1, -1, -1, -1, 0x11, 0x50, 0xFF, 0x52, 0x68 } } },
			{ "GuiViewTileReadSecond", Game::Address::GuiViewTileReadSecond, { { 0x8B, 0x44, 0x97, 0x04, 0x8B, 0x54, 0x24, 0x28 } } },
			{ "SurfaceCachePageCount", Game::Address::SurfaceCachePageCount, { { 0x6A, 0x18, 0x68, 0x98, 0x2D, 0x79, 0x00 } } },
			{ "DisplayModeCallback", Game::Address::DisplayModeCallback, { { 0x8B, 0x44, 0x24, 0x08, 0x56, 0x57, 0x8D, 0x0C, 0x40 } } },
			{ "GuiBoxToScreen", Game::Address::GuiBoxToScreen, { { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x24, 0x57 } } },
			{ "GuiPointToScreen", Game::Address::GuiPointToScreen, { { 0x8B, 0x51, 0x04, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0xB8, 0x67, 0x66, 0x66, 0x66 } } },
			{ "GuiPointToLayout", Game::Address::GuiPointToLayout, { { 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x8B, 0x06, 0x8D, 0x04, 0x80 } } },
			{ "GuiMouseToLayout", Game::Address::GuiMouseToLayout, { { 0x8B, 0x81, 0xAC, 0x00, 0x00, 0x00, 0x56, 0x8B, 0xB1, 0xB0, 0x00, 0x00, 0x00 } } },
			{ "GuiCreateElements", Game::Address::GuiCreateElements, { { 0x6A, 0xFF, 0x68, 0x6B, 0xDA, 0x64, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 } } },
			{ "GuiWindowRect", Game::Address::GuiWindowRect, { { 0x83, 0xEC, 0x20, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x4F, 0x56 } } },
			{ "GuiWindowOrigin", Game::Address::GuiWindowOrigin, { { 0x83, 0xEC, 0x20, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x4E, 0x56 } } },
			{ "MinimapNorthSize", Game::Address::MinimapNorthSize, { { 0xB8, 0x08, 0x00, 0x00, 0x00, 0x51, 0x52 } } },
			{ "MinimapNorthNudgeY", Game::Address::MinimapNorthNudgeY, { { 0x83, 0xE9, 0x06 } } },
			{ "MinimapNorthNudgeX", Game::Address::MinimapNorthNudgeX, { { 0x83, 0xE8, 0x03 } } },
			{ "HandIconSize", Game::Address::HandIconSize, { { 0xB8, 0x20, 0x00, 0x00, 0x00, 0x03, 0xF1 } } },
			{ "HandMoreIconSize", Game::Address::HandMoreIconSize, { { 0xB8, 0x20, 0x00, 0x00, 0x00, 0x8B, 0x35 } } },
			{ "CursorIconSize", Game::Address::CursorIconSize, { { 0xB8, 0x20, 0x00, 0x00, 0x00, 0x33, 0xFF } } },
			{ "CursorIconSizeOther", Game::Address::CursorIconSizeOther, { { 0xB8, 0x20, 0x00, 0x00, 0x00, 0x8B, 0x4C, 0x24, 0x18 } } },
			{ "CursorIconSizeThird", Game::Address::CursorIconSizeThird, { { 0xB8, 0x20, 0x00, 0x00, 0x00, 0x89, 0x44, 0x24, 0x18 } } },
			{ "CursorIconTextOffsetX", Game::Address::CursorIconTextOffsetX, { { 0x8D, 0x7A, 0x1E } } },
			{ "CursorIconTextOffsetY", Game::Address::CursorIconTextOffsetY, { { 0x8D, 0x59, 0x0A } } },
			{ "DefineKeysModifierColumn", Game::Address::DefineKeysModifierColumn, { { 0x68, 0xC8, 0x00, 0x00, 0x00, 0x55, 0x8B, 0xCE } } },
			{ "DefineKeysKeyColumn", Game::Address::DefineKeysKeyColumn, { { 0x68, 0x2C, 0x01, 0x00, 0x00, 0x55, 0x8B, 0xCE } } },
			{ "BarLeftEndWidth", Game::Address::BarLeftEndWidth, { { 0xC7, 0x44, 0x24, 0x20, 0x10, 0x00, 0x00, 0x00 } } },
			{ "BarLeftEndHeight", Game::Address::BarLeftEndHeight, { { 0xC7, 0x44, 0x24, 0x24, 0x20, 0x00, 0x00, 0x00 } } },
			{ "BarMiddleStart", Game::Address::BarMiddleStart, { { 0x83, 0xC1, 0x10 } } },
			{ "BarMiddleShorter", Game::Address::BarMiddleShorter, { { 0x83, 0xE8, 0x20 } } },
			{ "BarMiddleHeight", Game::Address::BarMiddleHeight, { { 0xC7, 0x44, 0x24, 0x28, 0x20, 0x00, 0x00, 0x00 } } },
			{ "BarRightEndWidth", Game::Address::BarRightEndWidth, { { 0xC7, 0x44, 0x24, 0x1C, 0x10, 0x00, 0x00, 0x00 } } },
			{ "BarRightEndHeight", Game::Address::BarRightEndHeight, { { 0xC7, 0x44, 0x24, 0x20, 0x20, 0x00, 0x00, 0x00 } } },
			{ "BarRightEndStart", Game::Address::BarRightEndStart, { { 0x83, 0xC0, 0xF0 } } },
			{ "GuiHoverTest", Game::Address::GuiHoverTest, { { 0x83, 0xEC, 0x18, 0x8B, 0x81, 0xAC, 0x00, 0x00, 0x00, 0x53, 0x56 } } },
			{ "GuiClick", Game::Address::GuiClick, { { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x10, 0x53, 0x55 } } },
			{ "MenuScreenSize", Game::Address::MenuScreenHeightPush, { { 0x68, 0xE0, 0x01, 0x00, 0x00, 0x68, 0x80, 0x02, 0x00, 0x00 } } },
			{ "InGameFontSetup", Game::Address::InGameFontSetup, { { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x10, 0xB0, 0xFF } } },
			{ "InGameFontId", Game::Address::InGameFontId, { { 0x8B, 0x44, 0x24, 0x08, 0x3D, 0x00, 0x02, 0x00, 0x00 } } },
			{ "ShowLoadingScreen", Game::Address::ShowLoadingScreen, { { 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00, 0x56, 0x57, 0x8B, 0xF9 } } },
			{ "LoadingScreenBlitFront", Game::Address::LoadingScreenBlitFront, { { 0xE8, 0xD3, 0x46, 0x0A, 0x00 } } },
			{ "LoadingScreenBlitBack", Game::Address::LoadingScreenBlitBack, { { 0xE8, 0xB2, 0x46, 0x0A, 0x00 } } },
			{ "TitleScreenBlit", Game::Address::TitleScreenBlit, { { 0xE8, 0x42, 0x16, 0x08, 0x00 } } },
			{ "PlayMovie", Game::Address::PlayMovie, { { 0xA1, 0xF8, 0x6E, 0x75, 0x00, 0x83, 0xEC, 0x18, 0x53, 0x33, 0xDB } } },
			{ "SetScreenMode", Game::Address::SetScreenMode, { { 0x83, 0xEC, 0x2C, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57 } } },
			{ "PlayerCameraFocal", Game::Address::PlayerCameraFocal, { { 0xC7, 0x44, 0x24, 0x10, 0x00, 0x00, 0x14, 0x00 } } },
			{ "PathCameraFocal", Game::Address::PathCameraFocal, { { 0xC7, 0x44, 0x24, 0x0C, 0x00, 0x00, 0x14, 0x00 } } },
			{ "PlayerCameraDistanceWidth", Game::Address::PlayerCameraDistanceWidth, { { 0xA1, 0xEC, 0x6E, 0x75, 0x00, 0x89, 0x44, 0x24, 0x10 } } },
		};
		bool MatchesPattern(std::uintptr_t address, const BytePattern& pattern) {
			const auto* code = reinterpret_cast<const std::uint8_t*>(address);
			for (std::size_t i = 0; i < pattern.size(); ++i) {
				if (pattern[i] >= 0 && code[i] != static_cast<std::uint8_t>(pattern[i])) { return false; }
			}
			return true;
		}
		bool MatchesAnyPattern(const PatchSite& site) {
			const auto address = Game::Resolve(site.address);
			return std::ranges::any_of(site.accepted, [address](const BytePattern& pattern) { return MatchesPattern(address, pattern); });
		}

		struct GuiTileJump {
			std::uintptr_t virtualAddress;
			const void* stub;
			std::size_t size;
		};
		const GuiTileJump guiTileJumps[] = {
			{ Game::Address::GuiViewTileWrite, reinterpret_cast<const void*>(&GuiTileWriteStub), 5 },
			{ Game::Address::GuiViewTileRead, reinterpret_cast<const void*>(&GuiTileReadStub), 6 },
			{ Game::Address::GuiViewTileReadSecond, reinterpret_cast<const void*>(&GuiTileReadSecondStub), 8 },
		};
		bool JumpPointsTo(std::uintptr_t address, const void* destination) {
			const auto* code = reinterpret_cast<const std::uint8_t*>(address);
			const auto relative = *reinterpret_cast<const std::int32_t*>(code + 1);
			return code[0] == 0xE9 && address + 5 + relative == reinterpret_cast<std::uintptr_t>(destination);
		}
		// Puts our jumps back if something (such as patch.dll loading late) wrote over them.
		void EnsureGuiTileJumps() {
			for (const auto& jump : guiTileJumps) {
				const auto address = Game::Resolve(jump.virtualAddress);
				if (JumpPointsTo(address, jump.stub)) { continue; }
				Logger::Warning("Widescreen: GUI tile patch at " + StringUtils::ToHex(jump.virtualAddress) + " was overwritten, applying it again.");
				Memory::WriteJump(address, jump.stub, jump.size);
			}
		}

		template<typename T> T ReadGame(std::uintptr_t address) { return *reinterpret_cast<const T*>(address); }

		struct DisplayMode {
			std::uint32_t width;
			std::uint32_t height;
			std::uint32_t bitDepth;
		};
		std::string ToString(const DisplayMode& mode) { return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "x" + std::to_string(mode.bitDepth); }
		// Index of the graphics device the game will use, or -1 if the device table is not set up yet.
		std::int32_t GetSelectedDevice() {
			const auto deviceCount = ReadGame<std::int32_t>(Game::Resolve(Game::Address::DisplayDeviceCount));
			const auto device = ReadGame<std::int32_t>(Game::Resolve(Game::Address::GameObject) + Game::Offset::GameSelectedDevice);
			const auto devices = ReadGame<std::uintptr_t>(Game::Resolve(Game::Address::DisplayDevices));
			return devices && device >= 0 && device < deviceCount ? device : -1;
		}
		// The modes DirectDraw reported for the selected graphics device. Screen setup refuses anything not in this list.
		std::vector<DisplayMode> GetAvailableModes() {
			const auto device = GetSelectedDevice();
			if (device < 0) { return {}; }
			const auto devices = ReadGame<std::uintptr_t>(Game::Resolve(Game::Address::DisplayDevices));
			const auto entry = devices + static_cast<std::uintptr_t>(device) * Game::DisplayDeviceSize;
			const auto count = ReadGame<std::int32_t>(entry + Game::Offset::DeviceModeCount);
			const auto modes = ReadGame<std::uintptr_t>(entry + Game::Offset::DeviceModes);
			if (!modes || count <= 0) { return {}; }
			std::vector<DisplayMode> result;
			for (std::int32_t i = 0; i < count; ++i) {
				const auto mode = modes + static_cast<std::uintptr_t>(i) * Game::DisplayModeSize;
				result.push_back({ ReadGame<std::uint32_t>(mode), ReadGame<std::uint32_t>(mode + 4), ReadGame<std::uint32_t>(mode + 8) });
			}
			return result;
		}
		// The GOG patch.dll puts its own DirectDraw in front of the real one. It reports a fixed 4:3 mode list
		// (plus the registry size) no matter what the display supports, but its SetDisplayMode accepts any size.
		bool IsGogDirectDrawActive() { return GetModuleHandleW(L"patch.dll") != nullptr; }
		// Adds a mode to the device's list through the game's own mode-list callback, the same way DirectDraw would.
		void AddDisplayMode(std::int32_t device, std::uint32_t modeWidth, std::uint32_t modeHeight, std::uint32_t bitDepth) {
			using DisplayModeCallbackFn = long(__stdcall*)(void* description, void* context);
			// A DDSURFACEDESC: dwSize at 0x0, dwHeight at 0x8, dwWidth at 0xC, pixel format bit count at 0x54.
			std::uint8_t description[0x6C]{};
			const std::uint32_t size = sizeof(description);
			std::memcpy(description + 0x00, &size, 4);
			std::memcpy(description + 0x08, &modeHeight, 4);
			std::memcpy(description + 0x0C, &modeWidth, 4);
			std::memcpy(description + 0x54, &bitDepth, 4);
			const auto callback = reinterpret_cast<DisplayModeCallbackFn>(Game::Resolve(Game::Address::DisplayModeCallback));
			callback(description, reinterpret_cast<void*>(static_cast<std::intptr_t>(device)));
		}
		// Picks the requested size if the mode list has it at the game's bit depth (adding it when patch.dll is
		// the DirectDraw in use), otherwise the closest aspect ratio, then the largest. Returns false if nothing
		// usable is listed at that bit depth.
		bool PickDisplayMode(std::uint32_t& chosenWidth, std::uint32_t& chosenHeight) {
			const auto bitDepth = ReadGame<std::uint32_t>(Game::Resolve(Game::Address::VideoBitDepth));
			const auto modes = GetAvailableModes();
			static bool loggedModes = false;
			if (!loggedModes && !modes.empty()) {
				std::string list;
				for (const auto& mode : modes) { list += (list.empty() ? "" : ", ") + ToString(mode); }
				Logger::Debug("Widescreen: display modes in the game's list: " + list);
				loggedModes = true;
			}
			if (modes.empty()) {
				// No list to check against yet; let the game try the requested size.
				chosenWidth = width;
				chosenHeight = height;
				return true;
			}
			const bool listed = std::ranges::any_of(modes, [bitDepth](const DisplayMode& mode) { return mode.width == width && mode.height == height && mode.bitDepth == bitDepth; });
			if (!listed && IsGogDirectDrawActive()) {
				AddDisplayMode(GetSelectedDevice(), width, height, bitDepth);
				Logger::Debug("Widescreen: added " + std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(bitDepth) + " to the game's mode list.");
				chosenWidth = width;
				chosenHeight = height;
				return true;
			}
			const double wantedAspect = static_cast<double>(width) / height;
			const DisplayMode* best = nullptr;
			double bestAspectError = 0.0;
			for (const auto& mode : modes) {
				if (mode.bitDepth != bitDepth || mode.width == 0 || mode.height == 0) { continue; }
				if (mode.width == width && mode.height == height) {
					chosenWidth = width;
					chosenHeight = height;
					return true;
				}
				// Aspect ratios within 1% count as the same, so area decides between them.
				const double aspectError = std::round(std::abs(static_cast<double>(mode.width) / mode.height - wantedAspect) * 100.0);
				const bool better = !best || aspectError < bestAspectError
					|| (aspectError == bestAspectError && mode.width * mode.height > best->width * best->height);
				if (better) {
					best = &mode;
					bestAspectError = aspectError;
				}
			}
			if (!best) {
				Logger::Warning("Widescreen: the driver offers no modes at " + std::to_string(bitDepth) + " bpp, keeping the game's own resolution.");
				return false;
			}
			chosenWidth = best->width;
			chosenHeight = best->height;
			Logger::Warning("Widescreen: " + std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(bitDepth) + " is not offered by the driver, using " + ToString(*best) + " instead.");
			return true;
		}
		// The game only knows its own 4:3 list and drops anything else to 640x480, so put ours in right before it reads it.
		// Makes sure the mode list has this size at the game's bit depth, adding it when patch.dll is the DirectDraw.
		bool EnsureModeListed(std::uint32_t modeWidth, std::uint32_t modeHeight) {
			const auto bitDepth = ReadGame<std::uint32_t>(Game::Resolve(Game::Address::VideoBitDepth));
			const auto modes = GetAvailableModes();
			const bool listed = std::ranges::any_of(modes, [&](const DisplayMode& mode) { return mode.width == modeWidth && mode.height == modeHeight && mode.bitDepth == bitDepth; });
			if (listed) { return true; }
			if (!IsGogDirectDrawActive() || GetSelectedDevice() < 0) { return false; }
			AddDisplayMode(GetSelectedDevice(), modeWidth, modeHeight, bitDepth);
			return true;
		}
		// The main menu sets its own hardcoded 640x480 mode. Its fonts, images and videos are made for a 480 pixel
		// tall screen, so it runs at that height (widened to the game's shape) and the display scales it up. The game
		// screen is always set first (at startup), so by then we know the game's size.
		void ApplyMenuResolution(std::uint32_t gameWidth, std::uint32_t gameHeight) {
			if (!widescreenMenus) { return; }
			std::uint32_t menuWidth = gameWidth;
			std::uint32_t menuHeight = gameHeight;
			if (menuHeightSetting < gameHeight) {
				menuHeight = menuHeightSetting;
				// Round to an even width.
				menuWidth = (static_cast<std::uint32_t>(std::lround(static_cast<double>(menuHeight) * gameWidth / gameHeight)) + 1) & ~1u;
				if (!EnsureModeListed(menuWidth, menuHeight)) {
					menuWidth = gameWidth;
					menuHeight = gameHeight;
				}
			}
			Memory::Write<std::uint32_t>(Game::Resolve(Game::Address::MenuScreenHeightPush) + 1, menuHeight);
			Memory::Write<std::uint32_t>(Game::Resolve(Game::Address::MenuScreenWidthPush) + 1, menuWidth);
			static std::uint32_t loggedWidth = 0;
			if (loggedWidth != menuWidth) {
				Logger::Debug("Widescreen: main menu runs at " + std::to_string(menuWidth) + "x" + std::to_string(menuHeight) + ".");
				loggedWidth = menuWidth;
			}
		}
		int __fastcall EnterGameScreenDetour(void* self, void* unused, void* argument) {
			EnsureGuiTileJumps();
			std::uint32_t chosenWidth = 0;
			std::uint32_t chosenHeight = 0;
			if (PickDisplayMode(chosenWidth, chosenHeight)) {
				*reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::VideoDisplayWidth)) = chosenWidth;
				*reinterpret_cast<std::uint32_t*>(Game::Resolve(Game::Address::VideoDisplayHeight)) = chosenHeight;
				ApplyMenuResolution(chosenWidth, chosenHeight);
				Logger::Info("Widescreen: setting game screen to " + std::to_string(chosenWidth) + "x" + std::to_string(chosenHeight) + ".");
			}
			const int result = originalEnterGameScreen(self, unused, argument);
			if (!result) { Logger::Warning("Widescreen: the game could not set its screen mode."); }
			return result;
		}

		// The engine heap grows in 2 MB chunks up to 32 of them, so a single full-screen buffer above about
		// 1024x1024 can never fit and the game exits. Serve every allocation from a normal Windows heap instead.
		bool IsInOriginalEngineHeap(std::uintptr_t block) {
			const auto count = *reinterpret_cast<const std::int32_t*>(Game::Resolve(Game::Address::EngineHeapChunkCount));
			const auto totalSize = *reinterpret_cast<const std::uint32_t*>(Game::Resolve(Game::Address::EngineHeapTotalSize));
			const auto* buffers = reinterpret_cast<const std::uintptr_t*>(Game::Resolve(Game::Address::EngineHeapChunkBuffers));
			for (std::int32_t i = 0; i < count && i < 32; ++i) {
				// The first chunk has the startup size; every later one is a fixed 2 MB. Each buffer has 0x20 bytes of alignment slack.
				const std::uint32_t chunkSize = i == 0 ? totalSize - static_cast<std::uint32_t>(count - 1) * Game::EngineHeapChunkSize : Game::EngineHeapChunkSize;
				if (block >= buffers[i] && block < buffers[i] + chunkSize + 0x20) { return true; }
			}
			return false;
		}
		// Every block gets a few spare bytes after it. The game's own heap always had memory there, and its code relies
		// on it: the pixel converter (0x5FC460) reads each pixel as 4 bytes, so the last pixel of a 3-byte-per-pixel
		// picture reads one byte past the end. In a Windows heap a block can end at the last mapped byte, and that read
		// crashed (after switching back to the game, as it remade its textures).
		constexpr int EngineBlockSlack = 16;
		void* __cdecl EngineHeapAllocDetour(int size) {
			void* block = HeapAlloc(engineHeap, HEAP_ZERO_MEMORY, static_cast<SIZE_T>((std::max)(size, 1)) + EngineBlockSlack);
			if (!block) { Logger::Critical("Out of memory allocating " + std::to_string(size) + " bytes for the game engine."); }
			return block;
		}
		// The game's texture buffers (0x590580, width * height * bytes per texel) are allocated with one more row: making
		// a bump map (0x591820) writes one row past the end of its buffer and reads one row past its height texture. The
		// game's own heap hid that; in a Windows heap it breaks the heap's records and every later allocation fails.
		// At the call, EDI holds the row size.
		__declspec(naked) void TextureBufferAllocStub() {
			__asm {
				mov eax, dword ptr [esp + 4]
				add eax, edi
				push eax
				call EngineHeapAllocDetour
				add esp, 4
				ret
			}
		}
		void PadTextureBuffers() {
			const auto call = Game::Resolve(Game::Address::TextureBufferAlloc);
			// imul eax, [ecx + 8] / push eax / mov dword ptr [esp + 0x18], 0 / call 0x57CEB0
			if (!Memory::Matches(call - 13, { 0x0F, 0xAF, 0x41, 0x08, 0x50, 0xC7, 0x44, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xD3, 0xC8, 0xFE, 0xFF })
				|| !Memory::WriteCall(call, reinterpret_cast<const void*>(&TextureBufferAllocStub))) {
				Logger::Warning("Widescreen: unexpected game code, texture buffers are not padded; bump mapping may break the game's memory.");
			}
		}
		void __cdecl EngineHeapFreeDetour(void* block) {
			if (!block) { return; }
			// Blocks handed out before our hook was installed still belong to the game's own heap.
			if (IsInOriginalEngineHeap(reinterpret_cast<std::uintptr_t>(block))) {
				originalEngineHeapFree(block);
				return;
			}
			HeapFree(engineHeap, 0, block);
		}

		std::string ReadLowercaseSetting(const std::string& key, const std::string& defaultValue) {
			return StringUtils::ToLower(Config::GetString(key, defaultValue));
		}
		bool ReadResolutionSetting() {
			const std::string setting = ReadLowercaseSetting("resolution", "auto");
			if (setting == "off") {
				Logger::Info("Widescreen: off in config.yml, using the game's own resolution.");
				return false;
			}
			if (setting == "auto") {
				if (!DisplayUtils::GetDesktopResolution(width, height)) {
					Logger::Warning("Widescreen: could not read the desktop resolution, using the game's own resolution.");
					return false;
				}
			} else if (!DisplayUtils::ParseResolution(setting, width, height)) {
				Logger::Warning("Widescreen: resolution '" + setting + "' in config.yml is not auto, off or WIDTHxHEIGHT, using the game's own resolution.");
				return false;
			}
			if (width < 640 || height < 480 || width > 7680 || height > 4320) {
				Logger::Warning("Widescreen: " + std::to_string(width) + "x" + std::to_string(height) + " is outside 640x480 to 7680x4320, using the game's own resolution.");
				return false;
			}
			return true;
		}

		// The surface cache preallocates 256x256 texture pages; each holds 16 GUI tiles. Make room for a
		// full screen of tiles on top of what the world needs. The old cap of 127 (the push operand is a signed byte) was far
		// too small at 3840x2160.
		// Tested: 24 + that (168 at 4K) still left Review Objective black. The cache also holds the world's own textures,
		// and the full-screen view pushed them out every frame (2547 uploads a frame), so the world was drawn with the view's
		// black see-through tiles in their places. So the size known to work for the world at 1080p (0x68) comes first, and
		// the full-screen view with a quarter more on top: 264 pages at 4K.
		std::uint32_t GetSurfaceCachePageCount() {
			const std::uint32_t tiles = ((width + 127) / 128) * ((height + 31) / 32);
			const std::uint32_t fullScreen = (tiles + 15) / 16;
			return 0x68 + fullScreen + fullScreen / 4;
		}
		// The setup gets the full count through its argument; the push keeps what fits in it as a fallback.
		std::uint32_t surfaceCachePages = 0x68;
		using SurfaceCacheSetupFn = void(__fastcall*)(void* cache, void* unused, void* data, std::uint32_t pages);
		SurfaceCacheSetupFn originalSurfaceCacheSetup = nullptr;
		void __fastcall SurfaceCacheSetupDetour(void* cache, void* unused, void* data, std::uint32_t pages) {
			originalSurfaceCacheSetup(cache, unused, data, (std::max)(pages, surfaceCachePages));
		}
		bool InstallSurfaceCacheHook() {
			const auto target = Game::Resolve(Game::Address::SurfaceCacheSetup);
			// mov eax, [esp + 4] / push ebx / push ebp / push esi / mov esi, ecx
			if (!Memory::Matches(target, { 0x8B, 0x44, 0x24, 0x04, 0x53, 0x55, 0x56, 0x8B, 0xF1 })) return false;
			if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&SurfaceCacheSetupDetour), reinterpret_cast<void**>(&originalSurfaceCacheSetup)) || !HookManager::EnableHook(target)) {
				HookManager::RemoveHook(target);
				return false;
			}
			return true;
		}

		bool InstallEngineHeap() {
			engineHeap = HeapCreate(0, 0, 0);
			if (!engineHeap) { return false; }
			const auto allocTarget = Game::Resolve(Game::Address::EngineHeapAlloc);
			const auto freeTarget = Game::Resolve(Game::Address::EngineHeapFree);
			if (!HookManager::CreateHook(allocTarget, reinterpret_cast<void*>(&EngineHeapAllocDetour), reinterpret_cast<void**>(&originalEngineHeapAlloc))) { return false; }
			if (!HookManager::CreateHook(freeTarget, reinterpret_cast<void*>(&EngineHeapFreeDetour), reinterpret_cast<void**>(&originalEngineHeapFree))) {
				HookManager::RemoveHook(allocTarget);
				return false;
			}
			// Free must be live before alloc, or a block from our heap could reach the game's free.
			if (!HookManager::EnableHook(freeTarget)) {
				HookManager::RemoveHook(allocTarget);
				HookManager::RemoveHook(freeTarget);
				return false;
			}
			if (!HookManager::EnableHook(allocTarget)) {
				HookManager::RemoveHook(allocTarget);
				return false;
			}
			PadTextureBuffers();
			return true;
		}
		bool InstallGuiTileTables() {
			guiTileWriteReturn = Game::Resolve(Game::Address::GuiViewTileWrite) + 5;
			guiTileReadReturn = Game::Resolve(Game::Address::GuiViewTileRead) + 6;
			guiTileReadSecondReturn = Game::Resolve(Game::Address::GuiViewTileReadSecond) + 8;
			for (const auto& jump : guiTileJumps) {
				if (!Memory::WriteJump(Game::Resolve(jump.virtualAddress), jump.stub, jump.size)) { return false; }
			}
			return true;
		}
		bool InstallResolutionHook() {
			const auto target = Game::Resolve(Game::Address::EnterGameScreen);
			if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&EnterGameScreenDetour), reinterpret_cast<void**>(&originalEnterGameScreen))) { return false; }
			if (!HookManager::EnableHook(target)) {
				HookManager::RemoveHook(target);
				return false;
			}
			return true;
		}

		// Every screen mode change (menu, game, videos) goes through the game's screen setup. Things that depend on
		// the screen height follow it from here.
		using SetScreenModeFn = int(__fastcall*)(void* game, void* unused, std::int32_t width, std::int32_t height, std::int32_t bitDepth, std::int32_t windowed, std::int32_t flagA, std::int32_t flagB);
		SetScreenModeFn originalSetScreenMode = nullptr;
		bool cameraEnabled = false;
		bool fontsEnabled = false;
		bool guiEnabled = false;
		int __fastcall SetScreenModeDetour(void* game, void* unused, std::int32_t modeWidth, std::int32_t modeHeight, std::int32_t bitDepth, std::int32_t windowed, std::int32_t flagA, std::int32_t flagB) {
			const int result = originalSetScreenMode(game, unused, modeWidth, modeHeight, bitDepth, windowed, flagA, flagB);
			if (result && modeHeight > 0) {
				if (cameraEnabled) { WidescreenCamera::OnScreenMode(modeHeight); }
				if (fontsEnabled) { WidescreenFonts::OnScreenMode(modeHeight); }
				if (guiEnabled) { WidescreenGui::OnScreenMode(modeHeight); }
			}
			return result;
		}
		// Videos are 640x480 films that the decoder writes straight into the screen surface (on its own thread), so the
		// screen mode must never be smaller than the film: a 640x360 mode made it write past the end of the surface
		// and crash. While a video plays, the screen is switched to exactly 640x480, which the film fills; with
		// "stretch", patch.dll is also told to stretch that frame over the whole screen instead of keeping its shape.
		// Afterwards the previous mode and scaling come back.
		enum class VideoMode { Unchanged, Bars, Stretch };
		VideoMode videoMode = VideoMode::Stretch;
		constexpr std::int32_t VideoWidth = 640;
		constexpr std::int32_t VideoHeight = 480;
		using PlayMovieFn = void(__fastcall*)(void* self, void* unused, const char* path);
		PlayMovieFn originalPlayMovie = nullptr;
		int __fastcall SetScreenModeDetour(void* game, void* unused, std::int32_t modeWidth, std::int32_t modeHeight, std::int32_t bitDepth, std::int32_t windowed, std::int32_t flagA, std::int32_t flagB);
		// patch.dll's "ScaleMode" (0 = stretch to the window, its default 1 = keep the shape). Checked against the
		// instruction that reads it ("cmp [setting], 0" in its present rectangle setup); null if it doesn't match.
		std::uint32_t* GogScaleMode() {
			const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"patch.dll"));
			if (!module) { return nullptr; }
			const auto setting = module + Game::GogPatch::ScaleModeSetting;
			const auto* code = reinterpret_cast<const std::uint8_t*>(module + Game::GogPatch::PresentReadsScaleMode);
			if (code[0] != 0x83 || code[1] != 0x3D || *reinterpret_cast<const std::uintptr_t*>(code + 2) != setting || code[6] != 0x00) { return nullptr; }
			return reinterpret_cast<std::uint32_t*>(setting);
		}
		// Where the player puts the film on the screen, worked out as 0x5AABF0 does from its settings block: the film's
		// size (doubled across / down by flags 0x40 / 0x30, or replaced by flags 0x80 / 0x100), centered by flags 4 / 8,
		// moved by flags 1 / 2. The film size is 0 until the player has read the film's header.
		bool VideoFilmArea(std::uint32_t screenWidth, std::uint32_t screenHeight, KeeperForge2::Renderer::VideoArea& area) {
			const auto config = Game::Resolve(Game::Address::VideoPlayerConfig);
			const auto field = [config](std::uintptr_t offset) { return ReadGame<std::int32_t>(config + offset); };
			const auto flags = static_cast<std::uint32_t>(field(Game::Offset::VideoConfigFlags));
			const std::int32_t filmWidth = field(Game::Offset::VideoConfigFilmWidth);
			const std::int32_t filmHeight = field(Game::Offset::VideoConfigFilmHeight);
			if (filmWidth <= 0 || filmHeight <= 0) { return false; }
			std::int32_t width = (flags & 0x40) != 0 ? filmWidth * 2 : filmWidth;
			std::int32_t height = (flags & 0x30) != 0 ? filmHeight * 2 : filmHeight;
			std::int32_t left = (flags & 4) != 0 ? (static_cast<std::int32_t>(screenWidth) - width) >> 1 : 0;
			std::int32_t top = (flags & 8) != 0 ? (static_cast<std::int32_t>(screenHeight) - height) >> 1 : 0;
			if ((flags & 1) != 0) { left += field(Game::Offset::VideoConfigOffsetX); }
			if ((flags & 2) != 0) { top += field(Game::Offset::VideoConfigOffsetY); }
			if ((flags & 0x80) != 0) { width = field(Game::Offset::VideoConfigShownWidth); }
			if ((flags & 0x100) != 0) { height = field(Game::Offset::VideoConfigShownHeight); }
			if (width <= 0 || height <= 0) { return false; }
			// The doubled width is made for a 16-bit screen, by writing each film pixel as two (4 bytes). On our 32-bit
			// screen those 4 bytes are one pixel, so the film covers only its own width there (tested: a 640-wide area
			// showed the film in its left half). Shown in the doubled shape.
			const std::int32_t pixelsAcross = (flags & 0x40) != 0 && (flags & 0x80) == 0 ? filmWidth : width;
			area = { left, top, pixelsAcross, height, static_cast<double>(width) / height };
			return true;
		}
		void __fastcall PlayMovieDetour(void* self, void* unused, const char* path) {
			const auto game = Game::Resolve(Game::Address::GameObject);
			// The game object keeps the arguments of the last screen setup at +4..+0x1C (as 0x5587F0 reuses them).
			const auto field = [game](std::uintptr_t offset) { return *reinterpret_cast<const std::int32_t*>(game + offset); };
			const std::int32_t savedWidth = field(0x4), savedHeight = field(0x8), savedDepth = field(0xC), savedWindowed = field(0x10), savedFlagA = field(0x14), savedFlagB = field(0x1C);
			if (videoMode == VideoMode::Unchanged || !originalSetScreenMode || savedWidth <= 0 || savedHeight <= 0) {
				originalPlayMovie(self, unused, path);
				return;
			}
			// With one of our renderers drawing, it shows the film itself (only the film's band of the screen, as patch.dll
			// scales the film with DirectDraw); patch.dll's setting does nothing then.
			const bool ourRenderer = KeeperForge2::Renderer::IsActive();
			const bool ourStretch = videoMode == VideoMode::Stretch && ourRenderer;
			if (ourRenderer) { KeeperForge2::Renderer::SetVideoArea(&VideoFilmArea); }
			std::uint32_t* scaleMode = videoMode == VideoMode::Stretch && !ourStretch ? GogScaleMode() : nullptr;
			const std::uint32_t savedScaleMode = scaleMode ? *scaleMode : 0;
			// patch.dll works out its stretch when the mode is set, so the setting goes in first.
			if (scaleMode) { *scaleMode = 0; }
			if (ourStretch) { KeeperForge2::Renderer::SetStretch(true); }
			// Our renderers find the film in any mode it fits in, so they only need a switch when the mode is too small.
			// Switching in the menu took the mouse pointer's image away (tested: the hand was gone after a video from the
			// Movies list); the game itself plays these in the menu's mode.
			const bool needsSwitch = ourRenderer ? savedWidth < VideoWidth || savedHeight < VideoHeight
				: savedWidth != VideoWidth || savedHeight != VideoHeight || scaleMode != nullptr;
			const bool switched = needsSwitch
				&& SetScreenModeDetour(reinterpret_cast<void*>(game), nullptr, VideoWidth, VideoHeight, savedDepth, savedWindowed, savedFlagA, savedFlagB);
			Logger::Debug("Widescreen: playing a video.");
			originalPlayMovie(self, unused, path);
			Logger::Debug("Widescreen: video finished.");
			if (scaleMode) { *scaleMode = savedScaleMode; }
			if (ourStretch) { KeeperForge2::Renderer::SetStretch(false); }
			if (ourRenderer) { KeeperForge2::Renderer::SetVideoArea(nullptr); }
			if (switched) { SetScreenModeDetour(reinterpret_cast<void*>(game), nullptr, savedWidth, savedHeight, savedDepth, savedWindowed, savedFlagA, savedFlagB); }
		}
		bool InstallMovieHook() {
			const auto target = Game::Resolve(Game::Address::PlayMovie);
			if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&PlayMovieDetour), reinterpret_cast<void**>(&originalPlayMovie))) { return false; }
			if (!HookManager::EnableHook(target)) {
				HookManager::RemoveHook(target);
				return false;
			}
			return true;
		}

		bool InstallScreenModeHook() {
			const auto target = Game::Resolve(Game::Address::SetScreenMode);
			if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&SetScreenModeDetour), reinterpret_cast<void**>(&originalSetScreenMode))) { return false; }
			if (!HookManager::EnableHook(target)) {
				HookManager::RemoveHook(target);
				return false;
			}
			return true;
		}
	}

	void Widescreen::Initialize() {
		if (!ReadResolutionSetting()) { return; }
		for (const auto& site : patchSites) {
			if (!MatchesAnyPattern(site)) {
				Logger::Warning("Widescreen: unexpected code at " + std::string(site.name) + " (" + StringUtils::ToHex(site.address) + "). This is not the DKII-DX.exe build we support, so widescreen is off.");
				return;
			}
		}
		// The engine fixes go in first; they are safe at any resolution. The resolution itself goes last.
		if (!InstallEngineHeap()) {
			Logger::Warning("Widescreen: failed to replace the engine heap, using the game's own resolution.");
			return;
		}
		if (!InstallGuiTileTables()) {
			Logger::Warning("Widescreen: failed to patch the GUI tile tables, using the game's own resolution.");
			return;
		}
		// The Direct3D texture pool (what the game uses with our renderers): 512 textures of 128x128 for the world and every GUI
		// view tile. A full-screen view at 3840x2160 is 2040 tiles (30x68), so with Review Objective open it pushed the world's
		// textures out every frame and the world was drawn with the view's black see-through tiles (renderer trace). One
		// texture per tile of a full screen is added: 2552 at 4K, 64 KB each in memory.
		const std::uint32_t screenTiles = ((width + 127) / 128) * ((height + 31) / 32);
		const std::uint32_t deviceTextures = 0x200 + screenTiles;
		const auto countPush = Game::Resolve(Game::Address::DeviceTextureCountPush);
		if (Memory::Matches(countPush, { 0x68, 0x00, 0x02, 0x00, 0x00, 0x6A, 0x20 })) {
			Memory::Write<std::uint32_t>(countPush + 1, deviceTextures);
		}
		else {
			Logger::Warning("Widescreen: unexpected code at the texture pool size; full-screen panels (Review Objective) may black out the world.");
		}
		std::uint32_t pages = GetSurfaceCachePageCount();
		surfaceCachePages = pages;
		// The push operand is a signed byte: what fits goes there, the rest through the hook.
		Memory::Patch(Game::Resolve(Game::Address::SurfaceCachePageCount) + 1, { static_cast<std::uint8_t>((std::min<std::uint32_t>)(pages, 0x7F)) });
		if (pages > 0x7F && !InstallSurfaceCacheHook()) {
			Logger::Warning("Widescreen: surface cache capped at 127 pages; full-screen panels (Review Objective) may not draw at this resolution.");
			pages = 0x7F;
		}
		if (!InstallResolutionHook()) {
			Logger::Warning("Widescreen: failed to hook the game screen setup, using the game's own resolution.");
			return;
		}
		const std::string guiLayout = ReadLowercaseSetting("gui_layout", "anchored");
		if (guiLayout == "anchored") {
			guiEnabled = WidescreenGui::Install();
			if (guiEnabled) { Logger::Debug("Widescreen: GUI keeps its proportions and sticks to the screen edges."); }
			else { Logger::Warning("Widescreen: failed to hook the GUI layout, the GUI will be stretched."); }
		} else if (guiLayout != "stretched") {
			Logger::Warning("Widescreen: gui_layout '" + guiLayout + "' in config.yml is not anchored or stretched, the GUI will be stretched.");
		}
		widescreenMenus = ReadLowercaseSetting("widescreen_menus", "on") != "off";
		const std::string menuHeightText = ReadLowercaseSetting("menu_height", "480");
		std::uint32_t parsedMenuHeight = 0;
		const auto menuHeightResult = std::from_chars(menuHeightText.data(), menuHeightText.data() + menuHeightText.size(), parsedMenuHeight);
		if (menuHeightResult.ec == std::errc{} && parsedMenuHeight >= 480) { menuHeightSetting = parsedMenuHeight; }
		else { Logger::Warning("Widescreen: menu_height '" + menuHeightText + "' in config.yml must be a number of at least 480, using 480."); }
		const std::string fieldOfViewText = ReadLowercaseSetting("field_of_view", "100");
		std::uint32_t fieldOfView = 0;
		const auto fieldOfViewResult = std::from_chars(fieldOfViewText.data(), fieldOfViewText.data() + fieldOfViewText.size(), fieldOfView);
		if (fieldOfViewResult.ec != std::errc{} || fieldOfView < 50 || fieldOfView > 300) {
			Logger::Warning("Widescreen: field_of_view '" + fieldOfViewText + "' in config.yml must be a number from 50 to 300, using 100.");
			fieldOfView = 100;
		}
		if (InstallScreenModeHook()) {
			WidescreenCamera::Install(fieldOfView);
			cameraEnabled = true;
			const std::string videos = ReadLowercaseSetting("fullscreen_videos", "stretch");
			if (videos == "off") { videoMode = VideoMode::Unchanged; }
			else if (videos == "bars") { videoMode = VideoMode::Bars; }
			else {
				if (videos != "stretch") { Logger::Warning("Widescreen: fullscreen_videos '" + videos + "' in config.yml is not stretch, bars or off, using stretch."); }
				videoMode = VideoMode::Stretch;
			}
			if (videoMode != VideoMode::Unchanged && !InstallMovieHook()) { Logger::Warning("Widescreen: failed to hook video playback, videos keep their bars."); }
		} else {
			Logger::Warning("Widescreen: failed to hook the screen setup, the 3D view will look too wide and some text small.");
		}
		if (WidescreenFonts::Install()) { fontsEnabled = true; }
		else { Logger::Warning("Widescreen: failed to hook the in-game fonts, text keeps its original size."); }
		if (!WidescreenLoadingScreen::Install()) { Logger::Warning("Widescreen: failed to patch the loading screen, it may show small or not at all."); }
		enabled = true;
		Logger::Debug("Widescreen: requested game resolution " + std::to_string(width) + "x" + std::to_string(height) + " (surface cache " + std::to_string(pages) + " pages, "
			+ std::to_string(deviceTextures) + " textures).");
	}
	bool Widescreen::IsEnabled() { return enabled; }
	std::uint32_t Widescreen::GetWidth() { return width; }
	std::uint32_t Widescreen::GetHeight() { return height; }
}
