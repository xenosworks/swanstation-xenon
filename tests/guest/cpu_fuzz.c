/* CPU fuzzer, run in place of the BIOS.
 *
 * Writes random blocks of MIPS I code into uncached RAM (the interpreter always uses its instruction cache for cached
 * fetches, the recompiler never) and runs each from random registers: ALU operations, shifts, multiplies and divides,
 * loads and stores of every width and alignment (lwl, lwr, swl and swr among them), branches with their delay slots,
 * jumps through registers, add, sub and addi that overflow, misaligned accesses, syscall and break (the exception
 * handler skips the instruction), COP0 and COP2 transfers, GTE commands, and stores that rewrite the block as it
 * runs. After each run the registers, HI and LO, COP0 and the GTE registers go into RAM at 0x140000 and are folded
 * into a checksum, which RAM[0x100] holds with the count of blocks done, and RAM[0x160000 + 4 * n] holds after block n.
 *
 * The seed picks the sequence. Bits of the mode parameter enable each group: bit 0 R-type ALU, 1 immediates, 2 multiply
 * and divide, 3 loads, 4 stores, 5 branches, 6 exceptions, 7 COP0 and COP2 transfers, 8 GTE commands, 9 self-modifying
 * stores, 10 loads and stores at addresses known when compiled. Bit 31 runs each block once, not once or twice. The
 * third parameter is the number of blocks (500). */
typedef unsigned int u32;
typedef int s32;

__attribute__((section(".params"))) const volatile u32 param_seed = 1;
__attribute__((section(".params"))) const volatile u32 param_mode = 0xFFFFFFFFu;
__attribute__((section(".params"))) const volatile u32 param_blocks = 500;

#define CODE_ADDR 0xA0100000u
#define CODE ((volatile u32*)CODE_ADDR)
#define DATA_GP 0x80180000u
#define DATA_FP 0x9F800180u
#define STATE ((volatile u32*)0x80140000u)
#define BLOCK_BUFFER ((volatile u32*)0x80150000u)
#define HISTORY ((volatile u32*)0x80160000u)
#define RESULT ((volatile u32*)0xA0000100u)

/* State: 0-31 registers, 32 HI, 33 LO, 34-37 SR, CAUSE, EPC, BadVAddr, 38-69 GTE data, 70-101 GTE control */
enum
{
  S_HI = 32,
  S_LO = 33,
  S_COP0 = 34,
  S_GTE_DATA = 38,
  S_GTE_CONTROL = 70,
  S_WORDS = 102
};

void run_block(volatile u32* state, u32 code);

__asm__(".set push\n"
        ".set noreorder\n"
        ".set noat\n"
        ".text\n"
        ".globl run_block\n"
        "run_block:\n"
        "  addiu $sp, $sp, -48\n"
        "  sw $ra, 0($sp)\n"
        "  sw $a0, 4($sp)\n"
        "  sw $gp, 8($sp)\n"
        "  sw $fp, 12($sp)\n"
        "  sw $s0, 16($sp)\n"
        "  sw $s1, 20($sp)\n"
        "  sw $s2, 24($sp)\n"
        "  sw $s3, 28($sp)\n"
        "  sw $s4, 32($sp)\n"
        "  sw $s5, 36($sp)\n"
        "  sw $s6, 40($sp)\n"
        "  sw $s7, 44($sp)\n"
        "  move $k0, $a1\n"
        "  lui $k1, %hi(1f)\n"
        "  addiu $k1, $k1, %lo(1f)\n"
        "  lw $t0, 128($a0)\n"
        "  lw $t1, 132($a0)\n"
        "  nop\n"
        "  mthi $t0\n"
        "  mtlo $t1\n"
        "  lui $gp, 0x8018\n"
        "  lui $fp, 0x9F80\n"
        "  ori $fp, $fp, 0x0180\n"
        "  .irp r, 1,2,3,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,31\n"
        "  lw $\\r, (\\r * 4)($a0)\n"
        "  .endr\n"
        "  lw $4, 16($a0)\n"
        "  nop\n"
        "  jr $k0\n"
        "  nop\n"
        "1:\n"
        "  lw $k0, 4($sp)\n"
        "  nop\n"
        "  .irp r, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,31\n"
        "  sw $\\r, (\\r * 4)($k0)\n"
        "  .endr\n"
        "  mfhi $t0\n"
        "  mflo $t1\n"
        "  sw $t0, 128($k0)\n"
        "  sw $t1, 132($k0)\n"
        "  mfc0 $t0, $12\n"
        "  mfc0 $t1, $13\n"
        "  mfc0 $t2, $14\n"
        "  mfc0 $t3, $8\n"
        "  nop\n"
        "  sw $t0, 136($k0)\n"
        "  sw $t1, 140($k0)\n"
        "  sw $t2, 144($k0)\n"
        "  sw $t3, 148($k0)\n"
        "  .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "  .word 0x48080000 + (\\r << 11)\n"
        "  nop\n"
        "  sw $t0, ((38 + \\r) * 4)($k0)\n"
        "  .word 0x48480000 + (\\r << 11)\n"
        "  nop\n"
        "  sw $t0, ((70 + \\r) * 4)($k0)\n"
        "  .endr\n"
        "  lw $ra, 0($sp)\n"
        "  lw $gp, 8($sp)\n"
        "  lw $fp, 12($sp)\n"
        "  lw $s0, 16($sp)\n"
        "  lw $s1, 20($sp)\n"
        "  lw $s2, 24($sp)\n"
        "  lw $s3, 28($sp)\n"
        "  lw $s4, 32($sp)\n"
        "  lw $s5, 36($sp)\n"
        "  lw $s6, 40($sp)\n"
        "  lw $s7, 44($sp)\n"
        "  addiu $sp, $sp, 48\n"
        "  jr $ra\n"
        "  nop\n"
        ".set pop\n");

