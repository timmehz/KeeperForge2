#include "PerformanceStats.h"
#include "BackgroundRunning.h"
#include "Config.h"
#include "FrameRate.h"
#include "Logger.h"
#include "MemoryWatch.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <Windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <Psapi.h>

namespace KeeperForge2 {
	namespace {
		constexpr DWORD SampleMs = 1000;
		constexpr double Mb = 1024.0 * 1024.0;

		// Minimum, maximum, current and average of the samples since the last reset.
		struct Figure {
			double minimum = 0;
			double maximum = 0;
			double current = 0;
			double sum = 0;
			std::uint64_t count = 0;
			void Add(double value) {
				minimum = count == 0 ? value : (std::min)(minimum, value);
				maximum = count == 0 ? value : (std::max)(maximum, value);
				current = value;
				sum += value;
				++count;
			}
			double Average() const { return count == 0 ? 0 : sum / static_cast<double>(count); }
		};

		struct CpuState {
			Figure game;           // percent of every logical processor, as Task Manager shows it
			double oneCore = 0;    // percent of one logical processor
			double system = 0;     // the whole machine, every program
			std::uint32_t processors = 0;
			ULONGLONG since = 0;
		};
		struct RamState {
			Figure workingSet;     // MB of the game's memory in RAM
			double privateMb = 0;  // MB the game has committed for itself
			MemoryWatch::AddressSpace addressSpace{};
			double systemUsedMb = 0;
			double systemTotalMb = 0;
			ULONGLONG since = 0;
		};
		struct GpuState {
			Figure load;           // percent, busiest engine (as Task Manager shows it)
			bool loadKnown = false;
			Figure vram;           // MB of the graphics card's own memory the game uses
			bool vramKnown = false;
			double sharedMb = 0;   // MB of system memory the graphics card uses for the game
			double budgetMb = 0;   // MB of video memory Windows lets the game use now
			double cardMb = 0;     // the card's own memory
			std::string adapter;
			ULONGLONG since = 0;
		};
		struct FpsState {
			Figure fps;
			std::uint64_t secondsLeftOut = 0; // not seen, or no frame at all (loading)
			// Frame time: shortest and longest single frame, the last second's average, and the average over all.
			double shortestMs = 0;
			double longestMs = 0;
			bool frameTimesKnown = false;
			double currentMs = 0;
			double countedSeconds = 0;
			double countedFrames = 0;
			ULONGLONG since = 0;
		};

		std::mutex stateMutex;
		CpuState cpu;
		RamState ram;
		GpuState gpu;
		FpsState fps;

		std::uint64_t FileTimeValue(const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; }

