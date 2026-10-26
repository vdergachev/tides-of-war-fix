#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#pragma warning(push)
#pragma warning(disable : 4201)
#include <d3d11.h>
#pragma warning(pop)
#include "scene.h"
#include "scene_gpu.h"
#include "cloud_art.h"
#include <stdlib.h>
#include "scene_vs.h"
#include "scene_base_ps.h"
#define tow_d3d11_ps tow_scene_layer_ps
#include "d3d11_ps.h"
#undef tow_d3d11_ps
#include "log.h"

#define HULL_BASE       6
#define REGION_PRECLOUD 5
#define MAST_BASE       (HULL_BASE + SCENE_SHIPS)
#define HULL_RESIDUAL   (MAST_BASE + SCENE_SHIPS)
#define MAST_RESIDUAL   (HULL_RESIDUAL + 1)
#define REGION_COUNT    (MAST_RESIDUAL + 1)
#define REGION_BOTTOM   0
#define REGION_MID      1
#define REGION_TOP      2
#define REGION_FINAL    3
#define REGION_EDGE     4

typedef struct {
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *view;
    RECT box;
} scene_region;

typedef struct {
    unsigned step;
    unsigned count;
    unsigned follow;
    POINT camera;
    RECT view;
    double qpc;
    scene_ship ships[SCENE_SHIPS];
    unsigned cloud_flags;
    int split_clouds;
    scene_cloud clouds[SCENE_CLOUDS];
    scene_region regions[REGION_COUNT];
} scene_entry;

enum { SNAP_NONE, SNAP_PREV, SNAP_GAP, SNAP_FOLLOW, SNAP_CHANGE, SNAP_JUMP, SNAP_COUNT };

static const char *const SNAP_NAMES[] = {"none", "no_prev", "gap", "no_follow", "follow_changed", "jump"};
static scene_region g_cloud_art[CLOUD_IMAGES];
static unsigned g_art_loaded;
static unsigned g_cloud_drawn;
static unsigned g_cloud_interpolated;
static unsigned g_cloud_snapped;
static unsigned g_cloud_fallback;
static int g_split_clouds;
static scene_entry g_entries[SCENE_RING];
static unsigned g_count;
static scene_record g_worker_record;
static scene_record *g_input = &g_worker_record;
static ID3D11VertexShader *g_vs;
static ID3D11Buffer *g_placement;
static ID3D11Buffer *g_base_constants;
static ID3D11PixelShader *g_base_ps;
static ID3D11PixelShader *g_layer_ps;
static unsigned g_draws;
static unsigned g_uploads;
static unsigned long long g_upload_bytes;
static RECT g_bounds[REGION_COUNT];
static ID3D11BlendState *g_blend;
static ID3D11RasterizerState *g_raster;
static DWORD g_pixels[SCENE_PIXELS];
/* C is sampled only outside view. Reuse unchanged UI across scene records. */
static scene_region g_final;
static WORD g_final_pixels[SCENE_PIXELS];
static RECT g_final_view;
static unsigned g_sets;
static unsigned g_reused_final;
static signed char g_owner[2][SCENE_PIXELS];
static LONG g_epoch;
static int g_valid;
static double g_frequency;
static double g_time;
static double g_now;
static double g_interval;
static uintptr_t g_follow;
static double g_changed_pair;
static unsigned g_rejected;
static unsigned g_frames;
static unsigned g_snaps[SNAP_COUNT];
static unsigned g_backward;
static unsigned g_mask_pixels[4];
static double g_cost;
static double g_max_cost;
static double g_last_camera[2];
static double g_last_ship[SCENE_SHIPS][2];
static uintptr_t g_last_ptr[SCENE_SHIPS];
static int g_motion;
static double g_frame_start;
static double g_frame_cost;
static double g_frame_max;
static unsigned g_frame_count;

#define DROP(object)                                                                                         \
    do {                                                                                                     \
        if (object) {                                                                                        \
            (object)->lpVtbl->Release(object);                                                               \
            (object) = NULL;                                                                                 \
        }                                                                                                    \
    } while (0)

static double ticks(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart;
}

static double clamp(double value, double low, double high)
{
    return max(low, min(value, high));
}

static DWORD color(WORD value)
{
    unsigned r = (value >> 11) & 31;
    unsigned g = (value >> 5) & 63;
    unsigned b = value & 31;
    return 0xff000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | (b << 3) | (b >> 2);
}

static void release_entry(scene_entry *entry)
{
    for (unsigned i = 0; i < REGION_COUNT; ++i) {
        DROP(entry->regions[i].view);
        DROP(entry->regions[i].texture);
    }
}