static u32 rng_state;
static u32 rng(void)
{
  rng_state = rng_state * 1103515245u + 12345u;
  return (rng_state >> 8) ^ (rng_state << 21);
}

static u32 edge(void)
{
  static const u32 values[] = {0u,          1u,          2u,          3u,          0xFFFFFFFFu, 0xFFFFFFFEu,
                               0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0x7FFFFFFEu, 0xFFFFu,     0x10000u,
                               0x8000u,     0xFFFF8000u, 0x1Fu,       0x20u,       0x3Fu,       0x40000000u};
  return values[rng() % (sizeof(values) / sizeof(values[0]))];
}

static u32 value32(void)
{
  switch (rng() & 3)
  {
    case 0:
      return edge();
    case 1:
      return rng() & 0xFFu;
    default:
      return rng();
  }
}

/* Registers the code reads and writes: $1-$25. $26 and $27 are the exception handler's and the return address, $28 and
 * $30 the data bases and $29 the stack. */
static u32 reg(void)
{
  return 1 + rng() % 25u;
}
static u32 src(void)
{
  return ((rng() & 7) == 0) ? 0 : reg();
}
static u32 dst(void)
{
  return ((rng() & 15) == 0) ? 0 : reg();
}

static u32 enc_r(u32 rs, u32 rt, u32 rd, u32 sa, u32 funct)
{
  return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
static u32 enc_i(u32 op, u32 rs, u32 rt, u32 imm)
{
  return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu);
}

static u32 imm16(void)
{
  switch (rng() & 3)
  {
    case 0:
      return edge() & 0xFFFFu;
    case 1:
      return rng() & 0xFFu;
    default:
      return rng() & 0xFFFFu;
  }
}

#define G_ALU 0x1u
#define G_IMM 0x2u
#define G_MULDIV 0x4u
#define G_LOAD 0x8u
#define G_STORE 0x10u
#define G_BRANCH 0x20u
#define G_EXC 0x40u
#define G_COPXFER 0x80u
#define G_GTE 0x100u
#define G_SMC 0x200u
#define G_CONSTANT 0x400u
#define G_ONE_RUN 0x80000000u

/* Instructions that never raise an exception, safe in a branch delay slot. */
static u32 safe_alu(void)
{
  static const u32 functs[] = {0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x04, 0x06, 0x07};
  switch (rng() % 6)
  {
    case 0:
      return enc_r(0, src(), dst(), rng() & 31, (u32[]){0x00, 0x02, 0x03}[rng() % 3]);
    case 1:
      return enc_i(0x09 + (rng() % 6u), src(), dst(), imm16()); /* addiu slti sltiu andi ori xori */
    case 2:
      return enc_i(0x0F, 0, dst(), imm16());
    case 3:
      return enc_r(0, 0, dst(), 0, (rng() & 1) ? 0x10 : 0x12); /* mfhi mflo */
    default:
      return enc_r(src(), src(), dst(), 0, functs[rng() % (sizeof(functs) / sizeof(functs[0]))]);
  }
}

static u32 base_reg(u32* offset_mask_out)
{
  if (rng() & 1)
  {
    *offset_mask_out = 0x3FFFu; /* RAM, 8 KB either side of $gp */
    return 28;
  }
  *offset_mask_out = 0x7Fu; /* scratchpad window, +-127 bytes around $fp */
  return 30;
}

