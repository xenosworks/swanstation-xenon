#include "common/log.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#include "gte.h"
#include "settings.h"
#include "timing_event.h"
Log_SetChannel(CPU::Recompiler);

// The backend for 32-bit PowerPC code: the console (xsdk) and 32-bit PowerPC Linux, both big-endian. Generated code is
// word instructions only (the console's registers are 64 bits wide in 32-bit mode, and a 32-bit value there has an
// undefined high half), so one backend serves both; only the dispatcher's frame differs (xsdk saves r14-r31 whole).
//
// Blocks have no frame: they run in the dispatcher's, call C with r1 as it is, and leave through RRET, which holds the
// address the dispatcher's call to a block returns to. They never touch LR except around their own calls into C.
//
// Registers:
//   r0, r11, r12  scratch, never allocated (r0 only as a destination or an RS/RB operand: addi and loads read it as 0)
//   r3-r6         arguments to C (RARG1-4) and the return value (RRETURN), never allocated
//   r7-r10        allocated, caller-saved
//   r14-r29       allocated, callee-saved
//   r30 (RRET)    the return address blocks leave through
//   r31 (RCPUPTR) &CPU::g_state
//   r2, r13       never written: the thread and small-data pointers
// Only CR0 is used, and only between a compare or test and the branch or set that reads it.

namespace CPU::Recompiler {

constexpr HostReg RRETURN = 3;
constexpr HostReg RARG1 = 3;
constexpr HostReg RARG2 = 4;
constexpr HostReg RARG3 = 5;
constexpr HostReg RARG4 = 6;
constexpr HostReg RSCRATCH = 12;
constexpr HostReg RSCRATCH2 = 11;
constexpr HostReg RSCRATCH3 = 0;
constexpr HostReg RRET = 30;
constexpr HostReg RCPUPTR = 31;

// The dispatcher's frame, from r1 up: the bottom 0x50 bytes are the callees' (SysV keeps the back chain and a link
// register slot there, the console's convention the parameter area, whose first 0x40 bytes a variadic callee spills
// into); then the blocks' spills of caller-saved registers around calls, then what the dispatcher saves.
constexpr int32_t FRAME_SPILL_OFFSET = 0x50;
constexpr uint32_t FRAME_SPILL_SLOTS = 8;
constexpr int32_t FRAME_SAVE_OFFSET = FRAME_SPILL_OFFSET + FRAME_SPILL_SLOTS * 8;
constexpr uint32_t FRAME_SAVE_SLOTS = 24;
constexpr int32_t FRAME_LR_OFFSET = FRAME_SAVE_OFFSET + FRAME_SAVE_SLOTS * 8;
constexpr int32_t FRAME_SIZE = 0x160;
static_assert(FRAME_LR_OFFSET + 8 <= FRAME_SIZE && (FRAME_SIZE % 16) == 0);

// Where blocks go to return to the dispatcher: mtlr RRET; blr.
static void* s_return_stub;

// A failed read leaves its exception code here (see Thunks below).
static uint32_t s_memory_exception;

// What the last EmitCmp, EmitTest, or EmitAdd or EmitSub with flags left for the branch or set that reads it. Compares
// and tests are emitted when read, since only then is it known whether they are signed.
struct PendingFlags
{
  enum class Kind : uint8_t
  {
    None,
    Compare,
    Test,
    Overflow
  };

