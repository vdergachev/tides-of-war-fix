#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <string.h>
/* Windows SDK C interfaces use anonymous unions. Keep this suppression local. */
#pragma warning(push)
#pragma warning(disable : 4201)
#include <d3d11.h>
#include <dxgi.h>
#pragma warning(pop)
#include "d3d11_out.h"
#include "d3d11_vs.h"
#include "d3d11_ps.h"
#include "log.h"
#include "scene.h"
#include "scene_gpu.h"

/* Supported image, Draw_CDibLayout and CGameMenuWnd (IDA 0x42a37e/0x433340). */
#define FRAME_DIB_ADDRESS   0x4cde34
#define DRAW_STATE_ADDRESS  0x4cde48
#define MENU_WINDOW_ADDRESS 0x4ce064
#define MENU_DIB_OFFSET     0x48
#define DIB_WIDTH_OFFSET    8
#define DIB_HEIGHT_OFFSET   0x0c
#define DIB_RAW_OFFSET      0x1c
#define DIB_HDC_OFFSET      0x24
#define DIB_BPP_OFFSET      0x42
#define DIB_ROW_OFFSET      0x60
#define DIB_STRIDE_OFFSET   0x64
#define FRAME_WIDTH         640
#define FRAME_HEIGHT        480
#define FRAME_BYTES         (FRAME_WIDTH * FRAME_HEIGHT * 4)
#define OUTPUT_CLASS        L"TowFixD3D11Output"
#define OUTPUT_TIMER        1
#define OUTPUT_FAILED       (WM_APP + 0x311)

typedef struct {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *target;
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *view;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11SamplerState *sampler;
    UINT width;
    UINT height;
    DXGI_SWAP_EFFECT swap_effect;
} output_gpu;

/* UI owns HWND/lifetime; worker owns every COM object. No game pointers cross threads.
 * RGB565 capture/publication buffers and a worker snapshot; BGRA only for fallback.
 * Epochs prevent a frame from before a hide/transition becoming visible afterwards. */
static SRWLOCK g_lock = SRWLOCK_INIT;
static BYTE g_buffers[2][FRAME_WIDTH * FRAME_HEIGHT * 2];
static BYTE *g_latest = g_buffers[0];
static BYTE *g_capture = g_buffers[1];
static WORD g_snapshot565[FRAME_WIDTH * FRAME_HEIGHT];
static BYTE g_snapshot[FRAME_BYTES];
static int g_snapshot_dirty;
static unsigned g_draws;
static unsigned g_uploads;
static unsigned long long g_upload_bytes;
static unsigned g_present_calls;
static unsigned g_present_tests;
static unsigned g_rendered;
static double g_present_ms;
static double g_present_max;
static double g_qpc_frequency;
static UINT g_back_width;
static UINT g_back_height;
static DXGI_SWAP_EFFECT g_swap_effect;
static HWND g_child;
static HWND g_root;
static HANDLE g_thread;
static HANDLE g_wake;
static HANDLE g_stop;
static HINSTANCE g_instance;
static int g_enabled;
static int g_warp;
static int g_diagnostics;
static int g_smooth;
static int g_factor;
static int g_wanted;
static int g_stopping;
static LONG g_epoch = 1;
static LONG g_ready_epoch;
static LONG g_drawn_epoch;
static LONG g_refresh;
static LONG g_failed;
static LONG g_received;
static LONG g_presented;
static LONG g_dropped;
static LONG g_stale;
static LONG g_fallback;
static LONG g_serial;
static LONG g_latest_epoch;

#define RELEASE_OBJECT(object)                                                                               \
    do {                                                                                                     \
        if (object) {                                                                                        \
            (object)->lpVtbl->Release(object);                                                               \
            (object) = NULL;                                                                                 \
        }                                                                                                    \
    } while (0)

static LONG atomic_read(LONG *value)
{
    return InterlockedCompareExchange(value, 0, 0);
}

static int screen_supported(void)
{
    int state = *(const int *)(uintptr_t)DRAW_STATE_ADDRESS;
    return (state >= 1 && state <= 4) || state == 8 || state == 9;
}

