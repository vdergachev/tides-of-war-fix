#ifndef TOW_SHIP_GPU_H
#define TOW_SHIP_GPU_H
#include "ship_live.h"
#include "texture2d.h"
#include <memory>

/* Identical input contract for live mailbox and read-only dump replay. */
struct HdGpuView {
    const hd_pass *passes;
    unsigned pass_count;
    const hd_draw *draws;
    unsigned draw_count;
    const hd_texture *textures;
    unsigned texture_count;
    const BYTE *geometry;
    unsigned geometry_bytes;
    const BYTE *pixels;
    unsigned pixel_bytes;
    const unsigned *texture_offsets;
};

class ShipGpu
{
  public:
    ShipGpu() noexcept;
    ~ShipGpu() noexcept;
    bool initialize(ID3D11Device *device, ID3D11DeviceContext *context);
    bool render(const HdGpuView &view, unsigned pass, unsigned side, Texture2D &out);
    unsigned uploads() const;
    /* Reset per-record hashes before rendering any pass of a new immutable payload. */
    void begin_record();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