  Kind kind = Kind::None;
  HostReg lhs = 0;
  bool rhs_is_constant = false;
  HostReg rhs_reg = 0;
  uint32_t rhs_constant = 0;
};

static PendingFlags s_flags;

static bool FitsSigned16(uint32_t value)
{
  const int32_t signed_value = static_cast<int32_t>(value);
  return signed_value >= -32768 && signed_value <= 32767;
}

// Whether value is one run of ones, wrapping from the low bits round to the high ones if need be, as rlwinm's mask is;
// mb and me are its first and last bit, counted from the most significant.
static bool IsMask(uint32_t value, uint32_t* mb, uint32_t* me)
{
  if (value == 0 || value == 0xFFFFFFFFu)
    return false;

  uint32_t transitions = 0;
  for (uint32_t bit = 0; bit < 32; bit++)
  {
    if (((value >> bit) & 1) != ((value >> ((bit + 1) & 31)) & 1))
      transitions++;
  }
  if (transitions != 2)
    return false;

  // The first set bit with a clear one before it, counted from the top, is MB; the last before a clear one is ME.
  for (uint32_t i = 0; i < 32; i++)
  {
    const uint32_t bit = 31 - i;
    const uint32_t previous_bit = 31 - ((i + 31) & 31);
    const bool set = ((value >> bit) & 1) != 0;
    const bool previous_set = ((value >> previous_bit) & 1) != 0;
    if (set && !previous_set)
      *mb = i;
    if (set && (((value >> (31 - ((i + 1) & 31))) & 1) == 0))
      *me = i;
  }
  return true;
}

static void EmitConstant(CodeEmitter* e, HostReg reg, uint32_t value)
{
  e->load_immediate(reg, value);
}

// The register a Value is in, or, for a constant, scratch loaded with it.
static HostReg RegisterOrScratch(CodeEmitter* e, const Value& value, HostReg scratch)
{
  if (value.IsInHostRegister())
    return value.host_reg;

  EmitConstant(e, scratch, static_cast<uint32_t>(value.constant_value));
  return scratch;
}

CodeGenerator::CodeGenerator(JitCodeBuffer* code_buffer)
  : m_code_buffer(code_buffer), m_register_cache(*this),
    m_near_emitter(code_buffer->GetFreeCodePointer(), code_buffer->GetFreeCodeSpace()),
    m_far_emitter(code_buffer->GetFreeFarCodePointer(), code_buffer->GetFreeFarCodeSpace()), m_emit(&m_near_emitter)
{
  InitHostRegs();
}

CodeGenerator::~CodeGenerator() = default;

void CodeGenerator::AlignCodeBuffer(JitCodeBuffer* code_buffer)
{
  code_buffer->Align(16, 0);
}

void CodeGenerator::InitHostRegs()
{
  // callee-saved first: their guest values survive calls without being saved
  m_register_cache.SetHostRegAllocationOrder(
    {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 7, 8, 9, 10});
  m_register_cache.SetCallerSavedHostRegs({0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  m_register_cache.SetCalleeSavedHostRegs({14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31});
  m_register_cache.SetCPUPtrHostReg(RCPUPTR);
}

void CodeGenerator::SwitchToFarCode()
{
  m_emit = &m_far_emitter;
}

void CodeGenerator::SwitchToNearCode()
{
  m_emit = &m_near_emitter;
}

void* CodeGenerator::GetCurrentNearCodePointer() const
{
  return m_near_emitter.cursor();
}

void* CodeGenerator::GetCurrentFarCodePointer() const
{
  return m_far_emitter.cursor();
}

Value CodeGenerator::GetValueInHostRegister(const Value& value, bool allow_zero_register /* = true */)
{
  if (value.IsInHostRegister())
    return Value::FromHostReg(&m_register_cache, value.host_reg, value.size);

  Value new_value = m_register_cache.AllocateScratch(value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

Value CodeGenerator::GetValueInHostOrScratchRegister(const Value& value, bool allow_zero_register /* = true */)
{
  if (value.IsInHostRegister())
    return Value::FromHostReg(&m_register_cache, value.host_reg, value.size);

  Value new_value = Value::FromHostReg(&m_register_cache, RSCRATCH, value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

void CodeGenerator::EmitBeginBlock(bool allocate_registers /* = true */)
{
  if (!allocate_registers)
    return;

  // the dispatcher saved every callee-saved register, so blocks use them without saving
  m_register_cache.AssumeCalleeSavedRegistersAreSaved();
  m_register_cache.AllocateHostReg(RCPUPTR);
  m_register_cache.AllocateHostReg(RRET);
}

void CodeGenerator::EmitEndBlock(bool free_registers /* = true */, bool emit_return /* = true */)
{
  if (free_registers)
  {
    m_register_cache.FreeHostReg(RRET);
    m_register_cache.FreeHostReg(RCPUPTR);
    m_register_cache.PopCalleeSavedRegisters(true);
  }

  if (emit_return)
  {
    m_emit->mtlr(RRET);
    m_emit->blr();
  }
}

void CodeGenerator::EmitExceptionExit()
{
  // the cycles of what ran before the exception, as the x64 backend adds them
  AddPendingCycles(false);

  // ensure all unflushed registers are written back
  m_register_cache.FlushAllGuestRegisters(false, false);

  // the interpreter load delay might have its own value, but we'll overwrite it here anyway
  // technically RaiseException() and FlushPipeline() have already been called, but that should be okay
  m_register_cache.FlushLoadDelay(false);

  m_register_cache.PopCalleeSavedRegisters(false);

  m_emit->mtlr(RRET);
  m_emit->blr();
}

void CodeGenerator::EmitExceptionExitOnBool(const Value& value)
{
  m_register_cache.PushState();

  // a bool from C has junk above its low byte
  LabelType skip_branch;
  m_emit->rlwinm(RSCRATCH3, value.host_reg, 0, 24, 31, true);
  m_emit->bt(CR_EQ, &skip_branch);
  EmitBranch(GetCurrentFarCodePointer());
  m_emit->bind(&skip_branch);

  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();
}

void CodeGenerator::FinalizeBlock(CodeBlock::HostCodePointer* out_host_code, uint32_t* out_host_code_size)
{
  if (m_near_emitter.failed() || m_far_emitter.failed())
  {
    Log_ErrorPrint("The PowerPC emitter ran out of room or a branch out of reach, the code is wrong");
    std::abort();
  }

  *out_host_code = reinterpret_cast<CodeBlock::HostCodePointer>(m_code_buffer->GetFreeCodePointer());
  *out_host_code_size = static_cast<uint32_t>(m_near_emitter.size_generated());

  m_code_buffer->CommitCode(static_cast<uint32_t>(m_near_emitter.size_generated()));
  m_code_buffer->CommitFarCode(static_cast<uint32_t>(m_far_emitter.size_generated()));

  m_near_emitter.reset(m_code_buffer->GetFreeCodePointer(), m_code_buffer->GetFreeCodeSpace());
  m_far_emitter.reset(m_code_buffer->GetFreeFarCodePointer(), m_code_buffer->GetFreeFarCodeSpace());
}

void CodeGenerator::EmitSignExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size)
{
  switch (to_size)
  {
    case RegSize_16:
    {
      if (from_size == RegSize_8)
      {
        m_emit->extsb(to_reg, from_reg);
        m_emit->clrlwi(to_reg, to_reg, 16);
      }
    }
    break;

    case RegSize_32:
    {
      if (from_size == RegSize_8)
        m_emit->extsb(to_reg, from_reg);
      else if (from_size == RegSize_16)
        m_emit->extsh(to_reg, from_reg);
    }
    break;

    default:
      break;
  }
}

void CodeGenerator::EmitZeroExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size)
{
  switch (to_size)
  {
    case RegSize_16:
    {
      if (from_size == RegSize_8)
        m_emit->clrlwi(to_reg, from_reg, 24);
    }
    break;

    case RegSize_32:
    {
      if (from_size == RegSize_8)
        m_emit->clrlwi(to_reg, from_reg, 24);
      else if (from_size == RegSize_16)
        m_emit->clrlwi(to_reg, from_reg, 16);
    }
    break;

    default:
      break;
  }
}

void CodeGenerator::EmitCopyValue(HostReg to_reg, const Value& value)
{
  if (value.IsConstant())
  {
    EmitConstant(m_emit, to_reg, static_cast<uint32_t>(value.constant_value));
    return;
  }

  if (to_reg != value.host_reg)
    m_emit->mr(to_reg, value.host_reg);
}

// to_reg = from_reg + value, and the overflow of that as a signed 32-bit add in CR0's LT, via the sign bits:
// overflow when the result's sign differs from both operands'.
static void EmitAddWithOverflow(CodeEmitter* e, HostReg to_reg, HostReg from_reg, const Value& value)
{
  const HostReg value_reg = RegisterOrScratch(e, value, RSCRATCH2);
  const HostReg result = (to_reg != from_reg && to_reg != value_reg) ? to_reg : RSCRATCH;

  e->add(result, from_reg, value_reg);
  e->xor_(RSCRATCH3, result, from_reg);
  e->xor_(RSCRATCH2, result, value_reg);
  e->and_(RSCRATCH3, RSCRATCH3, RSCRATCH2, true);
  if (result != to_reg)
    e->mr(to_reg, result);

  s_flags.kind = PendingFlags::Kind::Overflow;
}

// to_reg = from_reg - value, overflow in CR0's LT: the operands' signs differ and the result's differs from the
// first's.
static void EmitSubWithOverflow(CodeEmitter* e, HostReg to_reg, HostReg from_reg, const Value& value)
{
  const HostReg value_reg = RegisterOrScratch(e, value, RSCRATCH2);
  const HostReg result = (to_reg != from_reg && to_reg != value_reg) ? to_reg : RSCRATCH;

  e->subf(result, value_reg, from_reg);
  e->xor_(RSCRATCH3, from_reg, value_reg);
  e->xor_(RSCRATCH2, from_reg, result);
  e->and_(RSCRATCH3, RSCRATCH3, RSCRATCH2, true);
  if (result != to_reg)
    e->mr(to_reg, result);

  s_flags.kind = PendingFlags::Kind::Overflow;
}

void CodeGenerator::EmitAdd(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags)
{
  if (set_flags)
  {
    EmitAddWithOverflow(m_emit, to_reg, from_reg, value);
    return;
  }

  if (value.IsInHostRegister())
    m_emit->add(to_reg, from_reg, value.host_reg);
  else
    m_emit->add_immediate(to_reg, from_reg, static_cast<uint32_t>(value.constant_value));
}

void CodeGenerator::EmitSub(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags)
{
  if (set_flags)
  {
    EmitSubWithOverflow(m_emit, to_reg, from_reg, value);
    return;
  }

  if (value.IsInHostRegister())
    m_emit->subf(to_reg, value.host_reg, from_reg);
  else
    m_emit->add_immediate(to_reg, from_reg, 0u - static_cast<uint32_t>(value.constant_value));
}

void CodeGenerator::EmitCmp(HostReg to_reg, const Value& value)
{
  s_flags.kind = PendingFlags::Kind::Compare;
  s_flags.lhs = to_reg;
  s_flags.rhs_is_constant = !value.IsInHostRegister();
  s_flags.rhs_reg = value.host_reg;
  s_flags.rhs_constant = static_cast<uint32_t>(value.constant_value);
}

void CodeGenerator::EmitMul(HostReg to_reg_hi, HostReg to_reg_lo, const Value& lhs, const Value& rhs,
                            bool signed_multiply)
{
  const HostReg lhs_reg = RegisterOrScratch(m_emit, lhs, RSCRATCH);
  const HostReg rhs_reg = RegisterOrScratch(m_emit, rhs, RSCRATCH2);

  // the high half goes through a scratch when either result's register is an operand
  const bool aliased =
    (to_reg_hi == lhs_reg || to_reg_hi == rhs_reg || to_reg_lo == lhs_reg || to_reg_lo == rhs_reg);
  const HostReg high = aliased ? RSCRATCH3 : to_reg_hi;

  if (signed_multiply)
    m_emit->mulhw(high, lhs_reg, rhs_reg);
  else
    m_emit->mulhwu(high, lhs_reg, rhs_reg);
  m_emit->mullw(to_reg_lo, lhs_reg, rhs_reg);
  if (high != to_reg_hi)
    m_emit->mr(to_reg_hi, high);
}

void CodeGenerator::EmitDiv(HostReg to_reg_quotient, HostReg to_reg_remainder, HostReg num, HostReg denom, RegSize size,
                            bool signed_divide)
{
  // only 32-bit supported for now..
  const bool aliased = (to_reg_quotient == num || to_reg_quotient == denom);
  const HostReg quotient = aliased ? RSCRATCH : to_reg_quotient;

  if (signed_divide)
    m_emit->divw(quotient, num, denom);
  else
    m_emit->divwu(quotient, num, denom);

  if (to_reg_remainder != HostReg_Count)
  {
    m_emit->mullw(RSCRATCH2, quotient, denom);
    m_emit->subf(to_reg_remainder, RSCRATCH2, num);
  }

  if (quotient != to_reg_quotient)
    m_emit->mr(to_reg_quotient, quotient);
}

// rd = rs shifted by the amount, whose low 5 bits count: in the instruction's immediate, or masked into a scratch for
// slw, srw and sraw, which would take 6.
static HostReg MaskedShiftAmount(CodeEmitter* e, const Value& amount_value, bool assume_amount_masked)
{
  if (assume_amount_masked)
    return amount_value.host_reg;

  e->clrlwi(RSCRATCH, amount_value.host_reg, 27);
  return RSCRATCH;
}

static void MaskToSize(CodeEmitter* e, HostReg reg, RegSize size)
{
  if (size == RegSize_8)
    e->clrlwi(reg, reg, 24);
  else if (size == RegSize_16)
    e->clrlwi(reg, reg, 16);
}

void CodeGenerator::EmitShl(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
{
  if (amount_value.IsConstant())
  {
    const uint32_t amount = static_cast<uint32_t>(amount_value.constant_value) & 0x1F;
    if (amount == 0)
      m_emit->mr(to_reg, from_reg);
    else
      m_emit->slwi(to_reg, from_reg, amount);
  }
  else
  {
    m_emit->slw(to_reg, from_reg, MaskedShiftAmount(m_emit, amount_value, assume_amount_masked));
  }

  MaskToSize(m_emit, to_reg, size);
}

void CodeGenerator::EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
{
  // the part above the size shifts down into the result otherwise
  HostReg source = from_reg;
  if (size != RegSize_32)
  {
    EmitZeroExtend(RSCRATCH2, RegSize_32, from_reg, size);
    source = RSCRATCH2;
  }

  if (amount_value.IsConstant())
  {
    const uint32_t amount = static_cast<uint32_t>(amount_value.constant_value) & 0x1F;
    if (amount == 0)
      m_emit->mr(to_reg, source);
    else
      m_emit->srwi(to_reg, source, amount);
  }
  else
  {
    m_emit->srw(to_reg, source, MaskedShiftAmount(m_emit, amount_value, assume_amount_masked));
  }
}

void CodeGenerator::EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
{
  HostReg source = from_reg;
  if (size != RegSize_32)
  {
    EmitSignExtend(RSCRATCH2, RegSize_32, from_reg, size);
    source = RSCRATCH2;
  }

  if (amount_value.IsConstant())
  {
    const uint32_t amount = static_cast<uint32_t>(amount_value.constant_value) & 0x1F;
    if (amount == 0)
      m_emit->mr(to_reg, source);
    else
      m_emit->srawi(to_reg, source, amount);
  }
  else
  {
    m_emit->sraw(to_reg, source, MaskedShiftAmount(m_emit, amount_value, assume_amount_masked));
  }

  MaskToSize(m_emit, to_reg, size);
}

void CodeGenerator::EmitAnd(HostReg to_reg, HostReg from_reg, const Value& value)
{
  if (value.IsInHostRegister())
  {
    m_emit->and_(to_reg, from_reg, value.host_reg);
    return;
  }

  const uint32_t constant = static_cast<uint32_t>(value.constant_value);
  uint32_t mb, me;
  if (constant == 0xFFFFFFFFu)
  {
    if (to_reg != from_reg)
      m_emit->mr(to_reg, from_reg);
  }
  else if (constant == 0)
  {
    m_emit->li(to_reg, 0);
  }
  else if (IsMask(constant, &mb, &me))
  {
    m_emit->rlwinm(to_reg, from_reg, 0, mb, me);
  }
  else if (constant <= 0xFFFF)
  {
    m_emit->andi_(to_reg, from_reg, constant);
  }
  else if ((constant & 0xFFFF) == 0)
  {
    m_emit->andis_(to_reg, from_reg, constant >> 16);
  }
  else
  {
    EmitConstant(m_emit, RSCRATCH, constant);
    m_emit->and_(to_reg, from_reg, RSCRATCH);
  }
}

void CodeGenerator::EmitOr(HostReg to_reg, HostReg from_reg, const Value& value)
{
  if (value.IsInHostRegister())
  {
    m_emit->or_(to_reg, from_reg, value.host_reg);
    return;
  }

  const uint32_t constant = static_cast<uint32_t>(value.constant_value);
  if (constant == 0)
  {
    if (to_reg != from_reg)
      m_emit->mr(to_reg, from_reg);
    return;
  }

  if ((constant & 0xFFFF0000u) == 0)
  {
    m_emit->ori(to_reg, from_reg, constant);
    return;
  }

  m_emit->oris(to_reg, from_reg, constant >> 16);
  if ((constant & 0xFFFF) != 0)
    m_emit->ori(to_reg, to_reg, constant & 0xFFFF);
}

void CodeGenerator::EmitXor(HostReg to_reg, HostReg from_reg, const Value& value)
{
  if (value.IsInHostRegister())
  {
    m_emit->xor_(to_reg, from_reg, value.host_reg);
    return;
  }

  const uint32_t constant = static_cast<uint32_t>(value.constant_value);
  if (constant == 0)
  {
    if (to_reg != from_reg)
      m_emit->mr(to_reg, from_reg);
    return;
  }

  if ((constant & 0xFFFF0000u) == 0)
  {
    m_emit->xori(to_reg, from_reg, constant);
    return;
  }

  m_emit->xoris(to_reg, from_reg, constant >> 16);
  if ((constant & 0xFFFF) != 0)
    m_emit->xori(to_reg, to_reg, constant & 0xFFFF);
}

void CodeGenerator::EmitTest(HostReg to_reg, const Value& value)
{
  s_flags.kind = PendingFlags::Kind::Test;
  s_flags.lhs = to_reg;
  s_flags.rhs_is_constant = !value.IsInHostRegister();
  s_flags.rhs_reg = value.host_reg;
  s_flags.rhs_constant = static_cast<uint32_t>(value.constant_value);
}

void CodeGenerator::EmitNot(HostReg to_reg, RegSize size)
{
  m_emit->nor(to_reg, to_reg, to_reg);
  MaskToSize(m_emit, to_reg, size);
}

// Emits what a pending Test needs for CR0: the AND of its operands compared with zero.
static void EmitPendingTest(CodeEmitter* e)
{
  const PendingFlags& flags = s_flags;
  if (!flags.rhs_is_constant)
  {
    if (flags.rhs_reg == flags.lhs)
      e->cmpwi(0, flags.lhs, 0);
    else
      e->and_(RSCRATCH3, flags.lhs, flags.rhs_reg, true);
    return;
  }

  const uint32_t constant = flags.rhs_constant;
  uint32_t mb, me;
  if (constant <= 0xFFFF)
    e->andi_(RSCRATCH3, flags.lhs, constant);
  else if ((constant & 0xFFFF) == 0)
    e->andis_(RSCRATCH3, flags.lhs, constant >> 16);
  else if (IsMask(constant, &mb, &me))
    e->rlwinm(RSCRATCH3, flags.lhs, 0, mb, me, true);
  else
  {
    EmitConstant(e, RSCRATCH, constant);
    e->and_(RSCRATCH3, flags.lhs, RSCRATCH, true);
  }
}

static bool IsUnsignedCondition(Condition condition)
{
  return condition == Condition::Above || condition == Condition::AboveEqual || condition == Condition::Below ||
         condition == Condition::BelowEqual;
}

// Emits the pending compare into CR0, signed or unsigned as the condition asks; for equality, whichever has an
// immediate that fits.
static void EmitPendingCompare(CodeEmitter* e, Condition condition)
{
  const PendingFlags& flags = s_flags;
  const bool equality = (condition == Condition::Equal || condition == Condition::NotEqual ||
                         condition == Condition::Zero || condition == Condition::NotZero);
  bool use_unsigned = IsUnsignedCondition(condition);

  if (flags.rhs_is_constant)
  {
    const uint32_t constant = flags.rhs_constant;
    if (equality)
      use_unsigned = !FitsSigned16(constant);

    if (!use_unsigned && FitsSigned16(constant))
    {
      e->cmpwi(0, flags.lhs, static_cast<int32_t>(constant));
      return;
    }
    if (use_unsigned && constant <= 0xFFFF)
    {
      e->cmplwi(0, flags.lhs, constant);
      return;
    }

    EmitConstant(e, RSCRATCH, constant);
    if (use_unsigned)
      e->cmplw(0, flags.lhs, RSCRATCH);
    else
      e->cmpw(0, flags.lhs, RSCRATCH);
    return;
  }

  if (use_unsigned)
    e->cmplw(0, flags.lhs, flags.rhs_reg);
  else
    e->cmpw(0, flags.lhs, flags.rhs_reg);
}

// The CR0 bit a condition reads, and whether the branch is taken when it is set.
static bool ConditionBit(Condition condition, uint32_t* bit, bool* branch_if_set)
{
  switch (condition)
  {
    case Condition::Equal:
    case Condition::Zero:
      *bit = CR_EQ;
      *branch_if_set = true;
      return true;

    case Condition::NotEqual:
    case Condition::NotZero:
      *bit = CR_EQ;
      *branch_if_set = false;
      return true;

    case Condition::Greater:
    case Condition::Above:
      *bit = CR_GT;
      *branch_if_set = true;
      return true;

    case Condition::LessEqual:
    case Condition::BelowEqual:
      *bit = CR_GT;
      *branch_if_set = false;
      return true;

    case Condition::Less:
    case Condition::Below:
    case Condition::Overflow:
    case Condition::Negative:
      *bit = CR_LT;
      *branch_if_set = true;
      return true;

    case Condition::GreaterEqual:
    case Condition::AboveEqual:
    case Condition::PositiveOrZero:
      *bit = CR_LT;
      *branch_if_set = false;
      return true;

    default:
      return false;
  }
}

// rd = (l < r) unsigned, or (l >= r) unsigned when invert: the borrow of l - r. r is a register or a constant.
static void EmitUnsignedLess(CodeEmitter* e, HostReg rd, HostReg l, bool r_is_constant, HostReg r_reg,
                             uint32_t r_constant, bool invert)
{
  if (r_is_constant)
  {
    if (r_constant == 0)
    {
      // nothing is below zero
      e->li(rd, invert ? 1 : 0);
      return;
    }

    const int32_t signed_constant = static_cast<int32_t>(r_constant);
    if (signed_constant >= -32767 && signed_constant <= 32768)
    {
      // l + (2^32 - c) carries when l >= c
      e->addic(rd, l, -signed_constant);
      e->subfe(rd, rd, rd);
      if (invert)
        e->addi(rd, rd, 1);
      else
        e->neg(rd, rd);
      return;
    }

    EmitConstant(e, RSCRATCH2, r_constant);
    r_reg = RSCRATCH2;
  }

  e->subfc(rd, r_reg, l);
  e->subfe(rd, rd, rd);
  if (invert)
    e->addi(rd, rd, 1);
  else
    e->neg(rd, rd);
}

// rd = 1 when the pending compare of lhs with rhs satisfies the condition, else 0.
static void EmitMaterializeCompare(CodeEmitter* e, HostReg rd, Condition condition)
{
  const PendingFlags& flags = s_flags;
  const HostReg l = flags.lhs;
  const bool c = flags.rhs_is_constant;
  const uint32_t constant = flags.rhs_constant;
  const HostReg r = flags.rhs_reg;

  switch (condition)
  {
    case Condition::Equal:
    case Condition::NotEqual:
    case Condition::Zero:
    case Condition::NotZero:
    {
      // source is zero exactly when the operands are equal
      HostReg source = l;
      if (!c)
      {
        e->xor_(rd, l, r);
        source = rd;
      }
      else if (constant != 0)
      {
        if (FitsSigned16(0u - constant))
        {
          e->addi(rd, l, static_cast<int32_t>(0u - constant));
        }
        else
        {
          e->xoris(rd, l, constant >> 16);
          if ((constant & 0xFFFF) != 0)
            e->xori(rd, rd, constant & 0xFFFF);
        }
        source = rd;
      }

      if (condition == Condition::Equal || condition == Condition::Zero)
      {
        e->cntlzw(rd, source);
        e->srwi(rd, rd, 5);
      }
      else
      {
        // the carry out of source - 1 is set when source is not zero
        e->addic(RSCRATCH, source, -1);
        e->subfe(rd, RSCRATCH, source);
      }
    }
      return;

    case Condition::Below:
      EmitUnsignedLess(e, rd, l, c, r, constant, false);
      return;

    case Condition::AboveEqual:
      EmitUnsignedLess(e, rd, l, c, r, constant, true);
      return;

    case Condition::Above:
    case Condition::BelowEqual:
    {
      // l > r is r < l
      HostReg r_reg = r;
      if (c)
      {
        EmitConstant(e, RSCRATCH2, constant);
        r_reg = RSCRATCH2;
      }
      EmitUnsignedLess(e, rd, r_reg, false, l, 0, condition == Condition::BelowEqual);
    }
      return;

    default:
      break;
  }

  // the signed ones compare with the sign bits flipped, as unsigned; against zero the sign bit itself is the answer
  if (c && constant == 0)
  {
    switch (condition)
    {
      case Condition::Less:
        e->srwi(rd, l, 31);
        return;

      case Condition::GreaterEqual:
        e->srwi(rd, l, 31);
        e->xori(rd, rd, 1);
        return;

      case Condition::Greater:
      case Condition::LessEqual:
        // -l and not l is negative only for l > 0
        e->neg(RSCRATCH, l);
        e->andc(RSCRATCH, RSCRATCH, l);
        e->srwi(rd, RSCRATCH, 31);
        if (condition == Condition::LessEqual)
          e->xori(rd, rd, 1);
        return;

      default:
        break;
    }
  }

  e->xoris(RSCRATCH, l, 0x8000);
  HostReg r_reg = RSCRATCH3;
  uint32_t r_flipped = 0;
  if (c)
  {
    r_flipped = constant ^ 0x80000000u;
  }
  else
  {
    e->xoris(RSCRATCH3, r, 0x8000);
    r_reg = RSCRATCH3;
  }

  switch (condition)
  {
    case Condition::Less:
      EmitUnsignedLess(e, rd, RSCRATCH, c, r_reg, r_flipped, false);
      return;

    case Condition::GreaterEqual:
      EmitUnsignedLess(e, rd, RSCRATCH, c, r_reg, r_flipped, true);
      return;

    default:
    {
      // l > r is r < l
      if (c)
      {
        EmitConstant(e, RSCRATCH2, r_flipped);
        r_reg = RSCRATCH2;
      }
      EmitUnsignedLess(e, rd, r_reg, false, RSCRATCH, 0, condition == Condition::LessEqual);
    }
      return;
  }
}

// rd = 1 when the pending test, lhs AND rhs, satisfies the condition, else 0.
static void EmitMaterializeTest(CodeEmitter* e, HostReg rd, Condition condition)
{
  const PendingFlags& flags = s_flags;
  HostReg source = flags.lhs;
  if (!flags.rhs_is_constant)
  {
    if (flags.rhs_reg != flags.lhs)
    {
      e->and_(RSCRATCH3, flags.lhs, flags.rhs_reg);
      source = RSCRATCH3;
    }
  }
  else if (flags.rhs_constant != 0xFFFFFFFFu)
  {
    uint32_t mb, me;
    if (flags.rhs_constant == 0)
    {
      e->li(RSCRATCH3, 0);
    }
    else if (IsMask(flags.rhs_constant, &mb, &me))
    {
      e->rlwinm(RSCRATCH3, flags.lhs, 0, mb, me);
    }
    else
    {
      EmitConstant(e, RSCRATCH, flags.rhs_constant);
      e->and_(RSCRATCH3, flags.lhs, RSCRATCH);
    }
    source = RSCRATCH3;
  }

  switch (condition)
  {
    case Condition::Zero:
    case Condition::Equal:
      e->cntlzw(rd, source);
      e->srwi(rd, rd, 5);
      break;

    case Condition::NotZero:
    case Condition::NotEqual:
      e->addic(RSCRATCH, source, -1);
      e->subfe(rd, RSCRATCH, source);
      break;

    case Condition::Negative:
      e->srwi(rd, source, 31);
      break;

    default: // PositiveOrZero
      e->srwi(rd, source, 31);
      e->xori(rd, rd, 1);
      break;
  }
}

void CodeGenerator::EmitSetConditionResult(HostReg to_reg, RegSize to_size, Condition condition)
{
  if (condition == Condition::Always)
  {
    m_emit->li(to_reg, 1);
    return;
  }

  switch (s_flags.kind)
  {
    case PendingFlags::Kind::Compare:
      EmitMaterializeCompare(m_emit, to_reg, condition);
      break;

    case PendingFlags::Kind::Test:
      EmitMaterializeTest(m_emit, to_reg, condition);
      break;

    case PendingFlags::Kind::Overflow:
    {
      // RSCRATCH3 holds the AND of the sign differences; its sign is the overflow
      m_emit->srwi(to_reg, RSCRATCH3, 31);
      if (condition != Condition::Overflow)
        m_emit->xori(to_reg, to_reg, 1);
    }
    break;

    default:
      Log_ErrorPrint("Condition set without flags");
      m_emit->trap();
      break;
  }
}

uint32_t CodeGenerator::PrepareStackForCall()
{
  m_register_cache.PushCallerSavedRegisters();
  return 0;
}

void CodeGenerator::RestoreStackAfterCall(uint32_t adjust_size)
{
  m_register_cache.PopCallerSavedRegisters();
}

void CodeGenerator::EmitCall(const void* ptr)
{
  if (m_emit->b_reaches(ptr))
  {
    m_emit->bl(ptr);
    return;
  }

  EmitConstant(m_emit, RSCRATCH, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr)));
  m_emit->mtctr(RSCRATCH);
  m_emit->bctrl();
}

static void EmitCallWithArguments(CodeGenerator& cg, Value* return_value, const void* ptr,
                                  std::initializer_list<const Value*> arguments)
{
  static constexpr HostReg argument_registers[] = {RARG1, RARG2, RARG3, RARG4};

  if (return_value)
    return_value->Discard();

  // the stack is the dispatcher's frame, whose bottom is the callee's
  const uint32_t adjust_size = cg.PrepareStackForCall();

  uint32_t index = 0;
  for (const Value* argument : arguments)
  {
    cg.EmitCopyValue(argument_registers[index], *argument);

    // the callee may expect a char or short widened
    if (!argument->IsConstant() && argument->size < RegSize_32)
      cg.EmitZeroExtend(argument_registers[index], RegSize_32, argument_registers[index], argument->size);
    index++;
  }

  cg.EmitCall(ptr);

  cg.RestoreStackAfterCall(adjust_size);

  if (return_value)
  {
    return_value->Undiscard();
    cg.EmitCopyValue(return_value->GetHostRegister(), Value::FromHostReg(nullptr, RRETURN, return_value->size));
  }
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr)
{
  EmitCallWithArguments(*this, return_value, ptr, {});
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1)
{
  EmitCallWithArguments(*this, return_value, ptr, {&arg1});
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2)
{
  EmitCallWithArguments(*this, return_value, ptr, {&arg1, &arg2});
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2,
                                        const Value& arg3)
{
  EmitCallWithArguments(*this, return_value, ptr, {&arg1, &arg2, &arg3});
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2,
                                        const Value& arg3, const Value& arg4)
{
  EmitCallWithArguments(*this, return_value, ptr, {&arg1, &arg2, &arg3, &arg4});
}

// In a block, positions are the spills around a call; in the dispatcher, what it saves. The console's C code may keep
// 64-bit values in r14-r31, so the dispatcher saves them whole there.
void CodeGenerator::EmitPushHostReg(HostReg reg, uint32_t position)
{
  if (m_block)
  {
    m_emit->stw(reg, FRAME_SPILL_OFFSET + static_cast<int32_t>(position) * 8, 1);
    return;
  }

#ifdef __XSDK__
  m_emit->std(reg, FRAME_SAVE_OFFSET + static_cast<int32_t>(position) * 8, 1);
#else
  m_emit->stw(reg, FRAME_SAVE_OFFSET + static_cast<int32_t>(position) * 8, 1);
#endif
}

void CodeGenerator::EmitPushHostRegPair(HostReg reg, HostReg reg2, uint32_t position)
{
  EmitPushHostReg(reg, position);
  EmitPushHostReg(reg2, position + 1);
}

void CodeGenerator::EmitPopHostReg(HostReg reg, uint32_t position)
{
  if (m_block)
  {
    m_emit->lwz(reg, FRAME_SPILL_OFFSET + static_cast<int32_t>(position) * 8, 1);
    return;
  }

#ifdef __XSDK__
  m_emit->ld(reg, FRAME_SAVE_OFFSET + static_cast<int32_t>(position) * 8, 1);
#else
  m_emit->lwz(reg, FRAME_SAVE_OFFSET + static_cast<int32_t>(position) * 8, 1);
#endif
}

// reg2 is the one at position, reg the one before it
void CodeGenerator::EmitPopHostRegPair(HostReg reg, HostReg reg2, uint32_t position)
{
  EmitPopHostReg(reg, position - 1);
  EmitPopHostReg(reg2, position);
}

void CodeGenerator::EmitLoadCPUStructField(HostReg host_reg, RegSize guest_size, uint32_t offset)
{
  switch (guest_size)
  {
    case RegSize_8:
      m_emit->lbz(host_reg, static_cast<int32_t>(offset), RCPUPTR);
      break;

    case RegSize_16:
      m_emit->lhz(host_reg, static_cast<int32_t>(offset), RCPUPTR);
      break;

    case RegSize_32:
      m_emit->lwz(host_reg, static_cast<int32_t>(offset), RCPUPTR);
      break;

    default:
      break;
  }
}

void CodeGenerator::EmitStoreCPUStructField(uint32_t offset, const Value& value)
{
  const HostReg reg = RegisterOrScratch(m_emit, value, RSCRATCH);

  switch (value.size)
  {
    case RegSize_8:
      m_emit->stb(reg, static_cast<int32_t>(offset), RCPUPTR);
      break;

    case RegSize_16:
      m_emit->sth(reg, static_cast<int32_t>(offset), RCPUPTR);
      break;

    case RegSize_32:
      m_emit->stw(reg, static_cast<int32_t>(offset), RCPUPTR);
      break;

    default:
      break;
  }
}

void CodeGenerator::EmitAddCPUStructField(uint32_t offset, const Value& value)
{
  // the sum goes back to memory, so no masking
  switch (value.size)
  {
    case RegSize_8:
      m_emit->lbz(RSCRATCH, static_cast<int32_t>(offset), RCPUPTR);
      break;

    case RegSize_16:
      m_emit->lhz(RSCRATCH, static_cast<int32_t>(offset), RCPUPTR);
      break;

    default:
      m_emit->lwz(RSCRATCH, static_cast<int32_t>(offset), RCPUPTR);
      break;
  }

  if (value.IsInHostRegister())
    m_emit->add(RSCRATCH, RSCRATCH, value.host_reg);
  else
    m_emit->add_immediate(RSCRATCH, RSCRATCH, static_cast<uint32_t>(value.constant_value));

  switch (value.size)
  {
    case RegSize_8:
      m_emit->stb(RSCRATCH, static_cast<int32_t>(offset), RCPUPTR);
      break;

    case RegSize_16:
      m_emit->sth(RSCRATCH, static_cast<int32_t>(offset), RCPUPTR);
      break;

    default:
      m_emit->stw(RSCRATCH, static_cast<int32_t>(offset), RCPUPTR);
      break;
  }
}

// Guest memory is a little-endian image, so a multi-byte load or store is the byte-reversed form, which costs what the
// plain one does (docs/HARDWARE.md, "Byte-reversed loads and stores cost what plain ones do"). Only X-form has them:
// the address goes in a register.
// The scratchpad is CPU::g_state's, so a short offset from RCPUPTR reaches it where a constant address takes two
// instructions.
static bool GetCPUStateOffset(const void* ptr, int32_t* offset)
{
  const uintptr_t delta = reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(&g_state);
  if (delta > 0x7FFF)
    return false;

  *offset = static_cast<int32_t>(delta);
  return true;
}

void CodeGenerator::EmitLoadGlobal(HostReg host_reg, RegSize size, const void* ptr)
{
  int32_t offset;
  if (GetCPUStateOffset(ptr, &offset))
  {
    if (size == RegSize_8)
    {
      m_emit->lbz(host_reg, offset, RCPUPTR);
      return;
    }

    m_emit->li(RSCRATCH, offset);
    if (size == RegSize_16)
      m_emit->lhbrx(host_reg, RCPUPTR, RSCRATCH);
    else
      m_emit->lwbrx(host_reg, RCPUPTR, RSCRATCH);
    return;
  }

  EmitLoadGlobalAddress(RSCRATCH, ptr);
  switch (size)
  {
    case RegSize_8:
      m_emit->lbz(host_reg, 0, RSCRATCH);
      break;

    case RegSize_16:
      m_emit->lhbrx(host_reg, 0, RSCRATCH);
      break;

    case RegSize_32:
      m_emit->lwbrx(host_reg, 0, RSCRATCH);
      break;

    default:
      break;
  }
}

void CodeGenerator::EmitStoreGlobal(void* ptr, const Value& value)
{
  const HostReg value_reg = RegisterOrScratch(m_emit, value, RSCRATCH2);

  int32_t offset;
  if (GetCPUStateOffset(ptr, &offset))
  {
    if (value.size == RegSize_8)
    {
      m_emit->stb(value_reg, offset, RCPUPTR);
      return;
    }

    m_emit->li(RSCRATCH, offset);
    if (value.size == RegSize_16)
      m_emit->sthbrx(value_reg, RCPUPTR, RSCRATCH);
    else
      m_emit->stwbrx(value_reg, RCPUPTR, RSCRATCH);
    return;
  }

  EmitLoadGlobalAddress(RSCRATCH, ptr);
  switch (value.size)
  {
    case RegSize_8:
      m_emit->stb(value_reg, 0, RSCRATCH);
      break;

    case RegSize_16:
      m_emit->sthbrx(value_reg, 0, RSCRATCH);
      break;

    case RegSize_32:
      m_emit->stwbrx(value_reg, 0, RSCRATCH);
      break;

    default:
      break;
  }
}

void CodeGenerator::EmitLoadGlobalAddress(HostReg host_reg, const void* ptr)
{
  EmitConstant(m_emit, host_reg, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr)));
}

// There is no fastmem on PowerPC yet (Settings::IsUsingFastmem is false), so these are never reached.
void CodeGenerator::EmitLoadGuestRAMFastmem(const Value& address, RegSize size, Value& result)
{
  Log_ErrorPrint("Fastmem load on PowerPC");
  m_emit->trap();
}

void CodeGenerator::EmitLoadGuestMemoryFastmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                               Value& result)
{
  Log_ErrorPrint("Fastmem load on PowerPC");
  m_emit->trap();
}

void CodeGenerator::EmitStoreGuestMemoryFastmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                                const Value& value)
{
  Log_ErrorPrint("Fastmem store on PowerPC");
  m_emit->trap();
}