static void reset_timeline(void)
{
    DROP(g_final.view);
    DROP(g_final.texture);
    for (unsigned i = 0; i < g_count; ++i) {
        release_entry(&g_entries[i]);
    }
    memset(g_entries, 0, sizeof(g_entries));
    g_count = 0;
    g_time = 0;
    g_now = 0;
    g_interval = 0;
    g_follow = 0;
    g_changed_pair = 0;
    g_motion = 0;
    g_valid = 0;
}

void scene_gpu_release(void)
{
    reset_timeline();
    for (unsigned i = 0; i < CLOUD_IMAGES; ++i) {
        DROP(g_cloud_art[i].view);
        DROP(g_cloud_art[i].texture);
    }
    g_art_loaded = 0;
    DROP(g_raster);
    DROP(g_blend);
    DROP(g_placement);
    DROP(g_vs);
    DROP(g_base_constants);
    DROP(g_base_ps);
    DROP(g_layer_ps);
}

static void load_cloud_art(ID3D11Device *device)
{
    cloud_art images[CLOUD_IMAGES] = {0};
    cloud_art_load(images);
    for (unsigned i = 0; i < CLOUD_IMAGES; ++i) {
        cloud_art *image = &images[i];
        if (!image->pixels) {
            continue;
        }
        D3D11_TEXTURE2D_DESC desc = {0};
        desc.Width = image->width;
        desc.Height = image->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = {0};
        data.pSysMem = image->pixels;
        data.SysMemPitch = image->width * 4;
        scene_region *out = &g_cloud_art[i];
        HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, &data, &out->texture);
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)out->texture, NULL,
                                                       &out->view);
        }
        if (SUCCEEDED(hr)) {
            SetRect(&out->box, 0, 0, (int)image->width, (int)image->height);
            ++g_art_loaded;
            ++g_uploads;
            g_upload_bytes += (unsigned long long)image->width * image->height * 4;
        } else {
            DROP(out->view);
            DROP(out->texture);
        }
        free(image->pixels);
    }
    log_line("scene: cloud art loaded=%u/36 alpha=brightness-white", g_art_loaded);
}

/* Whole-record fallback avoids drawing an art cloud over a baked cloud. */
static int cloud_art_ready(const scene_record *record)
{
    if (!(record->cloud_flags & 8)) {
        return 0;
    }
    unsigned layers = (1u << SCENE_D) | (1u << SCENE_E);
    if ((record->layers & layers) != layers) {
        return 0;
    }
    for (unsigned i = 0; i < SCENE_CLOUDS; ++i) {
        const scene_cloud *cloud = &record->clouds[i];
        if (!cloud->valid || cloud->image >= 18) {
            return 0;
        }
        const scene_region *art = &g_cloud_art[cloud->image + ((record->cloud_flags & 2) ? 18 : 0)];
        if (!art->view || art->box.right != cloud->width || art->box.bottom != cloud->height) {
            return 0;
        }
    }
    return 1;
}

int scene_gpu_init(ID3D11Device *device)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_frequency = (double)frequency.QuadPart;
    HRESULT hr = ID3D11Device_CreateVertexShader(device, tow_scene_vs, sizeof(tow_scene_vs), NULL, &g_vs);
    D3D11_BUFFER_DESC buffer = {0};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateBuffer(device, &buffer, NULL, &g_placement);
    }
    buffer.ByteWidth = 64;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateBuffer(device, &buffer, NULL, &g_base_constants);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(device, tow_scene_base_ps, sizeof(tow_scene_base_ps), NULL,
                                            &g_base_ps);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(device, tow_scene_layer_ps, sizeof(tow_scene_layer_ps), NULL,
                                            &g_layer_ps);
    }
    D3D11_BLEND_DESC blend = {0};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateBlendState(device, &blend, &g_blend);
    }
    D3D11_RASTERIZER_DESC raster = {D3D11_FILL_SOLID, D3D11_CULL_NONE};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRasterizerState(device, &raster, &g_raster);
    }
    if (FAILED(hr)) {
        scene_gpu_release();
        log_line("scene: GPU setup failed hr=0x%08lx; plain frames only", (DWORD)hr);
        return 0;
    }
    load_cloud_art(device);
    return 1;
}

