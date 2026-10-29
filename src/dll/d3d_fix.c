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
#include "ship_capture.h"

/* DirectX 5 interfaces and bounded scratch storage for D3DLVERTEX copies. */
#define DEVICE_TABLE_LIMIT 4
#define VERTEX_LIMIT       65536
#define REPORT_INTERVAL    60000
#define MATERIAL_LIMIT     512
static const GUID D3D2_GUID = {0x6aae1ec1, 0x662a, 0x11d0, {0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a}};
static const GUID RAMP_GUID = {0xf2086b20, 0x259f, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
static const GUID RGB_GUID = {0xa4665c60, 0x2673, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
typedef char vertex_stride_check[sizeof(D3DLVERTEX) == 32 ? 1 : -1];
typedef char vertex_color_check[offsetof(D3DLVERTEX, color) == 16 ? 1 : -1];

typedef HRESULT(WINAPI *direct_draw_create_fn)(GUID *, LPDIRECTDRAW *, IUnknown *);
typedef HRESULT(STDMETHODCALLTYPE *create_device_fn)(IDirect3D2 *, REFCLSID, LPDIRECTDRAWSURFACE,
                                                     LPDIRECT3DDEVICE2 *);
typedef HRESULT(STDMETHODCALLTYPE *create_material_fn)(IDirect3D2 *, LPDIRECT3DMATERIAL2 *, IUnknown *);
typedef HRESULT(STDMETHODCALLTYPE *set_material_fn)(IDirect3DMaterial2 *, LPD3DMATERIAL);
typedef HRESULT(STDMETHODCALLTYPE *material_handle_fn)(IDirect3DMaterial2 *, LPDIRECT3DDEVICE2,
                                                       LPD3DMATERIALHANDLE);
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

typedef struct {
    IDirect3DMaterial2Vtbl *table;
    void *set;
    void *handle;
} material_table;

typedef struct {
    IDirect3DMaterial2 *object;
    IDirect3DDevice2 *device;
    D3DMATERIALHANDLE handle;
    DWORD color;
    int valid;
} material_color;

static direct_draw_create_fn g_direct_draw_create;
static void *g_create_device;
static void *g_create_material;
static material_table g_material_tables[DEVICE_TABLE_LIMIT];
static unsigned g_material_table_count;
static material_color g_materials[MATERIAL_LIMIT];
static unsigned g_material_next;
static DWORD g_untextured_color;
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
static unsigned long long g_textured;
static unsigned long long g_untextured;
static unsigned long long g_unknown;
static unsigned long long g_untextured_vertices;
static unsigned long long g_source_game;
static unsigned long long g_source_material;
static unsigned long long g_source_default;

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

static material_table *find_material_table(IDirect3DMaterial2 *self)
{
    for (unsigned i = 0; i < g_material_table_count; ++i) {
        if (g_material_tables[i].table == self->lpVtbl) {
            return &g_material_tables[i];
        }
    }
    return NULL;
}

static DWORD color_component(float value)
{
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    return (DWORD)(value * 255.0f + 0.5f);
}

static DWORD material_diffuse(const D3DMATERIAL *material)
{
    return (color_component(material->diffuse.r) << 16) | (color_component(material->diffuse.g) << 8) |
           color_component(material->diffuse.b);
}

static HRESULT STDMETHODCALLTYPE set_material_hook(IDirect3DMaterial2 *self, LPD3DMATERIAL material)
{
    material_table *table = find_material_table(self);
    if (!table || !table->set) {
        return E_UNEXPECTED;
    }
    HRESULT hr = ((set_material_fn)table->set)(self, material);
    if (SUCCEEDED(hr) && material) {
        for (unsigned i = 0; i < MATERIAL_LIMIT; ++i) {
            if (g_materials[i].object == self) {
                g_materials[i].color = material_diffuse(material);
                g_materials[i].valid = 1;
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE material_handle_hook(IDirect3DMaterial2 *self, LPDIRECT3DDEVICE2 device,
                                                      LPD3DMATERIALHANDLE handle)
{
    material_table *table = find_material_table(self);
    if (!table || !table->handle) {
        return E_UNEXPECTED;
    }
    HRESULT hr = ((material_handle_fn)table->handle)(self, device, handle);
    if (FAILED(hr) || !handle) {
        return hr;
    }

    material_color *record = NULL;
    for (unsigned i = 0; i < MATERIAL_LIMIT; ++i) {
        if (g_materials[i].device == device && g_materials[i].handle == *handle) {
            record = &g_materials[i];
            break;
        }
    }
    if (!record) {
        record = &g_materials[g_material_next++ % MATERIAL_LIMIT];
    }
    D3DMATERIAL material;
    memset(&material, 0, sizeof(material));
    material.dwSize = sizeof(material);
    record->object = self;
    record->device = device;
    record->handle = *handle;
    record->valid = SUCCEEDED(self->lpVtbl->GetMaterial(self, &material));
    record->color = record->valid ? material_diffuse(&material) : 0;
    return hr;
}

static HRESULT STDMETHODCALLTYPE create_material_hook(IDirect3D2 *self, LPDIRECT3DMATERIAL2 *out,
                                                      IUnknown *outer)
{
    HRESULT hr = ((create_material_fn)g_create_material)(self, out, outer);
    if (FAILED(hr) || !out || !*out) {
        return hr;
    }
    /* COM addresses can be reused; cache entries never own or dereference objects. */
    for (unsigned i = 0; i < MATERIAL_LIMIT; ++i) {
        if (g_materials[i].object == *out) {
            memset(&g_materials[i], 0, sizeof(g_materials[i]));
        }
    }
    material_table *record = find_material_table(*out);
    if (!record) {
        if (g_material_table_count == DEVICE_TABLE_LIMIT) {
            log_line("white_diffuse: material vtable limit; default color fallback");
            return hr;
        }
        record = &g_material_tables[g_material_table_count++];
        record->table = (*out)->lpVtbl;
    }
    if (!vtable_replace((void **)&record->table->SetMaterial, (void *)(uintptr_t)set_material_hook,
                        &record->set) ||
        !vtable_replace((void **)&record->table->GetHandle, (void *)(uintptr_t)material_handle_hook,
                        &record->handle)) {
        log_line("white_diffuse: material vtable patch failed; default color fallback");
    }
    return hr;
}

static int current_material_color(IDirect3DDevice2 *device, DWORD *color)
{
    DWORD handle;
    if (FAILED(device->lpVtbl->GetLightState(device, D3DLIGHTSTATE_MATERIAL, &handle))) {
        return 0;
    }
    for (unsigned i = 0; i < MATERIAL_LIMIT; ++i) {
        material_color *record = &g_materials[i];
        if (record->device == device && record->handle == handle && record->valid) {
            *color = record->color;
            return 1;
        }
    }
    return 0;
}

static void report_stats(void)
{
    DWORD now = GetTickCount();
    if ((DWORD)(now - g_last_report) < REPORT_INTERVAL) {
        return;
    }
    log_line("white_diffuse: draws %llu, textured %llu, untextured %llu, unknown_texture %llu, "
             "vertices %llu, untextured_vertices %llu, source_game %llu, source_material %llu, "
             "source_default %llu, skipped %llu",
             g_draws, g_textured, g_untextured, g_unknown, g_vertex_count, g_untextured_vertices,
             g_source_game, g_source_material, g_source_default, g_skipped);
    g_draws = 0;
    g_vertex_count = 0;
    g_skipped = 0;
    g_textured = 0;
    g_untextured = 0;
    g_unknown = 0;
    g_untextured_vertices = 0;
    g_source_game = 0;
    g_source_material = 0;
    g_source_default = 0;
    g_last_report = now;
}

/* Draw calls run on the game UI thread; the reusable buffer needs no locking. */
static LPVOID white_vertices(IDirect3DDevice2 *device, D3DVERTEXTYPE type, LPVOID vertices, DWORD count)
{
    ++g_draws;
    /* Query the actual device state: no shared-vtable shadow state or failed-set drift.
     * Device2 has TEXTUREHANDLE, not the Device3 SetTexture method. */
    DWORD texture = 0;
    int known = SUCCEEDED(device->lpVtbl->GetRenderState(device, D3DRENDERSTATE_TEXTUREHANDLE, &texture));
    int textured = !known || texture != 0;
    if (!known) {
        ++g_unknown;
    } else if (textured) {
        ++g_textured;
    } else {
        ++g_untextured;
    }
    if (type != D3DVT_LVERTEX || !count || !vertices) {
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
    DWORD material_color_value = 0;
    int material_known = 0;
    int material_checked = 0;
    for (DWORD i = 0; i < count; ++i) {
        DWORD color = 0x00ffffffUL;
        if (!textured) {
            ++g_untextured_vertices;
            color = g_vertices[i].color & 0x00ffffffUL;
            if (color) {
                ++g_source_game;
            } else {
                if (!material_checked) {
                    material_known = current_material_color(device, &material_color_value);
                    material_checked = 1;
                }
                if (material_known) {
                    color = material_color_value;
                    ++g_source_material;
                } else {
                    color = g_untextured_color;
                    ++g_source_default;
                }
            }
        }
        g_vertices[i].color = (g_vertices[i].color & 0xff000000UL) | color;
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
    LPVOID copy = white_vertices(self, vtype, vertices, count);
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
    LPVOID copy = white_vertices(self, vtype, vertices, count);
    HRESULT hr =
        ((draw_indexed_fn)record->indexed)(self, type, vtype, copy, count, indices, index_count, flags);
    report_stats();
    return hr;
}

/* Selected only at installation when ship capture is armed; normal draw hooks stay unchanged. */
static HRESULT STDMETHODCALLTYPE capture_primitive_hook(IDirect3DDevice2 *self, D3DPRIMITIVETYPE type,
                                                        D3DVERTEXTYPE vtype, LPVOID vertices, DWORD count,
                                                        DWORD flags)
{
    device_table *record = find_device_table(self);
    if (!record || !record->draw) {
        return E_UNEXPECTED;
    }
    LPVOID copy = g_white_diffuse ? white_vertices(self, vtype, vertices, count) : vertices;
    HRESULT hr = ((draw_primitive_fn)record->draw)(self, type, vtype, copy, count, flags);
    ship_capture_draw(self, type, vtype, copy, count, NULL, 0, flags, hr);
    if (g_white_diffuse) {
        report_stats();
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE capture_indexed_hook(IDirect3DDevice2 *self, D3DPRIMITIVETYPE type,
                                                      D3DVERTEXTYPE vtype, LPVOID vertices, DWORD count,
                                                      LPWORD indices, DWORD index_count, DWORD flags)
{
    device_table *record = find_device_table(self);
    if (!record || !record->indexed) {
        return E_UNEXPECTED;
    }
    LPVOID copy = g_white_diffuse ? white_vertices(self, vtype, vertices, count) : vertices;
    HRESULT hr =
        ((draw_indexed_fn)record->indexed)(self, type, vtype, copy, count, indices, index_count, flags);
    ship_capture_draw(self, type, vtype, copy, count, indices, index_count, flags, hr);
    if (g_white_diffuse) {
        report_stats();
    }
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
    void *draw = ship_capture_enabled() ? (void *)(uintptr_t)capture_primitive_hook
                                        : (void *)(uintptr_t)draw_primitive_hook;
    void *indexed = ship_capture_enabled() ? (void *)(uintptr_t)capture_indexed_hook
                                           : (void *)(uintptr_t)draw_indexed_primitive_hook;
    if (!vtable_replace((void **)&table->DrawPrimitive, draw, &record->draw)) {
        log_line("white_diffuse: failed: DrawPrimitive vtable patch");
        return;
    }
    if (!vtable_replace((void **)&table->DrawIndexedPrimitive, indexed, &record->indexed)) {
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
    if ((g_white_diffuse || ship_capture_enabled()) && SUCCEEDED(hr) && out && *out) {
        for (unsigned i = 0; i < MATERIAL_LIMIT; ++i) {
            if (g_materials[i].device == *out) {
                memset(&g_materials[i], 0, sizeof(g_materials[i]));
            }
        }
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
    /* Obtain the creation interfaces before the game starts creating devices/materials. */
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
    if (g_white_diffuse && !vtable_replace((void **)&table->CreateMaterial,
                                           (void *)(uintptr_t)create_material_hook, &g_create_material)) {
        log_line("white_diffuse: CreateMaterial patch failed; default color fallback");
    }
    return hr;
}

int d3d_fix_apply(int rgb_device, int white_diffuse, DWORD untextured_color)
{
    if (!rgb_device && !white_diffuse && !ship_capture_enabled()) {
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
    g_untextured_color = untextured_color & 0x00ffffffUL;
    g_last_report = GetTickCount();
    if (iat_replace(slot, current, (void *)(uintptr_t)direct_draw_create_hook) != IAT_OK) {
        log_line("d3d_fix: failed: DirectDrawCreate IAT patch");
        return 0;
    }
    log_line("d3d_fix: ok");
    return 1;
}
