#include "InstructionDecoder.h"
#include <array>
#include <cstring>
namespace KeeperForge2 {
	namespace {
		// What follows an opcode byte.
		enum Operands : std::uint8_t {
			NoOperands = 0,
			ModRm = 1 << 0,
			Imm8 = 1 << 1,
			Imm16 = 1 << 2,
			ImmSized = 1 << 3,   // 4 bytes, or 2 with the operand size prefix
			MemOffset = 1 << 4,  // 4 bytes, or 2 with the address size prefix
			FarPointer = 1 << 5, // 6 bytes, or 4 with the operand size prefix
			Unknown = 1 << 6,
			IsPrefix = 1 << 7,
		};
		constexpr std::size_t MaxLength = 15;

		constexpr std::array<std::uint8_t, 256> MakeOneByteTable() {
			std::array<std::uint8_t, 256> table{};
			// 00-3F: eight arithmetic groups (add, or, adc, sbb, and, sub, xor, cmp), each r/m forms then al/eax
			// with an immediate. The rest of each row is segment pushes and pops, prefixes and the BCD adjusts.
			for (int group = 0; group < 8; ++group) {
				const int base = group * 8;
				for (int i = 0; i < 4; ++i) table[base + i] = ModRm;
				table[base + 4] = Imm8;
				table[base + 5] = ImmSized;
			}
			for (const int prefix : { 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3 }) table[prefix] = IsPrefix;
			table[0x62] = ModRm; // bound
			table[0x63] = ModRm; // arpl
			table[0x68] = ImmSized;
			table[0x69] = ModRm | ImmSized;
			table[0x6A] = Imm8;
			table[0x6B] = ModRm | Imm8;
			for (int i = 0x70; i <= 0x7F; ++i) table[i] = Imm8; // jcc rel8
			table[0x80] = ModRm | Imm8;
			table[0x81] = ModRm | ImmSized;
			table[0x82] = ModRm | Imm8;
			table[0x83] = ModRm | Imm8;
			for (int i = 0x84; i <= 0x8F; ++i) table[i] = ModRm;
			table[0x9A] = FarPointer; // call far
			for (int i = 0xA0; i <= 0xA3; ++i) table[i] = MemOffset;
			table[0xA8] = Imm8;
			table[0xA9] = ImmSized;
			for (int i = 0xB0; i <= 0xB7; ++i) table[i] = Imm8;
			for (int i = 0xB8; i <= 0xBF; ++i) table[i] = ImmSized;
			table[0xC0] = ModRm | Imm8;
			table[0xC1] = ModRm | Imm8;
			table[0xC2] = Imm16; // ret n
			table[0xC4] = ModRm; // les (VEX when mod is 3, refused in Decode)
			table[0xC5] = ModRm; // lds (likewise)
			table[0xC6] = ModRm | Imm8;
			table[0xC7] = ModRm | ImmSized;
			table[0xC8] = Imm16 | Imm8; // enter
			table[0xCA] = Imm16; // retf n
			table[0xCD] = Imm8;
			for (int i = 0xD0; i <= 0xD3; ++i) table[i] = ModRm;
			table[0xD4] = Imm8;
			table[0xD5] = Imm8;
			for (int i = 0xD8; i <= 0xDF; ++i) table[i] = ModRm; // x87
			for (int i = 0xE0; i <= 0xE7; ++i) table[i] = Imm8; // loops, jecxz, in, out
			table[0xE8] = ImmSized;
			table[0xE9] = ImmSized;
			table[0xEA] = FarPointer;
			table[0xEB] = Imm8;
			table[0xF6] = ModRm; // test takes an immediate too, added in Decode
			table[0xF7] = ModRm;
			table[0xFE] = ModRm;
			table[0xFF] = ModRm;
			return table;
		}
		// Second byte after 0F. Most take a ModRM byte.
		constexpr std::array<std::uint8_t, 256> MakeTwoByteTable() {
			std::array<std::uint8_t, 256> table{};
			table.fill(ModRm);
			for (const int none : { 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0E, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x37, 0x77,
				0xA0, 0xA1, 0xA2, 0xA8, 0xA9, 0xAA }) {
				table[none] = NoOperands;
			}
			for (int i = 0xC8; i <= 0xCF; ++i) table[i] = NoOperands; // bswap
			for (const int unknown : { 0x04, 0x0A, 0x0C, 0x36, 0x39, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x7A, 0x7B, 0xA6, 0xA7 }) table[unknown] = Unknown;
			table[0x0F] = ModRm | Imm8; // 3DNow!, its opcode comes last
			for (int i = 0x70; i <= 0x73; ++i) table[i] = ModRm | Imm8;
			for (int i = 0x80; i <= 0x8F; ++i) table[i] = ImmSized; // jcc rel32
			for (const int withImmediate : { 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6 }) table[withImmediate] = ModRm | Imm8;
			return table;
		}
		constexpr auto OneByte = MakeOneByteTable();
		constexpr auto TwoByte = MakeTwoByteTable();