static int owner_at(const scene_record *record, int x, int y)
{
    int owner = -1;
    double distance = 1e30;
    for (unsigned i = 0; i < record->count; ++i) {
        const scene_ship *ship = &record->ships[i];
        int cx = ship->point.x - record->camera.x + record->view.left;
        int cy = ship->point.y - record->camera.y + record->view.top;
        RECT box = ship->box;
        if (IsRectEmpty(&box)) {
            SetRect(&box, cx - 72, cy - 72, cx + 72, cy + 72);
        } else {
            box.left = min(cx - 72, box.left - 16);
            box.top = min(cy - 72, box.top - 16);
            box.right = max(cx + 72, box.right + 16);
            box.bottom = max(cy + 72, box.bottom + 16);
        }
        if (x < box.left || x >= box.right || y < box.top || y >= box.bottom) {
            continue;
        }
        double dx = (double)x - cx;
        double dy = (double)y - cy;
        double squared = dx * dx + dy * dy;
        if (squared < distance) {
            owner = (int)i;
            distance = squared;
        }
    }
    return owner;
}

static void assign_masks(const scene_record *record)
{
    memset(g_owner, -2, sizeof(g_owner));
    for (int y = record->view.top; y < record->view.bottom; ++y) {
        for (int x = record->view.left; x < record->view.right; ++x) {
            int p = y * SCENE_WIDTH + x;
            int hull = record->pixels[SCENE_A0][p] != record->pixels[SCENE_A1][p];
            int masts = record->pixels[SCENE_A][p] != record->pixels[SCENE_B][p];
            if (hull || masts) {
                int owner = owner_at(record, x, y);
                if (hull) {
                    g_owner[0][p] = (signed char)owner;
                    ++g_mask_pixels[0];
                }
                if (masts) {
                    g_owner[1][p] = (signed char)owner;
                    ++g_mask_pixels[2];
                }
            }
        }
    }
}

static int region_pixel(const scene_record *record, unsigned region, int p)
{
    if (region == REGION_BOTTOM || region == REGION_FINAL || region == REGION_EDGE) {
        return 1;
    }
    if (region == REGION_MID) {
        return record->pixels[SCENE_A][p] != record->pixels[SCENE_A1][p];
    }
    if (region == REGION_TOP) {
        return record->pixels[SCENE_C][p] != record->pixels[g_split_clouds ? SCENE_E : SCENE_B][p];
    }
    if (region == REGION_PRECLOUD) {
        return g_split_clouds && record->pixels[SCENE_D][p] != record->pixels[SCENE_B][p];
    }
    if (region == HULL_RESIDUAL || region == MAST_RESIDUAL) {
        return g_owner[region == MAST_RESIDUAL][p] == -1;
    }
    unsigned pass = region >= MAST_BASE;
    unsigned ship = region - (pass ? MAST_BASE : HULL_BASE);
    return g_owner[pass][p] == (int)ship;
}

static unsigned source_layer(unsigned region)
{
    if (region == REGION_BOTTOM) {
        return SCENE_A0;
    }
    if (region == REGION_FINAL || region == REGION_TOP) {
        return SCENE_C;
    }
    if (region == REGION_PRECLOUD) {
        return SCENE_D;
    }
    if (region == REGION_MID) {
        return SCENE_A;
    }
    return region < MAST_BASE || region == HULL_RESIDUAL ? SCENE_A1 : SCENE_B;
}

static void include_pixel(unsigned index, int x, int y)
{
    RECT *box = &g_bounds[index];
    box->left = min(box->left, x);
    box->top = min(box->top, y);
    box->right = max(box->right, x + 1);
    box->bottom = max(box->bottom, y + 1);
}

static void find_bounds(const scene_record *record)
{
    for (unsigned i = 0; i < REGION_COUNT; ++i) {
        SetRect(&g_bounds[i], SCENE_WIDTH, SCENE_HEIGHT, 0, 0);
    }
    SetRect(&g_bounds[REGION_BOTTOM], 0, 0, SCENE_WIDTH, SCENE_HEIGHT);
    g_bounds[REGION_FINAL] = g_bounds[REGION_BOTTOM];
    g_bounds[REGION_EDGE] = g_bounds[REGION_BOTTOM];
    for (int y = record->view.top; y < record->view.bottom; ++y) {
        for (int x = record->view.left; x < record->view.right; ++x) {
            int p = y * SCENE_WIDTH + x;
            if (region_pixel(record, REGION_MID, p)) {
                include_pixel(REGION_MID, x, y);
                ++g_mask_pixels[1];
            }
            if (region_pixel(record, REGION_PRECLOUD, p)) {
                include_pixel(REGION_PRECLOUD, x, y);
            }
            if (region_pixel(record, REGION_TOP, p)) {
                include_pixel(REGION_TOP, x, y);
                ++g_mask_pixels[3];
            }
            for (unsigned pass = 0; pass < 2; ++pass) {
                signed char owner = g_owner[pass][p];
                if (owner >= -1) {
                    unsigned index = owner < 0 ? (pass ? MAST_RESIDUAL : HULL_RESIDUAL)
                                               : (pass ? MAST_BASE : HULL_BASE) + (unsigned)owner;
                    include_pixel(index, x, y);
                }
            }
        }
    }
}

