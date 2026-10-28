#include "renderer.h"
#include "output_window.h"
#include "d3d11_vs.h"
#include "d3d11_ps.h"
#include "diag.h"
#include <stdint.h>
#include <string.h>

Renderer::Renderer(OutputWindow &host) noexcept : host_(host), scene_(host.queue)
{
}

Renderer::~Renderer() noexcept
{
    release_gpu();
}

void Renderer::release_gpu(void)
{
    if (host_.smooth_) {
        scene_.release();
    }
    if (gpu_.context) {
        gpu_.context->ClearState();
    }
    gpu_.sampler.Reset();
    gpu_.ps.Reset();
    gpu_.vs.Reset();
    gpu_.view.Reset();
    gpu_.texture.Reset();
    gpu_.target.Reset();
    gpu_.swap.Reset();
    gpu_.context.Reset();
    gpu_.device.Reset();
}

HRESULT Renderer::create_pipeline(void)
{
    HRESULT hr = gpu_.device->CreateVertexShader(tow_d3d11_vs, sizeof(tow_d3d11_vs), NULL, &gpu_.vs);
    if (FAILED(hr)) {
        return hr;
    }
    hr = gpu_.device->CreatePixelShader(tow_d3d11_ps, sizeof(tow_d3d11_ps), NULL, &gpu_.ps);
    if (FAILED(hr)) {
        return hr;
    }
    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = FRAME_WIDTH;
    texture.Height = FRAME_HEIGHT;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = gpu_.device->CreateTexture2D(&texture, NULL, &gpu_.texture);
    if (FAILED(hr)) {
        return hr;
    }
    hr = gpu_.device->CreateShaderResourceView(gpu_.texture.Get(), NULL, &gpu_.view);
    if (FAILED(hr)) {
        return hr;
    }
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    return gpu_.device->CreateSamplerState(&sampler, &gpu_.sampler);
}

int Renderer::supports_flip_discard(void)
{
    /* RtlGetVersion is not affected by the executable's compatibility manifest. */
    typedef LONG(WINAPI * version_fn)(OSVERSIONINFOW *);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    version_fn version = module ? (version_fn)(uintptr_t)GetProcAddress(module, "RtlGetVersion") : NULL;
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    return version && version(&info) == 0 && info.dwMajorVersion >= 10;
}

const char *Renderer::swap_effect_name(DXGI_SWAP_EFFECT effect)
{
    return effect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip_discard" : "discard";
}

HRESULT Renderer::create_swap(D3D_DRIVER_TYPE driver, D3D_FEATURE_LEVEL *level)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = FRAME_WIDTH;
    desc.BufferDesc.Height = FRAME_HEIGHT;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = host_.child_;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr;
    if (supports_flip_discard()) {
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr =
            D3D11CreateDeviceAndSwapChain(NULL, driver, NULL, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                          &desc, &gpu_.swap, &gpu_.device, level, &gpu_.context);
        if (SUCCEEDED(hr)) {
            gpu_.swap_effect = desc.SwapEffect;
            return hr;
        }
        diag_log("d3d11: flip_discard unavailable driver=%u hr=0x%08lx; retry discard", (unsigned)driver,
                 (DWORD)hr);
        release_gpu();
        desc.BufferCount = 1;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    }
    hr = D3D11CreateDeviceAndSwapChain(NULL, driver, NULL, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       &desc, &gpu_.swap, &gpu_.device, level, &gpu_.context);
    gpu_.swap_effect = desc.SwapEffect;
    return hr;
}

