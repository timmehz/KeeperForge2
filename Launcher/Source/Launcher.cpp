#include "Launcher.h"
#include "Config.h"
#include "GameFiles.h"
#include "PEUtils.h"
#include "Logger.h"
#include "StringUtils.h"
#include <TlHelp32.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <system_error>
#include <array>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace KeeperForge2 {
    HANDLE Launcher::jobHandle = nullptr;
    void Launcher::CheckSteamRunning() {
        if (StringUtils::ToLower(Config::GetString("steam_multiplayer", "on")) == "off") { return; }
        const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        // If Windows cannot list the processes, the game's own start of Steam still reports a problem in its log.
        if (processes == INVALID_HANDLE_VALUE) {
            Logger::Warning("Could not check whether Steam is running.");
            return;
        }
        bool running = false;
        PROCESSENTRY32W process{};
        process.dwSize = sizeof(process);
        for (BOOL more = Process32FirstW(processes, &process); more && !running; more = Process32NextW(processes, &process)) {
            running = _wcsicmp(process.szExeFile, L"steam.exe") == 0;
        }
        CloseHandle(processes);
        if (!running) { Logger::Error("Steam MUST be open in order to use steam_multiplayer in config.yml.\nOpen Steam and start again, or set steam_multiplayer to off in KeeperForge2\\config.yml."); }
    }
    bool Launcher::FindGame(fs::path& gamePath) {
        const fs::path gameDirectory = fs::current_path();
        // Names, sizes and SHA-256 checksums; stops the launcher if a file is missing or not original.
        GameFiles::Verify(gameDirectory);
        gamePath = gameDirectory / "DKII-DX.exe";
        // Windows reads the 4 GB flag from the file when it creates the process, so the game has to start from a flagged
        // copy. It stays in the game's folder, where Windows looks for the game's DLLs first.
        const fs::path flaggedCopy = gameDirectory / "KeeperForge2-DKII-DX.exe";
        std::error_code error;
        if (Config::GetString("memory_4gb", "on") != "on") {
            fs::remove(flaggedCopy, error);
            return true;
        }
        const auto copied = PEUtils::MakeLargeAddressAwareCopy(gamePath, flaggedCopy);
        if (!copied) {
            Logger::Warning("memory_4gb: could not make KeeperForge2-DKII-DX.exe (" + std::string(copied.error()) + "), so the game starts with 2 GB.");
        }
        else if (*copied == PEUtils::LargeAddressResult::SourceAlreadyAware) { Logger::Info("memory_4gb: DKII-DX.exe already has the 4 GB flag."); }
        else {
            gamePath = flaggedCopy;
            Logger::Info("memory_4gb: starting KeeperForge2-DKII-DX.exe, a copy of DKII-DX.exe with the 4 GB flag.");
        }
        return true;
	}
	bool Launcher::LaunchGame(const fs::path& gamePath, PROCESS_INFORMATION& processInfo) {
        STARTUPINFOW startupInfo{};
        jobHandle = CreateJobObject(nullptr, nullptr);
        if (!jobHandle) { Logger::Error("Failed creating Job Object."); }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo))) { Logger::Error("Failed configuring Job Object."); }
        startupInfo.cb = sizeof(startupInfo);
        std::wstring commandLine = L"\"" + gamePath.wstring() + L"\"";
        std::wstring workingDirectory = gamePath.parent_path().wstring();
        // The chosen renderer, for the runtime while Windows starts the game's DLLs (it cannot read config.yml then).
        const std::string renderer = Config::GetString("renderer", "vulkan");
        SetEnvironmentVariableW(L"KEEPERFORGE2_RENDERER", std::wstring(renderer.begin(), renderer.end()).c_str());
        // Started from a Steam invite, Steam adds "+connect_lobby <id>": the runtime joins that friend's lobby.
        const std::wstring ownCommandLine = GetCommandLineW();
        if (const auto at = ownCommandLine.find(L"+connect_lobby"); at != std::wstring::npos) {
            const auto start = ownCommandLine.find_first_of(L"0123456789", at);
            if (start != std::wstring::npos) {
                const auto end = ownCommandLine.find_first_not_of(L"0123456789", start);
                SetEnvironmentVariableW(L"KEEPERFORGE2_STEAM_LOBBY", ownCommandLine.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start).c_str());
            }
        }
        // Paused, so KeeperForge2 can be added to the DLLs it loads before any of them start.
        BOOL result = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, workingDirectory.c_str(), &startupInfo, &processInfo);
        if (!result) { Logger::Error("Failed to launch DKII-DX.exe. CreateProcess error code: " + std::to_string(GetLastError())); }
        // If this fails the game is not in the job, so closing the launcher would not close it.
        if (!AssignProcessToJobObject(jobHandle, processInfo.hProcess)) {
            TerminateProcess(processInfo.hProcess, 1);
            Logger::Error("Failed assigning DKII-DX.exe to the Job Object.");
        }
        return true;
	}
    // The game is paused before Windows has loaded any of its DLLs: only the exe and ntdll are in memory. A new import
    // table is written into the game (KeeperForge2.dll's entry first, then the game's own) and the exe's header is
    // pointed at it. Windows then loads KeeperForge2.dll with the game's DLLs and starts it before patch.dll and the
    // DLLs that one brings (ddraw.dll), which lets the runtime act before any of them run (see the runtime's
    // StartEarly). The exe file on disk is not changed.
    std::expected<void, std::string> Launcher::LoadRuntimeFirst(HANDLE process, const fs::path& gamePath, const fs::path& dllPath) {
        static_assert(sizeof(void*) == 4, "the launcher reads the 32-bit game's structures as its own");
        // Windows finds an imported DLL by name in the game's folder, so it has to be there.
        std::error_code error;
        if (!fs::equivalent(dllPath.parent_path(), gamePath.parent_path(), error)) {
            return std::unexpected(std::string("KeeperForge2.dll is not in the game's folder, so it cannot be loaded with the game."));
        }
        const auto read = [process](std::uintptr_t address, void* out, std::size_t size) {
            SIZE_T done = 0;
            return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, size, &done) && done == size;
        };
        const auto write = [process](std::uintptr_t address, const void* data, std::size_t size) {
            SIZE_T done = 0;
            return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address), data, size, &done) && done == size;
        };
        const auto fail = [](const std::string& why) { return std::unexpected("Could not load KeeperForge2.dll with the game (" + why + ")."); };
        // Where the exe sits: the process environment block holds it (32-bit layout: ImageBaseAddress at +8).
        using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        const auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (!query) return fail("no NtQueryInformationProcess");
        struct BasicInformation {
            LONG exitStatus;
            std::uint32_t environmentBlock;
            std::uint32_t affinityMask;
            LONG basePriority;
            std::uint32_t processId;
            std::uint32_t parentProcessId;
        } basic{};
        if (query(process, 0, &basic, sizeof(basic), nullptr) < 0) return fail("could not read the process information");
        std::uint32_t base = 0;
        if (!read(basic.environmentBlock + 8, &base, sizeof(base))) return fail("could not read where the game is in memory");
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS32 nt{};
        if (!read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return fail("no exe header");
        if (!read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return fail("not a 32-bit exe header");
        }
        // The game's own import entries, up to the empty one that ends them.
        const IMAGE_DATA_DIRECTORY imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        std::vector<IMAGE_IMPORT_DESCRIPTOR> existing;
        for (std::uint32_t i = 0;; ++i) {
            IMAGE_IMPORT_DESCRIPTOR entry{};
            if (i > 1000 || !read(base + imports.VirtualAddress + i * sizeof(entry), &entry, sizeof(entry))) return fail("could not read the game's imports");
            if (entry.Name == 0 && entry.FirstThunk == 0) break;
            existing.push_back(entry);
        }
        // The new block: the import entries (ours, the game's, an empty one), then our entry's name list and address slots
        // (one function each, ended by 0), its function name and its DLL name. Everything is an offset from the exe.
        const std::string functionName = "KeeperForge2_Initialize";
        const std::string dllName = dllPath.filename().string();
        const std::uint32_t entriesSize = static_cast<std::uint32_t>((existing.size() + 2) * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        const std::uint32_t namesOffset = entriesSize;
        const std::uint32_t slotsOffset = namesOffset + 8;
        const std::uint32_t functionOffset = slotsOffset + 8;
        const std::uint32_t dllNameOffset = (functionOffset + 2 + static_cast<std::uint32_t>(functionName.size()) + 1 + 1) & ~1u;
        const std::uint32_t blockSize = dllNameOffset + static_cast<std::uint32_t>(dllName.size()) + 1;
        // Above the exe, as the offsets cannot be negative.
        std::uintptr_t block = 0;
        for (std::uintptr_t at = (base + nt.OptionalHeader.SizeOfImage + 0xFFFF) & ~static_cast<std::uintptr_t>(0xFFFF); at < 0x7FFE0000 && !block; at += 0x10000) {
            block = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process, reinterpret_cast<LPVOID>(at), blockSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
        if (!block) return fail("no memory above the game for the new import table");
        const std::uint32_t blockOffset = static_cast<std::uint32_t>(block - base);
        std::vector<std::uint8_t> data(blockSize, 0);
        IMAGE_IMPORT_DESCRIPTOR ours{};
        ours.OriginalFirstThunk = blockOffset + namesOffset;
        ours.Name = blockOffset + dllNameOffset;
        ours.FirstThunk = blockOffset + slotsOffset;
        std::memcpy(data.data(), &ours, sizeof(ours));
        if (!existing.empty()) std::memcpy(data.data() + sizeof(ours), existing.data(), existing.size() * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        const std::uint32_t function = blockOffset + functionOffset;
        std::memcpy(data.data() + namesOffset, &function, sizeof(function));
        std::memcpy(data.data() + slotsOffset, &function, sizeof(function));
        // The function name comes after a 2-byte hint (0: Windows looks it up by name).
        std::memcpy(data.data() + functionOffset + 2, functionName.c_str(), functionName.size() + 1);
        std::memcpy(data.data() + dllNameOffset, dllName.c_str(), dllName.size() + 1);
        if (!write(block, data.data(), data.size())) {
            VirtualFreeEx(process, reinterpret_cast<LPVOID>(block), 0, MEM_RELEASE);
            return fail("could not write the new import table");
        }
        // The header: imports point at the new table, and the pre-bound import list is dropped (it describes the old one).
        IMAGE_DATA_DIRECTORY directories[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
        std::memcpy(directories, nt.OptionalHeader.DataDirectory, sizeof(directories));
        directories[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = blockOffset;
        directories[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = entriesSize;
        directories[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = {};
        const std::uintptr_t directoriesAddress = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
        DWORD oldProtection = 0;
        if (!VirtualProtectEx(process, reinterpret_cast<LPVOID>(directoriesAddress), sizeof(directories), PAGE_READWRITE, &oldProtection)) {
            VirtualFreeEx(process, reinterpret_cast<LPVOID>(block), 0, MEM_RELEASE);
            return fail("the exe header could not be made writable");
        }
        const bool written = write(directoriesAddress, directories, sizeof(directories));
        VirtualProtectEx(process, reinterpret_cast<LPVOID>(directoriesAddress), sizeof(directories), oldProtection, &oldProtection);
        if (!written) {
            VirtualFreeEx(process, reinterpret_cast<LPVOID>(block), 0, MEM_RELEASE);
            return fail("could not write the exe header");
        }
        Logger::Debug("KeeperForge2.dll will load with the game's own DLLs.");
        return {};
    }
    bool Launcher::InjectDLL(HANDLE process, const fs::path& dllPath, uintptr_t& remoteModule) {
        remoteModule = 0;
        if (!process || process == INVALID_HANDLE_VALUE) { return false; }
        if (!fs::exists(dllPath)) { Logger::Error("DLL not found: " + dllPath.string()); }
        std::wstring dll = dllPath.wstring();
        SIZE_T size = (dll.size() + 1) * sizeof(wchar_t);
        void* memory = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!memory) { Logger::Error("Failed to inject KeeperForge2.dll. VirtualAllocEx error: " + std::to_string(GetLastError())); }
        if (!WriteProcessMemory(process, memory, dll.c_str(), size, nullptr)) { Logger::Error("Failed to inject KeeperForge2.dll. WriteProcessMemory error: " + std::to_string(GetLastError())); }
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(LoadLibraryW), memory, 0, nullptr);
        if (!thread) { Logger::Error("Failed to inject KeeperForge2.dll. CreateRemoteThread error: " + std::to_string(GetLastError())); }
        DWORD waitResult = WaitForSingleObject(thread, INFINITE);
        if (waitResult != WAIT_OBJECT_0) { Logger::Error("Failed waiting for the KeeperForge2.dll loader thread."); }
        DWORD exitCode = 0;
        if (!GetExitCodeThread(thread, &exitCode)) { Logger::Error("Failed to inject KeeperForge2.dll. GetExitCodeThread error: " + std::to_string(GetLastError())); }
        CloseHandle(thread);
        VirtualFreeEx(process, memory, 0, MEM_RELEASE);
        if (exitCode == 0) { Logger::Error("Failed to inject KeeperForge2.dll. LoadLibraryW failed inside DKII-DX.exe."); }
        remoteModule = static_cast<uintptr_t>(exitCode);
        return true;
    }
    bool Launcher::InitializeRuntime(HANDLE process, uintptr_t remoteModule) {
        if (!process || process == INVALID_HANDLE_VALUE || remoteModule == 0) { return false; }
        const fs::path dllPath = fs::current_path() / "KeeperForge2.dll";
        if (!fs::exists(dllPath)) { Logger::Error("KeeperForge2.dll not found."); }
        const auto initializeRva = PEUtils::GetExportRva(dllPath, "KeeperForge2_Initialize");
        if (!initializeRva) { Logger::Error("Failed to find KeeperForge2_Initialize export: " + std::string(initializeRva.error()) + "."); }
        Logger::Debug("KeeperForge2_Initialize RVA: " + StringUtils::ToHex(*initializeRva));
        const uintptr_t remoteFunction = remoteModule + static_cast<uintptr_t>(*initializeRva);
        Logger::Debug("Remote initialize address: " + StringUtils::ToHex(remoteFunction));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteFunction), nullptr, 0, nullptr);
        if (!thread) { Logger::Error("Failed to start KeeperForge2 runtime. CreateRemoteThread error: " + std::to_string(GetLastError())); }
        const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
        if (waitResult != WAIT_OBJECT_0) { Logger::Error("Failed waiting for KeeperForge2 runtime initialization."); }
        DWORD exitCode = 0;
        if (!GetExitCodeThread(thread, &exitCode)) { Logger::Error("Failed to start KeeperForge2 runtime. GetExitCodeThread error: " + std::to_string(GetLastError())); }
        CloseHandle(thread);
        // A fatal error inside the game already showed its own message and closed the game.
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            Logger::Info("DKII-DX.exe closed during KeeperForge2 runtime initialization.");
            return false;
        }
        if (exitCode == 0) { Logger::Error("KeeperForge2 runtime initialization failed."); }
        Logger::Info("KeeperForge2 runtime initialized successfully.");
        return true;
    }
}
