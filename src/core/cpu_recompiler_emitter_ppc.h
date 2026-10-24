#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// A small assembler for the PowerPC recompiler backend: the 32-bit instructions it uses, labels with forward fixups,
// and the code written in place, big-endian, whatever the host. Registers are numbers, 0 to 31. Where the manual lets
// RA = 0 mean the value 0 (addi, addis, loads and stores), pass 0 for that.

namespace CPU::Recompiler
{

// A CR field's bits; BI is 4 * the field + the bit.
enum : uint32_t {
	CR_LT = 0,
	CR_GT = 1,
	CR_EQ = 2,
	CR_SO = 3,
};

// BO for a branch when the CR bit is set, clear, and always (no prediction hint).
enum : uint32_t {
	BO_TRUE = 12,
	BO_FALSE = 4,
	BO_ALWAYS = 20,
};

// A place in the code: bound once, branched to before and after.
class LabelType
{
  public:
	LabelType() = default;
	LabelType(const LabelType &) = delete;
	LabelType &operator=(const LabelType &) = delete;

	bool is_bound() const
	{
		return m_bound;
	}

  private:
	friend class CodeEmitter;

	enum FixupKind : uint8_t {
		FIXUP_B24,      // b, bl: 24-bit word displacement
		FIXUP_B14,      // bc: 14-bit word displacement
		FIXUP_ABSOLUTE, // lis, ori: the label's address
	};

	struct Fixup {
		uint32_t offset;
		FixupKind kind;
	};

	std::vector<Fixup> m_fixups;
	uint32_t m_offset = 0;
	bool m_bound = false;
};

class CodeEmitter
{
  public:
	CodeEmitter(uint8_t *buffer, size_t size_bytes);

	uint8_t *buffer() const
	{
		return m_buffer;
	}
	uint8_t *cursor() const
	{
		return m_buffer + m_offset;
	}
	size_t cursor_offset() const
	{
		return m_offset;
	}
	size_t size_generated() const
	{
		return m_offset;
	}

	// True once a branch was out of range or the code ran past the buffer; the code is then wrong.
	bool failed() const
	{
		return m_failed;
	}

	// Starts again at the buffer's start with the same buffer.
	void reset();

	// Restarts on another buffer, as for a new block.
	void reset(uint8_t *buffer, size_t size_bytes);

	void word(uint32_t instruction);

	// Binds the label here and patches the branches waiting for it.
	void bind(LabelType *label);

	// Whether a b or bl from here reaches the address.
	bool b_reaches(const void *target) const;

	//////////////////////////////////////////////////////////////////////////
	// Integer arithmetic
	//////////////////////////////////////////////////////////////////////////
	void addi(uint32_t rd, uint32_t ra, int32_t simm)
	{
		d_form(14, rd, ra, simm);
	}
	void addis(uint32_t rd, uint32_t ra, int32_t simm)
	{
		d_form(15, rd, ra, simm);
	}
	void addic(uint32_t rd, uint32_t ra, int32_t simm)
	{
		d_form(12, rd, ra, simm);
	}
	void subfic(uint32_t rd, uint32_t ra, int32_t simm)
	{
		d_form(8, rd, ra, simm);
	}
	void mulli(uint32_t rd, uint32_t ra, int32_t simm)
	{
		d_form(7, rd, ra, simm);
	}
	void li(uint32_t rd, int32_t simm)
	{
		addi(rd, 0, simm);
	}
	void lis(uint32_t rd, int32_t simm)
	{
		addis(rd, 0, simm);
	}

	// rd = a 32-bit constant, in one to two instructions.
	void load_immediate(uint32_t rd, uint32_t value);

	// rd = ra + a 32-bit constant, in one to two instructions; ra can't be 0.
	void add_immediate(uint32_t rd, uint32_t ra, uint32_t value);