void CodeGenerator::EmitUpdateFastmemBase() {}

bool CodeGenerator::BackpatchLoadStore(const LoadStoreBackpatchInfo& lbi)
{
  return false;
}

// Raises the exception whose code a failed access left in code_reg, then leaves the block.
static void EmitRaiseMemoryException(CodeGenerator& cg, CodeEmitter* e, HostReg code_reg,
                                     const CodeBlockInstruction& cbi, const Value& pc)
{
  // cause_bits = (code << 2) | BD | cop_n
  e->slwi(code_reg, code_reg, 2);
  const Value cause = Value::FromHostReg(nullptr, code_reg, RegSize_32);
  cg.EmitOr(code_reg, code_reg,
            Value::FromConstantU32(Cop0Registers::CAUSE::MakeValueForException(
              static_cast<Exception>(0), cbi.is_branch_delay_slot, false, cbi.instruction.cop.cop_n)));
  cg.EmitFunctionCall(nullptr, static_cast<void (*)(uint32_t, uint32_t)>(&CPU::RaiseException), cause, pc);
  cg.EmitExceptionExit();
}

void CodeGenerator::EmitLoadGuestMemorySlowmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                               Value& result, bool in_far_code)
{
  if (g_settings.cpu_recompiler_memory_exceptions)
  {
    switch (size)
    {
      case RegSize_8:
        EmitFunctionCall(&result, &Thunks::ReadMemoryByte32, address);
        break;

      case RegSize_16:
        EmitFunctionCall(&result, &Thunks::ReadMemoryHalfWord32, address);
        break;

      case RegSize_32:
        EmitFunctionCall(&result, &Thunks::ReadMemoryWord32, address);
        break;

      default:
        break;
    }

    m_register_cache.PushState();

    // the exception code, 0 when the read worked
    EmitLoadGlobalAddress(RSCRATCH, &s_memory_exception);
    m_emit->lwz(RSCRATCH, 0, RSCRATCH);
    m_emit->cmpwi(0, RSCRATCH, 0);

    LabelType load_okay;
    m_emit->bt(CR_EQ, &load_okay);
    if (!in_far_code)
    {
      EmitBranch(GetCurrentFarCodePointer());
      m_emit->bind(&load_okay);

      SwitchToFarCode();
      EmitRaiseMemoryException(*this, m_emit, RSCRATCH, cbi, GetCurrentInstructionPC());
      SwitchToNearCode();
    }
    else
    {
      EmitRaiseMemoryException(*this, m_emit, RSCRATCH, cbi, GetCurrentInstructionPC());
      m_emit->bind(&load_okay);
    }

    m_register_cache.PopState();
  }
  else
  {
    switch (size)
    {
      case RegSize_8:
        EmitFunctionCall(&result, &Thunks::UncheckedReadMemoryByte, address);
        break;

      case RegSize_16:
        EmitFunctionCall(&result, &Thunks::UncheckedReadMemoryHalfWord, address);
        break;

      case RegSize_32:
        EmitFunctionCall(&result, &Thunks::UncheckedReadMemoryWord, address);
        break;

      default:
        break;
    }
  }
}

