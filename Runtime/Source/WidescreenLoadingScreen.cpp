#include "WidescreenLoadingScreen.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "StringUtils.h"
#include <string>
#include <algorithm>
#include <cstdint>
#include <Windows.h>
#include <ddraw.h>

namespace KeeperForge2 {
	namespace {
		struct ImageSize {
			std::int32_t width;
			std::int32_t height;
		};
		// The game's original screen sizes, largest first. [unverified] that every one has a loading screen.
		constexpr ImageSize imageSizes[] = { { 1600, 1200 }, { 1280, 1024 }, { 1024, 768 }, { 800, 600 }, { 640, 480 } };

		using ShowLoadingScreenFn = void(__fastcall*)(void* world, void* unused);
		ShowLoadingScreenFn originalShowLoadingScreen = nullptr;

		std::int32_t& GameScreenWidth() { return *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::GameScreenWidth)); }
		std::int32_t& GameScreenHeight() { return *reinterpret_cast<std::int32_t*>(Game::Resolve(Game::Address::GameScreenHeight)); }
		bool IsShown(void* world) { return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<std::uintptr_t>(world) + Game::Offset::WorldLoadingScreenShown) != 0; }

		// The size that loaded last time. The game redraws the loading screen many times while a level loads, so
		// remembering it avoids retrying sizes that don't exist on every redraw.
		const ImageSize* workingSize = nullptr;

		// The image name is built from the game object's screen size, so point that at an original size while the
		// image loads, trying smaller ones until one exists.
		void __fastcall ShowLoadingScreenDetour(void* world, void* unused) {
			const auto realWidth = GameScreenWidth();
			const auto realHeight = GameScreenHeight();
			bool tried = false;
			if (workingSize && workingSize->height <= realHeight) {
				GameScreenWidth() = workingSize->width;
				GameScreenHeight() = workingSize->height;
				originalShowLoadingScreen(world, unused);
				tried = true;
			}
			for (const auto& size : imageSizes) {
				if (tried && IsShown(world)) { break; }
				if (size.height > realHeight || &size == workingSize) { continue; }
				GameScreenWidth() = size.width;
				GameScreenHeight() = size.height;
				originalShowLoadingScreen(world, unused);
				tried = true;
				if (IsShown(world)) {
					workingSize = &size;
					Logger::Debug("Widescreen: using the " + std::to_string(size.width) + "x" + std::to_string(size.height) + " loading screen.");
				}
			}
			static bool loggedMissing = false;
			if (tried && !IsShown(world) && !loggedMissing) {
				Logger::Warning("Widescreen: no loading screen image could be loaded.");
				loggedMissing = true;
			}
			GameScreenWidth() = realWidth;
			GameScreenHeight() = realHeight;
			if (!tried) { originalShowLoadingScreen(world, unused); }
		}

		IDirectDrawSurface* DirectDrawSurfaceOf(void* surface) { return *reinterpret_cast<IDirectDrawSurface**>(reinterpret_cast<std::uintptr_t>(surface) + Game::Offset::SurfaceDirectDraw); }

		// The title screen has its copyright lines along the bottom edge, so it is fitted (full height, centered, black
		// at the sides) instead of cropped. It is drawn once, so the black fill does not flicker.
		void __cdecl TitleScreenBlit(std::int32_t* result, void* destination, std::int32_t, std::int32_t, void* source, RECT*, std::uint32_t) {
			*result = -1;
			IDirectDrawSurface* target = DirectDrawSurfaceOf(destination);
			IDirectDrawSurface* image = DirectDrawSurfaceOf(source);
			if (!target || !image) { return; }
			DDSURFACEDESC targetDesc{};
			DDSURFACEDESC imageDesc{};
			targetDesc.dwSize = sizeof(targetDesc);
			imageDesc.dwSize = sizeof(imageDesc);
			if (FAILED(target->GetSurfaceDesc(&targetDesc)) || FAILED(image->GetSurfaceDesc(&imageDesc)) || imageDesc.dwWidth == 0 || imageDesc.dwHeight == 0) { return; }
			const double scale = (std::min)(static_cast<double>(targetDesc.dwWidth) / imageDesc.dwWidth, static_cast<double>(targetDesc.dwHeight) / imageDesc.dwHeight);
			const auto fittedWidth = static_cast<LONG>(imageDesc.dwWidth * scale);
			const auto fittedHeight = static_cast<LONG>(imageDesc.dwHeight * scale);
			RECT area{};
			area.left = (static_cast<LONG>(targetDesc.dwWidth) - fittedWidth) / 2;
			area.top = (static_cast<LONG>(targetDesc.dwHeight) - fittedHeight) / 2;
			area.right = area.left + fittedWidth;
			area.bottom = area.top + fittedHeight;
			DDBLTFX fill{};
			fill.dwSize = sizeof(fill);
			target->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fill);
			if (SUCCEEDED(target->Blt(&area, image, nullptr, DDBLT_WAIT, nullptr))) { *result = 0; }
		}

		// Takes the place of the game's BltFast-based copy (cdecl: result, destination, x, y, source, source rect, flags).
		void __cdecl LoadingScreenBlit(std::int32_t* result, void* destination, std::int32_t, std::int32_t, void* source, RECT*, std::uint32_t) {
			*result = -1;
			IDirectDrawSurface* target = DirectDrawSurfaceOf(destination);
			IDirectDrawSurface* image = DirectDrawSurfaceOf(source);
			if (!target || !image) { return; }
			DDSURFACEDESC targetDesc{};
			DDSURFACEDESC imageDesc{};
			targetDesc.dwSize = sizeof(targetDesc);
			imageDesc.dwSize = sizeof(imageDesc);
			if (FAILED(target->GetSurfaceDesc(&targetDesc)) || FAILED(image->GetSurfaceDesc(&imageDesc)) || imageDesc.dwWidth == 0 || imageDesc.dwHeight == 0) { return; }
			// Fill the whole screen without changing the image's shape: cut the same amount off both sides of the
			// image's longer direction (top and bottom of a 4:3 image on a wide screen). Drawing straight over the
			// whole screen, with no black fill first, keeps the many redraws during loading from flickering.
			const double screenAspect = static_cast<double>(targetDesc.dwWidth) / targetDesc.dwHeight;
			const double imageAspect = static_cast<double>(imageDesc.dwWidth) / imageDesc.dwHeight;
			RECT crop{ 0, 0, static_cast<LONG>(imageDesc.dwWidth), static_cast<LONG>(imageDesc.dwHeight) };
			if (screenAspect > imageAspect) {
				const auto visibleHeight = static_cast<LONG>(imageDesc.dwWidth / screenAspect);
				crop.top = (static_cast<LONG>(imageDesc.dwHeight) - visibleHeight) / 2;
				crop.bottom = crop.top + visibleHeight;
			} else {
				const auto visibleWidth = static_cast<LONG>(imageDesc.dwHeight * screenAspect);
				crop.left = (static_cast<LONG>(imageDesc.dwWidth) - visibleWidth) / 2;
				crop.right = crop.left + visibleWidth;
			}
			RECT area{ 0, 0, static_cast<LONG>(targetDesc.dwWidth), static_cast<LONG>(targetDesc.dwHeight) };
			const HRESULT drawn = target->Blt(&area, image, &crop, DDBLT_WAIT, nullptr);
			if (SUCCEEDED(drawn)) { *result = 0; }
			static int logged = 0;
			if (logged < 4) {
				++logged;
				Logger::Debug("Widescreen: loading screen " + std::to_string(imageDesc.dwWidth) + "x" + std::to_string(imageDesc.dwHeight) + " drawn to " + std::to_string(targetDesc.dwWidth) + "x" + std::to_string(targetDesc.dwHeight) + ", result " + StringUtils::ToHex(static_cast<std::uint32_t>(drawn)) + ".");
			}
		}
	}

	bool WidescreenLoadingScreen::Install() {
		const auto target = Game::Resolve(Game::Address::ShowLoadingScreen);
		if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&ShowLoadingScreenDetour), reinterpret_cast<void**>(&originalShowLoadingScreen))) { return false; }
		if (!HookManager::EnableHook(target)) {
			HookManager::RemoveHook(target);
			return false;
		}
		return Memory::WriteCall(Game::Resolve(Game::Address::LoadingScreenBlitFront), reinterpret_cast<const void*>(&LoadingScreenBlit))
			&& Memory::WriteCall(Game::Resolve(Game::Address::LoadingScreenBlitBack), reinterpret_cast<const void*>(&LoadingScreenBlit))
			&& Memory::WriteCall(Game::Resolve(Game::Address::TitleScreenBlit), reinterpret_cast<const void*>(&TitleScreenBlit));
	}
}