static BYTE *source_dib(HDC source)
{
    BYTE *dib = *(BYTE **)(uintptr_t)FRAME_DIB_ADDRESS;
    if (dib && *(HDC *)(dib + DIB_HDC_OFFSET) == source) {
        return dib;
    }
    BYTE *menu = *(BYTE **)(uintptr_t)MENU_WINDOW_ADDRESS;
    if (menu && *(int *)(uintptr_t)DRAW_STATE_ADDRESS == 1) {
        dib = menu + MENU_DIB_OFFSET;
        if (*(HDC *)(dib + DIB_HDC_OFFSET) == source) {
            return dib;
        }
    }
    return NULL;
}

static int copy_pixels(HDC source)
{
    int result = 0;
    __try {
        BYTE *dib = source_dib(source);
        if (!dib || *(int *)(dib + DIB_WIDTH_OFFSET) != FRAME_WIDTH ||
            *(int *)(dib + DIB_HEIGHT_OFFSET) != FRAME_HEIGHT || *(WORD *)(dib + DIB_BPP_OFFSET) != 16 ||
            !*(BYTE **)(dib + DIB_RAW_OFFSET)) {
            __leave;
        }
        const BYTE *row = *(const BYTE **)(dib + DIB_ROW_OFFSET);
        int stride = *(int *)(dib + DIB_STRIDE_OFFSET);
        if (!row || (stride != FRAME_WIDTH * 2 && stride != -FRAME_WIDTH * 2)) {
            __leave;
        }
        /* Complete GDI writes to this thread's DIB before reading its CPU pixels. */
        GdiFlush();
        for (int y = 0; y < FRAME_HEIGHT; ++y) {
            memcpy(g_capture + y * FRAME_WIDTH * 2, row + y * stride, FRAME_WIDTH * 2);
        }
        if (g_smooth) {
            scene_present(source, atomic_read(&g_epoch));
        }
        /* Only the producer exchanges these pointers; the worker reads under g_lock.
         * Identical menu/map paints need neither a new upload nor another Present.
         * Mission records must still advance even when their final pixels match. */
        int state = *(const int *)(uintptr_t)DRAW_STATE_ADDRESS;
        if (state != 2 && state != 3 && g_serial && g_latest_epoch == atomic_read(&g_epoch) &&
            memcmp(g_capture, g_latest, sizeof(g_snapshot565)) == 0) {
            InterlockedIncrement(&g_received);
            return 1;
        }
        /* Publish only a complete capture, including after a guarded read fails.
         * Pixel copying itself never holds the consumer's lock. */
        AcquireSRWLockExclusive(&g_lock);
        BYTE *previous = g_latest;
        g_latest = g_capture;
        g_capture = previous;
        g_latest_epoch = atomic_read(&g_epoch);
        ++g_serial;
        ReleaseSRWLockExclusive(&g_lock);
        InterlockedIncrement(&g_received);
        result = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
    }
    return result;
}

static void release_gpu(output_gpu *gpu)
{
    if (g_smooth) {
        scene_gpu_release();
    }
    if (gpu->context) {
        ID3D11DeviceContext_ClearState(gpu->context);
    }
    RELEASE_OBJECT(gpu->sampler);
    RELEASE_OBJECT(gpu->ps);
    RELEASE_OBJECT(gpu->vs);
    RELEASE_OBJECT(gpu->view);
    RELEASE_OBJECT(gpu->texture);
    RELEASE_OBJECT(gpu->target);
    RELEASE_OBJECT(gpu->swap);
    RELEASE_OBJECT(gpu->context);
    RELEASE_OBJECT(gpu->device);
}

