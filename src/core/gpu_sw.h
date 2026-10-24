#pragma once
#include "common/heap_array.h"
#include "gpu.h"
#include "gpu_sw_backend.h"
#include "host_display.h"
#include <array>
#include <memory>
#include <vector>

class HostDisplayTexture;

class GPU_SW final : public GPU
{
public:
  GPU_SW();
  ~GPU_SW() override;

  bool Initialize(HostDisplay* host_display) override;
  bool DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display) override;
  void Reset(bool clear_vram) override;
  void UpdateSettings() override;

protected:
  void ReadVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
  void FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) override;
  void UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height) override;

  template<HostDisplayPixelFormat display_format>
  void CopyOut15Bit(uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height, uint32_t field, bool interlaced, bool interleaved);
  void CopyOut15Bit(HostDisplayPixelFormat display_format, uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height, uint32_t field,
                    bool interlaced, bool interleaved);

  template<HostDisplayPixelFormat display_format>
  void CopyOut24Bit(uint32_t src_x, uint32_t src_y, uint32_t skip_x, uint32_t width, uint32_t height, uint32_t field, bool interlaced,
                    bool interleaved);
  void CopyOut24Bit(HostDisplayPixelFormat display_format, uint32_t src_x, uint32_t src_y, uint32_t skip_x, uint32_t width, uint32_t height,
                    uint32_t field, bool interlaced, bool interleaved);

  void ClearDisplay() override;
  void UpdateDisplay() override;

  void DispatchRenderCommand() override;

  void FillBackendCommandParameters(GPUBackendCommand* cmd) const;
  void FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const;

  HeapArray<uint8_t, GPU_MAX_DISPLAY_WIDTH * GPU_MAX_DISPLAY_HEIGHT * sizeof(uint32_t)> m_display_texture_buffer;
  HostDisplayPixelFormat m_16bit_display_format = HostDisplayPixelFormat::RGB565;
  HostDisplayPixelFormat m_24bit_display_format = HostDisplayPixelFormat::RGBA8;

  GPU_SW_Backend m_backend;

#ifdef __XSDK__
  // What a vblank shows, as UpdateDisplay found it: VRAM's displayed rows, which the GPU thread copies in its order,
  // and the CRTC's state then. UpdateDisplay converts the last vblank's while the GPU thread copies this one's.
  struct DisplaySnapshot
  {
    HeapArray<uint16_t, VRAM_WIDTH * VRAM_HEIGHT> vram;
    retro_atomic_int_t done;
    // 0 until queued.
    int sequence = 0;
    bool disabled;
    bool interlaced;
    bool color_depth_24;
    bool interleaved;
    uint32_t field;
    uint32_t regs_x;
    uint32_t vram_left;
    uint32_t vram_top;
    uint32_t vram_width;
    uint32_t vram_height;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t origin_left;
    uint32_t origin_top;
    float aspect_ratio;
  };
  std::array<DisplaySnapshot, 2> m_display_snapshots;
  uint32_t m_display_snapshot_index = 0;
  int m_display_snapshot_sequence = 0;
#endif
};
