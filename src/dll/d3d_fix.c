#define DIRECTDRAW_VERSION 0x0500
#define DIRECT3D_VERSION   0x0500
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "d3d_fix.h"
#include "iat.h"
#include "log.h"

/* DirectX 5 interfaces and bounded scratch storage for D3DLVERTEX copies. */
#define DEVICE_TABLE_LIMIT 4
#define VERTEX_LIMIT       65536
#define REPORT_INTERVAL    60000
static const GUID D3D2_GUID = {0x6aae1ec1, 0x662a, 0x11d0, {0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a}};
static const GUID RAMP_GUID = {0xf2086b20, 0x259f, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
static const GUID RGB_GUID = {0xa4665c60, 0x2673, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
typedef char vertex_stride_check[sizeof(D3DLVERTEX) == 32 ? 1 : -1];
typedef char vertex_color_check[offsetof(D3DLVERTEX, color) == 16 ? 1 : -1];

typedef HRESULT(WINAPI *direct_draw_create_fn)(GUID *, LPDIRECTDRAW *, IUnknown *);
typedef HRESULT(STDMETHODCALLTYPE *create_device_fn)(IDirect3D2 *, REFCLSID, LPDIRECTDRAWSURFACE,
                                                     LPDIRECT3DDEVICE2 *);
typedef HRESULT(STDMETHODCALLTYPE *draw_primitive_fn)(IDirect3DDevice2 *, D3DPRIMITIVETYPE, D3DVERTEXTYPE,
                                                      LPVOID, DWORD, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *draw_indexed_fn)(IDirect3DDevice2 *, D3DPRIMITIVETYPE, D3DVERTEXTYPE,
                                                    LPVOID, DWORD, LPWORD, DWORD, DWORD);

typedef struct {
    IDirect3DDevice2Vtbl *table;
    void *draw;
    void *indexed;
    int reported;
} device_table;

static direct_draw_create_fn g_direct_draw_create;
static void *g_create_device;
static int g_rgb_device;
static int g_white_diffuse;
static int g_d3d_queried;
static int g_guid_logged;
static device_table g_tables[DEVICE_TABLE_LIMIT];
static unsigned g_table_count;
static D3DLVERTEX *g_vertices;
static SIZE_T g_capacity;
static DWORD g_last_report;
static unsigned long long g_draws;
static unsigned long long g_vertex_count;
static unsigned long long g_skipped;

static int vtable_replace(void **entry, void *replacement, void **original)
{
    if (*entry == replacement) {
        return 1;
    }
    void *previous = *entry;
    DWORD old;
    if (!previous || !VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &old)) {
        return 0;
    }
    *original = previous;
    *entry = replacement;
    DWORD ignored;
    if (!VirtualProtect(entry, sizeof(*entry), old, &ignored)) {
        *entry = previous;
        VirtualProtect(entry, sizeof(*entry), old, &ignored);
        return 0;
    }
    return 1;
}

static device_table *find_device_table(IDirect3DDevice2 *self)
{
    for (unsigned i = 0; i < g_table_count; ++i) {
        if (g_tables[i].table == self->lpVtbl) {
            return &g_tables[i];
        }
    }
    return NULL;
}

static void report_stats(void)
{
    DWORD now = GetTickCount();
    if ((DWORD)(now - g_last_report) < REPORT_INTERVAL) {
        return;
    }
    log_line("white_diffuse: draws %llu, vertices %llu, skipped %llu", g_draws, g_vertex_count, g_skipped);
    g_draws = 0;
    g_vertex_count = 0;
    g_skipped = 0;
    g_last_report = now;
}

/* Draw calls run on the game UI thread; the reusable buffer needs no locking. */
static LPVOID white_vertices(D3DVERTEXTYPE type, LPVOID vertices, DWORD count)
{
    ++g_draws;
    if (type != D3DVT_LVERTEX || !count) {
        return vertices;
    }
    if (count > VERTEX_LIMIT) {
        ++g_skipped;
        return vertices;
    }

    SIZE_T bytes = (SIZE_T)count * sizeof(D3DLVERTEX);
    if (bytes > g_capacity) {
        void *buffer = g_vertices ? HeapReAlloc(GetProcessHeap(), 0, g_vertices, bytes)
                                  : HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!buffer) {
            ++g_skipped;
            return vertices;
        }
        g_vertices = buffer;
        g_capacity = bytes;
    }
    memcpy(g_vertices, vertices, bytes);
    for (DWORD i = 0; i < count; ++i) {
        g_vertices[i].color = (g_vertices[i].color & 0xff000000UL) | 0x00ffffffUL;
    }
    g_vertex_count += count;
    return g_vertices;
}

static HRESULT STDMETHODCALLTYPE draw_primitive_hook(IDirect3DDevice2 *self, D3DPRIMITIVETYPE type,
                                                     D3DVERTEXTYPE vtype, LPVOID vertices, DWORD count,
                                                     DWORD flags)
{
    device_table *record = find_device_table(self);
    if (!record || !record->draw) {
        return E_UNEXPECTED;
    }
    LPVOID copy = white_vertices(vtype, vertices, count);
    HRESULT hr = ((draw_primitive_fn)record->draw)(self, type, vtype, copy, count, flags);
    report_stats();
    return hr;
}