	void add(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 266, false);
	}
	void addc(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 10, false);
	}
	void adde(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 138, false);
	}
	void addze(uint32_t rd, uint32_t ra)
	{
		xo_form(rd, ra, 0, 202, false);
	}
	// rd = rb - ra
	void subf(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 40, false);
	}
	void subfc(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 8, false);
	}
	void subfe(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 136, false);
	}
	void neg(uint32_t rd, uint32_t ra)
	{
		xo_form(rd, ra, 0, 104, false);
	}
	void mullw(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 235, false);
	}
	void mulhw(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 75, false);
	}
	void mulhwu(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 11, false);
	}
	void divw(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 491, false);
	}
	void divwu(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		xo_form(rd, ra, rb, 459, false);
	}

	//////////////////////////////////////////////////////////////////////////
	// Logical, shifts and rotates
	//////////////////////////////////////////////////////////////////////////
	void ori(uint32_t ra, uint32_t rs, uint32_t uimm)
	{
		d_form(24, rs, ra, uimm);
	}
	void oris(uint32_t ra, uint32_t rs, uint32_t uimm)
	{
		d_form(25, rs, ra, uimm);
	}
	void xori(uint32_t ra, uint32_t rs, uint32_t uimm)
	{
		d_form(26, rs, ra, uimm);
	}
	void xoris(uint32_t ra, uint32_t rs, uint32_t uimm)
	{
		d_form(27, rs, ra, uimm);
	}
	// andi. and andis. always set CR0.
	void andi_(uint32_t ra, uint32_t rs, uint32_t uimm)
	{
		d_form(28, rs, ra, uimm);
	}
	void andis_(uint32_t ra, uint32_t rs, uint32_t uimm)
	{
		d_form(29, rs, ra, uimm);
	}
	void nop()
	{
		ori(0, 0, 0);
	}

	void and_(uint32_t ra, uint32_t rs, uint32_t rb, bool rc = false)
	{
		x_form(rs, ra, rb, 28, rc);
	}
	void andc(uint32_t ra, uint32_t rs, uint32_t rb, bool rc = false)
	{
		x_form(rs, ra, rb, 60, rc);
	}
	void or_(uint32_t ra, uint32_t rs, uint32_t rb, bool rc = false)
	{
		x_form(rs, ra, rb, 444, rc);
	}
	void xor_(uint32_t ra, uint32_t rs, uint32_t rb, bool rc = false)
	{
		x_form(rs, ra, rb, 316, rc);
	}
	void nor(uint32_t ra, uint32_t rs, uint32_t rb)
	{
		x_form(rs, ra, rb, 124, false);
	}
	void mr(uint32_t ra, uint32_t rs)
	{
		or_(ra, rs, rs);
	}
	void extsb(uint32_t ra, uint32_t rs)
	{
		x_form(rs, ra, 0, 954, false);
	}
	void extsh(uint32_t ra, uint32_t rs)
	{
		x_form(rs, ra, 0, 922, false);
	}
	void cntlzw(uint32_t ra, uint32_t rs)
	{
		x_form(rs, ra, 0, 26, false);
	}

	// The shift amount is the low 6 bits of rb: 32 and over give 0 (slw, srw) or the sign (sraw).
	void slw(uint32_t ra, uint32_t rs, uint32_t rb)
	{
		x_form(rs, ra, rb, 24, false);
	}
	void srw(uint32_t ra, uint32_t rs, uint32_t rb)
	{
		x_form(rs, ra, rb, 536, false);
	}
	void sraw(uint32_t ra, uint32_t rs, uint32_t rb)
	{
		x_form(rs, ra, rb, 792, false);
	}
	void srawi(uint32_t ra, uint32_t rs, uint32_t sh)
	{
		x_form(rs, ra, sh, 824, false);
	}

	// ra = rotate left rs by sh, ANDed with the mask of bits mb to me (bit 0 is the most significant).
	void rlwinm(uint32_t ra, uint32_t rs, uint32_t sh, uint32_t mb, uint32_t me, bool rc = false)
	{
		m_form(21, rs, ra, sh, mb, me, rc);
	}
	void rlwimi(uint32_t ra, uint32_t rs, uint32_t sh, uint32_t mb, uint32_t me)
	{
		m_form(20, rs, ra, sh, mb, me, false);
	}
	void slwi(uint32_t ra, uint32_t rs, uint32_t n)
	{
		rlwinm(ra, rs, n, 0, 31 - n);
	}
	void srwi(uint32_t ra, uint32_t rs, uint32_t n)
	{
		rlwinm(ra, rs, 32 - n, n, 31);
	}
	void clrlwi(uint32_t ra, uint32_t rs, uint32_t n)
	{
		rlwinm(ra, rs, 0, n, 31);
	}

	//////////////////////////////////////////////////////////////////////////
	// Compare: cr is the CR field, 0 to 7
	//////////////////////////////////////////////////////////////////////////
	void cmpwi(uint32_t cr, uint32_t ra, int32_t simm)
	{
		d_form(11, cr << 2, ra, simm);
	}
	void cmplwi(uint32_t cr, uint32_t ra, uint32_t uimm)
	{
		d_form(10, cr << 2, ra, uimm);
	}
	void cmpw(uint32_t cr, uint32_t ra, uint32_t rb)
	{
		x_form(cr << 2, ra, rb, 0, false);
	}
	void cmplw(uint32_t cr, uint32_t ra, uint32_t rb)
	{
		x_form(cr << 2, ra, rb, 32, false);
	}
	// 64-bit compare of the whole registers (L = 1); the console's code only.
	void cmpdi(uint32_t cr, uint32_t ra, int32_t simm)
	{
		d_form(11, (cr << 2) | 1, ra, simm);
	}

	//////////////////////////////////////////////////////////////////////////
	// Loads and stores: D-form (ra + a 16-bit displacement), X-form (ra + rb); ra = 0 is the value 0
	//////////////////////////////////////////////////////////////////////////
	void lbz(uint32_t rd, int32_t disp, uint32_t ra)
	{
		d_form(34, rd, ra, disp);
	}
	void lhz(uint32_t rd, int32_t disp, uint32_t ra)
	{
		d_form(40, rd, ra, disp);
	}
	void lha(uint32_t rd, int32_t disp, uint32_t ra)
	{
		d_form(42, rd, ra, disp);
	}
	void lwz(uint32_t rd, int32_t disp, uint32_t ra)
	{
		d_form(32, rd, ra, disp);
	}
	void stb(uint32_t rs, int32_t disp, uint32_t ra)
	{
		d_form(38, rs, ra, disp);
	}
	void sth(uint32_t rs, int32_t disp, uint32_t ra)
	{
		d_form(44, rs, ra, disp);
	}
	void stw(uint32_t rs, int32_t disp, uint32_t ra)
	{
		d_form(36, rs, ra, disp);
	}
	void stwu(uint32_t rs, int32_t disp, uint32_t ra)
	{
		d_form(37, rs, ra, disp);
	}
	// ld and std take a displacement that is a multiple of 4.
	void ld(uint32_t rd, int32_t disp, uint32_t ra)
	{
		ds_form(58, rd, ra, disp);
	}
	void std(uint32_t rs, int32_t disp, uint32_t ra)
	{
		ds_form(62, rs, ra, disp);
	}

	void lbzx(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		x_form(rd, ra, rb, 87, false);
	}
	void lhzx(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		x_form(rd, ra, rb, 279, false);
	}
	void lwzx(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		x_form(rd, ra, rb, 23, false);
	}
	// Byte-reversed: the value as the little-endian memory holds it.
	void lhbrx(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		x_form(rd, ra, rb, 790, false);
	}
	void lwbrx(uint32_t rd, uint32_t ra, uint32_t rb)
	{
		x_form(rd, ra, rb, 534, false);
	}
	void stbx(uint32_t rs, uint32_t ra, uint32_t rb)
	{
		x_form(rs, ra, rb, 215, false);
	}
	void sthx(uint32_t rs, uint32_t ra, uint32_t rb)
	{
		x_form(rs, ra, rb, 407, false);
	}
	void stwx(uint32_t rs, uint32_t ra, uint32_t rb)
	{
		x_form(rs, ra, rb, 151, false);
	}
	void sthbrx(uint32_t rs, uint32_t ra, uint32_t rb)
	{
		x_form(rs, ra, rb, 918, false);
	}
	void stwbrx(uint32_t rs, uint32_t ra, uint32_t rb)
	{
		x_form(rs, ra, rb, 662, false);
	}

	//////////////////////////////////////////////////////////////////////////
	// Special registers and branches
	//////////////////////////////////////////////////////////////////////////
	void mflr(uint32_t rd)
	{
		word(0x7C0802A6 | (rd << 21));
	}
	void mtlr(uint32_t rs)
	{
		word(0x7C0803A6 | (rs << 21));
	}
	void mtctr(uint32_t rs)
	{
		word(0x7C0903A6 | (rs << 21));
	}
	void blr()
	{
		word(0x4E800020);
	}
	void bctr()
	{
		word(0x4E800420);
	}
	void bctrl()
	{
		word(0x4E800421);
	}
	void isync()
	{
		word(0x4C00012C);
	}
	// A trap that always fires: the code reached a place it must not.
	void trap()
	{
		word(0x7FE00008);
	}

	// Branch to a label, or to an address that b_reaches. bl sets LR.
	void b(LabelType *label);
	void b(const void *target);
	void bl(const void *target);

	// Branch when the CR bit bi is set (bt) or clear (bf), to a label within 32 KB.
	void bc(uint32_t bo, uint32_t bi, LabelType *label);
	void bt(uint32_t bi, LabelType *label)
	{
		bc(BO_TRUE, bi, label);
	}
	void bf(uint32_t bi, LabelType *label)
	{
		bc(BO_FALSE, bi, label);
	}

	// lis and ori of the label's address, patched when it is bound.
	void load_label_address(uint32_t rd, LabelType *label);

  private:
	void d_form(uint32_t opcode, uint32_t rt, uint32_t ra, int32_t imm)
	{
		word((opcode << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(imm) & 0xFFFF));
	}
	void ds_form(uint32_t opcode, uint32_t rt, uint32_t ra, int32_t disp)
	{
		word((opcode << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(disp) & 0xFFFC));
	}
	void x_form(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, bool rc)
	{
		word((31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | (rc ? 1u : 0u));
	}
	void xo_form(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, bool rc)
	{
		x_form(rt, ra, rb, xo, rc);
	}
	void m_form(uint32_t opcode, uint32_t rs, uint32_t ra, uint32_t sh, uint32_t mb, uint32_t me, bool rc)
	{
		word((opcode << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1u : 0u));
	}

	// Patches the instruction at offset to branch to the displacement in bytes; false if it doesn't reach.
	bool patch_branch(uint32_t offset, LabelType::FixupKind kind, int64_t displacement_bytes);

	void add_fixup(LabelType *label, LabelType::FixupKind kind);

	uint8_t *m_buffer;
	size_t m_size;
	size_t m_offset = 0;
	bool m_failed = false;
};

} // namespace CPU::Recompiler
