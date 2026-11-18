#include "Logger.h"
#include "Config.h"
#include "Paths.h"
#include "StringUtils.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>
#include <Windows.h>
namespace fs = std::filesystem;
namespace KeeperForge2 {
	namespace {
		std::string logFilePath;
		std::mutex logMutex;
		HANDLE consoleHandle = INVALID_HANDLE_VALUE;
		std::atomic<LogLevel> minimumLevel{ LogLevel::Info };
		std::atomic_bool fatalInProgress{ false };
		std::atomic_bool showErrorBox{ true };
		constexpr std::array<const char*, 6> levelNames{ "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL" };
		std::string GetTimestamp() {
			auto now = std::chrono::system_clock::now();
			auto time = std::chrono::system_clock::to_time_t(now);
			std::tm localTime{};
			localtime_s(&localTime,&time);
			auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
			return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.{:03}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
				localTime.tm_hour, localTime.tm_min, localTime.tm_sec, milliseconds.count());
		}
		// Keeps the Logs folder from growing without end: deletes the oldest .log files so that, with the one about to
		// be opened, at most kept stay (0 = keep all). Other files there (crash dumps) are not touched. Returns how many
		// were deleted.
		std::size_t RemoveOldLogs(std::size_t kept) {
			if (kept == 0) { return 0; }
			std::error_code error;
			std::vector<std::pair<fs::file_time_type, fs::path>> logs;
			for (const auto& entry : fs::directory_iterator(Paths::Logs(), error)) {
				if (!entry.is_regular_file(error) || entry.path().extension() != ".log") { continue; }
				logs.emplace_back(entry.last_write_time(error), entry.path());
			}
			if (logs.size() < kept) { return 0; }
			std::sort(logs.begin(), logs.end());
			std::size_t removed = 0;
			for (std::size_t i = 0; i + kept <= logs.size(); ++i) {
				if (fs::remove(logs[i].second, error)) { ++removed; }
			}
			return removed;
		}
		// The launcher owns a console. The game does not, so it attaches to its parent's (the launcher's) console.
		void OpenConsole() {
			auto open = [] { return CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); };
			consoleHandle = open();
			if (consoleHandle == INVALID_HANDLE_VALUE && AttachConsole(ATTACH_PARENT_PROCESS)) { consoleHandle = open(); }
		}
	}
	bool Logger::Initialize(const std::string& name, const std::string& tag) {
		const bool configLoaded = Config::Load();
		const std::string levelText = Config::GetString("log_level", "info");
		const std::optional<LogLevel> parsedLevel = ParseLevel(levelText);
		const LogLevel level = parsedLevel.value_or(LogLevel::Info);
		minimumLevel.store(level);
		const std::string keptText = Config::GetString("logs_kept", std::to_string(DefaultLogsKept));
		std::size_t logsKept = DefaultLogsKept;
		bool keptValid = true;
		try {
			std::size_t used = 0;
			const long long parsed = std::stoll(keptText, &used);
			if (used != keptText.size() || parsed < 0) { keptValid = false; } else { logsKept = static_cast<std::size_t>(parsed); }
		} catch (...) { keptValid = false; }
		std::size_t removedLogs = 0;
		{
			std::lock_guard<std::mutex> lock(logMutex);
			std::error_code error;
			fs::create_directories(Paths::Logs(), error);
			removedLogs = RemoveOldLogs(logsKept);
			logFilePath = (Paths::Logs() / (name + "-" + GetTimestamp() + (tag.empty() ? "" : "_" + tag) + ".log")).string();
			std::ofstream file(logFilePath, std::ios::out | std::ios::app);
			if (!file.is_open()) {
				MessageBoxA(nullptr,logFilePath.c_str(),"Failed opening log file",MB_OK);
				return false;
			}
			if (consoleHandle == INVALID_HANDLE_VALUE) { OpenConsole(); }
		}
		Info("Logger initialized. Minimum level: " + std::string(levelNames[std::to_underlying(level)]));
		if (!configLoaded) { Warning("Could not read or create config.yml, using default settings."); }
		for (const auto& change : Config::LoadChanges()) { Warning(change); }
		if (!parsedLevel) { Warning("Unknown log_level '" + levelText + "' in config.yml, using INFO."); }
		if (!keptValid) { Warning("Unknown logs_kept '" + keptText + "' in config.yml, using " + std::to_string(DefaultLogsKept) + "."); }
		if (removedLogs > 0) { Info("Deleted the " + std::to_string(removedLogs) + " oldest logs (logs_kept " + std::to_string(logsKept) + ")."); }
		return true;
	}
	void Logger::Shutdown() {
		std::lock_guard<std::mutex> lock(logMutex);
		if (consoleHandle != INVALID_HANDLE_VALUE) {
			CloseHandle(consoleHandle);
			consoleHandle = INVALID_HANDLE_VALUE;
		}
	}
	void Logger::SetMinimumLevel(LogLevel level) { minimumLevel.store(level); }
	void Logger::SetErrorBox(bool show) { showErrorBox.store(show); }
	LogLevel Logger::GetMinimumLevel() { return minimumLevel.load(); }
	std::optional<LogLevel> Logger::ParseLevel(std::string_view text) {
		const std::string lower = StringUtils::ToLower(text);
		for (const auto [index, name] : levelNames | std::views::enumerate) {
			if (lower == StringUtils::ToLower(name)) { return static_cast<LogLevel>(index); }
		}
		return std::nullopt;
	}
	void Logger::Write(LogLevel level, const std::string& message) {
		if (level < minimumLevel.load()) { return; }
		const std::string line = "[" + GetTimestamp() + "] [" + levelNames[std::to_underlying(level)] + "] " + message + '\n';
		std::lock_guard<std::mutex> lock(logMutex);
		if (consoleHandle != INVALID_HANDLE_VALUE) {
			DWORD written = 0;
			WriteFile(consoleHandle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
		}
		if (logFilePath.empty()) { return; }
		std::ofstream file(logFilePath, std::ios::out | std::ios::app);
		if (!file.is_open()) { return; }
		file << line;
		file.flush();
		file.close();
	}
	void Logger::Print(const std::string& text) {
		std::lock_guard<std::mutex> lock(logMutex);
		if (consoleHandle != INVALID_HANDLE_VALUE) {
			DWORD written = 0;
			WriteFile(consoleHandle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
		}
		if (logFilePath.empty()) { return; }
		std::ofstream file(logFilePath, std::ios::out | std::ios::app);
		if (file.is_open()) { file << text; }
	}
	void Logger::WriteRaw(const char* text, std::size_t size) {
		DWORD written = 0;
		// The file first: writing to a console can block (text being selected in its window, for example).
		if (!logFilePath.empty()) {
			const HANDLE file = CreateFileA(logFilePath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file != INVALID_HANDLE_VALUE) {
				WriteFile(file, text, static_cast<DWORD>(size), &written, nullptr);
				CloseHandle(file);
			}
		}
		if (consoleHandle != INVALID_HANDLE_VALUE) { WriteFile(consoleHandle, text, static_cast<DWORD>(size), &written, nullptr); }
	}
	void Logger::Fatal(LogLevel level, const std::string& message) {
		Write(level, message);
		// Only the first fatal error shows a box; any other thread hitting one waits for the process to close.
		if (fatalInProgress.exchange(true)) { Sleep(INFINITE); }
		if (showErrorBox.load()) {
			const std::string title = level == LogLevel::Critical ? "KeeperForge2 - Critical Error" : "KeeperForge2 - Error";
			const std::string text = message + "\n\nKeeperForge2 will now close.";
			MessageBoxA(nullptr, text.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
		}
		// TerminateProcess skips DLL detach handlers, which could deadlock on locks held by other threads.
		TerminateProcess(GetCurrentProcess(), 1);
		ExitProcess(1);
	}
	void Logger::Trace(const std::string& message) { Write(LogLevel::Trace, message); }
	void Logger::Debug(const std::string& message) { Write(LogLevel::Debug, message); }
	void Logger::Info(const std::string& message) { Write(LogLevel::Info, message); }
	void Logger::Warning(const std::string& message) { Write(LogLevel::Warning, message); }
	void Logger::Error(const std::string& message) { Fatal(LogLevel::Error, message); }
	void Logger::ErrorAndContinue(const std::string& message) { Write(LogLevel::Error, message); }
	void Logger::Critical(const std::string& message) { Fatal(LogLevel::Critical, message); }
}