static int reuse_final(const scene_record *record, scene_region *out)
{
    if (!g_final.view || !EqualRect(&record->view, &g_final_view)) {
        return 0;
    }
    for (int y = 0; y < SCENE_HEIGHT; ++y) {
        const WORD *old = g_final_pixels + y * SCENE_WIDTH;
        const WORD *row = record->pixels[SCENE_C] + y * SCENE_WIDTH;
        int inside = y >= record->view.top && y < record->view.bottom;
        size_t left = inside ? (size_t)record->view.left : SCENE_WIDTH;
        size_t right = inside ? (size_t)record->view.right : SCENE_WIDTH;
        if (memcmp(old, row, left * sizeof(WORD)) != 0 ||
            memcmp(old + right, row + right, (SCENE_WIDTH - right) * sizeof(WORD)) != 0) {
            return 0;
        }
    }
    *out = g_final;
    ID3D11Texture2D_AddRef(out->texture);
    ID3D11ShaderResourceView_AddRef(out->view);
    ++g_reused_final;
    return 1;
}

static int build_region(ID3D11Device *device, scene_entry *entry, const scene_record *record, unsigned index)
{
    if (index >= HULL_BASE && index < HULL_RESIDUAL) {
        unsigned ship_index = (index - HULL_BASE) % SCENE_SHIPS;
        if (ship_index >= record->count) {
            return 1;
        }
    }
    /* Most records have no mid layer; share A0 instead of uploading it twice. */
    if (index == REGION_EDGE && IsRectEmpty(&g_bounds[REGION_MID])) {
        entry->regions[index] = entry->regions[REGION_BOTTOM];
        ID3D11Texture2D_AddRef(entry->regions[index].texture);
        ID3D11ShaderResourceView_AddRef(entry->regions[index].view);
        return 1;
    }
    RECT bounds = g_bounds[index];
    if (index == REGION_FINAL && reuse_final(record, &entry->regions[index])) {
        return 1;
    }
    unsigned layer = source_layer(index);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return 1;
    }
    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int p = (y + bounds.top) * SCENE_WIDTH + x + bounds.left;
            unsigned pixel_layer =
                index == REGION_EDGE
                    ? (record->pixels[SCENE_A][p] != record->pixels[SCENE_A1][p] ? SCENE_A : SCENE_A0)
                    : layer;
            g_pixels[y * width + x] =
                region_pixel(record, index, p) ? color(record->pixels[pixel_layer][p]) : 0;
        }
    }
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {0};
    data.pSysMem = g_pixels;
    data.SysMemPitch = (UINT)width * 4;
    scene_region *out = &entry->regions[index];
    out->box = bounds;
    ++g_uploads;
    g_upload_bytes += (unsigned long long)width * height * 4;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, &data, &out->texture);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)out->texture, NULL, &out->view);
    }
    if (SUCCEEDED(hr) && index == REGION_FINAL) {
        DROP(g_final.view);
        DROP(g_final.texture);
        g_final = *out;
        ID3D11Texture2D_AddRef(g_final.texture);
        ID3D11ShaderResourceView_AddRef(g_final.view);
        memcpy(g_final_pixels, record->pixels[SCENE_C], sizeof(g_final_pixels));
        g_final_view = record->view;
    }
    return SUCCEEDED(hr);
}

static void discard_first(void)
{
    release_entry(&g_entries[0]);
    --g_count;
    memmove(g_entries, g_entries + 1, g_count * sizeof(g_entries[0]));
    memset(&g_entries[g_count], 0, sizeof(g_entries[0]));
}