static u32 mem_offset(u32 mask, u32 align_mask, int allow_misaligned)
{
  u32 off = rng() & mask;
  if (off & ((mask + 1) >> 1))
    off |= ~mask; /* negative */
  if (!allow_misaligned || (rng() & 7) != 0)
    off &= ~align_mask;
  return off & 0xFFFFu;
}

/* A load or store; with allow_trap the offset may be misaligned for lw, lh, sw, sh (an address error). */
static u32 mem_op(int store, int allow_trap)
{
  static const u32 loads[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
  static const u32 stores[] = {0x28, 0x29, 0x2A, 0x2B, 0x2E};
  u32 mask;
  const u32 base = base_reg(&mask);
  const u32 op = store ? stores[rng() % 5u] : loads[rng() % 7u];
  u32 align = 0;
  if (op == 0x21 || op == 0x25 || op == 0x29)
    align = 1;
  else if (op == 0x23 || op == 0x2B)
    align = 3;
  return enc_i(op, base, store ? src() : dst(), mem_offset(mask, align, allow_trap));
}

static u32 alu_r(void)
{
  static const u32 functs[] = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x20, 0x21, 0x22, 0x23,
                               0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B};
  const u32 f = functs[rng() % 16u];
  const u32 sa = (f <= 0x03) ? (rng() & 31u) : 0;
  return enc_r(src(), src(), dst(), sa, f);
}

static u32 alu_i(void)
{
  return enc_i(0x08 + (rng() % 8u), src(), dst(), imm16());
}

static u32 muldiv(void)
{
  switch (rng() % 8)
  {
    case 0:
      return enc_r(0, 0, dst(), 0, 0x10);
    case 1:
      return enc_r(src(), 0, 0, 0, 0x11);
    case 2:
      return enc_r(0, 0, dst(), 0, 0x12);
    case 3:
      return enc_r(src(), 0, 0, 0, 0x13);
    default:
      return enc_r(src(), src(), 0, 0, 0x18 + (rng() & 3u));
  }
}

static u32 gte_command(void)
{
  static const u32 commands[] = {0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1B, 0x1C,
                                 0x1E, 0x20, 0x28, 0x29, 0x2A, 0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F};
  const u32 command = commands[rng() % (sizeof(commands) / sizeof(commands[0]))];
  return 0x4A000000u | (rng() & 0x01F80000u) | (rng() & 0x400u) | (rng() & 0x7800u) | command;
}

/* A load or store whose address the recompiler knows when it compiles it, since a lui sets the base just before: the
 * scratchpad, the BIOS ROM and RAM, each through its segments. Returns the number of words written. */
static u32 constant_access(volatile u32* out, u32 mode)
{
  static const u32 loads[] = {0x20, 0x21, 0x23, 0x24, 0x25};
  static const u32 stores[] = {0x28, 0x29, 0x2B};
  const u32 base = reg();
  u32 high, offset, op, align = 0;
  int store = 0;

  u32 which = rng() % 5;
  if ((mode >> 11) & 0x1F)
  {
    while (!(((mode >> 11) & 0x1F) & (1u << which)))
      which = (which + 1) % 5;
  }
  switch (which)
  {
    case 0:
      high = (rng() & 1) ? 0x9F80 : 0x1F80; /* not KSEG1: the recompiler reads it as the scratchpad, the interpreter doesn't */
      offset = 0x101 + rng() % 0xFE; /* the scratchpad window the fuzzer owns */
      store = rng() & 1;
      break;
    case 1:
      high = ((u32[]){0xBFC0, 0x9FC0, 0x1FC0}[rng() % 3]);
      offset = rng() & 0x7FFF; /* the BIOS ROM */
      break;
    case 2:
      high = 0x8014; /* the state array, in RAM */
      offset = rng() & 0x1FF;
      break;
    case 3:
      high = 0xA014;
      offset = rng() & 0x1FF;
      break;
    default:
      high = 0x8018; /* the data the loads read, stored to as well */
      offset = rng() & 0x1FFF;
      store = rng() & 1;
      break;
  }

  op = store ? stores[rng() % 3u] : loads[rng() % 5u];
  if (op == 0x21 || op == 0x25 || op == 0x29)
    align = 1;
  else if (op == 0x23 || op == 0x2B)
    align = 3;
  offset &= ~align;

  out[0] = 0x3C000000u | (base << 16) | high;
  out[1] = enc_i(op, base, store ? src() : dst(), offset);
  return 2;
}

/* Multiply or divide with both operands set to edge values just before, so the zero divisor, the unrepresentable
 * quotient and the constant-folded forms all happen; then the results are read. */
