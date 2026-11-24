#include "Renderer.h"
#include "Renderer/Backend.h"
#include "Renderer/DirectDrawLayer.h"
#include "Config.h"
#include "GameAddresses.h"
#include "Logger.h"
#include "Memory.h"
#include "StringUtils.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		using Render::Api;

		// What config.yml says.
		Api configured = Render::DefaultApi;
		bool importsPatched = false;

		// ---- DDrawCompat. The Steam release's ddraw.dll is DDrawCompat, which patch.dll loads at start. While loading
		// it hooks the graphics driver and then installs DirectDraw, Direct3D and GDI hooks of its own, and with those
		// in, nothing our renderers showed reached the window (the window's own pixels stayed black, tested). It does
		// this before a late-injected runtime starts, so the launcher loads KeeperForge2 among the game's own DLLs
		// (StartEarly): every DLL is then in memory but none has started, and when one of our renderers is chosen
		// DDrawCompat's start routine is turned into "return TRUE", so it never starts. Whatever patch.dll still asks of
		// DirectDraw goes to Windows' own ddraw.dll instead. See docs/game/renderer.md.
		enum class DDrawCompatState { Untouched, KeptOut, NotDDrawCompat, AlreadyStarted };
		DDrawCompatState ddrawCompatState = DDrawCompatState::Untouched;
		bool SectionsHave(std::uintptr_t base, const char* text) {
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
			const auto* sections = IMAGE_FIRST_SECTION(nt);
			const std::size_t length = std::strlen(text);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
				if ((sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
				const auto* start = reinterpret_cast<const char*>(base + sections[i].VirtualAddress);
				const std::size_t size = sections[i].Misc.VirtualSize;
				for (std::size_t at = 0; at + length <= size; ++at) if (std::memcmp(start + at, text, length) == 0) return true;
			}
			return false;
		}
		// Points patch.dll's ddraw.dll imports at Windows' own ddraw.dll.
		bool UseSystemDDraw() {
			wchar_t path[MAX_PATH]{};
			const UINT length = GetSystemDirectoryW(path, MAX_PATH);
			if (length == 0 || length + 11 >= MAX_PATH) return false;
			std::wcscat(path, L"\\ddraw.dll");
			// A different file from the one already loaded under the same name, so Windows loads it as well.
			HMODULE system = LoadLibraryW(path);
			const auto patch = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"patch.dll"));
			if (!system || !patch) return false;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(patch);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(patch + dos->e_lfanew);
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			int redirected = 0;
			for (const auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(patch + directory.VirtualAddress); entry->Name != 0; ++entry) {
				if (_stricmp(reinterpret_cast<const char*>(patch + entry->Name), "ddraw.dll") != 0) continue;
				const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(patch + (entry->OriginalFirstThunk ? entry->OriginalFirstThunk : entry->FirstThunk));
				auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(patch + entry->FirstThunk);
				for (; names->u1.AddressOfData != 0; ++names, ++slots) {
					const char* name = IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)
						? reinterpret_cast<const char*>(static_cast<std::uintptr_t>(IMAGE_ORDINAL32(names->u1.Ordinal)))
						: reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(patch + names->u1.AddressOfData)->Name;
					if (const FARPROC function = GetProcAddress(system, name)) {
						Memory::Write<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&slots->u1.Function), reinterpret_cast<std::uint32_t>(function));
						++redirected;
					}
				}
			}
			return redirected > 0;
		}

		// The game is not DPI-aware, so on a scaled desktop (such as 4K at 300%) Windows gives its window and the
		// monitor smaller "logical" sizes (1280x720 there, seen in the log) and stretches the picture up afterwards: the
		// game drew 3840x2160 into a 1280x720 swap chain. Saying the process handles DPI itself makes every size real
		// pixels. Only window sizes and positions change; the game's own screen modes were already real pixels.
		// Per-monitor v2 (Windows 10 1703) first, then the older calls. Before any window of ours is made.
		void MakeDpiAware() {
			using ContextFn = BOOL(WINAPI*)(HANDLE);
			// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 and _PER_MONITOR_AWARE, as numbers so no newer SDK is needed.
			const auto perMonitorV2 = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4));
			const auto perMonitor = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-3));
			const HMODULE user32 = GetModuleHandleW(L"user32.dll");
			if (const auto setContext = user32 ? reinterpret_cast<ContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr) {
				if (setContext(perMonitorV2) || setContext(perMonitor)) {
					Logger::Debug("Renderer: the game is DPI-aware (per monitor); its window uses real pixels.");
					return;
				}
				if (GetLastError() == ERROR_ACCESS_DENIED) {
					Logger::Debug("Renderer: the game's DPI awareness was already set (by Windows' compatibility settings or another DLL); left as it is.");
					return;
				}
			}
			if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
				using AwarenessFn = HRESULT(WINAPI*)(int);
				// PROCESS_PER_MONITOR_DPI_AWARE = 2 (Windows 8.1).
				if (const auto setAwareness = reinterpret_cast<AwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness")); setAwareness && SUCCEEDED(setAwareness(2))) {
					Logger::Debug("Renderer: the game is DPI-aware (per monitor, Windows 8.1 way); its window uses real pixels.");
					return;
				}
			}
			if (SetProcessDPIAware()) Logger::Debug("Renderer: the game is DPI-aware (system); its window uses real pixels.");
			else Logger::Warning("Renderer: the game could not be made DPI-aware; on a scaled desktop its picture is stretched by Windows.");
		}

		void PatchImports() {
			const auto create = Game::Resolve(Game::Address::DirectDrawCreateImport);
			const auto enumerate = Game::Resolve(Game::Address::DirectDrawEnumerateImport);
			Memory::Write<std::uint32_t>(create, reinterpret_cast<std::uint32_t>(&Render::Layer::DirectDrawCreate));
			Memory::Write<std::uint32_t>(enumerate, reinterpret_cast<std::uint32_t>(&Render::Layer::DirectDrawEnumerateA));
			importsPatched = true;
		}
	}

	void Renderer::BeforeDllsStart() {
		// Windows' loader lock is held here: no logging, no loading; Install reports what happened. The launcher sets this,
		// so it tells this is a start through the launcher. Every renderer is our own now, whatever it says.
		wchar_t word[32]{};
		const DWORD length = GetEnvironmentVariableW(L"KEEPERFORGE2_RENDERER", word, 32);
		if (length == 0 || length >= 32) return;
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"ddraw.dll"));
		if (!base) return;
		if (!SectionsHave(base, "DDrawCompat loaded successfully")) {
			ddrawCompatState = DDrawCompatState::NotDDrawCompat;
			return;
		}
		// It loads these itself when it installs its hooks.
		if (GetModuleHandleW(L"d3dim700.dll")) {
			ddrawCompatState = DDrawCompatState::AlreadyStarted;
			return;
		}
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
		// mov eax, 1 / ret 12: its DllMain, reporting success without doing anything.
		Memory::Patch(base + nt->OptionalHeader.AddressOfEntryPoint, { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00 });
		ddrawCompatState = DDrawCompatState::KeptOut;
	}

	namespace {
		// bump_mapping: the game's own bump mapping of water and lava (its textures, its texture stages), drawn by our
		// layer. The game's setting is not changed: the two places that read it get the mode instead. bump_mapping all,
		// water (water tiles and the Temple's pool) or lava; bump_luminance picks the game's luminance kind.
		// ---- Bump mapping of a room's own liquid (the Temple's pool). Water tiles (0x5A2570) get the engine's bump water
		// when its bump flag (0x10 in 0x764BB8) is on and the water setting at 0x759AE0 is 0: the BumpWater texture, its
		// bump and environment maps, flag 0x8000 (+ 1 when 0x759AD8 is set) and full opacity. A room's liquid (0x5A2F10,
		// the Temple's "Water") is always made plain; its surface is created by the call at 0x5A31E0 (0x5873A0, with the
		// engine object in EBP). The stub gives it what water tiles get, so the pool matches the water around it.
		std::uintptr_t liquidSurfaceCreate = 0;
		const std::uint8_t* engineFlags = nullptr;
		const std::uint32_t* waterSettingTranslucent = nullptr;
		const std::uint32_t* waterSettingSimple = nullptr;
		// The surface's stack arguments: 4 texture, 5 bump map, 6 environment map, 7 flags, 8 opacity (float).
		void __cdecl RoomLiquidBump(const std::uint8_t* engine, std::uint32_t* arguments) {
			if (engine == nullptr || (*engineFlags & 0x10) == 0 || *waterSettingSimple != 0) return;
			const auto field = [engine](std::uintptr_t offset) { return *reinterpret_cast<const std::uint32_t*>(engine + offset); };
			if (field(0x72) == 0 || field(0x76) == 0 || field(0x7A) == 0) return;
			arguments[4] = field(0x72);
			arguments[5] = field(0x76);
			arguments[6] = field(0x7A);
			arguments[7] |= 0x8000u | (*waterSettingTranslucent != 0 ? 1u : 0u);
			const float opaque = 1.0f;
			std::memcpy(&arguments[8], &opaque, sizeof(opaque));
		}
		__declspec(naked) void RoomLiquidSurfaceStub() {
			__asm {
				pushad
				lea eax, [esp + 0x24]
				push eax
				push ebp
				call RoomLiquidBump
				add esp, 8
				popad
				jmp dword ptr [liquidSurfaceCreate]
			}
		}
		void BumpRoomLiquids() {
			const auto call = Game::Resolve(Game::Address::RoomLiquidSurfaceCreate);
			liquidSurfaceCreate = Game::Resolve(Game::Address::LiquidSurfaceCreate);
			engineFlags = reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::EngineBumpFlags));
			waterSettingTranslucent = reinterpret_cast<const std::uint32_t*>(Game::Resolve(Game::Address::WaterSettingTranslucent));
			waterSettingSimple = reinterpret_cast<const std::uint32_t*>(Game::Resolve(Game::Address::WaterSettingSimple));
			// push esi / push edx / mov ecx, eax / call 0x5873A0
			if (!Memory::Matches(call - 4, { 0x56, 0x52, 0x8B, 0xC8, 0xE8, 0xBB, 0x41, 0xFE, 0xFF })
				|| !Memory::WriteCall(call, reinterpret_cast<const void*>(&RoomLiquidSurfaceStub))) {
				Logger::Warning("Renderer: unexpected game code, the Temple's pool is not bump mapped.");
			}
		}

		// Keeps one liquid flat: its "jz" past the bump values becomes "jmp".
		bool KeepFlat(std::uintptr_t address, std::uint8_t distance) {
			const auto jump = Game::Resolve(address);
			if (!Memory::Matches(jump, { 0x74, distance })) return false;
			Memory::Patch(jump, { 0xEB, distance });
			return true;
		}

		void ApplyBumpMapping() {
			auto word = StringUtils::ToLower(Config::GetString("bump_mapping", "all"));
			auto luminanceWord = StringUtils::ToLower(Config::GetString("bump_luminance", "off"));
			// The old words: on is all; luminance is all with bump_luminance on.
			if (word == "on" || word == "luminance") {
				if (word == "luminance") {
					luminanceWord = "on";
					Config::SetString("bump_luminance", "on");
				}
				word = "all";
				Config::SetString("bump_mapping", "all");
			}
			if (word != "all" && word != "water" && word != "lava" && word != "off") {
				Logger::Warning("Renderer: \"" + word + "\" in config.yml is not all, water, lava or off; using all and writing that to config.yml.");
				Config::SetString("bump_mapping", "all");
				word = "all";
			}
			if (luminanceWord != "on" && luminanceWord != "off") {
				Logger::Warning("Renderer: bump_luminance \"" + luminanceWord + "\" in config.yml is not on or off; using off and writing that to config.yml.");
				Config::SetString("bump_luminance", "off");
				luminanceWord = "off";
			}
			auto templeWord = StringUtils::ToLower(Config::GetString("bump_temple", "on"));
			if (templeWord != "on" && templeWord != "off") {
				Logger::Warning("Renderer: bump_temple \"" + templeWord + "\" in config.yml is not on or off; using on and writing that to config.yml.");
				Config::SetString("bump_temple", "on");
				templeWord = "on";
			}
			const int mode = word == "off" ? 0 : (luminanceWord == "on" ? 2 : 1);
			const bool water = word == "all" || word == "water";
			const bool lava = word == "all" || word == "lava";
			// bump_temple off: the Temple's pool stays flat (the game's own look) while the water tiles are bumped.
			const bool temple = water && templeWord == "on";
			Render::Layer::SetBumpMapping(mode);
			if (mode == 0) return;
			const auto setting = static_cast<std::uint32_t>(Game::Resolve(Game::Address::BumpSetting));
			const std::vector<std::uint8_t> read = { 0xA1, static_cast<std::uint8_t>(setting), static_cast<std::uint8_t>(setting >> 8),
				static_cast<std::uint8_t>(setting >> 16), static_cast<std::uint8_t>(setting >> 24) };
			const auto textures = Game::Resolve(Game::Address::BumpSettingTexturesRead);
			const auto engine = Game::Resolve(Game::Address::BumpSettingEngineRead);
			if (!Memory::Matches(textures, read) || !Memory::Matches(engine, read)) {
				Render::Layer::SetBumpMapping(0);
				Logger::Warning("Renderer: unexpected game code, bump mapping stays off.");
				return;
			}
			// mov eax, mode
			const std::vector<std::uint8_t> value = { 0xB8, static_cast<std::uint8_t>(mode), 0x00, 0x00, 0x00 };
			Memory::Patch(textures, value);
			Memory::Patch(engine, value);
			if (!water && !KeepFlat(Game::Address::WaterBumpSkip, 0x27)) {
				Logger::Warning("Renderer: unexpected game code, water stays bump mapped.");
			}
			if (!lava && !KeepFlat(Game::Address::LavaBumpSkip, 0x11)) {
				Logger::Warning("Renderer: unexpected game code, lava stays bump mapped.");
			}
			if (temple) BumpRoomLiquids();
			const std::string waterText = temple ? "water and the Temple's pool" : "water (not the Temple's pool)";
			Logger::Info(std::string("Renderer: bump mapping of ") + (word == "all" ? waterText + " and lava" : water ? waterText : "lava")
				+ " on (" + (mode == 2 ? "with luminance" : "du/dv") + ").");
		}
	}

	void Renderer::Install() {
		const auto word = Config::GetString("renderer", "vulkan");
		if (const auto chosen = Render::ApiFromConfigName(word)) configured = *chosen;
		else {
			// Such as "original", which is no longer offered: the file gets the default, so it says what is used.
			configured = Render::DefaultApi;
			Config::SetString("renderer", Render::ApiConfigName(configured));
			Logger::Warning("Renderer: \"" + word + "\" in config.yml is not dx11, dx12 or vulkan; using " + Render::ApiName(configured) + " and writing that to config.yml.");
		}
		switch (ddrawCompatState) {
			case DDrawCompatState::KeptOut:
				Logger::Debug("Renderer: DDrawCompat (the game folder's ddraw.dll) was kept from starting.");
				// Nothing may reach DDrawCompat now that it never started: patch.dll's DirectDraw goes to Windows' own.
				if (!UseSystemDDraw()) Logger::Warning("Renderer: patch.dll could not be pointed at Windows' ddraw.dll.");
				break;
			case DDrawCompatState::NotDDrawCompat:
				Logger::Debug("Renderer: the game folder's ddraw.dll is not DDrawCompat; it is left as it is.");
				break;
			case DDrawCompatState::AlreadyStarted:
				Logger::Warning("Renderer: DDrawCompat (ddraw.dll) had already started; the picture may stay black this session.");
				break;
			case DDrawCompatState::Untouched:
				if (GetModuleHandleW(L"d3dim700.dll")) {
					Logger::Warning("Renderer: DDrawCompat (ddraw.dll) started before KeeperForge2 (the game was not started by KeeperForge2's launcher, or the launcher could not load it first); the picture may stay black this session.");
				}
				break;
		}
		Render::Layer::SetVsync(StringUtils::ToLower(Config::GetString("frame_rate", "60")) == "vsync");
		const auto windowMode = StringUtils::ToLower(Config::GetString("window_mode", "borderless"));
		if (windowMode != "borderless" && windowMode != "fullscreen") {
			Logger::Warning("Renderer: \"" + windowMode + "\" in config.yml is not borderless or fullscreen; using borderless and writing that to config.yml.");
			Config::SetString("window_mode", "borderless");
		}
		Render::Layer::SetBorderless(windowMode != "fullscreen");
		const auto antiAliasingWord = Config::GetString("anti_aliasing", Render::AntiAliasingConfigName(Render::DefaultAntiAliasing));
		auto antiAliasing = Render::AntiAliasingFromConfigName(antiAliasingWord);
		if (!antiAliasing) {
			antiAliasing = Render::DefaultAntiAliasing;
			Logger::Warning("Renderer: \"" + antiAliasingWord + "\" in config.yml is not off, fxaa, smaa or msaa with x2, x4 or x8; using "
				+ Render::AntiAliasingConfigName(*antiAliasing) + " and writing that to config.yml.");
			Config::SetString("anti_aliasing", Render::AntiAliasingConfigName(*antiAliasing));
		}
		Render::Layer::SetAntiAliasing(*antiAliasing);
		// texture_filtering: off (the game's own: no mip levels), trilinear (mip levels), or anisotropic x2 to x16 (mip
		// levels and anisotropic sampling).
		auto filteringWord = StringUtils::ToLower(Config::GetString("texture_filtering", "anisotropic x16"));
		std::uint32_t anisotropy = 0;
		if (filteringWord == "trilinear") anisotropy = 1;
		else if (filteringWord.starts_with("anisotropic x")) {
			const auto times = filteringWord.substr(13);
			if (times == "2" || times == "4" || times == "8" || times == "16") anisotropy = static_cast<std::uint32_t>(std::stoul(times));
		}
		if (anisotropy == 0 && filteringWord != "off") {
			Logger::Warning("Renderer: \"" + filteringWord + "\" in config.yml is not off, trilinear or anisotropic x2, x4, x8 or x16; using anisotropic x16 and writing that to config.yml.");
			Config::SetString("texture_filtering", "anisotropic x16");
			filteringWord = "anisotropic x16";
			anisotropy = 16;
		}
		Render::Layer::SetTextureFiltering(anisotropy != 0, anisotropy == 0 ? 1 : anisotropy);
		// model_smoothing: off, low, medium or high (each curved edge of the 3D world cut into 2, 3 or 4).
		auto smoothingWord = StringUtils::ToLower(Config::GetString("model_smoothing", "medium"));
		if (smoothingWord != "off" && smoothingWord != "low" && smoothingWord != "medium" && smoothingWord != "high") {
			Logger::Warning("Renderer: model_smoothing \"" + smoothingWord + "\" in config.yml is not off, low, medium or high; using medium and writing that to config.yml.");
			Config::SetString("model_smoothing", "medium");
			smoothingWord = "medium";
		}
		const std::uint32_t smoothingSegments = smoothingWord == "low" ? 2 : smoothingWord == "medium" ? 3 : smoothingWord == "high" ? 4 : 0;
		Render::Layer::SetModelSmoothing(smoothingSegments);
		if (smoothingSegments != 0) Logger::Info("Renderer: model smoothing " + smoothingWord + " (curved edges cut into " + std::to_string(smoothingSegments) + ").");
		Logger::Info("Renderer: texture filtering " + filteringWord + (anisotropy != 0 ? " (mip levels made by KeeperForge2)." : " (the game's own)."));
		const auto gpuBlitsWord = StringUtils::ToLower(Config::GetString("gpu_blits", "on"));
		const bool gpuBlits = gpuBlitsWord != "off" && gpuBlitsWord != "clears";
		const bool gpuClears = gpuBlitsWord != "off";
		Render::Layer::SetGpuBlits(gpuBlits, gpuClears);
		const bool rayTracing = StringUtils::ToLower(Config::GetString("ray_tracing", "off")) == "on";
		const bool rayOcclusion = StringUtils::ToLower(Config::GetString("ray_tracing_occlusion", "on")) == "on";
		const bool rayShadows = StringUtils::ToLower(Config::GetString("ray_tracing_shadows", "on")) == "on";
		const bool rayReflections = StringUtils::ToLower(Config::GetString("ray_tracing_reflections", "on")) == "on";
		const bool rayBounce = StringUtils::ToLower(Config::GetString("ray_tracing_bounce", "on")) == "on";
		const auto rayQuality = StringUtils::ToLower(Config::GetString("ray_tracing_quality", "medium"));
		const std::uint32_t rayDivisor = rayQuality == "high" ? 2 : rayQuality == "low" ? 4 : 3;
		// Traces a second (0: every frame).
		std::uint32_t rayRate = 30;
		{
			const auto rateWord = StringUtils::ToLower(Config::GetString("ray_tracing_rate", "30"));
			if (rateWord == "0") rayRate = 0;
			else {
				try { rayRate = static_cast<std::uint32_t>(std::stoul(rateWord)); }
				catch (...) { rayRate = 30; }
				rayRate = std::clamp<std::uint32_t>(rayRate, 5, 240);
			}
		}
		Render::Layer::SetRayTracing(rayTracing, rayOcclusion, rayShadows, rayReflections, rayBounce, rayDivisor, rayRate);
		if (rayTracing) {
			Logger::Info(std::string("Renderer: ray_tracing is on (our own compute-shader tracer): ambient occlusion ") + (rayOcclusion ? "on" : "off")
				+ ", shadows " + (rayShadows ? "on" : "off") + ", reflections " + (rayReflections ? "on" : "off") + ", bounce light " + (rayBounce ? "on" : "off")
				+ ", quality " + rayQuality
				+ " (1/" + std::to_string(rayDivisor) + " of the screen), " + (rayRate == 0 ? std::string("traced every frame") : "traced up to " + std::to_string(rayRate) + " times a second") + ".");
		}
		if (!gpuClears) Logger::Info("Renderer: gpu_blits is off; the game's blits and clears of the frame are done in memory.");
		else if (!gpuBlits) Logger::Info("Renderer: gpu_blits is clears; whole-frame clears are done on the graphics card, blits in memory.");
		if (Memory::Read<std::uintptr_t>(Game::Resolve(Game::Address::GameDirectDraw)) != 0) {
			Logger::Warning(std::string("Renderer: the game had already set up its display before KeeperForge2 started (it was not started by KeeperForge2's launcher), so ")
				+ Render::ApiName(configured) + " cannot draw it this time.");
			return;
		}
		// Before the test window below and the game's own window are made.
		MakeDpiAware();
		// The imports first, so the game finds our DirectDraw even if it sets up its display while the API is tried.
		PatchImports();
		ApplyBumpMapping();
		if (Render::Layer::Select(configured)) {
			Logger::Info(std::string("Renderer: ") + Render::ApiName(configured) + " will draw the game.");
			return;
		}
		// The others in turn. If none can, the layer tries them all again on the game's window when it first draws.
		for (const Api other : { Api::D3D11, Api::Vulkan, Api::D3D12 }) {
			if (other == configured || !Render::Layer::Select(other)) continue;
			Logger::Warning(std::string("Renderer: ") + Render::ApiName(configured) + " cannot run here, " + Render::ApiName(other) + " will draw the game.");
			return;
		}
		Logger::Warning(std::string("Renderer: none of DirectX 11, DirectX 12 or Vulkan could start on a test window; they are tried again on the game's window."));
	}

	bool Renderer::IsActive() { return importsPatched && Render::Layer::IsInUse(); }

	bool Renderer::IsChosen() { return importsPatched; }

	void Renderer::SetStretch(bool stretch) { Render::Layer::SetStretch(stretch); }

	void Renderer::SetVideoArea(VideoAreaFn area) { Render::Layer::SetVideoArea(area); }

	void Renderer::WindowPointToScreen(void* window, std::int32_t screenWidth, std::int32_t screenHeight, std::int32_t& x, std::int32_t& y) {
		Render::Layer::WindowPointToPicture(static_cast<HWND>(window), screenWidth, screenHeight, x, y);
	}
}