		// ---- Processor: the game's processor time against the time passed.
		struct CpuSampler {
			std::uint64_t lastProcess = 0, lastIdle = 0, lastBusy = 0;
			LARGE_INTEGER lastTime{};
			bool started = false;
			void Sample(LARGE_INTEGER now, std::int64_t frequency) {
				FILETIME created{}, exited{}, kernel{}, user{};
				FILETIME idle{}, systemKernel{}, systemUser{};
				if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) || !GetSystemTimes(&idle, &systemKernel, &systemUser)) return;
				const std::uint64_t process = FileTimeValue(kernel) + FileTimeValue(user);
				const std::uint64_t idleTime = FileTimeValue(idle);
				// The system's kernel time includes its idle time.
				const std::uint64_t busy = FileTimeValue(systemKernel) + FileTimeValue(systemUser) - idleTime;
				if (started) {
					// 100-nanosecond units, like the times above.
					const double passed = static_cast<double>(now.QuadPart - lastTime.QuadPart) * 10'000'000.0 / static_cast<double>(frequency);
					const std::uint32_t processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
					const double used = static_cast<double>(process - lastProcess);
					const double systemBusy = static_cast<double>(busy - lastBusy);
					const double systemAll = systemBusy + static_cast<double>(idleTime - lastIdle);
					if (passed > 0) {
						std::lock_guard lock(stateMutex);
						cpu.processors = processors;
						cpu.oneCore = used * 100.0 / passed;
						cpu.game.Add(std::clamp(used * 100.0 / (passed * processors), 0.0, 100.0));
						cpu.system = systemAll > 0 ? std::clamp(systemBusy * 100.0 / systemAll, 0.0, 100.0) : 0.0;
					}
				}
				lastProcess = process;
				lastIdle = idleTime;
				lastBusy = busy;
				lastTime = now;
				started = true;
			}
		};

		// ---- Memory.
		void SampleRam() {
			PROCESS_MEMORY_COUNTERS_EX counters{};
			counters.cb = sizeof(counters);
			if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return;
			// GlobalMemoryStatusEx is not the call MemoryWatch caps for the game (GlobalMemoryStatus), so it is exact.
			MEMORYSTATUSEX system{};
			system.dwLength = sizeof(system);
			const bool systemKnown = GlobalMemoryStatusEx(&system) != FALSE;
			const auto space = MemoryWatch::Read();
			std::lock_guard lock(stateMutex);
			ram.workingSet.Add(static_cast<double>(counters.WorkingSetSize) / Mb);
			ram.privateMb = static_cast<double>(counters.PrivateUsage) / Mb;
			ram.addressSpace = space;
			if (systemKnown) {
				ram.systemTotalMb = static_cast<double>(system.ullTotalPhys) / Mb;
				ram.systemUsedMb = static_cast<double>(system.ullTotalPhys - system.ullAvailPhys) / Mb;
			}
		}

		// ---- Video memory: DXGI tells each process what it uses on each graphics card, whatever draws the game
		// (DirectX 11/12, Vulkan or the original renderer).
		struct VramSampler {
			struct Adapter {
				IDXGIAdapter3* adapter = nullptr;
				std::string name;
				double cardMb = 0;
			};
			std::vector<Adapter> adapters;
			bool tried = false;
			void Open() {
				tried = true;
				IDXGIFactory1* factory = nullptr;
				if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
					Logger::Debug("Performance stats: no DXGI factory, so no video memory figures.");
					return;
				}
				IDXGIAdapter1* adapter = nullptr;
				for (UINT index = 0; SUCCEEDED(factory->EnumAdapters1(index, &adapter)); ++index) {
					DXGI_ADAPTER_DESC1 description{};
					IDXGIAdapter3* adapter3 = nullptr;
					if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
						&& SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
						char name[128]{};
						WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
						adapters.push_back({ adapter3, name, static_cast<double>(description.DedicatedVideoMemory) / Mb });
					}
					adapter->Release();
				}
				factory->Release();
				if (adapters.empty()) Logger::Debug("Performance stats: no graphics card gives video memory figures (needs Windows 10 or later).");
			}
			void Sample() {
				if (!tried) Open();
				if (adapters.empty()) return;
				double local = 0, shared = 0;
				const Adapter* busiest = nullptr;
				double busiestLocal = -1, busiestBudget = 0;
				for (const auto& entry : adapters) {
					DXGI_QUERY_VIDEO_MEMORY_INFO localInfo{}, sharedInfo{};
					if (FAILED(entry.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo))) continue;
					entry.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sharedInfo);
					const double used = static_cast<double>(localInfo.CurrentUsage) / Mb;
					const double usedShared = static_cast<double>(sharedInfo.CurrentUsage) / Mb;
					local += used;
					shared += usedShared;
					// The card the game draws on is the one it has the most memory on.
					if (used + usedShared > busiestLocal) {
						busiestLocal = used + usedShared;
						busiest = &entry;
						busiestBudget = static_cast<double>(localInfo.Budget) / Mb;
					}
				}
				if (!busiest) return;
				std::lock_guard lock(stateMutex);
				gpu.vram.Add(local);
				gpu.vramKnown = true;
				gpu.sharedMb = shared;
				gpu.budgetMb = busiestBudget;
				gpu.cardMb = busiest->cardMb;
				gpu.adapter = busiest->name;
			}
		};

		// ---- Graphics card load: Windows' "GPU Engine" performance counters (what Task Manager reads). Each of the
		// game's engines (3D, copy, video...) has an instance named pid_<id>_..._eng_<n>_engtype_<kind>; the load is
		// the busiest engine's.
		struct GpuLoadSampler {
			PDH_HQUERY query = nullptr;
			PDH_HCOUNTER counter = nullptr;
			bool unavailable = false;
			std::uint32_t emptySamples = 0;
			std::wstring prefix;
			std::vector<std::byte> buffer;
			void Close() {
				if (query) PdhCloseQuery(query);
				query = nullptr;
				counter = nullptr;
			}
			bool Open() {
				prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
				if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return false; }
				if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) { Close(); return false; }
				// A rate needs two readings; the first is taken now.
				PdhCollectQueryData(query);
				return true;
			}
			void Sample() {
				if (unavailable) return;
				if (!query && !Open()) {
					unavailable = true;
					Logger::Debug("Performance stats: Windows has no GPU Engine counters here, so no graphics card load.");
					return;
				}
				if (PdhCollectQueryData(query) != ERROR_SUCCESS) return;
				DWORD size = 0, count = 0;
				PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
				if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || size == 0) return;
				buffer.resize(size);
				auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
				status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
				if (status != ERROR_SUCCESS) return;
				std::map<std::wstring, double> engines;
				for (DWORD i = 0; i < count; ++i) {
					const std::wstring_view name = items[i].szName ? items[i].szName : L"";
					if (!name.starts_with(prefix)) continue;
					const auto& value = items[i].FmtValue;
					if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) continue;
					engines[std::wstring(name.substr(prefix.size()))] += value.doubleValue;
				}
				if (engines.empty()) {
					// The instances the wildcard found may be from before the game used the card: look again now
					// and then.
					if (++emptySamples % 10 == 0) Close();
					return;
				}
				emptySamples = 0;
				double busiest = 0;
				for (const auto& [name, load] : engines) busiest = (std::max)(busiest, load);
				std::lock_guard lock(stateMutex);
				gpu.load.Add(std::clamp(busiest, 0.0, 100.0));
				gpu.loadKnown = true;
			}
		};

		// ---- Frames per second: the frames shown in each second. Seconds while the player cannot see the game
		// (it may run at 1 fps then) and seconds without any frame (loading) are left out.
		struct FpsSampler {
			std::uint32_t lastFrames = 0;
			LARGE_INTEGER lastTime{};
			bool lastSeen = true;
			bool started = false;
			void Sample(LARGE_INTEGER now, std::int64_t frequency) {
				const std::uint32_t frames = FrameRate::FrameCount();
				const bool seen = BackgroundRunning::IsSeen();
				// Taken every time, so each second has only its own frames.
				std::uint32_t shortestUs = 0, longestUs = 0;
				const bool timed = FrameRate::TakeFrameTimes(shortestUs, longestUs);
				if (started) {
					const std::uint32_t shown = frames - lastFrames;
					const double seconds = static_cast<double>(now.QuadPart - lastTime.QuadPart) / static_cast<double>(frequency);
					std::lock_guard lock(stateMutex);
					if (seconds > 0 && shown > 0 && seen && lastSeen) {
						fps.fps.Add(shown / seconds);
						fps.currentMs = seconds * 1000.0 / shown;
						fps.countedSeconds += seconds;
						fps.countedFrames += shown;
						if (timed) {
							const double shortest = shortestUs / 1000.0, longest = longestUs / 1000.0;
							fps.shortestMs = fps.frameTimesKnown ? (std::min)(fps.shortestMs, shortest) : shortest;
							fps.longestMs = fps.frameTimesKnown ? (std::max)(fps.longestMs, longest) : longest;
							fps.frameTimesKnown = true;
						}
					}
					else ++fps.secondsLeftOut;
				}
				lastFrames = frames;
				lastTime = now;
				lastSeen = seen;
				started = true;
			}
		};

		void SampleLoop() {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			CpuSampler cpuSampler;
			VramSampler vramSampler;
			GpuLoadSampler gpuLoadSampler;
			FpsSampler fpsSampler;
			for (;;) {
				LARGE_INTEGER now{};
				QueryPerformanceCounter(&now);
				cpuSampler.Sample(now, frequency.QuadPart);
				fpsSampler.Sample(now, frequency.QuadPart);
				SampleRam();
				vramSampler.Sample();
				gpuLoadSampler.Sample();
				Sleep(SampleMs);
			}
		}

		// ---- Text.
		std::string Duration(ULONGLONG since) {
			const ULONGLONG seconds = (GetTickCount64() - since) / 1000;
			return std::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
		}
		std::string Line(const char* label, const Figure& figure, const char* format) {
			const auto value = [&](double number) { return std::vformat(format, std::make_format_args(number)); };
			return std::format("  {:<14}minimum {} | maximum {} | current {} | average {}\n", label,
				value(figure.minimum), value(figure.maximum), value(figure.current), value(figure.Average()));
		}
		std::string Megabytes(double mb) {
			return mb >= 10240 ? std::format("{:.1f} GB", mb / 1024.0) : std::format("{:.0f} MB", mb);
		}
		std::string MegabytesLine(const char* label, const Figure& figure) {
			return std::format("  {:<14}minimum {} | maximum {} | current {} | average {}\n", label,
				Megabytes(figure.minimum), Megabytes(figure.maximum), Megabytes(figure.current), Megabytes(figure.Average()));
		}
		constexpr const char* NoSamples = "  no figures yet, ask again in a second\n";
	}

	void PerformanceStats::Install() {
		const ULONGLONG now = GetTickCount64();
		cpu.since = ram.since = gpu.since = fps.since = now;
		std::thread(SampleLoop).detach();
	}

	std::string PerformanceStats::Report(Part part) {
		std::lock_guard lock(stateMutex);
		std::string text;
		switch (part) {
			case Part::Cpu:
				text = "CPU, the game (since " + Duration(cpu.since) + "):\n";
				if (cpu.game.count == 0) return text + NoSamples;
				text += Line("game", cpu.game, "{:.1f}%");
				text += std::format("  {:<14}of all {} logical processors; current {:.0f}% of one (the game does most of its work on one)\n", "", cpu.processors, cpu.oneCore);
				text += std::format("  {:<14}current {:.0f}% (every program)\n", "whole PC", cpu.system);
				return text;
			case Part::Ram:
				text = "RAM, the game (since " + Duration(ram.since) + "):\n";
				if (ram.workingSet.count == 0) return text + NoSamples;
				text += MegabytesLine("in RAM", ram.workingSet);
				text += std::format("  {:<14}current {} (private)\n", "committed", Megabytes(ram.privateMb));
				if (ram.addressSpace.limitMb != 0) {
					text += std::format("  {:<14}{} of {} MB used, largest free block {} MB (a 32-bit game runs out here first)\n", "address space",
						ram.addressSpace.limitMb - ram.addressSpace.freeMb, ram.addressSpace.limitMb, ram.addressSpace.largestFreeMb);
				}
				if (ram.systemTotalMb > 0) {
					text += std::format("  {:<14}{} of {} used (every program)\n", "whole PC", Megabytes(ram.systemUsedMb), Megabytes(ram.systemTotalMb));
				}
				return text;
			case Part::Gpu:
				text = "GPU, the game" + (gpu.adapter.empty() ? std::string() : " on " + gpu.adapter) + " (since " + Duration(gpu.since) + "):\n";
				if (!gpu.loadKnown && !gpu.vramKnown) return text + "  no figures (yet): Windows 10 or later gives them\n";
				if (gpu.loadKnown) text += Line("load", gpu.load, "{:.1f}%");
				else text += std::format("  {:<14}not known here (Windows' GPU Engine counters are missing)\n", "load");
				if (gpu.vramKnown) {
					text += MegabytesLine("VRAM", gpu.vram);
					text += std::format("  {:<14}current {} (system memory the card uses for the game)\n", "shared", Megabytes(gpu.sharedMb));
					text += std::format("  {:<14}{} the game may use now, of the card's {}\n", "VRAM budget", Megabytes(gpu.budgetMb), Megabytes(gpu.cardMb));
				}
				return text;
			case Part::Fps:
				text = "FPS (since " + Duration(fps.since) + "):\n";
				if (!FrameRate::IsPresentHooked()) return text + "  not measured: the frame present hook is not in place\n";
				if (fps.fps.count == 0) return text + NoSamples;
				text += Line("frames/second", fps.fps, "{:.1f}");
				if (fps.frameTimesKnown && fps.countedFrames > 0) {
					text += std::format("  {:<14}minimum {:.1f} ms | maximum {:.1f} ms | current {:.1f} ms | average {:.1f} ms\n", "frame time",
						fps.shortestMs, fps.longestMs, fps.currentMs, fps.countedSeconds * 1000.0 / fps.countedFrames);
					text += std::format("  {:<14}(minimum and maximum: single frames, so the maximum shows stutters; current: the last second)\n", "");
				}
				text += std::format("  {:<14}{} seconds counted, {} left out (game not seen, or loading)\n", "", fps.fps.count, fps.secondsLeftOut);
				text += std::format("  {:<14}{} (config.yml)\n", "frame_rate", Config::GetString("frame_rate", "60"));
				return text;
		}
		return text;
	}

	void PerformanceStats::Reset(Part part) {
		std::lock_guard lock(stateMutex);
		const ULONGLONG now = GetTickCount64();
		switch (part) {
			case Part::Cpu: cpu.game = {}; cpu.since = now; break;
			case Part::Ram: ram.workingSet = {}; ram.since = now; break;
			case Part::Gpu: gpu.load = {}; gpu.vram = {}; gpu.since = now; break;
			case Part::Fps: fps = {}; fps.since = now; break;
		}
	}
}