static u32 directed_muldiv(volatile u32* out)
{
  const u32 rs = reg();
  const u32 rt = reg();
  u32 n = 0, k;
  const u32 values[2] = {edge(), edge()};

  for (k = 0; k < 2; k++)
  {
    out[n++] = 0x3C000000u | ((k ? rt : rs) << 16) | (values[k] >> 16);
    out[n++] = 0x34000000u | ((k ? rt : rs) << 21) | ((k ? rt : rs) << 16) | (values[k] & 0xFFFFu);
  }
  out[n++] = enc_r(rs, rt, 0, 0, 0x18 + (rng() & 3u));
  out[n++] = enc_r(0, 0, dst(), 0, 0x12);
  out[n++] = enc_r(0, 0, dst(), 0, 0x10);
  return n;
}

/* The words of one block, built in a buffer and copied to CODE at the end; returns their count. */
static u32 build_block(volatile u32* out, u32 mode)
{
  u32 n = 0;
  const u32 want = 4 + rng() % 40u;

  while (n < want)
  {
    const u32 pick = rng() % 100u;
    if (pick < 20 && (mode & G_ALU))
    {
      out[n++] = alu_r();
    }
    else if (pick < 36 && (mode & G_IMM))
    {
      out[n++] = alu_i();
    }
    else if (pick < 44 && (mode & G_MULDIV))
    {
      if (rng() & 1)
        n += directed_muldiv(out + n);
      else
        out[n++] = muldiv();
    }
    else if (pick < 54 && (mode & G_LOAD))
    {
      out[n++] = mem_op(0, (mode & G_EXC) != 0);
    }
    else if (pick < 63 && (mode & G_STORE))
    {
      out[n++] = mem_op(1, (mode & G_EXC) != 0);
    }
    else if (pick < 74 && (mode & G_BRANCH) && n + 8 < 60)
    {
      /* a branch, its delay slot, and a short run of code it may skip */
      const u32 skip = 1 + rng() % 4u;
      const u32 kind = rng() % 12u;
      if (kind < 8)
      {
        static const u32 ops[] = {4, 5, 6, 7};
        u32 word;
        if (kind < 4)
          word = enc_i(ops[kind], src(), (kind < 2) ? src() : 0, skip + 1);
        else
        {
          static const u32 rts[] = {0, 1, 0x10, 0x11};
          word = enc_i(1, src(), rts[kind - 4], skip + 1);
        }
        out[n++] = word;
        out[n++] = ((rng() & 3) == 0) ? mem_op(rng() & 1, 0) : safe_alu();
        {
          u32 k;
          for (k = 0; k < skip; k++)
            out[n++] = (rng() & 3) ? safe_alu() : mem_op(rng() & 1, 0);
        }
      }
      else if (kind < 10)
      {
        /* j or jal over the next words */
        const u32 target = CODE_ADDR + (n + 2 + skip) * 4;
        out[n++] = ((kind == 8) ? 0x08000000u : 0x0C000000u) | ((target >> 2) & 0x03FFFFFFu);
        out[n++] = safe_alu();
        {
          u32 k;
          for (k = 0; k < skip; k++)
            out[n++] = safe_alu();
        }
      }
      else
      {
        /* jr or jalr through $25 loaded with an address ahead */
        const u32 target = CODE_ADDR + (n + 4 + skip) * 4;
        out[n++] = 0x3C000000u | (25u << 16) | (target >> 16);
        out[n++] = 0x34000000u | (25u << 21) | (25u << 16) | (target & 0xFFFFu);
        out[n++] = (kind == 10) ? enc_r(25, 0, 0, 0, 0x08) : enc_r(25, 0, dst(), 0, 0x09);
        out[n++] = safe_alu();
        {
          u32 k;
          for (k = 0; k < skip; k++)
            out[n++] = safe_alu();
        }
      }
    }
    else if (pick < 79 && (mode & G_EXC))
    {
      switch (rng() % 6)
      {
        case 0:
          out[n++] = enc_r(src(), src(), dst(), 0, 0x20);
          break;
        case 1:
          out[n++] = enc_r(src(), src(), dst(), 0, 0x22);
          break;
        case 2:
          out[n++] = enc_i(0x08, src(), dst(), imm16());
          break;
        case 3:
          out[n++] = enc_r(0, 0, 0, 0, 0x0C);
          break;
        case 4:
          out[n++] = enc_r(0, 0, 0, 0, 0x0D);
          break;
        default:
          out[n++] = mem_op(rng() & 1, 1);
          break;
      }
    }
    else if (pick < 84 && (mode & G_CONSTANT))
    {
      n += constant_access(out + n, mode);
    }
    else if (pick < 89 && (mode & G_COPXFER))
    {
      const u32 r = rng() & 31u;
      switch (rng() % 8)
      {
        case 0:
          out[n++] = 0x40000000u | (dst() << 16) | ((u32[]){3, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15}[rng() % 11] << 11); /* mfc0 */
          break;
        case 1:
          out[n++] = 0x48000000u | (dst() << 16) | (r << 11); /* mfc2 */
          break;
        case 2:
          out[n++] = 0x48400000u | (dst() << 16) | (r << 11); /* cfc2 */
          break;
        case 3:
          out[n++] = 0x48800000u | (src() << 16) | (r << 11); /* mtc2 */
          break;
        case 4:
          out[n++] = 0x48C00000u | (src() << 16) | (r << 11); /* ctc2 */
          break;
        case 5:
        {
          u32 mask;
          const u32 base = base_reg(&mask);
          out[n++] = enc_i(0x32, base, r, mem_offset(mask, 3, 0)); /* lwc2 */
        }
        break;
        case 6:
        {
          u32 mask;
          const u32 base = base_reg(&mask);
          out[n++] = enc_i(0x3A, base, r, mem_offset(mask, 3, 0)); /* swc2 */
        }
        break;
        default:
          out[n++] = 0x40800000u | (src() << 16) | ((u32[]){3, 5, 9, 11, 13}[rng() % 5] << 11); /* mtc0 */
          break;
      }
    }
    else if (pick < 93 && (mode & G_GTE))
    {
      out[n++] = gte_command();
    }
    else if ((mode & G_SMC) && pick < 95)
    {
      /* store a new instruction over one ahead in the block: lui $25 code; lui, ori $24 word; sw */
      const u32 victim = n + 5 + rng() % 8u;
      const u32 word = safe_alu();
      out[n++] = 0x3C000000u | (25u << 16) | (CODE_ADDR >> 16);
      out[n++] = 0x3C000000u | (24u << 16) | (word >> 16);
      out[n++] = 0x34000000u | (24u << 21) | (24u << 16) | (word & 0xFFFFu);
      out[n++] = enc_i(0x2B, 25, 24, victim * 4);
      out[n++] = safe_alu();
      while (n < victim)
        out[n++] = safe_alu();
      out[n++] = safe_alu();
    }
    else
    {
      out[n++] = safe_alu();
    }
  }

  /* jr $27 and its delay slot */
  out[n++] = enc_r(27, 0, 0, 0, 0x08);
  out[n++] = 0;
  return n;
}

