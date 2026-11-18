#include "Screenshots.h"
#include "Config.h"
#include "FrameRate.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "LevelEditor.h"
#include "Logger.h"
#include "Memory.h"
#include "Paths.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <Windows.h>
#include <wincodec.h>

namespace KeeperForge2 {
	namespace {
		template<typename T> void Release(T*& object) {
			if (object) {
				object->Release();
				object = nullptr;
			}
		}
		std::wstring Widen(const char* text) {
			const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
			std::wstring wide(length > 0 ? length - 1 : 0, L'\0');
			if (length > 1) { MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), length); }
			return wide;
		}
		bool EndsWithPng(const char* fileName) {
			const std::size_t length = std::strlen(fileName);
			return length >= 4 && _stricmp(fileName + length - 4, ".PNG") == 0;
		}

		// Reads the bitmap the game wrote and writes it back over the same file as PNG, through a temporary file so a
		// failure leaves the original in place.
		bool ConvertToPng(const std::wstring& path) {
			const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			// RPC_E_CHANGED_MODE: COM is already set up on this thread in another mode, which is fine to use.
			const bool uninitialize = SUCCEEDED(comResult);
			const std::wstring temporary = path + L".tmp";
			IWICImagingFactory* factory = nullptr;
			IWICBitmapDecoder* decoder = nullptr;
			IWICBitmapFrameDecode* source = nullptr;
			IWICStream* stream = nullptr;
			IWICBitmapEncoder* encoder = nullptr;
			IWICBitmapFrameEncode* frame = nullptr;
			bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
				&& SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
				&& SUCCEEDED(decoder->GetFrame(0, &source))
				&& SUCCEEDED(factory->CreateStream(&stream))
				&& SUCCEEDED(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE))
				&& SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
				&& SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))
				&& SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr))
				&& SUCCEEDED(frame->Initialize(nullptr))
				&& SUCCEEDED(frame->WriteSource(source, nullptr))
				&& SUCCEEDED(frame->Commit())
				&& SUCCEEDED(encoder->Commit());
			Release(frame);
			Release(encoder);
			Release(stream);
			Release(source);
			// The decoder holds the original file open, so it must be gone before the file is replaced.
			Release(decoder);
			Release(factory);
			if (ok) { ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE; }
			if (!ok) { DeleteFileW(temporary.c_str()); }
			if (uninitialize) { CoUninitialize(); }
			return ok;
		}

		using WriteBitmapFn = std::int32_t*(__fastcall*)(void* writer, void* unused, std::int32_t* status, const char* fileName, void* description, void* palette);
		WriteBitmapFn originalWriteBitmap = nullptr;
		std::int32_t* __fastcall WriteBitmapDetour(void* writer, void* unused, std::int32_t* status, const char* fileName, void* description, void* palette) {
			std::int32_t* result = originalWriteBitmap(writer, unused, status, fileName, description, palette);
			// Only the screenshots are renamed to .PNG; any other bitmap the game writes is left as it is.
			if (fileName && *status >= 0 && EndsWithPng(fileName)) {
				if (ConvertToPng(Widen(fileName))) { Logger::Info(std::string("Screenshot saved: ") + fileName); }
				else { Logger::Warning(std::string("Screenshot could not be converted to PNG and was left as a bitmap: ") + fileName); }
			}
			return result;
		}

		// The main menu asks for its screenshot before it draws its 2D layer, so the title and menu text were missing.
		// The request is held and carried out right before the finished frame is shown, when everything is drawn.
		using TakeScreenshotFn = int(__fastcall*)(void* game, void* unused);
		TakeScreenshotFn originalTakeScreenshot = nullptr;
		void* pendingScreenshot = nullptr;
		// Holding Print Screen asks for a screenshot every frame; after one is taken, requests are ignored for this long
		// so they do not pile up. A real clock is fine here: screenshots are not part of the simulation.
		constexpr ULONGLONG ScreenshotCooldownMs = 2000;
		ULONGLONG lastScreenshotTime = 0;
		bool tookScreenshot = false;
		int __fastcall TakeScreenshotDetour(void* game, void* unused) {
			if (tookScreenshot && GetTickCount64() - lastScreenshotTime < ScreenshotCooldownMs) { return 1; }
			pendingScreenshot = game;
			return 1;
		}
		// The mouse pointer (the hand in the game) is drawn by its own thread straight onto the screen, never into the
		// frame being finished, so the screenshot would not show it. Copy it onto that frame the way the cursor drawer
		// puts it on the screen (0x5DC530: its pointer image, source rectangle and position, copied transparently)
		// right before the screenshot reads the frame. It lands where the real pointer covers anyway.
		using BlitFn = std::int32_t*(__cdecl*)(std::int32_t* result, void* destination, std::int32_t x, std::int32_t y, void* source, void* sourceRect, std::uint32_t flags);
		using WindowSurfaceFn = void*(__fastcall*)(void* window, void* unused);
		constexpr std::uint32_t TransparentCopy = 1;
		void DrawCursorOnFrame(void* game) {
			// The editor has Windows' pointer, not the hand, and the drawer may still hold the hand's last image.
			if (LevelEditor::InSession()) { return; }
			const auto gameBase = reinterpret_cast<std::uintptr_t>(game);
			// The same surface the screenshot reads: the window's in windowed mode, the back buffer otherwise.
			void* frame = *reinterpret_cast<const std::int32_t*>(gameBase + Game::Offset::GameWindowed) != 0
				? reinterpret_cast<WindowSurfaceFn>(Game::Resolve(Game::Address::WindowScreenSurface))(reinterpret_cast<void*>(gameBase + Game::Offset::GameWindow), nullptr)
				: *reinterpret_cast<void**>(Game::Resolve(Game::Address::BackBufferSurface));
			if (frame == nullptr) { return; }
			auto* drawer = reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::CursorDrawer));
			// The drawer only shows the pointer while it has a screen to draw on.
			if (*reinterpret_cast<void* const*>(drawer + Game::Offset::CursorScreen) == nullptr) { return; }
			// Both need a DirectDraw surface: the copy reads them without checking, and the pointer image has been seen
			// without one (the game crashed taking a screenshot in the Movies list after a minimize and restore).
			const auto hasSurface = [](const void* wrapper) {
				return *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(wrapper) + Game::Offset::SurfaceWrapperSurface) != nullptr;
			};
			if (!hasSurface(frame) || !hasSurface(drawer + Game::Offset::CursorImage)) { return; }
			const auto x = *reinterpret_cast<const std::int32_t*>(drawer + Game::Offset::CursorX);
			const auto y = *reinterpret_cast<const std::int32_t*>(drawer + Game::Offset::CursorY);
			std::int32_t status = 0;
			reinterpret_cast<BlitFn>(Game::Resolve(Game::Address::BlitSurface))(&status, frame, x, y,
				drawer + Game::Offset::CursorImage, drawer + Game::Offset::CursorSourceRect, TransparentCopy);
		}
		// For the log: when the pointer image loses its surface and when it gets one back. It lost it after the screen mode
		// was switched for a video in the menu (see docs/research-log.md).
		void WatchPointerImage() {
			static bool hadSurface = false;
			const auto image = Game::Resolve(Game::Address::CursorDrawer) + Game::Offset::CursorImage;
			const bool hasSurface = *reinterpret_cast<void* const*>(image + Game::Offset::SurfaceWrapperSurface) != nullptr;
			if (hasSurface == hadSurface) { return; }
			hadSurface = hasSurface;
			Logger::Debug(hasSurface ? "Screenshots: the mouse pointer's image has a surface." : "Screenshots: the mouse pointer's image lost its surface (the hand is not drawn).");
		}
		void TakePendingScreenshot() {
			WatchPointerImage();
			if (!pendingScreenshot) { return; }
			void* game = pendingScreenshot;
			pendingScreenshot = nullptr;
			DrawCursorOnFrame(game);
			originalTakeScreenshot(game, nullptr);
			lastScreenshotTime = GetTickCount64();
			tookScreenshot = true;
		}
		// screenshots_cleared: the game numbers its screenshots SHOT0000 onwards, so the old ones are deleted at every
		// start (only those files; anything else in the folder is left alone).
		void ClearOldScreenshots() {
			if (Config::GetString("screenshots_cleared", "on") != "on") { return; }
			std::error_code error;
			std::size_t removed = 0;
			for (const auto& entry : std::filesystem::directory_iterator(Paths::Screenshots(), error)) {
				if (!entry.is_regular_file(error)) { continue; }
				const std::string name = entry.path().filename().string();
				const std::string extension = entry.path().extension().string();
				const bool image = _stricmp(extension.c_str(), ".png") == 0 || _stricmp(extension.c_str(), ".bmp") == 0;
				if (!image || _strnicmp(name.c_str(), "SHOT", 4) != 0) { continue; }
				if (std::filesystem::remove(entry.path(), error)) { ++removed; }
			}
			if (removed > 0) { Logger::Debug("Screenshots: deleted " + std::to_string(removed) + " old screenshots (screenshots_cleared on)."); }
		}
		// The game reads its screenshot folder's name ("SCRSHOTS", relative to the game folder) through one pointer,
		// then creates the folder if it is missing. Point it at KeeperForge2\SCRSHOTS instead.
		void MoveScreenshotFolder() {
			static const std::string folder = Paths::Screenshots().string();
			const auto pointer = Game::Resolve(Game::Address::ScreenshotFolderName);
			const auto original = static_cast<std::uint32_t>(Game::Resolve(Game::Address::ScreenshotFolderText));
			const char expected[] = "SCRSHOTS";
			if (Memory::Read<std::uint32_t>(pointer) != original || !Memory::Matches(original, std::vector<std::uint8_t>(expected, expected + sizeof(expected)))) {
				Logger::Warning("Screenshots: unexpected game code, screenshots stay in SCRSHOTS.");
				return;
			}
			Memory::Write<std::uint32_t>(pointer, reinterpret_cast<std::uint32_t>(folder.c_str()));
			Logger::Info("Screenshots: saved in " + folder + ".");
		}
		void DelayScreenshotsToPresent() {
			const auto target = Game::Resolve(Game::Address::TakeScreenshot);
			if (!Memory::Matches(target, { 0x81, 0xEC, 0x64, 0x05, 0x00, 0x00, 0x53, 0x56, 0x8B, 0xF1 })) {
				Logger::Warning("Screenshots: unexpected screenshot code, menu screenshots may miss the menu text.");
				return;
			}
			if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&TakeScreenshotDetour), reinterpret_cast<void**>(&originalTakeScreenshot)) || !HookManager::EnableHook(target)) {
				HookManager::RemoveHook(target);
				Logger::Warning("Screenshots: failed to hook the screenshot code, menu screenshots may miss the menu text.");
				return;
			}
			FrameRate::SetBeforePresent(&TakePendingScreenshot);
		}
	}

	void Screenshots::Initialize() {
		ClearOldScreenshots();
		const auto pattern = Game::Resolve(Game::Address::ScreenshotNamePattern);
		const auto writer = Game::Resolve(Game::Address::WriteBitmapFile);
		const char expected[] = "%s\\SHOT%04d.BMP";
		if (!Memory::Matches(pattern, std::vector<std::uint8_t>(expected, expected + sizeof(expected)))
			|| !Memory::Matches(writer, { 0x6A, 0xFF, 0x68, 0x48, 0x06, 0x65, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 })) {
			Logger::Warning("Screenshots: unexpected game code, screenshots stay as BMP.");
			return;
		}
		if (!HookManager::CreateHook(writer, reinterpret_cast<void*>(&WriteBitmapDetour), reinterpret_cast<void**>(&originalWriteBitmap)) || !HookManager::EnableHook(writer)) {
			HookManager::RemoveHook(writer);
			Logger::Warning("Screenshots: failed to hook the bitmap writer, screenshots stay as BMP.");
			return;
		}
		// "BMP" -> "PNG" at the end of "%s\SHOT%04d.BMP" (same length).
		Memory::Patch(pattern + 12, { 'P', 'N', 'G' });
		Logger::Debug("Screenshots: saved as PNG.");
		MoveScreenshotFolder();
		// Needs the per-frame present hook from FrameRate (installed first).
		if (FrameRate::IsPresentHooked()) { DelayScreenshotsToPresent(); }
	}
}
