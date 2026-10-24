/* GTE test, run in place of the BIOS.
 *
 * Runs each GTE command with pseudo-random registers (small, large and fixed-point values, so the saturation flags
 * set too), many times, and folds all 64 registers after each into a checksum. Case 22 only moves values through
 * the registers with MTC2, CTC2, MFC2 and CFC2, case 23 through LWC2 and SWC2 and memory. For case n,
 * RAM[0x100 + n * 4] holds the checksum. RAM[0] is 0xC0FFEE when all cases ran. */
typedef unsigned int u32;

__attribute__((section(".params"))) const volatile u32 param_seed = 1;
__attribute__((section(".params"))) const volatile u32 param_mode = 0;

#define RAM32(a) (*(volatile u32*)(0xA0000000u + (a)))

#define NUM_CASES 24
#define ITERATIONS 60
#define STUB 0xA0010000u /* a command, jr ra, nop, run from uncached RAM */
#define WORDS 0xA0020000u
#define BACK 0xA0021000u

/* The assembler for MIPS I has no COP2 moves, so these are their encodings through $t0 ($8), and LWC2 and SWC2 through
 * $t1 ($9). The register number goes in bits 11-15 (bits 16-20 for LWC2 and SWC2). */
#define MFC2_T0 0x48080000u
#define CFC2_T0 0x48480000u
#define MTC2_T0 0x48880000u
#define CTC2_T0 0x48C80000u
#define LWC2_BASE_T1 0xC9200000u
#define SWC2_BASE_T1 0xE9200000u

#define FOR_32(X)                                                                                                      \
  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20)    \
  X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

static const u32 opcodes[22] = {0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1B, 0x1C,
                                0x1E, 0x20, 0x28, 0x29, 0x2A, 0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F};

static u32 rng_state;
static u32 rng(void)
{
  rng_state = rng_state * 1103515245u + 12345u;
  return rng_state >> 8;
}

static u32 value(void)
{
  const u32 r = rng();
  switch (rng() & 3u)
  {
    case 0:
      return (u32)(int)(short)r;
    case 1:
      return r ^ (rng() << 16);
    case 2:
      return (r & 0x3FFFu) - 0x2000u;
    default:
      return r & 0x1FFFu;
  }
}

static u32 regs[64];

static void load_regs(void)
{
  u32 i;
  for (i = 0; i < 64; i++)
    regs[i] = value();

#define SET_D(n) asm volatile("move $8, %0\n\t.word %1" : : "r"(regs[n]), "i"(MTC2_T0 | (n << 11)) : "$8", "memory");
#define SET_C(n) asm volatile("move $8, %0\n\t.word %1" : : "r"(regs[32 + n]), "i"(CTC2_T0 | (n << 11)) : "$8", "memory");
  FOR_32(SET_D)
  FOR_32(SET_C)
#undef SET_D
#undef SET_C
}

static u32 fold(u32 sum)
{
  u32 v;
#define GET_D(n)                                                                                                       \
  asm volatile(".set noreorder\n\t.word %1\n\tnop\n\tmove %0, $8\n\t.set reorder"                                      \
               : "=r"(v)                                                                                               \
               : "i"(MFC2_T0 | (n << 11))                                                                              \
               : "$8", "memory");                                                                                      \
  sum = sum * 31u + v;
#define GET_C(n)                                                                                                       \
  asm volatile(".set noreorder\n\t.word %1\n\tnop\n\tmove %0, $8\n\t.set reorder"                                      \
               : "=r"(v)                                                                                               \
               : "i"(CFC2_T0 | (n << 11))                                                                              \
               : "$8", "memory");                                                                                      \
  sum = sum * 31u + v;
  FOR_32(GET_D)
  FOR_32(GET_C)
#undef GET_D
#undef GET_C
  return sum;
}

static void command(u32 word)
{
  RAM32(STUB - 0xA0000000u) = word;
  RAM32(STUB - 0xA0000000u + 4) = 0x03E00008u; /* jr ra */
  RAM32(STUB - 0xA0000000u + 8) = 0;
  ((void (*)(void))STUB)();
}

static u32 lwc2_swc2(u32 sum)
{
  u32 i;
  for (i = 0; i < 32; i++)
    *(volatile u32*)(WORDS + i * 4) = value();

#define LOAD(n)                                                                                                        \
  asm volatile("move $9, %0\n\t.word %1" : : "r"(WORDS), "i"(LWC2_BASE_T1 | (n << 16) | (n * 4)) : "$9", "memory");
#define STORE(n)                                                                                                       \
  asm volatile("move $9, %0\n\t.word %1" : : "r"(BACK), "i"(SWC2_BASE_T1 | (n << 16) | (n * 4)) : "$9", "memory");
  FOR_32(LOAD)
  FOR_32(STORE)
#undef LOAD
#undef STORE

  for (i = 0; i < 32; i++)
    sum = sum * 31u + *(volatile u32*)(BACK + i * 4);
  return fold(sum);
}

void test_main(void)
{
  u32 c, i, sum;

  /* Coprocessor 2 usable: SR bit 30. */
  asm volatile(".set noreorder\n\tlui $8, 0x4000\n\tmtc0 $8, $12\n\tnop\n\tnop\n\t.set reorder" : : : "$8", "memory");

  for (c = 0; c < NUM_CASES; c++)
  {
    rng_state = param_seed + c;
    sum = 0;
    for (i = 0; i < ITERATIONS; i++)
    {
      load_regs();
      if (c < 22)
        command(0x4A000000u | (rng() & 0xFE400u) | opcodes[c]);
      if (c == 23)
        sum = lwc2_swc2(sum);
      else
        sum = fold(sum);
    }
    RAM32(0x100 + c * 4) = sum;
  }

  RAM32(0) = 0xC0FFEEu;
  for (;;)
  {
  }
}