void scene_gpu_update(ID3D11Device *device, ID3D11DeviceContext *context, LONG epoch)
{
    (void)context;
    if (g_epoch != epoch) {
        reset_timeline();
        g_epoch = epoch;
    }
    while (scene_take(&g_input)) {
        if (g_input->epoch != epoch) {
            continue;
        }
        if (!g_input->valid) {
            reset_timeline();
            continue;
        }
        if (!g_vs) {
            continue;
        }
        if (g_count) {
            scene_entry *last = &g_entries[g_count - 1];
            if (g_input->step <= last->step || (double)g_input->qpc <= last->qpc) {
                ++g_rejected;
                continue;
            }
            double sample = ((double)g_input->qpc - last->qpc) / (g_input->step - last->step);
            g_interval = g_interval ? g_interval + .1 * (sample - g_interval) : sample;
        }
        if (g_count == SCENE_RING) {
            discard_first();
        }
        scene_entry *out = &g_entries[g_count];
        out->step = g_input->step;
        out->count = g_input->count;
        out->follow = g_input->follow;
        out->camera = g_input->camera;
        out->view = g_input->view;
        out->qpc = (double)g_input->qpc;
        memcpy(out->ships, g_input->ships, sizeof(out->ships));
        out->cloud_flags = g_input->cloud_flags;
        memcpy(out->clouds, g_input->clouds, sizeof(out->clouds));
        g_split_clouds = cloud_art_ready(g_input);
        out->split_clouds = g_split_clouds;
        assign_masks(g_input);
        find_bounds(g_input);
        int ok = 1;
        for (unsigned i = 0; i < REGION_COUNT && ok; ++i) {
            ok = build_region(device, out, g_input, i);
        }
        if (!ok) {
            release_entry(out);
            reset_timeline();
            log_line("scene: texture creation failed; plain frame");
            continue;
        }
        ++g_count;
        ++g_sets;
        g_valid = 1;
    }
}

static const scene_ship *find_ship(const scene_entry *entry, uintptr_t ptr)
{
    for (unsigned i = 0; i < entry->count; ++i) {
        if (entry->ships[i].ptr == ptr) {
            return &entry->ships[i];
        }
    }
    return NULL;
}

static void camera_fraction(const scene_entry *entry, uintptr_t follow, double *camera)
{
    camera[0] = entry->camera.x;
    camera[1] = entry->camera.y;
    const scene_ship *ship = find_ship(entry, follow);
    if (ship) {
        double fx = ship->x - ship->point.x;
        double fy = ship->y - ship->point.y;
        camera[0] += fabs(fx) <= .500001 ? fx : 0;
        camera[1] += fabs(fy) <= .500001 ? fy : 0;
    }
}

static int classify(const scene_entry *before, const scene_entry *entry, double *cam0, double *cam1)
{
    if (!before) {
        return SNAP_PREV;
    }
    if (entry->step != before->step + 1 || entry->qpc <= before->qpc ||
        memcmp(&entry->view, &before->view, sizeof(RECT)) != 0) {
        return SNAP_GAP;
    }
    uintptr_t follow = 0;
    double nearest = 1e30;
    for (unsigned i = 0; i < entry->count; ++i) {
        const scene_ship *ship = &entry->ships[i];
        const scene_ship *old = find_ship(before, ship->ptr);
        if (!old || (entry->follow != 0xffff && entry->follow != ship->id) ||
            ship->point.x - entry->camera.x != old->point.x - before->camera.x ||
            ship->point.y - entry->camera.y != old->point.y - before->camera.y) {
            continue;
        }
        double dx = ship->point.x - entry->camera.x - (entry->view.right - entry->view.left) * .5;
        double dy = ship->point.y - entry->camera.y - (entry->view.bottom - entry->view.top) * .5;
        double distance = ship->ptr == g_follow ? -1 : dx * dx + dy * dy;
        if (distance < nearest) {
            nearest = distance;
            follow = ship->ptr;
        }
    }
    if (!follow) {
        return SNAP_FOLLOW;
    }
    camera_fraction(before, follow, cam0);
    camera_fraction(entry, follow, cam1);
    int changed = g_follow && follow != g_follow;
    g_follow = follow;
    if (changed || (before->follow != entry->follow)) {
        g_changed_pair = entry->qpc;
    }
    if (g_changed_pair == entry->qpc) {
        return SNAP_CHANGE;
    }
    if (abs(entry->camera.x - before->camera.x) > 16 || abs(entry->camera.y - before->camera.y) > 16 ||
        fabs(cam1[0] - cam0[0]) > 12 || fabs(cam1[1] - cam0[1]) > 12) {
        return SNAP_JUMP;
    }
    return SNAP_NONE;
}

