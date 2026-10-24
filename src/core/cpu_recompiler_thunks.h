#pragma once
#include "common/platform.h"
#include "cpu_code_cache.h"
#include "cpu_types.h"

namespace CPU {
struct CodeBlock;
struct CodeBlockInstruction;

namespace Recompiler::Thunks {

//////////////////////////////////////////////////////////////////////////
// Trampolines for calling back from the JIT
// Needed because we can't cast member functions to void*...
// TODO: Abuse carry flag or something else for exception
//////////////////////////////////////////////////////////////////////////
bool InterpretInstruction();
bool InterpretInstructionPGXP();

// Memory access functions for the JIT - MSB is set on exception.
uint64_t ReadMemoryByte(uint32_t address);
uint64_t ReadMemoryHalfWord(uint32_t address);
uint64_t ReadMemoryWord(uint32_t address);
uint32_t WriteMemoryByte(uint32_t address, uint32_t value);
uint32_t WriteMemoryHalfWord(uint32_t address, uint32_t value);
uint32_t WriteMemoryWord(uint32_t address, uint32_t value);

// Unchecked memory access variants. No alignment or bus exceptions.
uint32_t UncheckedReadMemoryByte(uint32_t address);
uint32_t UncheckedReadMemoryHalfWord(uint32_t address);
uint32_t UncheckedReadMemoryWord(uint32_t address);
void UncheckedWriteMemoryByte(uint32_t address, uint32_t value);
void UncheckedWriteMemoryHalfWord(uint32_t address, uint32_t value);
void UncheckedWriteMemoryWord(uint32_t address, uint32_t value);

void ResolveBranch(CodeBlock* block, void* host_pc, void* host_resolve_pc, uint32_t host_pc_size);

#ifdef CPU_POWERPC
// The PowerPC ABIs differ on 64-bit returns (a register pair, or one register) and on a union by value (a pointer to a
// copy, or the bits in a register), so the generated code calls these in their place, which take and give 32-bit values.
// A failed read returns 0 and leaves its exception code where the generated code looks, 0 when it worked.
uint32_t ReadMemoryByte32(uint32_t address);
uint32_t ReadMemoryHalfWord32(uint32_t address);
uint32_t ReadMemoryWord32(uint32_t address);
void ExecuteGTE(uint32_t instruction_bits, const void* implementation);
#endif

} // namespace Recompiler::Thunks

} // namespace CPU
