#include "cpu_recompiler_emitter_ppc.h"
#include <cassert>

namespace CPU::Recompiler
{

CodeEmitter::CodeEmitter(uint8_t *buffer, size_t size_bytes) : m_buffer(buffer), m_size(size_bytes)
{
}

void CodeEmitter::reset()
{
	m_offset = 0;
	m_failed = false;
}

void CodeEmitter::reset(uint8_t *buffer, size_t size_bytes)
{
	m_buffer = buffer;
	m_size = size_bytes;
	reset();
}

void CodeEmitter::word(uint32_t instruction)
{
	if (m_offset + 4 > m_size) {
		m_failed = true;
		return;
	}

	uint8_t *at = m_buffer + m_offset;
	at[0] = static_cast<uint8_t>(instruction >> 24);
	at[1] = static_cast<uint8_t>(instruction >> 16);
	at[2] = static_cast<uint8_t>(instruction >> 8);
	at[3] = static_cast<uint8_t>(instruction);
	m_offset += 4;
}

static uint32_t read_word(const uint8_t *at)
{
	return (static_cast<uint32_t>(at[0]) << 24) | (static_cast<uint32_t>(at[1]) << 16) |
		(static_cast<uint32_t>(at[2]) << 8) | static_cast<uint32_t>(at[3]);
}

static void write_word(uint8_t *at, uint32_t instruction)
{
	at[0] = static_cast<uint8_t>(instruction >> 24);
	at[1] = static_cast<uint8_t>(instruction >> 16);
	at[2] = static_cast<uint8_t>(instruction >> 8);
	at[3] = static_cast<uint8_t>(instruction);
}

bool CodeEmitter::patch_branch(uint32_t offset, LabelType::FixupKind kind, int64_t displacement_bytes)
{
	uint8_t *at = m_buffer + offset;
	uint32_t instruction = read_word(at);

	if (kind == LabelType::FIXUP_B24) {
		if (displacement_bytes < -(INT64_C(1) << 25) || displacement_bytes >= (INT64_C(1) << 25))
			return false;
		instruction = (instruction & ~0x03FFFFFCu) | (static_cast<uint32_t>(displacement_bytes) & 0x03FFFFFCu);
	} else {
		if (displacement_bytes < -(INT64_C(1) << 15) || displacement_bytes >= (INT64_C(1) << 15))
			return false;
		instruction = (instruction & ~0x0000FFFCu) | (static_cast<uint32_t>(displacement_bytes) & 0x0000FFFCu);
	}

	write_word(at, instruction);
	return true;
}

void CodeEmitter::add_fixup(LabelType *label, LabelType::FixupKind kind)
{
	label->m_fixups.push_back({ static_cast<uint32_t>(m_offset), kind });
}

void CodeEmitter::bind(LabelType *label)
{
	assert(!label->m_bound);
	label->m_bound = true;
	label->m_offset = static_cast<uint32_t>(m_offset);

	for (const LabelType::Fixup &fixup : label->m_fixups) {
		if (fixup.kind == LabelType::FIXUP_ABSOLUTE) {
			const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_buffer + label->m_offset));
			uint8_t *lis_at = m_buffer + fixup.offset;
			uint8_t *ori_at = lis_at + 4;
			write_word(lis_at, (read_word(lis_at) & 0xFFFF0000u) | (address >> 16));
			write_word(ori_at, (read_word(ori_at) & 0xFFFF0000u) | (address & 0xFFFFu));
		} else if (!patch_branch(fixup.offset, fixup.kind,
					   static_cast<int64_t>(label->m_offset) - static_cast<int64_t>(fixup.offset))) {
			m_failed = true;
		}
	}
	label->m_fixups.clear();
}

bool CodeEmitter::b_reaches(const void *target) const
{
	const int64_t displacement = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cursor());
	return displacement >= -(INT64_C(1) << 25) && displacement < (INT64_C(1) << 25) && (displacement & 3) == 0;
}

void CodeEmitter::load_immediate(uint32_t rd, uint32_t value)
{
	const int32_t signed_value = static_cast<int32_t>(value);
	if (signed_value >= -32768 && signed_value <= 32767) {
		li(rd, signed_value);
		return;
	}

	lis(rd, static_cast<int16_t>(value >> 16));
	if ((value & 0xFFFF) != 0)
		ori(rd, rd, value & 0xFFFF);
}

void CodeEmitter::add_immediate(uint32_t rd, uint32_t ra, uint32_t value)
{
	assert(ra != 0);
	const int32_t signed_value = static_cast<int32_t>(value);
	if (signed_value >= -32768 && signed_value <= 32767) {
		if (signed_value != 0 || rd != ra)
			addi(rd, ra, signed_value);
		return;
	}

	// addi sign-extends its immediate, so the high half takes the carry
	const uint32_t high = (value >> 16) + ((value & 0x8000) ? 1 : 0);
	addis(rd, ra, static_cast<int16_t>(high));
	if ((value & 0xFFFF) != 0)
		addi(rd, rd, static_cast<int16_t>(value & 0xFFFF));
}

void CodeEmitter::b(LabelType *label)
{
	if (label->m_bound) {
		const int64_t displacement = static_cast<int64_t>(label->m_offset) - static_cast<int64_t>(m_offset);
		if (displacement < -(INT64_C(1) << 25) || displacement >= (INT64_C(1) << 25)) {
			m_failed = true;
			return;
		}
		word(0x48000000 | (static_cast<uint32_t>(displacement) & 0x03FFFFFCu));
		return;
	}

	add_fixup(label, LabelType::FIXUP_B24);
	word(0x48000000);
}

void CodeEmitter::b(const void *target)
{
	if (!b_reaches(target)) {
		m_failed = true;
		return;
	}

	const int64_t displacement = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cursor());
	word(0x48000000 | (static_cast<uint32_t>(displacement) & 0x03FFFFFCu));
}

void CodeEmitter::bl(const void *target)
{
	if (!b_reaches(target)) {
		m_failed = true;
		return;
	}

	const int64_t displacement = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cursor());
	word(0x48000001 | (static_cast<uint32_t>(displacement) & 0x03FFFFFCu));
}

void CodeEmitter::bc(uint32_t bo, uint32_t bi, LabelType *label)
{
	const uint32_t base = 0x40000000 | (bo << 21) | (bi << 16);
	if (label->m_bound) {
		const int64_t displacement = static_cast<int64_t>(label->m_offset) - static_cast<int64_t>(m_offset);
		if (displacement < -(INT64_C(1) << 15) || displacement >= (INT64_C(1) << 15)) {
			m_failed = true;
			return;
		}
		word(base | (static_cast<uint32_t>(displacement) & 0xFFFCu));
		return;
	}

	add_fixup(label, LabelType::FIXUP_B14);
	word(base);
}

void CodeEmitter::load_label_address(uint32_t rd, LabelType *label)
{
	if (label->m_bound) {
		load_immediate(rd, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_buffer + label->m_offset)));
		return;
	}

	add_fixup(label, LabelType::FIXUP_ABSOLUTE);
	lis(rd, 0);
	ori(rd, rd, 0);
}

} // namespace CPU::Recompiler