static u32 checksum;

static void absorb(volatile u32* state)
{
  u32 i;
  for (i = 0; i < S_WORDS; i++)
    checksum = ((checksum << 5) | (checksum >> 27)) ^ state[i] ^ (i * 0x9E3779B9u);
}

void test_main(void)
{
  volatile u32* const scratch = BLOCK_BUFFER;
  const u32 mode = param_mode;
  const u32 blocks = param_blocks;
  u32 i, b;

  rng_state = param_seed;
  *(volatile u32*)0xBF801074 = 0; /* no interrupts */

  /* COP2 enable, as the BIOS sets it (the recompiler never raises coprocessor unusable), and BEV, so exceptions reach the
   handler in the ROM */
  __asm__ volatile("mfc0 $8, $12\n"
                   "nop\n"
                   "lui $9, 0x4040\n"
                   "or $8, $8, $9\n"
                   "mtc0 $8, $12\n"
                   "nop\n" ::: "$8", "$9");

  /* the data the loads read */
  for (i = 0; i < 0x800; i++)
    ((volatile u32*)(DATA_GP - 0x2000))[i] = rng();
  for (i = 0; i < 64; i++)
    ((volatile u32*)(DATA_FP - 0x80))[i] = rng();
  for (i = 0; i < S_WORDS; i++)
    STATE[i] = 0;

  for (b = 0; b < blocks; b++)
  {
    const u32 n = build_block(scratch, mode);
    u32 runs = 1 + (rng() & 1u);
    if (mode & G_ONE_RUN)
      runs = 1;

    for (i = 0; i < n; i++)
      CODE[i] = scratch[i];

    while (runs--)
    {
      for (i = 1; i <= 31; i++)
        STATE[i] = ((rng() & 3) == 0) ? STATE[i] : value32();
      STATE[S_HI] = value32();
      STATE[S_LO] = value32();
      run_block(STATE, CODE_ADDR);
      absorb(STATE);
    }

    RESULT[0] = b;
    RESULT[1] = checksum;
    HISTORY[b & 0x3FF] = checksum;
  }

  for (;;)
    ;
}
