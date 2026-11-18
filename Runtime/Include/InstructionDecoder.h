#pragma once
#include <cstddef>
#include <cstdint>
namespace KeeperForge2 {
	// One 32-bit x86 instruction as far as moving it elsewhere needs: its length and, for a branch relative to where it
	// sits, where it goes. Covers the general-purpose, x87, MMX and SSE instructions; VEX/EVEX/XOP encoded ones (AVX)
	// and 16-bit relative branches are not understood.
	struct Instruction {
		enum class Branch : std::uint8_t {
			None,
			Jump,        // jmp rel8 / rel32
			Call,        // call rel32
			Conditional, // jcc rel8 / rel32, condition in condition
			Loop,        // loop, loope, loopne, jecxz (rel8 only)
		};
		std::size_t length = 0; // 0 = not understood
		Branch branch = Branch::None;
		std::uint8_t condition = 0; // 0-15, the low four bits of a jcc's opcode
		std::uintptr_t branchTarget = 0;
		// ret, iret and every unconditional jump (relative, indirect or far): the next instruction is not reached from
		// this one.
		bool endsFlow = false;
	};

	class InstructionDecoder {
		public:
			// Reads the instruction at address (up to 15 bytes).
			static Instruction Decode(std::uintptr_t address);
	};
}
