// Emits one of each instruction the PowerPC recompiler backend uses, with labels, and writes the bytes to a file;
// check.sh disassembles them and compares with ppc_emitter.expected.
#include "core/cpu_recompiler_emitter_ppc.h"
#include <cstdio>

using namespace CPU::Recompiler;

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;

	static uint8_t buffer[4096];
	CodeEmitter e(buffer, sizeof(buffer));

	e.addi(3, 4, -5);
	e.addis(3, 4, 0x1234);
	e.addic(5, 6, 7);
	e.subfic(5, 6, 8);
	e.mulli(5, 6, 9);
	e.li(7, -1);
	e.lis(7, 0x8000 - 0x10000);
	e.load_immediate(8, 0x00001234);
	e.load_immediate(8, 0xFFFF8000);
	e.load_immediate(8, 0x12340000);
	e.load_immediate(8, 0x12345678);
	e.add_immediate(9, 10, 0x12348000);
	e.add_immediate(9, 10, 100);
	e.add(3, 4, 5);
	e.addc(3, 4, 5);
	e.adde(3, 4, 5);
	e.addze(3, 4);
	e.subf(3, 4, 5);
	e.subfc(3, 4, 5);
	e.subfe(3, 4, 5);
	e.neg(3, 4);
	e.mullw(3, 4, 5);
	e.mulhw(3, 4, 5);
	e.mulhwu(3, 4, 5);
	e.divw(3, 4, 5);
	e.divwu(3, 4, 5);
	e.ori(3, 4, 0xFFFF);
	e.oris(3, 4, 0x1234);
	e.xori(3, 4, 0x1234);
	e.xoris(3, 4, 0x8000);
	e.andi_(3, 4, 3);
	e.andis_(3, 4, 0x8000);
	e.nop();
	e.and_(3, 4, 5);
	e.and_(3, 4, 5, true);
	e.andc(3, 4, 5);
	e.or_(3, 4, 5);
	e.xor_(3, 4, 5);
	e.nor(3, 4, 5);
	e.mr(3, 4);
	e.extsb(3, 4);
	e.extsh(3, 4);
	e.cntlzw(3, 4);
	e.slw(3, 4, 5);
	e.srw(3, 4, 5);
	e.sraw(3, 4, 5);
	e.srawi(3, 4, 31);
	e.rlwinm(3, 4, 5, 6, 7);
	e.rlwinm(3, 4, 0, 31, 31, true);
	e.rlwimi(3, 4, 5, 6, 7);
	e.slwi(3, 4, 2);
	e.srwi(3, 4, 16);
	e.clrlwi(3, 4, 24);
	e.cmpwi(0, 3, -1);
	e.cmplwi(1, 3, 0xFFFF);
	e.cmpw(7, 3, 4);
	e.cmplw(0, 3, 4);
	e.cmpdi(0, 3, 0);
	e.lbz(3, 4, 5);
	e.lhz(3, 4, 5);
	e.lha(3, -4, 5);
	e.lwz(3, 0x7FFC, 5);
	e.stb(3, 4, 5);
	e.sth(3, 4, 5);
	e.stw(3, -8, 5);
	e.stwu(1, -304, 1);
	e.ld(14, 0x90, 1);
	e.std(14, 0x90, 1);
	e.lbzx(3, 4, 5);
	e.lhzx(3, 4, 5);
	e.lwzx(3, 4, 5);
	e.lhbrx(3, 0, 5);
	e.lwbrx(3, 4, 5);
	e.stbx(3, 4, 5);
	e.sthx(3, 4, 5);
	e.stwx(3, 4, 5);
	e.sthbrx(3, 0, 5);
	e.stwbrx(3, 4, 5);
	e.mflr(0);
	e.mtlr(30);
	e.mtctr(12);
	e.blr();
	e.bctr();
	e.bctrl();
	e.isync();
	e.trap();

	LabelType back;
	e.bind(&back);
	e.nop();
	LabelType forward;
	e.b(&forward);
	e.bt(CR_EQ, &forward);
	e.bf(CR_LT, &forward);
	e.bc(BO_TRUE, 4 * 6 + CR_GT, &back);
	e.b(&back);
	e.nop();
	e.bind(&forward);
	e.b(buffer);
	e.bl(buffer + 8);
	LabelType address;
	const size_t load_at = e.cursor_offset();
	e.load_label_address(3, &address);
	e.nop();
	e.bind(&address);
	e.nop();

	if (e.failed())
		return 1;

	// The label's address depends on where the buffer is: check the lis and ori, then zero their immediates.
	const uint32_t label_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer + e.cursor_offset() - 4));
	uint8_t *load = buffer + load_at;
	if (((load[2] << 8) | load[3]) != (label_address >> 16) || ((load[6] << 8) | load[7]) != (label_address & 0xFFFF)) {
		fputs("the label's address was patched wrong\n", stderr);
		return 1;
	}
	load[2] = load[3] = load[6] = load[7] = 0;

	FILE *f = fopen(argv[1], "wb");
	if (!f)
		return 1;
	fwrite(buffer, 1, e.size_generated(), f);
	fclose(f);
	return 0;
}
