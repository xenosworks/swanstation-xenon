#pragma once
// The console's full-speed measure (xsdk's docs/scope/62-swanstation.md, the gate): time-base ticks spent running each
// timed event, in the GPU thread's work, and in the CPU thread's waits for it, which the host (src/xenon/host.c) reads
// and clears every report. 32-bit counts, which another thread reads whole: a report's ticks fit at 49.875 MHz.
#ifdef __XSDK__
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
  XENON_PROFILE_EVENTS = 16
};

typedef struct
{
  // Each timed event's name, and the ticks its callbacks took, the events they ran early included.
  const char* event_names[XENON_PROFILE_EVENTS];
  uint32_t event_ticks[XENON_PROFILE_EVENTS];
  uint32_t event_count;
  // The GPU thread running commands.
  uint32_t gpu_thread_ticks;
  // The CPU thread waiting for the GPU thread to finish its commands, and for room for more.
  uint32_t gpu_sync_ticks;
  uint32_t gpu_space_ticks;
  // The SPU making its samples, which it does on demand as the CPU reaches its registers, outside the events mostly.
  uint32_t spu_ticks;
  // The CPU thread waiting for the CD-ROM read thread's sector.
  uint32_t cdrom_wait_ticks;
} XenonProfile;

extern XenonProfile xenon_profile;

static inline uint32_t xenon_profile_now(void)
{
  return (uint32_t)__builtin_ppc_get_timebase();
}

#ifdef __cplusplus
}

// Adds the ticks from its making to its end to a count.
class XenonProfileScope
{
public:
  explicit XenonProfileScope(uint32_t* ticks) : m_ticks(ticks), m_start(xenon_profile_now()) {}
  ~XenonProfileScope() { *m_ticks += xenon_profile_now() - m_start; }

private:
  uint32_t* m_ticks;
  uint32_t m_start;
};
#endif
#endif