void CodeGenerator::EmitStoreGuestMemorySlowmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                                const Value& value, bool in_far_code)
{
  Value value_in_hr = GetValueInHostRegister(value);

  if (g_settings.cpu_recompiler_memory_exceptions)
  {
    Value result = m_register_cache.AllocateScratch(RegSize_32);
    switch (size)
    {
      case RegSize_8:
        EmitFunctionCall(&result, &Thunks::WriteMemoryByte, address, value_in_hr);
        break;

      case RegSize_16:
        EmitFunctionCall(&result, &Thunks::WriteMemoryHalfWord, address, value_in_hr);
        break;

      case RegSize_32:
        EmitFunctionCall(&result, &Thunks::WriteMemoryWord, address, value_in_hr);
        break;

      default:
        break;
    }

    m_register_cache.PushState();

    // the exception code, 0 when the write worked
    m_emit->cmpwi(0, result.host_reg, 0);
    LabelType store_okay;
    m_emit->bt(CR_EQ, &store_okay);
    if (!in_far_code)
    {
      EmitBranch(GetCurrentFarCodePointer());
      m_emit->bind(&store_okay);

      SwitchToFarCode();
      m_emit->mr(RSCRATCH, result.host_reg);
      EmitRaiseMemoryException(*this, m_emit, RSCRATCH, cbi, GetCurrentInstructionPC());
      SwitchToNearCode();
    }
    else
    {
      m_emit->mr(RSCRATCH, result.host_reg);
      EmitRaiseMemoryException(*this, m_emit, RSCRATCH, cbi, GetCurrentInstructionPC());
      m_emit->bind(&store_okay);
    }

    m_register_cache.PopState();
  }
  else
  {
    switch (size)
    {
      case RegSize_8:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryByte, address, value_in_hr);
        break;

      case RegSize_16:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryHalfWord, address, value_in_hr);
        break;

      case RegSize_32:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryWord, address, value_in_hr);
        break;

      default:
        break;
    }
  }
}

