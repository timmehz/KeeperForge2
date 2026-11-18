#include "HookManager.h"
#include "InstructionDecoder.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// Our own function hooking (32-bit x86 only). Hooking a function:
//  - Its first whole instructions covering at least 5 bytes are copied into a trampoline, with relative branches
//    pointed back at where they went (short ones widened), and a jump to the rest of the function after them.
//    Calling the trampoline runs the original function.
//  - Enabling writes "jmp detour" over the function's first 5 bytes; disabling puts the bytes back.
//  - While the bytes change, every other thread of the game is suspended, and a thread stopped inside the bytes being
//    replaced is moved to the same instruction in the trampoline (or back, when disabling). Nothing is allocated
//    while threads are suspended: one of them may hold the heap's lock.
namespace KeeperForge2 {
	namespace {
		static_assert(sizeof(void*) == 4, "the hooks write 32-bit jumps");
		constexpr std::size_t JumpSize = 5;
		constexpr std::size_t TrampolineSize = 64;
		constexpr std::size_t BlockSize = 0x10000;
		// The most one moved instruction can grow to (a loop: itself, a short jump and a long jump).
		constexpr std::size_t LargestMoved = 15 + 2 + 5;

		struct Hook {
			std::uintptr_t target = 0;
			void* detour = nullptr;
			std::uint8_t* trampoline = nullptr;
			std::array<std::uint8_t, JumpSize> saved{};
			// Where each moved instruction starts: in the function, and in the trampoline.
			std::array<std::pair<std::uint8_t, std::uint8_t>, JumpSize> moved{};
			std::size_t movedCount = 0;
			bool enabled = false;
		};

		std::mutex lock;
		bool initialized = false;
		std::vector<std::unique_ptr<Hook>> hooks;
		std::vector<std::uint8_t*> freeTrampolines;