static HRESULT STDMETHODCALLTYPE draw_indexed_primitive_hook(IDirect3DDevice2 *self, D3DPRIMITIVETYPE type,
                                                             D3DVERTEXTYPE vtype, LPVOID vertices,
                                                             DWORD count, LPWORD indices, DWORD index_count,
                                                             DWORD flags)
{
    device_table *record = find_device_table(self);
    if (!record || !record->indexed) {
        return E_UNEXPECTED;
    }
    LPVOID copy = white_vertices(vtype, vertices, count);
    HRESULT hr =
        ((draw_indexed_fn)record->indexed)(self, type, vtype, copy, count, indices, index_count, flags);
    report_stats();
    return hr;
}

static void patch_device(IDirect3DDevice2 *device)
{
    device_table *record = find_device_table(device);
    if (!record) {
        if (g_table_count == DEVICE_TABLE_LIMIT) {
            log_line("white_diffuse: failed: device vtable limit");
            return;
        }
        record = &g_tables[g_table_count++];
        record->table = device->lpVtbl;
    }
    IDirect3DDevice2Vtbl *table = record->table;
    if (!vtable_replace((void **)&table->DrawPrimitive, (void *)(uintptr_t)draw_primitive_hook,
                        &record->draw)) {
        log_line("white_diffuse: failed: DrawPrimitive vtable patch");
        return;
    }
    if (!vtable_replace((void **)&table->DrawIndexedPrimitive, (void *)(uintptr_t)draw_indexed_primitive_hook,
                        &record->indexed)) {
        void *ignored;
        if (!vtable_replace((void **)&table->DrawPrimitive, record->draw, &ignored)) {
            log_line("white_diffuse: failed: DrawPrimitive rollback");
        }
        log_line("white_diffuse: failed: DrawIndexedPrimitive vtable patch");
        return;
    }
    if (!record->reported) {
        log_line("white_diffuse: device vtable patched");
        record->reported = 1;
    }
}

static void log_requested_guid(REFCLSID guid)
{
    if (g_guid_logged || !guid) {
        return;
    }
    g_guid_logged = 1;
    log_line("d3d_fix: CreateDevice requested {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             guid->Data1, guid->Data2, guid->Data3, guid->Data4[0], guid->Data4[1], guid->Data4[2],
             guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

static HRESULT STDMETHODCALLTYPE create_device_hook(IDirect3D2 *self, REFCLSID guid,
                                                    LPDIRECTDRAWSURFACE surface, LPDIRECT3DDEVICE2 *out)
{
    HRESULT hr;
    if (g_rgb_device && guid && memcmp(guid, &RAMP_GUID, sizeof(GUID)) == 0) {
        hr = ((create_device_fn)g_create_device)(self, &RGB_GUID, surface, out);
        log_line("rgb_device: CreateDevice Ramp -> RGB, hr 0x%08lx", (unsigned long)hr);
    } else {
        log_requested_guid(guid);
        hr = ((create_device_fn)g_create_device)(self, guid, surface, out);
    }
    if (g_white_diffuse && SUCCEEDED(hr) && out && *out) {
        patch_device(*out);
    }
    return hr;
}

static HRESULT WINAPI direct_draw_create_hook(GUID *guid, LPDIRECTDRAW *out, IUnknown *outer)
{
    HRESULT hr = g_direct_draw_create(guid, out, outer);
    if (FAILED(hr) || !out || !*out || g_d3d_queried) {
        return hr;
    }
    g_d3d_queried = 1;
    IDirect3D2 *d3d = NULL;
    /* This single extra QI/Release pair is the only new call compared to the game. */
    HRESULT qi = (*out)->lpVtbl->QueryInterface(*out, &D3D2_GUID, (void **)&d3d);
    if (FAILED(qi) || !d3d) {
        log_line("d3d_fix: IDirect3D2 not available (hr 0x%08lx)", (unsigned long)qi);
        return hr;
    }
    IDirect3D2Vtbl *table = d3d->lpVtbl;
    d3d->lpVtbl->Release(d3d);
    if (!vtable_replace((void **)&table->CreateDevice, (void *)(uintptr_t)create_device_hook,
                        &g_create_device)) {
        log_line("d3d_fix: failed: CreateDevice vtable patch");
    }
    return hr;
}

int d3d_fix_apply(int rgb_device, int white_diffuse)
{
    if (!rgb_device && !white_diffuse) {
        return 1;
    }
    DWORD *slot = iat_find_slot("ddraw.dll", "DirectDrawCreate");
    HMODULE module = GetModuleHandleA("ddraw.dll");
    FARPROC original = module ? GetProcAddress(module, "DirectDrawCreate") : NULL;
    if (!slot || !original) {
        log_line("d3d_fix: failed: missing DirectDrawCreate import or export");
        return 0;
    }

    /* A Windows compatibility shim may already own the slot; wrap whatever the game would call. */
    void *current = (void *)(uintptr_t)*slot;
    if (current != (void *)(uintptr_t)original) {
        log_line("d3d_fix: DirectDrawCreate import already redirected (compatibility shim?), wrapping it");
    }
    g_direct_draw_create = (direct_draw_create_fn)(uintptr_t)current;
    g_rgb_device = rgb_device;
    g_white_diffuse = white_diffuse;
    g_last_report = GetTickCount();
    if (iat_replace(slot, current, (void *)(uintptr_t)direct_draw_create_hook) != IAT_OK) {
        log_line("d3d_fix: failed: DirectDrawCreate IAT patch");
        return 0;
    }
    log_line("d3d_fix: ok");
    return 1;
}