// The branch is rewritten in place: a block's link site is one b, which becomes a b to its successor, back to its
// resolver, or a return.
static void WriteBranch(void* pc, uint32_t pc_size, const void* target)
{
  CodeEmitter e(static_cast<uint8_t*>(pc), pc_size);
  if (e.b_reaches(target))
  {
    e.b(target);
  }
  else
  {
    e.load_immediate(RSCRATCH, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target)));
    e.mtctr(RSCRATCH);
    e.bctr();
  }

  while (e.cursor_offset() < pc_size)
    e.nop();

  if (e.failed())
  {
    Log_ErrorPrint("A block's link site is too small for its new branch");
    std::abort();
  }

  JitCodeBuffer::FlushInstructionCache(pc, pc_size);
}

void CodeGenerator::BackpatchReturn(void* pc, uint32_t pc_size)
{
  WriteBranch(pc, pc_size, s_return_stub);
}

void CodeGenerator::BackpatchBranch(void* pc, uint32_t pc_size, void* target)
{
  WriteBranch(pc, pc_size, target);
}

void CodeGenerator::EmitFlushInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_32);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  LabelType skip_flush;

  // reg = load_delay_reg
  m_emit->lbz(reg.host_reg, offsetof(State, load_delay_reg), RCPUPTR);

  // if load_delay_reg == Reg::count goto skip_flush
  m_emit->cmpwi(0, reg.host_reg, static_cast<int32_t>(Reg::count));
  m_emit->bt(CR_EQ, &skip_flush);

  // value = load_delay_value
  m_emit->lwz(value.host_reg, offsetof(State, load_delay_value), RCPUPTR);

  // r[reg] = value
  m_emit->slwi(reg.host_reg, reg.host_reg, 2);
  m_emit->addi(reg.host_reg, reg.host_reg, offsetof(State, regs.r[0]));
  m_emit->stwx(value.host_reg, RCPUPTR, reg.host_reg);

  // load_delay_reg = Reg::count
  m_emit->li(reg.host_reg, static_cast<int32_t>(Reg::count));
  m_emit->stb(reg.host_reg, offsetof(State, load_delay_reg), RCPUPTR);

  m_emit->bind(&skip_flush);
}

