#include "BackgroundRunning.h"
#include "Config.h"
#include "GameAddresses.h"
#include "Logger.h"
#include "Memory.h"
#include "StringUtils.h"
#include <atomic>
#include <string>
#include <Windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

namespace KeeperForge2 {
	namespace {
		bool enabled = false;
		std::atomic<bool> seen = true;
		ULONGLONG lastFrame = 0;
		constexpr ULONGLONG UnseenFrameMs = 1000;
		constexpr std::int32_t PlayModeCampaign = 1;
		constexpr std::int32_t PlayModeSkirmish = 2;

		// ---- Sound: the game's audio session (what Windows' volume mixer shows for it) is muted on every output device.
		// The game's sound libraries are left alone. COM is used on a thread of our own, so the game's threads keep
		// whatever COM setup they have.
		std::atomic<bool> wantMuted = false;
		HANDLE muteChanged = nullptr;
		void ApplyMute(bool mute) {
			IMMDeviceEnumerator* devices = nullptr;
			if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices)))) return;
			IMMDeviceCollection* outputs = nullptr;
			if (SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &outputs))) {
				UINT count = 0;
				outputs->GetCount(&count);
				for (UINT i = 0; i < count; ++i) {
					IMMDevice* device = nullptr;
					if (FAILED(outputs->Item(i, &device))) continue;
					IAudioSessionManager* sessions = nullptr;
					if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessions)))) {
						ISimpleAudioVolume* volume = nullptr;
						// The default session of this process on that device.
						if (SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume))) {
							volume->SetMute(mute ? TRUE : FALSE, nullptr);
							volume->Release();
						}
						sessions->Release();
					}
					device->Release();
				}
				outputs->Release();
			}
			devices->Release();
		}
		DWORD WINAPI MuteThread(LPVOID) {
			// Lives as long as the game, so COM is never uninitialized here.
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			// Windows keeps a program's mute between runs: a game closed while muted would start muted, so the first
			// pass unmutes.
			bool muted = true;
			for (;;) {
				const bool mute = wantMuted.load();
				if (mute != muted) {
					ApplyMute(mute);
					muted = mute;
				}
				WaitForSingleObject(muteChanged, INFINITE);
			}
		}

		bool SlowsWhileUnseen() {
			const auto mode = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::PlayMode));
			return mode == PlayModeCampaign || mode == PlayModeSkirmish;
		}
	}

	void BackgroundRunning::Initialize() {
		if (StringUtils::ToLower(Config::GetString("run_in_background", "on")) == "off") {
			Logger::Info("Background running: off, the game pauses when it loses focus.");
			return;
		}
		enabled = true;
		muteChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (muteChanged) {
			if (HANDLE thread = CreateThread(nullptr, 0, &MuteThread, nullptr, 0, nullptr)) CloseHandle(thread);
		}
		// Whether one of our renderers draws the game is known only once the game makes its DirectDraw.
		Logger::Info("Background running: with renderer dx11, dx12 or vulkan the game keeps running when minimized; while unseen its sound is muted"
			" and campaign and skirmish run at 1 fps.");
	}
	// Only our renderer's window code asks and tells (it takes no lock here: the window's messages can come while a
	// present holds the renderer's lock), so with the original renderer the game is always "seen" and nothing changes.
	bool BackgroundRunning::KeepsRunning() { return enabled; }
	void BackgroundRunning::SetSeen(bool nowSeen) {
		if (!enabled || seen.exchange(nowSeen) == nowSeen) return;
		Logger::Debug(nowSeen ? "Background running: the game is seen again, back to its frame rate and sound."
			: (SlowsWhileUnseen() ? "Background running: the game is not seen, 1 fps and no sound until it is."
				: "Background running: the game is not seen, no sound until it is (multiplayer keeps its frame rate)."));
		wantMuted = !nowSeen;
		if (muteChanged) SetEvent(muteChanged);
	}
	bool BackgroundRunning::IsSeen() { return !enabled || seen.load(); }
	bool BackgroundRunning::WaitWhileUnseen() {
		if (!enabled) return false;
		// The game's window gets its messages on this thread, so it hears it is seen again only after the wait: coming
		// back can take up to a second.
		bool waited = false;
		const ULONGLONG now = GetTickCount64();
		if (!seen.load() && SlowsWhileUnseen() && now - lastFrame < UnseenFrameMs) {
			Sleep(static_cast<DWORD>(UnseenFrameMs - (now - lastFrame)));
			waited = true;
		}
		lastFrame = GetTickCount64();
		return waited;
	}
}
