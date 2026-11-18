#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
namespace KeeperForge2 {
	enum class LogLevel { Trace, Debug, Info, Warning, Error, Critical };
	class Logger {
		public:
			// Loads config.yml for the minimum level, then opens KeeperForge2\Logs\<name>-<timestamp>.log (or
			// <name>-<timestamp>_<tag>.log with a tag) and the console. First, the oldest .log files are deleted so
			// that, with the new one, the folder holds at most logs_kept of them (config.yml, 0 = keep all).
			static bool Initialize(const std::string& name, const std::string& tag = {});
			static constexpr std::size_t DefaultLogsKept = 10;
			static void Shutdown();
			static void SetMinimumLevel(LogLevel level);
			static LogLevel GetMinimumLevel();
			static std::optional<LogLevel> ParseLevel(std::string_view text);
			// Whether Error and Critical show a message box before closing (on by default). A fullscreen game
			// turns it off: the box can end up hidden behind the game and keep the process alive.
			static void SetErrorBox(bool show);
			static void Trace(const std::string& message);
			static void Debug(const std::string& message);
			static void Info(const std::string& message);
			static void Warning(const std::string& message);
			// Error and Critical log the message, show the box if enabled, then close the process.
			[[noreturn]] static void Error(const std::string& message);
			// An Error line that does not close the process (for mods' errors, which are theirs to handle).
			static void ErrorAndContinue(const std::string& message);
			// Text as it is (no time or level), to the console and the log whatever log_level is: answers to console
			// commands.
			static void Print(const std::string& text);
			[[noreturn]] static void Critical(const std::string& message);
			// For a crash report: adds the text to the log and the console as it is. Uses no heap and no lock, as a
			// crashed thread may hold them or the heap may be damaged.
			static void WriteRaw(const char* text, std::size_t size);
		private:
			static void Write(LogLevel level, const std::string& message);
			[[noreturn]] static void Fatal(LogLevel level, const std::string& message);
	};
}