void CodeGenerator::EmitMoveNextInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_32);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  m_emit->lbz(reg.host_reg, offsetof(State, next_load_delay_reg), RCPUPTR);
  m_emit->lwz(value.host_reg, offsetof(State, next_load_delay_value), RCPUPTR);
  m_emit->stb(reg.host_reg, offsetof(State, load_delay_reg), RCPUPTR);
  m_emit->stw(value.host_reg, offsetof(State, load_delay_value), RCPUPTR);
  m_emit->li(reg.host_reg, static_cast<int32_t>(Reg::count));
  m_emit->stb(reg.host_reg, offsetof(State, next_load_delay_reg), RCPUPTR);
}

void CodeGenerator::EmitCancelInterpreterLoadDelayForReg(Reg reg)
{
  if (!m_load_delay_dirty)
    return;

  Value temp = m_register_cache.AllocateScratch(RegSize_8);

  LabelType skip_cancel;

  // if load_delay_reg != reg goto skip_cancel
  m_emit->lbz(temp.host_reg, offsetof(State, load_delay_reg), RCPUPTR);
  m_emit->cmpwi(0, temp.host_reg, static_cast<int32_t>(reg));
  m_emit->bf(CR_EQ, &skip_cancel);

  // load_delay_reg = Reg::count
  m_emit->li(temp.host_reg, static_cast<int32_t>(Reg::count));
  m_emit->stb(temp.host_reg, offsetof(State, load_delay_reg), RCPUPTR);

  m_emit->bind(&skip_cancel);
}

