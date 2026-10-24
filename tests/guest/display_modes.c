/* Display test, run in place of the BIOS.
 *
 * Fills VRAM with pseudo-random pixels and shows it in a display mode that the mode parameter picks: 0 is 15-bit
 * colour, 1 is 24-bit, 2 is 15-bit interlaced and 3 is 24-bit interlaced. The test frontend's OUTPUTSUM line holds a
 * checksum of the last frame the core made, which no longer changes by then. RAM[0] is 0xC0FFEE when the mode has
 * been shown. */
typedef unsigned int u32;

__attribute__((section(".params"))) const volatile u32 param_seed = 1;
__attribute__((section(".params"))) const volatile u32 param_mode = 0;

#define IO32(a) (*(volatile u32*)(0xBF800000u + (a)))
#define RAM32(a) (*(volatile u32*)(0xA0000000u + (a)))
#define GP0(v) (IO32(0x1810) = (v))
#define GP1(v) (IO32(0x1814) = (v))

#define I_STAT IO32(0x1070)

/* 320 x 240 15-bit, 24-bit, 320 x 480 interlaced 15-bit, and 24-bit */
static const u32 modes[4] = {0x01, 0x11, 0x25, 0x35};

static u32 rng_state;
static u32 rng(void)
{
  rng_state = rng_state * 1103515245u + 12345u;
  return (rng_state >> 8) ^ (rng_state << 16);
}

/* Width in pixels, 240 lines. */
static void fill_vram(u32 width)
{
  u32 i;
  GP0(0xA0000000u);
  GP0(0x00000000u);
  GP0((240u << 16) | width);
  for (i = 0; i < width * 240u / 2u; i++)
    GP0(rng());
}

/* Six frames: the vblank flag is set at the start of each, however fast the core runs the CPU. */
static void show(u32 mode)
{
  u32 frame;
  GP1(0x08000000u | mode);
  for (frame = 0; frame < 6; frame++)
  {
    I_STAT = ~1u;
    while (!(I_STAT & 1u))
    {
    }
  }
}

void test_main(void)
{
  rng_state = param_seed;

  GP1(0x00000000u); /* the display is off after a reset */
  GP1(0x05000000u); /* display area at 0, 0 */
  GP1(0x06C60260u); /* 320 pixels across */
  GP1(0x07040010u); /* 240 lines */

  fill_vram(480); /* 24-bit pixels are three bytes: 320 of them span 480 VRAM pixels */
  GP1(0x03000000u); /* display on */
  show(modes[param_mode & 3u]);

  RAM32(0) = 0xC0FFEEu;
  for (;;)
  {
  }
}