static void draw_sprite(ID3D11DeviceContext *context, const scene_region *region, double dx, double dy,
                        RECT clip, UINT width, UINT height)
{
    if (!region->view) {
        return;
    }
    FLOAT placement[4] = {(FLOAT)((region->box.left + dx) / SCENE_WIDTH),
                          (FLOAT)((region->box.top + dy) / SCENE_HEIGHT),
                          (FLOAT)(region->box.right - region->box.left) / SCENE_WIDTH,
                          (FLOAT)(region->box.bottom - region->box.top) / SCENE_HEIGHT};
    RECT physical = {(LONG)floor((double)clip.left * width / SCENE_WIDTH + .5),
                     (LONG)floor((double)clip.top * height / SCENE_HEIGHT + .5),
                     (LONG)floor((double)clip.right * width / SCENE_WIDTH + .5),
                     (LONG)floor((double)clip.bottom * height / SCENE_HEIGHT + .5)};
    ID3D11DeviceContext_RSSetScissorRects(context, 1, &physical);
    ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)g_placement, 0, NULL, placement, 0, 0);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &region->view);
    ID3D11DeviceContext_Draw(context, 4, 0);
    ++g_draws;
}

static void draw_region(ID3D11DeviceContext *context, const scene_entry *entry, unsigned index, double dx,
                        double dy, RECT clip, UINT width, UINT height)
{
    draw_sprite(context, &entry->regions[index], dx, dy, clip, width, height);
}

static int cloud_snap(const scene_entry *before, const scene_entry *entry, unsigned i)
{
    const scene_cloud *cloud = &entry->clouds[i];
    if (!before || !before->split_clouds || before->cloud_flags != entry->cloud_flags) {
        return 1;
    }
    const scene_cloud *old = &before->clouds[i];
    return !old->valid || !old->ticks || cloud->ticks != old->ticks - 1 || cloud->image != old->image ||
           cloud->width != old->width || cloud->height != old->height || cloud->vx != old->vx ||
           cloud->vy != old->vy || fabs(cloud->x - old->x - cloud->vx) > 1e-6 ||
           fabs(cloud->y - old->y - cloud->vy) > 1e-6;
}

static void draw_clouds(ID3D11DeviceContext *context, const scene_entry *before, const scene_entry *entry,
                        int reason, double alpha, const double *shift, UINT width, UINT height)
{
    if (!(entry->cloud_flags & 8)) {
        return;
    }
    if (!entry->split_clouds) {
        ++g_cloud_fallback;
        return;
    }
    draw_region(context, entry, REGION_PRECLOUD, 0, 0, entry->view, width, height);
    for (unsigned i = 0; i < SCENE_CLOUDS; ++i) {
        const scene_cloud *cloud = &entry->clouds[i];
        int snap = reason || cloud_snap(before, entry, i);
        double dx = reason ? 0 : shift[0];
        double dy = reason ? 0 : shift[1];
        if (!snap) {
            const scene_cloud *old = &before->clouds[i];
            double x = old->x + (cloud->x - old->x) * alpha;
            double y = old->y + (cloud->y - old->y) * alpha;
            /* Preserve the game's render/world camera offset and rounding. */
            dx += x - (cloud->x < 0 ? ceil(cloud->x - .5) : floor(cloud->x + .5));
            dy += y - (cloud->y < 0 ? ceil(cloud->y - .5) : floor(cloud->y + .5));
        }
        const scene_region *art = &g_cloud_art[cloud->image + ((entry->cloud_flags & 2) ? 18 : 0)];
        draw_sprite(context, art, cloud->dest.x + dx, cloud->dest.y + dy, entry->view, width, height);
        ++g_cloud_drawn;
        if (snap) {
            ++g_cloud_snapped;
        } else {
            ++g_cloud_interpolated;
        }
    }
}

static void observe_motion(const scene_entry *before, const scene_entry *entry, const double *camera,
                           const double *cam0, const double *cam1, double positions[SCENE_SHIPS][2])
{
    if (g_motion) {
        double dot = (camera[0] - g_last_camera[0]) * (cam1[0] - cam0[0]) +
                     (camera[1] - g_last_camera[1]) * (cam1[1] - cam0[1]);
        if (dot < -1e-6) {
            ++g_backward;
        }
    }
    for (unsigned i = 0; i < entry->count; ++i) {
        const scene_ship *ship = &entry->ships[i];
        const scene_ship *old = find_ship(before, ship->ptr);
        if (g_motion && old) {
            for (unsigned j = 0; j < SCENE_SHIPS; ++j) {
                if (g_last_ptr[j] != ship->ptr) {
                    continue;
                }
                double dot = (positions[i][0] - g_last_ship[j][0]) * (ship->x - old->x - cam1[0] + cam0[0]) +
                             (positions[i][1] - g_last_ship[j][1]) * (ship->y - old->y - cam1[1] + cam0[1]);
                if (dot < -1e-6) {
                    ++g_backward;
                }
            }
        }
    }
    memset(g_last_ptr, 0, sizeof(g_last_ptr));
    for (unsigned i = 0; i < entry->count; ++i) {
        g_last_ptr[i] = entry->ships[i].ptr;
        memcpy(g_last_ship[i], positions[i], sizeof(g_last_ship[i]));
    }
    memcpy(g_last_camera, camera, sizeof(g_last_camera));
    g_motion = 1;
}

