#include "CrashHandler.h"
#include "Logger.h"
#include "MemoryWatch.h"
#include "Paths.h"
#include "StringUtils.h"
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <Windows.h>
#include <DbgHelp.h>

// A crash can mean the heap is damaged or full, or the crashed thread's stack has run out. So the report is written by
// a thread started in advance, with its own stack, and it formats into fixed buffers without the heap. Each line goes to
// the log as soon as it is ready, the riskiest parts last, so whatever fails later the earlier lines are kept.
namespace KeeperForge2 {
	namespace {
		constexpr DWORD ReportTimeoutMs = 10000;
		constexpr std::size_t AddressTextSize = MAX_PATH + 48;
		std::atomic_bool handling{ false };
		HANDLE crashEvent = nullptr;
		HANDLE doneEvent = nullptr;
		// Tells the launcher, which can close the game if the report hangs. The name must match main.cpp in the launcher.
		HANDLE launcherEvent = nullptr;
		DWORD reporterThread = 0;
		EXCEPTION_POINTERS* crashPointers = nullptr;
		DWORD crashThread = 0;
		char logsFolder[MAX_PATH]{};
		// wsprintf writes at most 1024 characters.
		char reportLine[1025]{};

		// "DKII-DX.exe+0x1234" for an address inside a loaded module, the plain address otherwise. Runs inside a
		// crashed process, so it only asks Windows about the address and never reads through it.
		void FormatAddress(std::uintptr_t address, char (&text)[AddressTextSize]) {
			HMODULE module = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) || !module) {
				wsprintfA(text, "0x%lX", static_cast<unsigned long>(address));
				return;
			}
			char path[MAX_PATH]{};
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* slash = std::strrchr(path, '\\');
			const char* name = slash ? slash + 1 : path;
			const auto offset = static_cast<unsigned long>(address - reinterpret_cast<std::uintptr_t>(module));
			// The Ghidra export uses the exe's own addresses (image base 0x400000), so give that too.
			if (module == GetModuleHandleA(nullptr)) { wsprintfA(text, "%s+0x%lX (0x%lX)", name, offset, offset + 0x400000); }
			else { wsprintfA(text, "%s+0x%lX", name, offset); }
		}
		std::string DescribeAddress(std::uintptr_t address) {
			char text[AddressTextSize];
			FormatAddress(address, text);
			return text;
		}
		bool IsCodeAddress(std::uintptr_t value) {
			MEMORY_BASIC_INFORMATION info{};
			if (!VirtualQuery(reinterpret_cast<const void*>(value), &info, sizeof(info)) || info.State != MEM_COMMIT) { return false; }
			const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return (info.Protect & executable) != 0 && info.Type == MEM_IMAGE;
		}
		void Report(const char* format, ...) {
			va_list arguments;
			va_start(arguments, format);
			const int size = wvsprintfA(reportLine, format, arguments);
			va_end(arguments);
			if (size > 0) { Logger::WriteRaw(reportLine, static_cast<std::size_t>(size)); }
		}
		void ReportText(const char* text) { Logger::WriteRaw(text, std::strlen(text)); }
		const char* ExceptionName(DWORD code) {
			switch (code) {
				case EXCEPTION_ACCESS_VIOLATION: return " (access violation)";
				case EXCEPTION_STACK_OVERFLOW: return " (stack overflow)";
				case 0xE06D7363: return " (C++ exception, such as out of memory)";
				default: return "";
			}
		}
		// Code addresses on the stack are mostly return addresses, which show how the crash was reached.
		void ReportStack(const CONTEXT& context) {
			Report("Code addresses on the stack:\n");
			auto* slot = reinterpret_cast<const std::uintptr_t*>(context.Esp);
			int found = 0;
			for (int i = 0; i < 1024 && found < 24; ++i, ++slot) {
				std::uintptr_t value = 0;
				// Fails instead of faulting at the end of the stack.
				if (!ReadProcessMemory(GetCurrentProcess(), slot, &value, sizeof(value), nullptr)) { break; }
				if (!IsCodeAddress(value)) { continue; }
				char where[AddressTextSize];
				FormatAddress(value, where);
				Report("  [esp+0x%lX] %s\n", static_cast<unsigned long>(i * 4), where);
				++found;
			}
		}
		void ReportMinidump(const SYSTEMTIME& time) {
			char name[MAX_PATH + 64];
			wsprintfA(name, "%s\\Crash-%04d-%02d-%02d_%02d-%02d-%02d.dmp", logsFolder, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
			const HANDLE file = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				Report("Minidump could not be created (error %lu).\n", GetLastError());
				return;
			}
			MINIDUMP_EXCEPTION_INFORMATION information{};
			information.ThreadId = crashThread;
			information.ExceptionPointers = crashPointers;
			information.ClientPointers = FALSE;
			const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs);
			if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &information, nullptr, nullptr)) { Report("Minidump: %s\n", name); }
			else { Report("Minidump could not be written (error %lu).\n", GetLastError()); }
			CloseHandle(file);
		}
		DWORD WINAPI ReporterThread(void*) {
			WaitForSingleObject(crashEvent, INFINITE);
			if (launcherEvent) { SetEvent(launcherEvent); }
			const EXCEPTION_RECORD& record = *crashPointers->ExceptionRecord;
			const CONTEXT& context = *crashPointers->ContextRecord;
			SYSTEMTIME time{};
			GetLocalTime(&time);
			Report("[%04d-%02d-%02d_%02d-%02d-%02d.%03d] [CRITICAL] The game crashed: exception 0x%lX%s at 0x%lX.", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
				record.ExceptionCode, ExceptionName(record.ExceptionCode), reinterpret_cast<std::uintptr_t>(record.ExceptionAddress));
			if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
				const char* kind = record.ExceptionInformation[0] == 0 ? "reading" : record.ExceptionInformation[0] == 1 ? "writing" : "executing";
				Report(" Access violation %s 0x%lX.", kind, static_cast<unsigned long>(record.ExceptionInformation[1]));
			}
			Report("\nRegisters: EAX=0x%lX EBX=0x%lX ECX=0x%lX EDX=0x%lX ESI=0x%lX EDI=0x%lX EBP=0x%lX ESP=0x%lX EIP=0x%lX\n", context.Eax, context.Ebx, context.Ecx, context.Edx, context.Esi, context.Edi, context.Ebp, context.Esp, context.Eip);
			const MemoryWatch::AddressSpace space = MemoryWatch::Read();
			Report("Memory: %lu MB committed, %lu MB free, largest free block %lu MB, of %lu MB.\n", space.committedMb, space.freeMb, space.largestFreeMb, space.limitMb);
			char where[AddressTextSize];
			FormatAddress(reinterpret_cast<std::uintptr_t>(record.ExceptionAddress), where);
			Report("Crashed in %s\n", where);
			ReportStack(context);
			ReportMinidump(time);
			SetEvent(doneEvent);
			return 0;
		}

		LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exception) {
			if (handling.exchange(true)) {
				if (GetCurrentThreadId() == reporterThread) {
					ReportText("The crash report itself crashed, so it stops here.\n");
					TerminateProcess(GetCurrentProcess(), 1);
				}
				// Another thread crashed too: the first crash's report is still being written.
				Sleep(INFINITE);
			}
			crashPointers = exception;
			crashThread = GetCurrentThreadId();
			if (!crashEvent || !doneEvent || !reporterThread) {
				ReportText("The game crashed, and the crash report could not be started.\n");
				TerminateProcess(GetCurrentProcess(), 1);
			}
			SetEvent(crashEvent);
			// The report can block inside a crashed process (a lock held by the broken thread, for example). Whatever
			// happens, the game is closed a few seconds after the crash.
			if (WaitForSingleObject(doneEvent, ReportTimeoutMs) != WAIT_OBJECT_0) { ReportText("The crash report did not finish in time, so it stops here.\n"); }
			TerminateProcess(GetCurrentProcess(), 1);
			return EXCEPTION_EXECUTE_HANDLER;
		}
	}

	void CrashHandler::Install() {
		lstrcpynA(logsFolder, Paths::Logs().string().c_str(), MAX_PATH);
		char launcherEventName[64];
		wsprintfA(launcherEventName, "Local\\KeeperForge2Crash-%lu", GetCurrentProcessId());
		launcherEvent = CreateEventA(nullptr, TRUE, FALSE, launcherEventName);
		crashEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		doneEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		if (const HANDLE thread = CreateThread(nullptr, 0, &ReporterThread, nullptr, 0, &reporterThread)) { CloseHandle(thread); }
		SetUnhandledExceptionFilter(&CrashFilter);
	}

	std::string CrashHandler::DescribeThread(unsigned long threadId) {
		if (threadId == GetCurrentThreadId()) { return "(the thread asking)"; }
		HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
		if (!thread) { return "(could not open the thread)"; }
		// While it is paused only plain values are copied: it may hold the heap's or the loader's lock, which formatting
		// the text needs, so that waits until it runs again.
		constexpr int StackSlots = 1024;
		static std::uintptr_t stack[StackSlots];
		int copied = 0;
		CONTEXT context{};
		context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		bool read = false;
		if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
			CloseHandle(thread);
			return "(could not pause the thread)";
		}
		if (GetThreadContext(thread, &context)) {
			read = true;
			auto* slot = reinterpret_cast<const std::uintptr_t*>(context.Esp);
			for (; copied < StackSlots; ++copied, ++slot) {
				MEMORY_BASIC_INFORMATION info{};
				if (!VirtualQuery(slot, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) == 0) { break; }
				stack[copied] = *slot;
			}
		}
		ResumeThread(thread);
		CloseHandle(thread);
		if (!read) { return "(could not read the thread)"; }
		std::string text = "at " + DescribeAddress(context.Eip) + "\nCode addresses on its stack:";
		int found = 0;
		for (int i = 0; i < copied && found < 24; ++i) {
			if (!IsCodeAddress(stack[i])) { continue; }
			text += "\n  [esp+" + StringUtils::ToHex(static_cast<std::uintptr_t>(i * 4)) + "] " + DescribeAddress(stack[i]);
			++found;
		}
		return text;
	}
}
