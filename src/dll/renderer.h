#ifndef TOW_RENDERER_H
#define TOW_RENDERER_H
#include <dxgi.h>
#include "scene_gpu.h"
class OutputWindow;

struct OutputResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    UINT width;
    UINT height;
    DXGI_SWAP_EFFECT swap_effect;
};

/* One heap root per worker; constructed, released and deleted on that worker. */
class Renderer
{
  public:
    explicit Renderer(OutputWindow &host) noexcept;
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    ~Renderer() noexcept;
    DWORD run();

  private:
    struct FrameState {
        HRESULT hr = S_OK;
        LONG previous = 0;
        LONG snapshot_serial = 0;
        LONG snapshot_epoch = 0;
        LONG scene_epoch = 0;
        int composed = 0;
        HRESULT last_status = S_OK;
        int occluded = 0;
    };

    void render_next(FrameState &frame);
    void record_present(FrameState &frame, LONG epoch);
    void release_gpu(void);
    HRESULT create_pipeline(void);
    static int supports_flip_discard(void);
    static const char *swap_effect_name(DXGI_SWAP_EFFECT effect);
    HRESULT create_swap(D3D_DRIVER_TYPE driver, D3D_FEATURE_LEVEL *level);
    HRESULT create_gpu(void);
    HRESULT resize_gpu(UINT width, UINT height);
    void upload_snapshot(void);
    HRESULT present_frame(LONG epoch, LONG snapshot_epoch, int *composed);
    void log_counters(DWORD *last);
    OutputWindow &host_;
    SceneCompositor scene_;
    OutputResources gpu_ = {};
    BYTE snapshot_[FRAME_BYTES] = {};
    int snapshot_dirty_ = {};
    unsigned draws_ = {};
    unsigned uploads_ = {};
    unsigned long long upload_bytes_ = {};
    unsigned present_calls_ = {};
    unsigned present_tests_ = {};
    unsigned rendered_ = {};
    double present_ms_ = {};
    double present_max_ = {};
    double qpc_frequency_ = {};
    UINT back_width_ = {};
    UINT back_height_ = {};
    DXGI_SWAP_EFFECT swap_effect_ = {};
    unsigned last_draws_ = {};
    unsigned last_uploads_ = {};
    unsigned last_plain_uploads_ = {};
    unsigned long long last_bytes_ = {};
    LONG last_received_ = {};
    LONG last_dropped_ = {};
    LONG last_stale_ = {};
};
#endif