static void draw_base(ID3D11DeviceContext *context, const scene_entry *entry, const scene_entry *edge,
                      const double *camera, const double *shift, UINT width, UINT height)
{
    /* Current bottom over previous bottom/mid, C outside view.
     * Physical rasterization preserves fractional motion at every scale. */

    FLOAT constants[16] = {(FLOAT)entry->view.left,
                           (FLOAT)entry->view.top,
                           (FLOAT)entry->view.right,
                           (FLOAT)entry->view.bottom,
                           (FLOAT)shift[0],
                           (FLOAT)shift[1],
                           (FLOAT)(edge->camera.x - camera[0]),
                           (FLOAT)(edge->camera.y - camera[1]),
                           (FLOAT)SCENE_WIDTH / (FLOAT)width,
                           (FLOAT)SCENE_HEIGHT / (FLOAT)height};
    RECT physical = {0, 0, (LONG)width, (LONG)height};
    ID3D11ShaderResourceView *base_views[] = {
        entry->regions[REGION_BOTTOM].view, edge->regions[REGION_EDGE].view,
        entry->regions[REGION_FINAL].view, entry->regions[REGION_EDGE].view};
    FLOAT placement[4] = {0, 0, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)g_placement, 0, NULL, placement, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)g_base_constants, 0, NULL, constants, 0,
                                          0);
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &g_base_constants);
    ID3D11DeviceContext_RSSetScissorRects(context, 1, &physical);
    ID3D11DeviceContext_OMSetBlendState(context, NULL, NULL, 0xffffffff);
    ID3D11DeviceContext_PSSetShader(context, g_base_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 4, base_views);
    ID3D11DeviceContext_Draw(context, 4, 0);
    ++g_draws;
    ID3D11ShaderResourceView *empty[4] = {NULL, NULL, NULL, NULL};
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 4, empty);
    ID3D11DeviceContext_PSSetShader(context, g_layer_ps, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(context, g_blend, NULL, 0xffffffff);
}

