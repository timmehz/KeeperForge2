#include "FolderLayout.h"
#include "Launcher.h"
#include "Logger.h"
#include "StringUtils.h"
#include <string>

int main() {
	using KeeperForge2::Logger;
	// Before the logger: it opens its file in the KeeperForge2 folder and reads config.yml from there.
	const auto layoutReport = KeeperForge2::FolderLayout::Prepare();
	if (!Logger::Initialize("KeeperForge2Launcher")) { return 1; }
	for (const auto& line : layoutReport) { Logger::Info(line); }
	KeeperForge2::Launcher::CheckSteamRunning();
	std::filesystem::path gamePath;
	if (!KeeperForge2::Launcher::FindGame(gamePath)) { return 1; }
	PROCESS_INFORMATION processInfo{};
	if (!KeeperForge2::Launcher::LaunchGame(gamePath, processInfo)) { return 1; }
	Logger::Info("Launched DKII-DX.exe successfully.");
	std::filesystem::path dllPath = std::filesystem::current_path() / "KeeperForge2.dll";
	// Loaded with the game's own DLLs, the runtime starts itself on the game's main thread before the game's code.
	if (const auto loadedFirst = KeeperForge2::Launcher::LoadRuntimeFirst(processInfo.hProcess, gamePath, dllPath)) {
		ResumeThread(processInfo.hThread);
	}
	else {
		Logger::Warning(loadedFirst.error());
		// The older way: into the running game, which then races the runtime (see docs/research-log.md).
		ResumeThread(processInfo.hThread);
		uintptr_t remoteModule = 0;
		if (!KeeperForge2::Launcher::InjectDLL(processInfo.hProcess, dllPath, remoteModule)) {
			Logger::Error("KeeperForge2.dll failed to inject into DKII-DX.exe");
		}
		Logger::Info("KeeperForge2.dll successfully injected into DKII-DX.exe");
		Logger::Debug("KeeperForge2.dll loaded at " + KeeperForge2::StringUtils::ToHex(remoteModule));
		if (!KeeperForge2::Launcher::InitializeRuntime(processInfo.hProcess, remoteModule)) {
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			Logger::Shutdown();
			return 1;
		}
	}
	CloseHandle(processInfo.hThread);
	// The runtime's crash report sets this event. Writing a minidump pauses the game's other threads, so if that hangs
	// nothing inside the game can close it. The name must match CrashHandler.cpp in the runtime.
	const std::wstring crashEventName = L"Local\\KeeperForge2Crash-" + std::to_wstring(processInfo.dwProcessId);
	if (const HANDLE crashEvent = CreateEventW(nullptr, TRUE, FALSE, crashEventName.c_str())) {
		const HANDLE waits[] = { processInfo.hProcess, crashEvent };
		if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1 && WaitForSingleObject(processInfo.hProcess, 20000) == WAIT_TIMEOUT) {
			Logger::Warning("The game's crash report did not finish, so the launcher is closing the game.");
			TerminateProcess(processInfo.hProcess, 1);
		}
		CloseHandle(crashEvent);
	}
	WaitForSingleObject(processInfo.hProcess, INFINITE);
	DWORD exitCode = 0;
	GetExitCodeProcess(processInfo.hProcess, &exitCode);
	Logger::Info("DKII-DX.exe exited with code " + std::to_string(exitCode) + ".");
	CloseHandle(processInfo.hProcess);
	Logger::Shutdown();
	return 0;
}