		std::int32_t Read32(const std::uint8_t* at) {
			std::int32_t value;
			std::memcpy(&value, at, sizeof(value));
			return value;
		}
	}

	Instruction InstructionDecoder::Decode(std::uintptr_t address) {
		static_assert(sizeof(void*) == 4, "the decoder reads 32-bit code");
		const auto* start = reinterpret_cast<const std::uint8_t*>(address);
		const std::uint8_t* at = start;
		const auto tooLong = [&] { return static_cast<std::size_t>(at - start) >= MaxLength; };
		bool operandSize16 = false;
		bool addressSize16 = false;
		while (OneByte[*at] & IsPrefix) {
			if (*at == 0x66) operandSize16 = true;
			if (*at == 0x67) addressSize16 = true;
			++at;
			if (tooLong()) return {};
		}

		const std::uint8_t opcode = *at++;
		bool twoByte = false;
		std::uint8_t second = 0;
		std::uint8_t operands = OneByte[opcode];
		if (opcode == 0x0F) {
			twoByte = true;
			second = *at++;
			if (second == 0x38) {
				++at;
				operands = ModRm;
			}
			else if (second == 0x3A) {
				++at;
				operands = ModRm | Imm8;
			}
			else operands = TwoByte[second];
		}
		if (operands & Unknown) return {};

		std::uint8_t reg = 0;
		if (operands & ModRm) {
			const std::uint8_t modRm = *at++;
			const std::uint8_t mod = modRm >> 6;
			const std::uint8_t rm = modRm & 7;
			reg = (modRm >> 3) & 7;
			// les/lds/bound with a register operand are VEX/EVEX prefixes; pop r/m with reg not 0 is XOP.
			if (!twoByte && mod == 3 && (opcode == 0xC4 || opcode == 0xC5 || opcode == 0x62)) return {};
			if (!twoByte && opcode == 0x8F && reg != 0) return {};
			if (!twoByte && reg <= 1) {
				if (opcode == 0xF6) operands |= Imm8;
				if (opcode == 0xF7) operands |= ImmSized;
			}
			if (mod != 3) {
				if (addressSize16) {
					if (mod == 1) at += 1;
					else if (mod == 2 || rm == 6) at += 2;
				}
				else {
					bool disp32 = mod == 2 || (mod == 0 && rm == 5);
					if (rm == 4) {
						const std::uint8_t sib = *at++;
						if (mod == 0 && (sib & 7) == 5) disp32 = true;
					}
					if (mod == 1) at += 1;
					else if (disp32) at += 4;
				}
			}
		}
		if (operands & Imm8) at += 1;
		if (operands & Imm16) at += 2;
		if (operands & ImmSized) at += operandSize16 ? 2 : 4;
		if (operands & MemOffset) at += addressSize16 ? 2 : 4;
		if (operands & FarPointer) at += operandSize16 ? 4 : 6;

		Instruction result;
		result.length = static_cast<std::size_t>(at - start);
		if (result.length > MaxLength) return {};
		const std::uintptr_t next = address + result.length;
		const auto rel8 = [&] { return next + static_cast<std::uintptr_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(at[-1]))); };
		const auto rel32 = [&] { return next + static_cast<std::uintptr_t>(Read32(at - 4)); };
		if (!twoByte) {
			if (opcode >= 0x70 && opcode <= 0x7F) {
				result.branch = Instruction::Branch::Conditional;
				result.condition = opcode & 0x0F;
				result.branchTarget = rel8();
			}
			else if (opcode >= 0xE0 && opcode <= 0xE3) {
				result.branch = Instruction::Branch::Loop;
				result.branchTarget = rel8();
			}
			else if (opcode == 0xEB) {
				result.branch = Instruction::Branch::Jump;
				result.branchTarget = rel8();
			}
			else if (opcode == 0xE8 || opcode == 0xE9) {
				// The 16-bit forms cut the target to 16 bits: not worth moving.
				if (operandSize16) return {};
				result.branch = opcode == 0xE8 ? Instruction::Branch::Call : Instruction::Branch::Jump;
				result.branchTarget = rel32();
			}
			result.endsFlow = opcode == 0xC2 || opcode == 0xC3 || opcode == 0xCA || opcode == 0xCB || opcode == 0xCF
				|| opcode == 0xE9 || opcode == 0xEA || opcode == 0xEB || (opcode == 0xFF && (reg == 4 || reg == 5));
		}
		else if (second >= 0x80 && second <= 0x8F) {
			if (operandSize16) return {};
			result.branch = Instruction::Branch::Conditional;
			result.condition = second & 0x0F;
			result.branchTarget = rel32();
		}
		return result;
	}
}
