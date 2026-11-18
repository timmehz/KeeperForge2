#include "FrameRate.h"
#include "BackgroundRunning.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "StringUtils.h"
#include <atomic>
#include <charconv>
#include <cstdint>
#include <string>
#include <vector>
#include <Windows.h>
#include <timeapi.h>

namespace KeeperForge2 {
	namespace {
		// 0 means no limit.
		std::uint32_t framesPerSecond = 0;
		bool limitFrames = false;
		LARGE_INTEGER counterFrequency{};
		std::int64_t nextFrame = 0;
		// Frame rate measurement for the log.
		std::int64_t measureStart = 0;
		std::uint32_t measuredFrames = 0;

		std::int64_t Now() {
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			return counter.QuadPart;
		}
		// Waits until the next frame is due. Sleeps for most of the wait and spins for the last moment, since Sleep
		// is only accurate to about a millisecond.
		void WaitForNextFrame() {
			const std::int64_t period = counterFrequency.QuadPart / framesPerSecond;
			std::int64_t now = Now();
			// First frame, or the game fell well behind (loading, a slow frame): start counting from now.
			if (nextFrame == 0 || now - nextFrame > period * 4) {
				nextFrame = now + period;
				return;
			}
			while (now < nextFrame) {
				const auto remainingMs = (nextFrame - now) * 1000 / counterFrequency.QuadPart;
				if (remainingMs > 2) { Sleep(static_cast<DWORD>(remainingMs - 1)); }
				else { YieldProcessor(); }
				now = Now();
			}
			nextFrame += period;
		}
		void CountFrame() {
			const std::int64_t now = Now();
			if (measureStart == 0) { measureStart = now; }
			++measuredFrames;
			const std::int64_t elapsed = now - measureStart;
			if (elapsed >= counterFrequency.QuadPart * 5) {
				Logger::Debug("Frame rate: " + std::to_string(measuredFrames * counterFrequency.QuadPart / elapsed) + " fps over the last 5 seconds.");
				measureStart = now;
				measuredFrames = 0;
			}
		}

		// thiscall, no arguments: shows the finished frame.
		using PresentFrameFn = void(__fastcall*)(void* game, void* unused);
		PresentFrameFn originalPresentFrame = nullptr;
		bool presentHooked = false;
		void (*beforePresent)() = nullptr;
		// For the console's fps command (PerformanceStats), read on another thread.
		std::atomic<std::uint32_t> framesShown = 0;
		// Frame times for it: present to present, so a frame limiter's wait is in them. Written on the game's thread,
		// taken (and started again) by the stats thread.
		constexpr std::uint32_t NoFrameTime = 0xFFFFFFFF;
		std::atomic<std::uint32_t> shortestFrameUs = NoFrameTime;
		std::atomic<std::uint32_t> longestFrameUs = 0;
		std::int64_t lastPresent = 0;
		void TimeFrame() {
			const std::int64_t now = Now();
			const std::int64_t previous = lastPresent;
			lastPresent = now;
			if (previous == 0 || counterFrequency.QuadPart == 0) return;
			const std::int64_t us = (now - previous) * 1'000'000 / counterFrequency.QuadPart;
			if (us <= 0 || us >= 1'000'000) return;
			const auto value = static_cast<std::uint32_t>(us);
			std::uint32_t seen = shortestFrameUs.load(std::memory_order_relaxed);
			while (value < seen && !shortestFrameUs.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
			seen = longestFrameUs.load(std::memory_order_relaxed);
			while (value > seen && !longestFrameUs.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
		}
		void __fastcall PresentFrameDetour(void* game, void* unused) {
			if (beforePresent) { beforePresent(); }
			// While the player cannot see the game it may run at 1 fps instead (BackgroundRunning).
			if (!BackgroundRunning::WaitWhileUnseen() && limitFrames && framesPerSecond != 0) { WaitForNextFrame(); }
			originalPresentFrame(game, unused);
			framesShown.fetch_add(1, std::memory_order_relaxed);
			TimeFrame();
			CountFrame();
		}

		// patch.dll's Flip reads its "Vwait" setting with "mov eax, [setting]"; clearing the setting stops its extra
		// wait for the vertical blank. Checked against the instruction first, so another patch.dll is left alone.
		bool DisableGogVerticalBlankWait() {
			const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"patch.dll"));
			if (!module) { return false; }
			const auto read = module + Game::GogPatch::FlipReadsVwait;
			const auto setting = module + Game::GogPatch::VwaitSetting;
			const auto* code = reinterpret_cast<const std::uint8_t*>(read);
			if (code[0] != 0xA1 || *reinterpret_cast<const std::uintptr_t*>(code + 1) != setting) {
				Logger::Warning("Frame rate: patch.dll is not the version we know, its vsync wait stays on.");
				return false;
			}
			Memory::Write<std::uint32_t>(setting, 0);
			return true;
		}

	}

	void FrameRate::Initialize() {
		const std::string setting = StringUtils::ToLower(Config::GetString("frame_rate", "60"));
		std::uint32_t value = 0;
		const bool vsync = setting == "vsync";
		if (!vsync) {
			const auto parsed = std::from_chars(setting.data(), setting.data() + setting.size(), value);
			if (parsed.ec != std::errc{} || parsed.ptr != setting.data() + setting.size() || (value != 0 && (value < 15 || value > 1000))) {
				Logger::Warning("Frame rate: frame_rate '" + setting + "' in config.yml must be vsync, 0 (no limit) or 15 to 1000, using 60.");
				value = 60;
			}
		}
		// The present hook is also used for screenshots, so it goes in even when the frame rate is left alone.
		const auto target = Game::Resolve(Game::Address::PresentFrame);
		if (!Memory::Matches(target, { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x7D, 0x0E, 0x00, 0x00 })) {
			Logger::Warning("Frame rate: unexpected code at the frame present, keeping the game's own frame pacing.");
			return;
		}
		QueryPerformanceFrequency(&counterFrequency);
		if (!HookManager::CreateHook(target, reinterpret_cast<void*>(&PresentFrameDetour), reinterpret_cast<void**>(&originalPresentFrame)) || !HookManager::EnableHook(target)) {
			HookManager::RemoveHook(target);
			Logger::Warning("Frame rate: failed to hook the frame present, keeping the game's own frame pacing.");
			return;
		}
		presentHooked = true;
		if (vsync) {
			Logger::Info("Frame rate: vsync in config.yml, keeping the game's own frame pacing.");
			return;
		}
		// Makes Sleep accurate to about a millisecond.
		timeBeginPeriod(1);
		framesPerSecond = value;
		limitFrames = true;
		DisableGogVerticalBlankWait();
		Logger::Info(value == 0 ? std::string("Frame rate: no limit.") : "Frame rate: limited to " + std::to_string(value) + " fps.");
	}
	bool FrameRate::IsPresentHooked() { return presentHooked; }
	void FrameRate::SetBeforePresent(void (*callback)()) { beforePresent = callback; }
	std::uint32_t FrameRate::FrameCount() { return framesShown.load(std::memory_order_relaxed); }
	bool FrameRate::TakeFrameTimes(std::uint32_t& shortestUs, std::uint32_t& longestUs) {
		shortestUs = shortestFrameUs.exchange(NoFrameTime, std::memory_order_relaxed);
		longestUs = longestFrameUs.exchange(0, std::memory_order_relaxed);
		return shortestUs != NoFrameTime && longestUs != 0;
	}
}
