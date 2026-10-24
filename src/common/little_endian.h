#pragma once
#include "types.h"
#include <cstring>

// PS1 memory is little-endian byte for byte on every host: RAM, scratchpad, BIOS, VRAM, SPU RAM, sector data. C++ reaches
// its multi-byte values through these. Under MSB_FIRST they swap, and GCC turns a swap of a load or a store into one
// byte-reversed instruction (lhbrx, lwbrx, sthbrx, stwbrx on PowerPC). Otherwise they are plain loads and stores.

#ifdef MSB_FIRST
// Converts a value between host order and little-endian order (the same swap both ways).
ALWAYS_INLINE uint16_t SwapLE16(uint16_t value)
{
  return __builtin_bswap16(value);
}
ALWAYS_INLINE uint32_t SwapLE32(uint32_t value)
{
  return __builtin_bswap32(value);
}
ALWAYS_INLINE uint64_t SwapLE64(uint64_t value)
{
  return __builtin_bswap64(value);
}
#else
ALWAYS_INLINE constexpr uint16_t SwapLE16(uint16_t value)
{
  return value;
}
ALWAYS_INLINE constexpr uint32_t SwapLE32(uint32_t value)
{
  return value;
}
ALWAYS_INLINE constexpr uint64_t SwapLE64(uint64_t value)
{
  return value;
}
#endif

ALWAYS_INLINE uint16_t LoadLE16(const void* address)
{
  uint16_t value;
  std::memcpy(&value, address, sizeof(value));
  return SwapLE16(value);
}

ALWAYS_INLINE uint32_t LoadLE32(const void* address)
{
  uint32_t value;
  std::memcpy(&value, address, sizeof(value));
  return SwapLE32(value);
}

ALWAYS_INLINE void StoreLE16(void* address, uint16_t value)
{
  value = SwapLE16(value);
  std::memcpy(address, &value, sizeof(value));
}

ALWAYS_INLINE void StoreLE32(void* address, uint32_t value)
{
  value = SwapLE32(value);
  std::memcpy(address, &value, sizeof(value));
}