void CodeGenerator::EmitICacheCheckAndUpdate()
{
  if (GetSegmentForAddress(m_pc) >= Segment::KSEG1)
  {
    EmitAddCPUStructField(offsetof(State, pending_ticks),
                          Value::FromConstantU32(static_cast<uint32_t>(m_block->uncached_fetch_ticks)));
    return;
  }

  VirtualMemoryAddress current_pc = m_pc & ICACHE_TAG_ADDRESS_MASK;
  for (uint32_t i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
  {
    const VirtualMemoryAddress tag = GetICacheTagForAddress(current_pc);
    const TickCount fill_ticks = GetICacheFillTicks(current_pc);
    if (fill_ticks <= 0)
      continue;

    const uint32_t line = GetICacheLine(current_pc);
    const uint32_t offset = offsetof(State, icache_tags) + (line * sizeof(uint32_t));
    LabelType cache_hit;

    EmitConstant(m_emit, RSCRATCH2, tag);
    m_emit->lwz(RSCRATCH, static_cast<int32_t>(offset), RCPUPTR);
    m_emit->cmpw(0, RSCRATCH, RSCRATCH2);
    m_emit->bt(CR_EQ, &cache_hit);
    m_emit->stw(RSCRATCH2, static_cast<int32_t>(offset), RCPUPTR);
    EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<uint32_t>(fill_ticks)));
    m_emit->bind(&cache_hit);
  }
}

void CodeGenerator::EmitStallUntilGTEComplete()
{
  m_emit->lwz(RARG1, offsetof(State, pending_ticks), RCPUPTR);
  m_emit->lwz(RARG2, offsetof(State, gte_completion_tick), RCPUPTR);

  if (m_delayed_cycles_add > 0)
  {
    m_emit->add_immediate(RARG1, RARG1, static_cast<uint32_t>(m_delayed_cycles_add));
    m_delayed_cycles_add = 0;
  }

  // pending_ticks = max(pending_ticks, gte_completion_tick), unsigned
  LabelType no_stall;
  m_emit->cmplw(0, RARG2, RARG1);
  m_emit->bc(BO_FALSE, CR_GT, &no_stall);
  m_emit->mr(RARG1, RARG2);
  m_emit->bind(&no_stall);
  m_emit->stw(RARG1, offsetof(State, pending_ticks), RCPUPTR);
}

void CodeGenerator::EmitBranch(const void* address, bool allow_scratch)
{
  if (m_emit->b_reaches(address))
  {
    m_emit->b(address);
    return;
  }

  EmitConstant(m_emit, RSCRATCH, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address)));
  m_emit->mtctr(RSCRATCH);
  m_emit->bctr();
}

void CodeGenerator::EmitBranch(LabelType* label)
{
  m_emit->b(label);
}

// Branches on CR0 as the pending flags and condition set it.
static void EmitBranchOnFlags(CodeEmitter* e, Condition condition, bool invert, LabelType* label)
{
  switch (s_flags.kind)
  {
    case PendingFlags::Kind::Compare:
      EmitPendingCompare(e, condition);
      break;

    case PendingFlags::Kind::Test:
      EmitPendingTest(e);
      break;

    case PendingFlags::Kind::Overflow:
      break;

    default:
      Log_ErrorPrint("Branch on a condition without flags");
      e->trap();
      return;
  }

  uint32_t bit;
  bool branch_if_set;
  if (!ConditionBit(condition, &bit, &branch_if_set))
  {
    e->trap();
    return;
  }

  if (invert)
    branch_if_set = !branch_if_set;

  e->bc(branch_if_set ? BO_TRUE : BO_FALSE, bit, label);
}

void CodeGenerator::EmitConditionalBranch(Condition condition, bool invert, HostReg value, RegSize size,
                                          LabelType* label)
{
  switch (condition)
  {
    case Condition::Always:
      m_emit->b(label);
      return;

    case Condition::Zero:
    case Condition::NotZero:
    case Condition::Negative:
    case Condition::PositiveOrZero:
    {
      // the value's own low bits, size of them
      HostReg source = value;
      if (size != RegSize_32)
      {
        const uint32_t bits = (size == RegSize_8) ? 8 : 16;
        const bool sign_test = (condition == Condition::Negative || condition == Condition::PositiveOrZero);
        if (sign_test)
          m_emit->rlwinm(RSCRATCH3, value, 0, 32 - bits, 32 - bits, true);
        else
          m_emit->rlwinm(RSCRATCH3, value, 0, 32 - bits, 31, true);

        // the sign test is a bit that is set or not: Negative is not-zero, PositiveOrZero is zero
        bool branch_if_set = (condition == Condition::Zero || condition == Condition::PositiveOrZero);
        if (invert)
          branch_if_set = !branch_if_set;
        m_emit->bc(branch_if_set ? BO_TRUE : BO_FALSE, CR_EQ, label);
        return;
      }

      m_emit->cmpwi(0, source, 0);
      uint32_t bit;
      bool branch_if_set;
      ConditionBit(condition, &bit, &branch_if_set);
      if (invert)
        branch_if_set = !branch_if_set;
      m_emit->bc(branch_if_set ? BO_TRUE : BO_FALSE, bit, label);
      return;
    }

    default:
    {
      // against zero
      PendingFlags saved = s_flags;
      s_flags.kind = PendingFlags::Kind::Compare;
      s_flags.lhs = value;
      s_flags.rhs_is_constant = true;
      s_flags.rhs_constant = 0;
      EmitBranchOnFlags(m_emit, condition, invert, label);
      s_flags = saved;
      return;
    }
  }
}

void CodeGenerator::EmitConditionalBranch(Condition condition, bool invert, HostReg lhs, const Value& rhs,
                                          LabelType* label)
{
  switch (condition)
  {
    case Condition::Always:
      m_emit->b(label);
      return;

    case Condition::Negative:
    case Condition::PositiveOrZero:
    case Condition::NotZero:
    case Condition::Zero:
      EmitConditionalBranch(condition, invert, lhs, rhs.size, label);
      return;

    default:
      EmitCmp(lhs, rhs);
      EmitConditionalBranch(condition, invert, label);
      return;
  }
}