static HRESULT create_pipeline(output_gpu *gpu)
{
    HRESULT hr =
        ID3D11Device_CreateVertexShader(gpu->device, tow_d3d11_vs, sizeof(tow_d3d11_vs), NULL, &gpu->vs);
    if (FAILED(hr)) {
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(gpu->device, tow_d3d11_ps, sizeof(tow_d3d11_ps), NULL, &gpu->ps);
    if (FAILED(hr)) {
        return hr;
    }
    D3D11_TEXTURE2D_DESC texture = {0};
    texture.Width = FRAME_WIDTH;
    texture.Height = FRAME_HEIGHT;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(gpu->device, &texture, NULL, &gpu->texture);
    if (FAILED(hr)) {
        return hr;
    }
    hr = ID3D11Device_CreateShaderResourceView(gpu->device, (ID3D11Resource *)gpu->texture, NULL, &gpu->view);
    if (FAILED(hr)) {
        return hr;
    }
    D3D11_SAMPLER_DESC sampler = {0};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    return ID3D11Device_CreateSamplerState(gpu->device, &sampler, &gpu->sampler);
}

static int supports_flip_discard(void)
{
    /* RtlGetVersion is not affected by the executable's compatibility manifest. */
    typedef LONG(WINAPI * version_fn)(OSVERSIONINFOW *);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    version_fn version = module ? (version_fn)(uintptr_t)GetProcAddress(module, "RtlGetVersion") : NULL;
    OSVERSIONINFOW info = {0};
    info.dwOSVersionInfoSize = sizeof(info);
    return version && version(&info) == 0 && info.dwMajorVersion >= 10;
}

static const char *swap_effect_name(DXGI_SWAP_EFFECT effect)
{
    return effect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip_discard" : "discard";
}

static HRESULT create_swap(output_gpu *gpu, D3D_DRIVER_TYPE driver, D3D_FEATURE_LEVEL *level)
{
    DXGI_SWAP_CHAIN_DESC desc = {0};
    desc.BufferDesc.Width = FRAME_WIDTH;
    desc.BufferDesc.Height = FRAME_HEIGHT;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = g_child;
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
                                          &desc, &gpu->swap, &gpu->device, level, &gpu->context);
        if (SUCCEEDED(hr)) {
            gpu->swap_effect = desc.SwapEffect;
            return hr;
        }
        log_line("d3d11: flip_discard unavailable driver=%u hr=0x%08lx; retry discard", (unsigned)driver,
                 (DWORD)hr);
        release_gpu(gpu);
        desc.BufferCount = 1;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    }
    hr = D3D11CreateDeviceAndSwapChain(NULL, driver, NULL, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       &desc, &gpu->swap, &gpu->device, level, &gpu->context);
    gpu->swap_effect = desc.SwapEffect;
    return hr;
}

static HRESULT create_gpu(output_gpu *gpu)
{
    D3D_FEATURE_LEVEL level;
    HRESULT hr = create_swap(gpu, D3D_DRIVER_TYPE_HARDWARE, &level);
    if (FAILED(hr) && g_warp) {
        release_gpu(gpu);
        hr = create_swap(gpu, D3D_DRIVER_TYPE_WARP, &level);
    }
    if (FAILED(hr)) {
        return hr;
    }
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC adapter_desc = {0};
    /* Local IIDs avoid an additional uuid library dependency. */
    static const GUID dxgi_device_id = {
        0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
    if (SUCCEEDED(ID3D11Device_QueryInterface(gpu->device, &dxgi_device_id, (void **)&dxgi)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter))) {
        IDXGIAdapter_GetDesc(adapter, &adapter_desc);
    }
    char name[256] = "unknown";
    WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, name, sizeof(name), NULL, NULL);
    g_swap_effect = gpu->swap_effect;
    log_line("d3d11: init adapter=%s feature_level=0x%x swap_effect=%s", name, (unsigned)level,
             swap_effect_name(gpu->swap_effect));
    RELEASE_OBJECT(adapter);
    RELEASE_OBJECT(dxgi);
    hr = create_pipeline(gpu);
    if (SUCCEEDED(hr) && g_smooth) {
        scene_gpu_init(gpu->device);
    }
    return hr;
}

static HRESULT resize_gpu(output_gpu *gpu, UINT width, UINT height)
{
    if (gpu->width == width && gpu->height == height) {
        return S_OK;
    }
    ID3D11DeviceContext_OMSetRenderTargets(gpu->context, 0, NULL, NULL);
    RELEASE_OBJECT(gpu->target);
    /* Preserve the selected model's buffer count (two for flip, one for discard). */
    HRESULT hr = IDXGISwapChain_ResizeBuffers(gpu->swap, 0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        return hr;
    }
    static const GUID texture_id = {
        0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
    ID3D11Texture2D *backbuffer = NULL;
    hr = IDXGISwapChain_GetBuffer(gpu->swap, 0, &texture_id, (void **)&backbuffer);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRenderTargetView(gpu->device, (ID3D11Resource *)backbuffer, NULL,
                                                 &gpu->target);
    }
    RELEASE_OBJECT(backbuffer);
    if (SUCCEEDED(hr)) {
        gpu->width = width;
        gpu->height = height;
    }
    return hr;
}