		bool IsExecutable(const void* address) {
			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)) return false;
			return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}
		Hook* Find(std::uintptr_t target) {
			const auto found = std::ranges::find(hooks, target, [](const auto& hook) { return hook->target; });
			return found != hooks.end() ? found->get() : nullptr;
		}

		// Trampolines come from blocks of executable memory, cut into equal slots. A removed hook's trampoline is kept
		// as it is, never reused or freed: a thread may still be running in it. Only one whose hook could not be made
		// goes back.
		std::uint8_t* TakeTrampoline() {
			if (freeTrampolines.empty()) {
				auto* block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
				if (!block) return nullptr;
				for (std::size_t offset = BlockSize; offset >= TrampolineSize; offset -= TrampolineSize) freeTrampolines.push_back(block + offset - TrampolineSize);
			}
			std::uint8_t* trampoline = freeTrampolines.back();
			freeTrampolines.pop_back();
			std::memset(trampoline, 0xCC, TrampolineSize);
			return trampoline;
		}
		void GiveBackTrampoline(std::uint8_t* trampoline) {
			std::memset(trampoline, 0xCC, TrampolineSize);
			freeTrampolines.push_back(trampoline);
		}

		// A 5-byte call or jump to destination, written at out, for code that will run from runsAt (out itself unless
		// the bytes are made in a buffer first).
		void WriteRelative(std::uint8_t* out, std::uint8_t opcode, std::uintptr_t destination, std::uintptr_t runsAt) {
			out[0] = opcode;
			const auto offset = static_cast<std::int32_t>(destination - (runsAt + 5));
			std::memcpy(out + 1, &offset, sizeof(offset));
		}
		void WriteRelative(std::uint8_t* at, std::uint8_t opcode, std::uintptr_t destination) {
			WriteRelative(at, opcode, destination, reinterpret_cast<std::uintptr_t>(at));
		}

		// Fills hook.trampoline from the start of hook.target. False if the start cannot be moved.
		bool BuildTrampoline(Hook& hook) {
			const std::uintptr_t target = hook.target;
			std::uint8_t* out = hook.trampoline;
			std::size_t written = 0;
			std::size_t offset = 0;
			bool flowEnded = false;
			while (offset < JumpSize) {
				const std::uintptr_t at = target + offset;
				const Instruction instruction = InstructionDecoder::Decode(at);
				if (instruction.length == 0) return false;
				// A branch into the bytes the jump replaces would land in the middle of the jump.
				if (instruction.branch != Instruction::Branch::None && instruction.branchTarget > target && instruction.branchTarget < target + JumpSize) return false;
				if (written + LargestMoved + JumpSize > TrampolineSize) return false;
				hook.moved[hook.movedCount++] = { static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(written) };
				const auto* source = reinterpret_cast<const std::uint8_t*>(at);
				switch (instruction.branch) {
					case Instruction::Branch::None:
						std::memcpy(out + written, source, instruction.length);
						written += instruction.length;
						break;
					case Instruction::Branch::Call:
						WriteRelative(out + written, 0xE8, instruction.branchTarget);
						written += 5;
						break;
					case Instruction::Branch::Jump:
						WriteRelative(out + written, 0xE9, instruction.branchTarget);
						written += 5;
						break;
					case Instruction::Branch::Conditional:
						out[written++] = 0x0F;
						WriteRelative(out + written, static_cast<std::uint8_t>(0x80 | instruction.condition), instruction.branchTarget);
						written += 5;
						break;
					case Instruction::Branch::Loop:
						// "loop +2; jmp +5; jmp target": the loop (prefixes kept) skips to the long jump when taken.
						std::memcpy(out + written, source, instruction.length - 1);
						written += instruction.length - 1;
						out[written++] = 0x02;
						out[written++] = 0xEB;
						out[written++] = 0x05;
						WriteRelative(out + written, 0xE9, instruction.branchTarget);
						written += 5;
						break;
				}
				offset += instruction.length;
				if (instruction.endsFlow) {
					flowEnded = true;
					break;
				}
			}
			// A function shorter than the jump: the bytes after its end are overwritten too, so they must be the
			// filler compilers put between functions.
			for (std::size_t i = offset; i < JumpSize; ++i) {
				const std::uint8_t filler = reinterpret_cast<const std::uint8_t*>(target)[i];
				if (filler != 0xCC && filler != 0x90) return false;
			}
			if (!flowEnded) WriteRelative(out + written, 0xE9, target + offset);
			return true;
		}

		// The other threads of the game, suspended.
		class FrozenThreads {
			public:
				FrozenThreads() {
					// Their ids first, while nothing is suspended (the list allocates).
					const DWORD process = GetCurrentProcessId();
					const DWORD self = GetCurrentThreadId();
					std::vector<DWORD> ids;
					const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
					if (snapshot != INVALID_HANDLE_VALUE) {
						THREADENTRY32 entry{};
						entry.dwSize = sizeof(entry);
						for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
							if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) ids.push_back(entry.th32ThreadID);
						}
						CloseHandle(snapshot);
					}
					threads.reserve(ids.size());
					for (const DWORD id : ids) {
						const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
						if (!thread) continue;
						if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
							CloseHandle(thread);
							continue;
						}
						threads.push_back(thread);
					}
				}
				~FrozenThreads() {
					for (const HANDLE thread : threads) {
						ResumeThread(thread);
						CloseHandle(thread);
					}
				}
				FrozenThreads(const FrozenThreads&) = delete;
				FrozenThreads& operator=(const FrozenThreads&) = delete;

				// Moves threads stopped in the bytes being replaced into the trampoline (enabling), or threads stopped in
				// the trampoline's copy back into the function (disabling).
				void MoveThreads(const Hook& hook, bool enabling) const {
					const auto trampoline = reinterpret_cast<std::uintptr_t>(hook.trampoline);
					for (const HANDLE thread : threads) {
						CONTEXT context{};
						context.ContextFlags = CONTEXT_CONTROL;
						if (!GetThreadContext(thread, &context)) continue;
						const std::uintptr_t from = enabling ? hook.target : trampoline;
						if (context.Eip < from || context.Eip >= from + TrampolineSize) continue;
						for (std::size_t i = 0; i < hook.movedCount; ++i) {
							const auto [functionOffset, trampolineOffset] = hook.moved[i];
							if (context.Eip != from + (enabling ? functionOffset : trampolineOffset)) continue;
							context.Eip = static_cast<DWORD>(enabling ? trampoline + trampolineOffset : hook.target + functionOffset);
							SetThreadContext(thread, &context);
							break;
						}
					}
				}
			private:
				std::vector<HANDLE> threads;
		};

		bool WriteCode(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size) {
			DWORD oldProtect = 0;
			if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
			std::memcpy(reinterpret_cast<void*>(address), bytes, size);
			VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
			return true;
		}
		bool SetEnabled(Hook& hook, bool enable, const FrozenThreads& frozen) {
			if (hook.enabled == enable) return true;
			std::array<std::uint8_t, JumpSize> bytes = hook.saved;
			// Made here, run at the function's start: the jump's distance is from there.
			if (enable) WriteRelative(bytes.data(), 0xE9, reinterpret_cast<std::uintptr_t>(hook.detour), hook.target);
			if (!WriteCode(hook.target, bytes.data(), bytes.size())) return false;
			frozen.MoveThreads(hook, enable);
			hook.enabled = enable;
			return true;
		}
	}

	bool HookManager::Initialize() {
		std::lock_guard guard(lock);
		initialized = true;
		return true;
	}
	void HookManager::Shutdown() {
		std::lock_guard guard(lock);
		if (!initialized) return;
		{
			const FrozenThreads frozen;
			for (const auto& hook : hooks) SetEnabled(*hook, false, frozen);
		}
		hooks.clear();
		initialized = false;
	}
	bool HookManager::CreateHook(std::uintptr_t target, void* detour, void** original) {
		std::lock_guard guard(lock);
		if (!initialized || target == 0 || detour == nullptr || original == nullptr) return false;
		if (Find(target) || !IsExecutable(reinterpret_cast<const void*>(target)) || !IsExecutable(detour)) return false;
		auto hook = std::make_unique<Hook>();
		hook->target = target;
		hook->detour = detour;
		hook->trampoline = TakeTrampoline();
		if (!hook->trampoline) return false;
		if (!BuildTrampoline(*hook)) {
			GiveBackTrampoline(hook->trampoline);
			return false;
		}
		FlushInstructionCache(GetCurrentProcess(), hook->trampoline, TrampolineSize);
		std::memcpy(hook->saved.data(), reinterpret_cast<const void*>(target), JumpSize);
		*original = hook->trampoline;
		hooks.push_back(std::move(hook));
		return true;
	}
	bool HookManager::EnableHook(std::uintptr_t target) {
		std::lock_guard guard(lock);
		Hook* hook = initialized ? Find(target) : nullptr;
		if (!hook) return false;
		if (hook->enabled) return true;
		const FrozenThreads frozen;
		return SetEnabled(*hook, true, frozen);
	}
	bool HookManager::DisableHook(std::uintptr_t target) {
		std::lock_guard guard(lock);
		Hook* hook = initialized ? Find(target) : nullptr;
		if (!hook) return false;
		if (!hook->enabled) return true;
		const FrozenThreads frozen;
		return SetEnabled(*hook, false, frozen);
	}
	bool HookManager::RemoveHook(std::uintptr_t target) {
		std::lock_guard guard(lock);
		Hook* hook = initialized ? Find(target) : nullptr;
		if (!hook) return false;
		if (hook->enabled) {
			const FrozenThreads frozen;
			if (!SetEnabled(*hook, false, frozen)) return false;
		}
		std::erase_if(hooks, [hook](const auto& entry) { return entry.get() == hook; });
		return true;
	}
	bool HookManager::IsInitialized() {
		std::lock_guard guard(lock);
		return initialized;
	}
}