void CodeGenerator::EmitConditionalBranch(Condition condition, bool invert, LabelType* label)
{
  if (condition == Condition::Always)
  {
    m_emit->b(label);
    return;
  }

  EmitBranchOnFlags(m_emit, condition, invert, label);
}

void CodeGenerator::EmitBranchIfBitClear(HostReg reg, RegSize size, uint8_t bit, LabelType* label)
{
  m_emit->rlwinm(RSCRATCH3, reg, 0, 31 - bit, 31 - bit, true);
  m_emit->bt(CR_EQ, label);
}

void CodeGenerator::EmitBranchIfBitSet(HostReg reg, RegSize size, uint8_t bit, LabelType* label)
{
  m_emit->rlwinm(RSCRATCH3, reg, 0, 31 - bit, 31 - bit, true);
  m_emit->bf(CR_EQ, label);
}

void CodeGenerator::EmitBindLabel(LabelType* label)
{
  m_emit->bind(label);
}

CodeCache::DispatcherFunction CodeGenerator::CompileDispatcher()
{
  m_emit->stwu(1, -FRAME_SIZE, 1);
  m_emit->mflr(RSCRATCH3);
  m_emit->stw(RSCRATCH3, FRAME_LR_OFFSET, 1);
  m_register_cache.ReserveCalleeSavedRegisters();
  const uint32_t stack_adjust = PrepareStackForCall();

  // blocks leave to the instruction after the call below
  LabelType after_block_call;
  m_emit->load_label_address(RRET, &after_block_call);
  EmitLoadGlobalAddress(RCPUPTR, &g_state);

  LabelType frame_done_loop;
  LabelType exit_dispatcher;
  m_emit->bind(&frame_done_loop);

  // if frame_done goto exit_dispatcher
  m_emit->lbz(RSCRATCH, offsetof(State, frame_done), RCPUPTR);
  m_emit->cmpwi(0, RSCRATCH, 0);
  m_emit->bf(CR_EQ, &exit_dispatcher);

  // RSCRATCH <- sr
  LabelType no_interrupt;
  m_emit->lwz(RSCRATCH, offsetof(State, cop0_regs.sr.bits), RCPUPTR);

  // if Iec == 0 then goto no_interrupt
  m_emit->rlwinm(RSCRATCH3, RSCRATCH, 0, 31, 31, true);
  m_emit->bt(CR_EQ, &no_interrupt);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  m_emit->lwz(RSCRATCH2, offsetof(State, cop0_regs.cause.bits), RCPUPTR);
  m_emit->and_(RSCRATCH, RSCRATCH, RSCRATCH2);
  m_emit->andi_(RSCRATCH3, RSCRATCH, 0xFF00);
  m_emit->bt(CR_EQ, &no_interrupt);

  // we have an interrupt
  EmitCall(reinterpret_cast<const void*>(&DispatchInterrupt));

  // no interrupt or we just serviced it
  m_emit->bind(&no_interrupt);

  // TimingEvents::UpdateCPUDowncount: downcount <- head event->downcount
  EmitLoadGlobalAddress(RSCRATCH, TimingEvents::GetHeadEventPtr());
  m_emit->lwz(RSCRATCH, 0, RSCRATCH);
  m_emit->lwz(RSCRATCH, offsetof(TimingEvent, m_downcount), RSCRATCH);
  m_emit->stw(RSCRATCH, offsetof(State, downcount), RCPUPTR);

  // main dispatch loop
  LabelType main_loop;
  m_emit->bind(&main_loop);

  // while pending_ticks < downcount
  LabelType downcount_hit;
  m_emit->lwz(RSCRATCH, offsetof(State, pending_ticks), RCPUPTR);
  m_emit->lwz(RSCRATCH2, offsetof(State, downcount), RCPUPTR);
  m_emit->cmpw(0, RSCRATCH, RSCRATCH2);
  m_emit->bf(CR_LT, &downcount_hit);

  // time to lookup the block: the slot is s_fast_map[pc >> 16] + pc, the table pointers being biased
  m_emit->lwz(RSCRATCH, offsetof(State, regs.pc), RCPUPTR);
  EmitLoadGlobalAddress(RSCRATCH2, CodeCache::GetFastMapPointer());
  m_emit->rlwinm(RARG1, RSCRATCH, 18, 14, 29);
  m_emit->lwzx(RSCRATCH2, RSCRATCH2, RARG1);
  m_emit->lwzx(RSCRATCH, RSCRATCH2, RSCRATCH);

  // call the block
  m_emit->mtctr(RSCRATCH);
  m_emit->bctrl();
  m_emit->bind(&after_block_call);
  m_emit->b(&main_loop);

  // end while
  m_emit->bind(&downcount_hit);

  // check events then for frame done
  EmitLoadGlobalAddress(RSCRATCH, TimingEvents::GetHeadEventPtr());
  m_emit->lwz(RSCRATCH, 0, RSCRATCH);
  m_emit->lwz(RSCRATCH, offsetof(TimingEvent, m_downcount), RSCRATCH);
  m_emit->lwz(RSCRATCH2, offsetof(State, pending_ticks), RCPUPTR);
  m_emit->cmpw(0, RSCRATCH, RSCRATCH2);
  m_emit->bt(CR_GT, &frame_done_loop);
  EmitCall(reinterpret_cast<const void*>(&TimingEvents::RunEvents));
  m_emit->b(&frame_done_loop);

  // all done
  m_emit->bind(&exit_dispatcher);
  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->lwz(RSCRATCH3, FRAME_LR_OFFSET, 1);
  m_emit->mtlr(RSCRATCH3);
  m_emit->addi(1, 1, FRAME_SIZE);
  m_emit->blr();

  // where a block that links to nothing returns from
  s_return_stub = GetCurrentCodePointer();
  m_emit->mtlr(RRET);
  m_emit->blr();

  CodeBlock::HostCodePointer ptr;
  uint32_t code_size;
  FinalizeBlock(&ptr, &code_size);
  return reinterpret_cast<CodeCache::DispatcherFunction>(ptr);
}

CodeCache::SingleBlockDispatcherFunction CodeGenerator::CompileSingleBlockDispatcher()
{
  m_emit->stwu(1, -FRAME_SIZE, 1);
  m_emit->mflr(RSCRATCH3);
  m_emit->stw(RSCRATCH3, FRAME_LR_OFFSET, 1);
  m_register_cache.ReserveCalleeSavedRegisters();
  const uint32_t stack_adjust = PrepareStackForCall();

  LabelType after_block_call;
  m_emit->load_label_address(RRET, &after_block_call);
  EmitLoadGlobalAddress(RCPUPTR, &g_state);

  // RARG1 is the block
  m_emit->mtctr(RARG1);
  m_emit->bctrl();
  m_emit->bind(&after_block_call);

  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->lwz(RSCRATCH3, FRAME_LR_OFFSET, 1);
  m_emit->mtlr(RSCRATCH3);
  m_emit->addi(1, 1, FRAME_SIZE);
  m_emit->blr();

  CodeBlock::HostCodePointer ptr;
  uint32_t code_size;
  FinalizeBlock(&ptr, &code_size);
  return reinterpret_cast<CodeCache::SingleBlockDispatcherFunction>(ptr);
}

namespace Thunks {

// The 64-bit return of the checked reads is a register pair on one PowerPC ABI and a single register on the other, so
// these return the value alone; a failed read returns 0 and leaves its exception code in s_memory_exception, 0 when it
// worked.
static uint32_t FinishRead(uint64_t result)
{
  if (static_cast<int64_t>(result) < 0)
  {
    s_memory_exception = static_cast<uint32_t>(-static_cast<int64_t>(result));
    return 0;
  }

  s_memory_exception = 0;
  return static_cast<uint32_t>(result);
}

uint32_t ReadMemoryByte32(uint32_t address)
{
  return FinishRead(ReadMemoryByte(address));
}

uint32_t ReadMemoryHalfWord32(uint32_t address)
{
  return FinishRead(ReadMemoryHalfWord(address));
}

uint32_t ReadMemoryWord32(uint32_t address)
{
  return FinishRead(ReadMemoryWord(address));
}

// A GTE command takes its instruction as a union by value: a register on the console's convention, a pointer to a copy
// under SysV's.
void ExecuteGTE(uint32_t instruction_bits, const void* implementation)
{
  reinterpret_cast<GTE::InstructionImpl>(const_cast<void*>(implementation))(GTE::Instruction{instruction_bits});
}

} // namespace Thunks

} // namespace CPU::Recompiler