static void upload_snapshot(output_gpu *gpu)
{
    if (g_snapshot_dirty) {
        DWORD *out = (DWORD *)g_snapshot;
        for (unsigned i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; ++i) {
            unsigned pixel = g_snapshot565[i];
            unsigned r = (pixel >> 11) & 31;
            unsigned g = (pixel >> 5) & 63;
            unsigned b = pixel & 31;
            out[i] = 0xff000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | (b << 3) |
                     (b >> 2);
        }
        /* Unbind before writing the fallback texture. */
        ID3D11ShaderResourceView *empty = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu->context, 0, 1, &empty);
        ID3D11DeviceContext_UpdateSubresource(gpu->context, (ID3D11Resource *)gpu->texture, 0, NULL,
                                              g_snapshot, FRAME_WIDTH * 4, 0);
        ++g_uploads;
        g_upload_bytes += (unsigned long long)FRAME_BYTES;
        g_snapshot_dirty = 0;
    }
}

static HRESULT present_frame(output_gpu *gpu, LONG epoch, LONG snapshot_epoch, int *composed)
{
    RECT rect;
    if (!GetClientRect(g_child, &rect) || rect.right <= 0 || rect.bottom <= 0) {
        return DXGI_STATUS_OCCLUDED;
    }
    HRESULT hr = resize_gpu(gpu, (UINT)rect.right, (UINT)rect.bottom);
    if (FAILED(hr)) {
        return hr;
    }
    ID3D11DeviceContext_OMSetRenderTargets(gpu->context, 1, &gpu->target, NULL);
    D3D11_VIEWPORT viewport = {0};
    viewport.Width = (FLOAT)gpu->width;
    viewport.Height = (FLOAT)gpu->height;
    viewport.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(gpu->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(gpu->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(gpu->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(gpu->context, gpu->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(gpu->context, gpu->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(gpu->context, 0, 1, &gpu->view);
    ID3D11DeviceContext_PSSetSamplers(gpu->context, 0, 1, &gpu->sampler);
    *composed = g_smooth && scene_gpu_draw(gpu->context, gpu->width, gpu->height, epoch);
    if (!*composed) {
        /* S_FALSE cancels a frame whose matching fallback was not captured. */
        if (snapshot_epoch != epoch) {
            return S_FALSE;
        }
        upload_snapshot(gpu);
        ID3D11DeviceContext_PSSetShaderResources(gpu->context, 0, 1, &gpu->view);
        ID3D11DeviceContext_Draw(gpu->context, 4, 0);
        ++g_draws;
    }
    ++g_rendered;
    g_back_width = gpu->width;
    g_back_height = gpu->height;
    if (g_smooth) {
        scene_gpu_frame_end();
    }
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    QueryPerformanceCounter(&start);
    hr = IDXGISwapChain_Present(gpu->swap, 1, 0);
    QueryPerformanceCounter(&end);
    double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / g_qpc_frequency;
    ++g_present_calls;
    g_present_ms += ms;
    g_present_max = max(g_present_max, ms);
    return hr;
}

static void log_counters(DWORD *last)
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
    scene_gpu_counters(&scene_draws, &scene_uploads, &scene_bytes);
    static unsigned last_draws;
    static unsigned last_uploads;
    static unsigned last_plain_uploads;
    static unsigned long long last_bytes;
    unsigned draws = g_draws + scene_draws;
    unsigned uploads = g_uploads + scene_uploads;
    unsigned long long bytes = g_upload_bytes + scene_bytes;
    log_line("d3d11: presents_s=%.2f calls=%u tests=%u draws_frame=%.2f uploads_s=%.2f "
             "upload_bytes_s=%.0f plain_uploads_interval=%u backbuffer=%ux%u swap_effect=%s sync_interval=1 "
             "present_ms_mean=%.3f present_ms_max=%.3f",
             g_present_calls / seconds, g_present_calls, g_present_tests,
             g_rendered ? (double)(draws - last_draws) / g_rendered : 0, (uploads - last_uploads) / seconds,
             (double)(bytes - last_bytes) / seconds, g_uploads - last_plain_uploads, g_back_width,
             g_back_height, swap_effect_name(g_swap_effect),
             g_present_calls ? g_present_ms / g_present_calls : 0, g_present_max);
    last_draws = draws;
    last_uploads = uploads;
    last_plain_uploads = g_uploads;
    last_bytes = bytes;
    g_present_calls = 0;
    g_present_tests = 0;
    g_rendered = 0;
    g_present_ms = 0;
    g_present_max = 0;
    log_line("d3d11: presented=%ld received=%ld dropped=%ld fallback=%ld", atomic_read(&g_presented),
             atomic_read(&g_received), atomic_read(&g_dropped), atomic_read(&g_fallback));
    static LONG last_received;
    static LONG last_dropped;
    static LONG last_stale;
    LONG received = atomic_read(&g_received);
    LONG dropped = atomic_read(&g_dropped);
    LONG stale = atomic_read(&g_stale);
    log_line("d3d11: received_interval=%ld replaced_interval=%ld stale_epoch_interval=%ld",
             received - last_received, dropped - last_dropped - (stale - last_stale), stale - last_stale);
    last_received = received;
    last_dropped = dropped;
    last_stale = stale;
    if (g_smooth) {
        scene_capture_log();
        scene_gpu_log();
    }
}

static DWORD WINAPI render_thread(void *unused)
{
    (void)unused;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_qpc_frequency = (double)frequency.QuadPart;
    output_gpu gpu = {0};
    HRESULT hr = create_gpu(&gpu);
    LONG previous = 0;
    LONG snapshot_serial = 0;
    LONG snapshot_epoch = 0;
    LONG scene_epoch = 0;
    int composed = 0;
    DWORD last_log = GetTickCount();
    HRESULT last_status = S_OK;
    int occluded = 0;
    HANDLE events[] = {g_stop, g_wake};
    while (SUCCEEDED(hr) &&
           WaitForMultipleObjects(2, events, FALSE,
                                  occluded || (g_smooth && scene_gpu_active() &&
                                               scene_epoch == atomic_read(&g_epoch) &&
                                               atomic_read(&g_latest_epoch) == atomic_read(&g_epoch))
                                      ? 0
                                      : 1000) != WAIT_OBJECT_0) {
        log_counters(&last_log);
        if (occluded) {
            /* Wake events must not turn occlusion checks into a busy loop. */
            if (WaitForSingleObject(g_stop, 100) == WAIT_OBJECT_0) {
                break;
            }
            ++g_present_tests;
            hr = IDXGISwapChain_Present(gpu.swap, 0, DXGI_PRESENT_TEST);
            if (hr != S_OK) {
                continue;
            }
            occluded = 0;
            InterlockedExchange(&g_refresh, 1);
        }

        if (g_smooth) {
            scene_gpu_frame_begin();
            scene_epoch = atomic_read(&g_epoch);
            scene_gpu_update(gpu.device, gpu.context, scene_epoch);
        }
        /* Consume the latest plain frame after scene work. Intermediate snapshots
         * stay RGB565 in the mailbox; the scene path never copies/converts them. */
        AcquireSRWLockShared(&g_lock);
        LONG serial = g_serial;
        LONG epoch = g_latest_epoch;
        /* The worker owns scene state; update's epoch stays valid until its next update. */
        int scene_ready = g_smooth && scene_epoch == epoch && scene_gpu_active();
        int snapshot_copied = 0;
        if (epoch == atomic_read(&g_epoch) && serial != snapshot_serial && !scene_ready) {
            memcpy(g_snapshot565, g_latest, sizeof(g_snapshot565));
            g_snapshot_dirty = 1;
            snapshot_serial = serial;
            snapshot_epoch = epoch;
            snapshot_copied = 1;
        }
        ReleaseSRWLockShared(&g_lock);
        if (serial == previous && !InterlockedExchange(&g_refresh, 0) && !snapshot_copied &&
            !(composed && !scene_ready) && !(scene_ready && epoch == atomic_read(&g_epoch))) {
            continue;
        }
        if (serial != previous) {
            InterlockedExchangeAdd(&g_dropped, serial - previous - 1);
        }
        previous = serial;
        if (epoch != atomic_read(&g_epoch)) {
            InterlockedIncrement(&g_dropped);
            InterlockedIncrement(&g_stale);
            continue;
        }
        hr = present_frame(&gpu, epoch, snapshot_epoch, &composed);
        if (hr == S_FALSE) {
            continue;
        }
        if (g_diagnostics && hr != last_status) {
            log_line("d3d11: Present status=0x%08lx", (DWORD)hr);
            last_status = hr;
        }
        if (hr == S_OK || hr == DXGI_STATUS_OCCLUDED) {
            /* A hidden HWND may be occluded even after the backbuffer is drawn.
             * Ask UI to show it, then present that snapshot again. */
            InterlockedExchange(&g_drawn_epoch, epoch);
        }
        if (hr == S_OK) {
            InterlockedIncrement(&g_presented);
            InterlockedExchange(&g_ready_epoch, epoch);
        } else if (SUCCEEDED(hr)) {
            occluded = 1;
        }
    }
    if (FAILED(hr)) {
        InterlockedExchange(&g_failed, 1);
        InterlockedIncrement(&g_fallback);
        log_line("d3d11: fallback to StretchBlt hr=0x%08lx removed=0x%08lx", (DWORD)hr,
                 gpu.device ? (DWORD)ID3D11Device_GetDeviceRemovedReason(gpu.device) : 0);
        PostMessageW(g_child, OUTPUT_FAILED, 0, 0);
    }
    release_gpu(&gpu);
    return 0;
}

void d3d11_out_hide(void)
{
    if (!g_child || !g_wanted) {
        return;
    }
    g_wanted = 0;
    if (g_diagnostics) {
        log_line("d3d11: hide epoch=%ld", atomic_read(&g_epoch));
    }
    InterlockedIncrement(&g_epoch);
    ShowWindow(g_child, SW_HIDE);
    InvalidateRect(g_root, NULL, FALSE);
}

static BOOL CALLBACK find_dialog(HWND hwnd, LPARAM parameter)
{
    WCHAR name[64];
    if (IsWindowVisible(hwnd) && GetClassNameW(hwnd, name, ARRAYSIZE(name)) && !wcscmp(name, L"#32770")) {
        *(int *)parameter = 1;
        return FALSE;
    }
    return TRUE;
}

static int blocked_by_dialog(void)
{
    int found = 0;
    EnumChildWindows(g_root, find_dialog, (LPARAM)&found);
    return found || !IsWindowEnabled(g_root);
}

static void update_window(void)
{
    if (!screen_supported() || atomic_read(&g_failed) || IsIconic(g_root) || blocked_by_dialog()) {
        d3d11_out_hide();
        return;
    }
    if (g_wanted && atomic_read(&g_drawn_epoch) == atomic_read(&g_epoch)) {
        int was_visible = IsWindowVisible(g_child);
        SetWindowPos(g_child, HWND_TOP, 0, 0, FRAME_WIDTH * g_factor, FRAME_HEIGHT * g_factor,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!was_visible) {
            InterlockedExchange(&g_refresh, 1);
            SetEvent(g_wake);
            if (g_diagnostics) {
                log_line("d3d11: show epoch=%ld factor=%d", atomic_read(&g_epoch), g_factor);
            }
        }
    }
}

static void stop_worker(void)
{
    if (g_stopping) {
        return;
    }
    g_stopping = 1;
    if (g_thread) {
        SetEvent(g_stop);
        /* DXGI may send synchronous window messages. Drain sent messages while joining,
         * without dispatching posted game work inside its destruction handler. */
        while (WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT) {
            MSG message;
            PeekMessageW(&message, NULL, WM_NULL, WM_NULL, PM_NOREMOVE);
            MsgWaitForMultipleObjects(1, &g_thread, FALSE, 10, QS_SENDMESSAGE);
        }
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = NULL;
    }
    if (g_wake) {
        CloseHandle(g_wake);
        g_wake = NULL;
    }
    g_stopping = 0;
}

static LRESULT CALLBACK output_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);

        InterlockedExchange(&g_refresh, 1);
        return 0;
    }
    if (message == WM_SIZE || message == WM_SHOWWINDOW) {
        InterlockedExchange(&g_refresh, 1);
    }
    if (message == WM_TIMER || message == OUTPUT_FAILED) {
        update_window();
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(hwnd, OUTPUT_TIMER);
        stop_worker();
        g_child = NULL;
        g_root = NULL;
        g_wanted = 0;
        /* A destroyed game window is not a request to restart the backend. */
        InterlockedExchange(&g_failed, 1);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

static int create_window(HWND root, int factor)
{
    if (GetWindowThreadProcessId(root, NULL) != GetCurrentThreadId()) {
        return 0;
    }
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = output_proc;
    wc.hInstance = g_instance;
    wc.lpszClassName = OUTPUT_CLASS;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }
    g_root = root;
    g_factor = factor;
    g_child = CreateWindowExW(WS_EX_NOACTIVATE, OUTPUT_CLASS, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                              FRAME_WIDTH * factor, FRAME_HEIGHT * factor, root, NULL, g_instance, NULL);
    if (!g_child) {
        return 0;
    }
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_stop || !g_wake || !SetTimer(g_child, OUTPUT_TIMER, 100, NULL)) {
        DestroyWindow(g_child);
        return 0;
    }
    g_thread = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
    if (!g_thread) {
        DestroyWindow(g_child);
        return 0;
    }
    return 1;
}

int d3d11_out_frame(HWND destination, HDC source, int factor)
{
    if (!g_enabled || atomic_read(&g_failed) || g_stopping) {
        d3d11_out_hide();
        return 0;
    }
    HWND root = GetAncestor(destination, GA_ROOT);
    int qualifies = 0;
    __try {
        qualifies = screen_supported() && source_dib(source) != NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        qualifies = 0;
    }
    if (!qualifies || !root || IsIconic(root)) {
        d3d11_out_hide();
        return 0;
    }
    if (!g_child && !create_window(root, factor)) {
        InterlockedExchange(&g_failed, 1);
        InterlockedIncrement(&g_fallback);
        log_line("d3d11: window/thread creation failed; using StretchBlt error=%lu", GetLastError());
        return 0;
    }
    if (root != g_root || blocked_by_dialog() || !copy_pixels(source)) {
        d3d11_out_hide();
        return 0;
    }
    g_wanted = 1;
    SetEvent(g_wake);
    update_window();
    /* Until the first successful Present, leave the existing GDI picture visible. */
    return IsWindowVisible(g_child) && atomic_read(&g_ready_epoch) == atomic_read(&g_epoch) &&
           !atomic_read(&g_failed);
}

void d3d11_out_window(HWND hwnd, UINT message)
{
    if (hwnd != g_root || !g_child) {
        return;
    }
    if (message == WM_DESTROY) {
        DestroyWindow(g_child);
    } else if (message == WM_SIZE || message == WM_WINDOWPOSCHANGED) {
        update_window();
    }
}

int d3d11_out_owns(HWND hwnd)
{
    return hwnd && hwnd == g_child;
}

int d3d11_out_enabled(void)
{
    return g_enabled;
}

void d3d11_out_configure(const char *dir)
{
    char path[MAX_PATH];
    if (strlen(dir) + sizeof("tow_fix.ini") > sizeof(path)) {
        return;
    }
    strcpy_s(path, sizeof(path), dir);
    strcat_s(path, sizeof(path), "tow_fix.ini");
    g_enabled = GetPrivateProfileIntA("d3d11", "enabled", 0, path) != 0;
    g_warp = GetPrivateProfileIntA("d3d11", "warp", 0, path) != 0;
    g_diagnostics = GetPrivateProfileIntA("d3d11", "log", 0, path) != 0;
    g_smooth = g_enabled && GetPrivateProfileIntA("d3d11", "smooth", 0, path) != 0;
    if (g_smooth) {
        g_smooth = scene_init(&g_lock);
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(uintptr_t)output_proc, &g_instance);
    log_line("[d3d11] enabled=%d warp=%d log=%d smooth=%d", g_enabled, g_warp, g_diagnostics, g_smooth);
}
