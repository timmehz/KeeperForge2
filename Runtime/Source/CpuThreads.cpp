#include "CpuThreads.h"
#include "Config.h"
#include "Logger.h"
#include "StringUtils.h"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		constexpr int DefaultThreads = 4;
		constexpr int AllThreads = 32;

		struct Core { DWORD mask; BYTE efficiency; };
		// The cores this process may use, fastest kind first (on a CPU with performance and efficiency cores).
		std::vector<Core> UsableCores(DWORD usable) {
			DWORD size = 0;
			GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
			std::vector<std::uint8_t> buffer(size);
			auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
			if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &size)) return {};
			std::vector<Core> cores;
			for (DWORD offset = 0; offset < size;) {
				const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
				offset += entry->Size;
				const auto& core = entry->Processor;
				if (core.GroupCount == 0 || core.GroupMask[0].Group != 0) continue;
				const DWORD mask = static_cast<DWORD>(core.GroupMask[0].Mask) & usable;
				if (mask != 0) cores.push_back({ mask, core.EfficiencyClass });
			}
			std::ranges::stable_sort(cores, [](const Core& a, const Core& b) { return a.efficiency > b.efficiency; });
			return cores;
		}
		// Whole cores for as long as they fit, so 4 threads on a CPU with hyper-threading are 2 cores.
		DWORD ChooseThreads(DWORD usable, int wanted) {
			DWORD chosen = 0;
			for (const auto& core : UsableCores(usable)) {
				for (DWORD left = core.mask; left != 0 && std::popcount(chosen) < wanted; left &= left - 1) chosen |= left & (~left + 1);
			}
			// Without the cores' layout: the first ones.
			for (DWORD left = usable; left != 0 && std::popcount(chosen) < wanted; left &= left - 1) chosen |= left & (~left + 1);
			return chosen;
		}
		// 0: leave what the game set.
		int WantedThreads() {
			const auto value = StringUtils::ToLower(Config::GetString("cpu_threads", std::to_string(DefaultThreads)));
			if (value == "game") return 0;
			if (value == "all") return AllThreads;
			int number = 0;
			const auto* end = value.data() + value.size();
			const auto [parsed, error] = std::from_chars(value.data(), end, number);
			if (error == std::errc{} && parsed == end && number >= 1) return number;
			Logger::Warning("CPU threads: cpu_threads should be a number, all or game, so it is " + std::to_string(DefaultThreads) + ".");
			return DefaultThreads;
		}
	}

	void CpuThreads::Install() {
		const int wanted = WantedThreads();
		const HANDLE process = GetCurrentProcess();
		DWORD_PTR before = 0;
		DWORD_PTR system = 0;
		if (!GetProcessAffinityMask(process, &before, &system)) {
			Logger::Warning("CPU threads: could not read which ones the game may use.");
			return;
		}
		if (wanted == 0) {
			Logger::Info(std::format("CPU threads: left as the game set them, {} of {}.", std::popcount(static_cast<DWORD>(before)), std::popcount(static_cast<DWORD>(system))));
			return;
		}
		const DWORD chosen = ChooseThreads(static_cast<DWORD>(system), wanted);
		DWORD_PTR after = 0;
		// DDrawCompat, which the original renderer keeps, can stand in the way of this, so what counts is what is read back.
		if (chosen == 0 || !SetProcessAffinityMask(process, chosen) || !GetProcessAffinityMask(process, &after, &system) || after != chosen) {
			Logger::Warning(std::format("CPU threads: could not change them, the game may use mask {:#x} of {:#x}.", after != 0 ? after : before, system));
			return;
		}
		Logger::Info(std::format("CPU threads: {} of {} (mask {:#x}, it was {:#x}).", std::popcount(chosen), std::popcount(static_cast<DWORD>(system)), chosen, before));
	}
}