HRESULT Renderer::create_gpu(void)
{
    D3D_FEATURE_LEVEL level;
    HRESULT hr = create_swap(D3D_DRIVER_TYPE_HARDWARE, &level);
    if (FAILED(hr) && host_.warp_) {
        release_gpu();
        hr = create_swap(D3D_DRIVER_TYPE_WARP, &level);
    }
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc = {};
    /* Local IIDs avoid an additional uuid library dependency. */
    static const GUID dxgi_device_id = {
        0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
    if (SUCCEEDED(
            gpu_.device->QueryInterface(dxgi_device_id, reinterpret_cast<void **>(dxgi.GetAddressOf()))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        adapter->GetDesc(&adapter_desc);
    }
    char name[256] = "unknown";
    WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, name, sizeof(name), NULL, NULL);
    swap_effect_ = gpu_.swap_effect;
    diag_log("d3d11: init adapter=%s feature_level=0x%x swap_effect=%s", name, (unsigned)level,
             swap_effect_name(gpu_.swap_effect));
    adapter.Reset();
    dxgi.Reset();
    hr = create_pipeline();
    if (SUCCEEDED(hr) && host_.smooth_) {
        scene_.init(gpu_.device.Get());
    }
    return hr;
}

HRESULT Renderer::resize_gpu(UINT width, UINT height)
{
    if (gpu_.width == width && gpu_.height == height) {
        return S_OK;
    }
    gpu_.context->OMSetRenderTargets(0, NULL, NULL);
    gpu_.target.Reset();
    /* Preserve the selected model's buffer count (two for flip, one for discard). */
    HRESULT hr = gpu_.swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        return hr;
    }
    static const GUID texture_id = {
        0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
    ComPtr<ID3D11Texture2D> backbuffer;
    hr = gpu_.swap->GetBuffer(0, texture_id, reinterpret_cast<void **>(backbuffer.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        hr = gpu_.device->CreateRenderTargetView(backbuffer.Get(), NULL, &gpu_.target);
    }
    backbuffer.Reset();
    if (SUCCEEDED(hr)) {
        gpu_.width = width;
        gpu_.height = height;
    }
    return hr;
}

void Renderer::upload_snapshot(void)
{
    if (snapshot_dirty_) {
        DWORD *out = (DWORD *)snapshot_;
        for (unsigned i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; ++i) {
            unsigned pixel = host_.queue.snapshot565_[i];
            unsigned r = (pixel >> 11) & 31;
            unsigned g = (pixel >> 5) & 63;
            unsigned b = pixel & 31;
            out[i] = 0xff000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | (b << 3) |
                     (b >> 2);
        }
        /* Unbind before writing the fallback texture. */
        ComPtr<ID3D11ShaderResourceView> empty;
        gpu_.context->PSSetShaderResources(0, 1, empty.GetAddressOf());
        gpu_.context->UpdateSubresource(gpu_.texture.Get(), 0, NULL, snapshot_, FRAME_WIDTH * 4, 0);
        ++uploads_;
        upload_bytes_ += (unsigned long long)FRAME_BYTES;
        snapshot_dirty_ = 0;
    }
}

HRESULT Renderer::present_frame(LONG epoch, LONG snapshot_epoch, int *composed)
{
    RECT rect;
    if (!GetClientRect(host_.child_, &rect) || rect.right <= 0 || rect.bottom <= 0) {
        return DXGI_STATUS_OCCLUDED;
    }
    HRESULT hr = resize_gpu((UINT)rect.right, (UINT)rect.bottom);
    if (FAILED(hr)) {
        return hr;
    }
    gpu_.context->OMSetRenderTargets(1, gpu_.target.GetAddressOf(), NULL);
    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)gpu_.width;
    viewport.Height = (FLOAT)gpu_.height;
    viewport.MaxDepth = 1;
    gpu_.context->RSSetViewports(1, &viewport);
    gpu_.context->IASetInputLayout(NULL);
    gpu_.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    gpu_.context->VSSetShader(gpu_.vs.Get(), NULL, 0);
    gpu_.context->PSSetShader(gpu_.ps.Get(), NULL, 0);
    gpu_.context->PSSetShaderResources(0, 1, gpu_.view.GetAddressOf());
    gpu_.context->PSSetSamplers(0, 1, gpu_.sampler.GetAddressOf());
    *composed = host_.smooth_ && scene_.draw(gpu_.context.Get(), gpu_.width, gpu_.height, epoch);
    if (!*composed) {
        /* S_FALSE cancels a frame whose matching fallback was not captured. */
        if (snapshot_epoch != epoch) {
            return S_FALSE;
        }
        upload_snapshot();
        gpu_.context->PSSetShaderResources(0, 1, gpu_.view.GetAddressOf());
        gpu_.context->Draw(4, 0);
        ++draws_;
        scene_.plain_frame();
    }
    ++rendered_;
    back_width_ = gpu_.width;
    back_height_ = gpu_.height;
    if (host_.smooth_) {
        scene_.frame_end();
    }
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    QueryPerformanceCounter(&start);
    hr = gpu_.swap->Present(1, 0);
    QueryPerformanceCounter(&end);
    if (diag_enabled(DIAG_TRANSITIONS)) {
        diag_transition("present", *composed ? &scene_.diagnostic() : &host_.queue.diag_snapshot_, *composed,
                        hr);
    }
    double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / qpc_frequency_;
    ++present_calls_;
    present_ms_ += ms;
    present_max_ = max(present_max_, ms);
    return hr;
}

void Renderer::log_counters(DWORD *last)
{
    DWORD now = GetTickCount();
    if ((DWORD)(now - *last) < 60000) {
        return;
    }
    double seconds = (now - *last) / 1000.0;
    *last = now;
    unsigned scene_draws = 0;
    unsigned scene_uploads = 0;
    unsigned long long scene_bytes = 0;
    scene_.counters(&scene_draws, &scene_uploads, &scene_bytes);
    unsigned draws = draws_ + scene_draws;
    unsigned uploads = uploads_ + scene_uploads;
    unsigned long long bytes = upload_bytes_ + scene_bytes;
    diag_log("d3d11: presents_s=%.2f calls=%u tests=%u draws_frame=%.2f uploads_s=%.2f "
             "upload_bytes_s=%.0f plain_uploads_interval=%u backbuffer=%ux%u swap_effect=%s sync_interval=1 "
             "present_ms_mean=%.3f present_ms_max=%.3f",
             present_calls_ / seconds, present_calls_, present_tests_,
             rendered_ ? (double)(draws - last_draws_) / rendered_ : 0, (uploads - last_uploads_) / seconds,
             (double)(bytes - last_bytes_) / seconds, uploads_ - last_plain_uploads_, back_width_,
             back_height_, swap_effect_name(swap_effect_), present_calls_ ? present_ms_ / present_calls_ : 0,
             present_max_);
    last_draws_ = draws;
    last_uploads_ = uploads;
    last_plain_uploads_ = uploads_;
    last_bytes_ = bytes;
    present_calls_ = 0;
    present_tests_ = 0;
    rendered_ = 0;
    present_ms_ = 0;
    present_max_ = 0;
    diag_log("d3d11: presented=%ld received=%ld dropped=%ld fallback=%ld",
             FrameQueue::atomic_read(&host_.presented_), FrameQueue::atomic_read(&host_.queue.received_),
             FrameQueue::atomic_read(&host_.dropped_), FrameQueue::atomic_read(&host_.fallback_));
    LONG received = FrameQueue::atomic_read(&host_.queue.received_);
    LONG dropped = FrameQueue::atomic_read(&host_.dropped_);
    LONG stale = FrameQueue::atomic_read(&host_.stale_);
    diag_log("d3d11: received_interval=%ld replaced_interval=%ld stale_epoch_interval=%ld "
             "gdi_refresh_interval=%ld",
             received - last_received_, dropped - last_dropped_ - (stale - last_stale_), stale - last_stale_,
             InterlockedExchange(&host_.gdi_refresh_count_, 0));
    last_received_ = received;
    last_dropped_ = dropped;
    last_stale_ = stale;
    if (host_.smooth_) {
        scene_capture_log();
        scene_.log();
    }
}

void Renderer::record_present(FrameState &frame, LONG epoch)
{
    if (host_.diagnostics_ && frame.hr != frame.last_status) {
        diag_log("d3d11: Present status=0x%08lx", (DWORD)frame.hr);
        frame.last_status = frame.hr;
    }
    if (frame.hr == S_OK || frame.hr == DXGI_STATUS_OCCLUDED) {
        /* A hidden HWND may be occluded even after the backbuffer is drawn.
         * Ask UI to show it, then present that snapshot again. */
        InterlockedExchange(&host_.drawn_epoch_, epoch);
    }
    if (frame.hr == S_OK) {
        InterlockedIncrement(&host_.presented_);
        InterlockedExchange(&host_.ready_epoch_, epoch);
    } else if (SUCCEEDED(frame.hr)) {
        frame.occluded = 1;
    }
}

void Renderer::render_next(FrameState &frame)
{
    if (host_.smooth_) {
        scene_.frame_begin();
        frame.scene_epoch = FrameQueue::atomic_read(&host_.queue.epoch_);
        scene_.update(gpu_.device.Get(), gpu_.context.Get(), frame.scene_epoch);
    }
    /* Consume the latest plain frame after scene work. Intermediate snapshots
     * stay RGB565 in CPU memory; conversion/upload waits for fallback drawing. */
    FrameQueue::Snapshot snapshot = host_.queue.consume(frame.snapshot_serial, frame.snapshot_epoch,
                                                        frame.scene_epoch, host_.smooth_ && scene_.active());
    LONG serial = snapshot.serial;
    LONG epoch = snapshot.epoch;
    int scene_ready = snapshot.scene_ready;
    int snapshot_copied = snapshot.copied;
    if (snapshot_copied) {
        snapshot_dirty_ = 1;
    }
    if (serial == frame.previous && !InterlockedExchange(&host_.refresh_, 0) && !snapshot_copied &&
        !(frame.composed && !scene_ready) &&
        !(scene_ready && epoch == FrameQueue::atomic_read(&host_.queue.epoch_))) {
        return;
    }
    if (serial != frame.previous) {
        InterlockedExchangeAdd(&host_.dropped_, serial - frame.previous - 1);
    }
    frame.previous = serial;
    if (epoch != FrameQueue::atomic_read(&host_.queue.epoch_)) {
        InterlockedIncrement(&host_.dropped_);
        InterlockedIncrement(&host_.stale_);
        return;
    }
    frame.hr = present_frame(epoch, frame.snapshot_epoch, &frame.composed);
    if (frame.hr == S_FALSE) {
        return;
    }
    record_present(frame, epoch);
}

DWORD Renderer::run(void)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    qpc_frequency_ = (double)frequency.QuadPart;
    FrameState frame;
    frame.hr = create_gpu();
    DWORD last_log = GetTickCount();
    HANDLE events[] = {host_.stop_, host_.wake_};
    while (SUCCEEDED(frame.hr) &&
           WaitForMultipleObjects(
               2, events, FALSE,
               frame.occluded || (host_.smooth_ && scene_.active() &&
                                  frame.scene_epoch == FrameQueue::atomic_read(&host_.queue.epoch_) &&
                                  FrameQueue::atomic_read(&host_.queue.latest_epoch_) ==
                                      FrameQueue::atomic_read(&host_.queue.epoch_))
                   ? 0
                   : 1000) != WAIT_OBJECT_0) {
        log_counters(&last_log);
        if (frame.occluded) {
            /* Wake events must not turn occlusion checks into a busy loop. */
            if (WaitForSingleObject(host_.stop_, 100) == WAIT_OBJECT_0) {
                break;
            }
            ++present_tests_;
            frame.hr = gpu_.swap->Present(0, DXGI_PRESENT_TEST);
            if (frame.hr != S_OK) {
                continue;
            }
            frame.occluded = 0;
            InterlockedExchange(&host_.refresh_, 1);
        }
        render_next(frame);
    }
    if (FAILED(frame.hr)) {
        InterlockedExchange(&host_.failed_, 1);
        InterlockedIncrement(&host_.fallback_);
        diag_log("d3d11: fallback to StretchBlt hr=0x%08lx removed=0x%08lx", (DWORD)frame.hr,
                 gpu_.device ? (DWORD)gpu_.device->GetDeviceRemovedReason() : 0);
        PostMessageW(host_.child_, OUTPUT_FAILED, 0, 0);
    }
    return 0;
}