int scene_gpu_draw(ID3D11DeviceContext *context, UINT width, UINT height, LONG epoch)
{
    if (!g_count || !g_valid || epoch != g_epoch) {
        g_motion = 0;
        return 0;
    }
    double start = ticks();
    double elapsed = g_now ? max(0, start - g_now) : 0;
    g_now = start;
    if (!g_time) {
        g_time = g_entries[0].qpc;
    }
    double interval = g_interval ? g_interval : g_frequency * .078;
    double target = start - interval;
    double rate = clamp(1 + (target - g_time) / (4 * interval), .75, 1.25);
    g_time = max(g_time, min(g_entries[g_count - 1].qpc, g_time + elapsed * rate));
    while (g_count > 2 && g_entries[1].qpc <= g_time) {
        discard_first();
    }
    unsigned current = g_count - 1;
    for (unsigned i = 1; i < g_count; ++i) {
        if (g_entries[i].qpc >= g_time) {
            current = i;
            break;
        }
    }
    const scene_entry *entry = &g_entries[current];
    const scene_entry *before = current ? &g_entries[current - 1] : NULL;
    double cam0[2] = {0};
    double cam1[2] = {0};
    int reason = classify(before, entry, cam0, cam1);
    double alpha = reason ? 1 : clamp((g_time - before->qpc) / (entry->qpc - before->qpc), 0, 1);
    double camera[2] = {cam0[0] + (cam1[0] - cam0[0]) * alpha, cam0[1] + (cam1[1] - cam0[1]) * alpha};
    double shift[2] = {entry->camera.x - camera[0], entry->camera.y - camera[1]};
    ++g_frames;
    ++g_snaps[reason];
    if (reason) {
        shift[0] = 0;
        shift[1] = 0;
        camera[0] = entry->camera.x;
        camera[1] = entry->camera.y;
        g_motion = 0;
    }
    ID3D11DeviceContext_VSSetShader(context, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &g_placement);
    ID3D11DeviceContext_OMSetBlendState(context, g_blend, NULL, 0xffffffff);
    ID3D11DeviceContext_RSSetState(context, g_raster);
    draw_base(context, entry, reason ? entry : before, camera, shift, width, height);
    double positions[SCENE_SHIPS][2] = {{0}};
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (pass) {
            draw_region(context, entry, REGION_MID, shift[0], shift[1], entry->view, width, height);
        }
        draw_region(context, entry, pass ? MAST_RESIDUAL : HULL_RESIDUAL, shift[0], shift[1], entry->view,
                    width, height);
        for (unsigned i = 0; i < entry->count; ++i) {
            const scene_ship *ship = &entry->ships[i];
            const scene_ship *old = before ? find_ship(before, ship->ptr) : NULL;
            double dx = 0;
            double dy = 0;
            if (!reason && old && ship->ptr != g_follow) {
                dx = old->x + (ship->x - old->x) * alpha - camera[0] - (ship->point.x - entry->camera.x);
                dy = old->y + (ship->y - old->y) * alpha - camera[1] - (ship->point.y - entry->camera.y);
            }
            positions[i][0] = ship->point.x - entry->camera.x + dx;
            positions[i][1] = ship->point.y - entry->camera.y + dy;
            draw_region(context, entry, (pass ? MAST_BASE : HULL_BASE) + i, dx, dy, entry->view, width,
                        height);
        }
    }
    draw_clouds(context, before, entry, reason, alpha, shift, width, height);
    draw_region(context, entry, REGION_TOP, 0, 0, entry->view, width, height);
    if (!reason) {
        observe_motion(before, entry, camera, cam0, cam1, positions);
    }
    ID3D11DeviceContext_OMSetBlendState(context, NULL, NULL, 0xffffffff);
    ID3D11DeviceContext_RSSetState(context, NULL);
    double cost = (ticks() - start) * 1000 / g_frequency;
    g_cost += cost;
    g_max_cost = max(g_max_cost, cost);
    return 1;
}

int scene_gpu_active(void)
{
    return g_valid;
}

void scene_gpu_counters(unsigned *draws, unsigned *uploads, unsigned long long *bytes)
{
    *draws = g_draws;
    *uploads = g_uploads;
    *bytes = g_upload_bytes;
}

void scene_gpu_log(void)
{
    log_line("scene: clouds_drawn=%u clouds_interpolated=%u clouds_snapped=%u cloud_art_loaded=%u "
             "cloud_fallback_frames=%u",
             g_cloud_drawn, g_cloud_interpolated, g_cloud_snapped, g_art_loaded, g_cloud_fallback);
    static unsigned last_uploads;
    static unsigned long long last_bytes;
    unsigned uploads = g_uploads - last_uploads;
    unsigned long long bytes = g_upload_bytes - last_bytes;
    log_line("scene: texture_sets_interval=%u texture_uploads_interval=%u uploads_record=%.2f "
             "upload_bytes_record=%.0f reused_final_interval=%u",
             g_sets, uploads, g_sets ? (double)uploads / g_sets : 0, g_sets ? (double)bytes / g_sets : 0,
             g_reused_final);
    last_uploads = g_uploads;
    last_bytes = g_upload_bytes;
    g_sets = 0;
    g_reused_final = 0;
    log_line("scene: interpolated=%u rejected=%u backward_motion=%u render_ms_mean=%.3f render_ms_max=%.3f "
             "mask_pixels_hull=%u mid=%u masts=%u top=%u",
             g_snaps[SNAP_NONE], g_rejected, g_backward, g_frames ? g_cost / g_frames : 0, g_max_cost,
             g_mask_pixels[0], g_mask_pixels[1], g_mask_pixels[2], g_mask_pixels[3]);
    for (unsigned i = 1; i < SNAP_COUNT; ++i) {
        log_line("scene: snapped reason=%s frames=%u", SNAP_NAMES[i], g_snaps[i]);
    }
    log_line("scene: render_frame_ms_mean=%.3f render_frame_ms_max=%.3f frames=%u (before vsync)",
             g_frame_count ? g_frame_cost / g_frame_count : 0, g_frame_max, g_frame_count);
}

void scene_gpu_frame_begin(void)
{
    g_frame_start = ticks();
}

void scene_gpu_frame_end(void)
{
    double cost = (ticks() - g_frame_start) * 1000 / g_frequency;
    g_frame_cost += cost;
    g_frame_max = max(g_frame_max, cost);
    ++g_frame_count;
}
