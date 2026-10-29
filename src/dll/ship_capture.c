#define DIRECTDRAW_VERSION 0x0500
#define DIRECT3D_VERSION   0x0500
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ship_capture.h"
#include "ship_live.h"
#include "scene.h"
#include "patch.h"
#include "log.h"
#include "../common/sha256.h"

/* Verified CALL sites and complete displaced instructions in the supported EXE. */
#define WORLD_CALL        0x42afaf
#define WORLD_FN          0x44d390
#define DRAW_STATE        0x4cde48
#define HULL_ENTRY        0x456c40
#define OTHER_ENTRY       0x456d30
#define QUEUE_FN          0x414a20
#define POLY_FN           0x414c50
#define HANDLER_KEY       0x54
#define HANDLER_TARGET    0xd4
#define HANDLER_MATERIALS 0xcc
#define MATERIAL_SURFACE  0x3c
#define MATERIAL_TEXTURE  0x48
#define MATERIAL_INDEX    0x1c
#define DAMAGE_INDEX      0x20
#define POLY_DAMAGE       0x24
#define POLY_VERTICES     0x18
#define POLY_TRIANGLES    0x6c
#define POLY_CLASS        0x74

typedef struct {
    hd_header header;
    hd_pass *passes;
    hd_draw *draws;
    hd_texture *textures;
    uint32_t *texture_offsets;
    BYTE *geometry;
    BYTE *pixels;
    WORD (*reference)[HD_SIZE * HD_SIZE];
    unsigned pass_capacity;
    unsigned draw_capacity;
    unsigned texture_capacity;
    unsigned geometry_capacity;
    unsigned pixel_capacity;
    unsigned texture_attempted[HD_TEXTURES];
    unsigned pixel_bytes;
    unsigned invalid[HD_PASSES];
    LONGLONG capture_ticks;
} capture;

static capture *g_data;
/* Viewport is fixed by the handler for one queue pass; matrices vary per draw. */
static DWORD g_viewport_device;
static int g_viewport_cached;
static int32_t g_viewport_hr;
static uint32_t g_viewport[11];
static unsigned g_requested;
static int g_render_scale;
static int g_dump_done;
static LONGLONG g_frequency;
static unsigned g_steps;
static int g_active;
static int g_current = -1;
static hd_pass g_identity;
static int g_identity_valid;
static DWORD g_poly;
static DWORD g_material;
static char g_root[MAX_PATH];
static DWORD g_world_fn = WORLD_FN;
static DWORD g_queue_fn = QUEUE_FN;
static DWORD g_poly_fn = POLY_FN;
static void *g_hull_resume;
static void *g_other_resume;

static LONGLONG capture_clock(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static unsigned copy_geometry(const void *source, unsigned bytes)
{
    unsigned offset = g_data->header.geometry_bytes;
    if (bytes > g_data->geometry_capacity - offset) {
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
        return UINT32_MAX;
    }
    if (bytes) {
        memcpy(g_data->geometry + offset, source, bytes);
    }
    g_data->header.geometry_bytes += bytes;
    return offset;
}

int ship_capture_enabled(void)
{
    return g_requested != 0 || g_render_scale != 0;
}

void ship_capture_configure(const char *ini)
{
    int value = (int)GetPrivateProfileIntA("hd_ships", "dump_step", 0, ini);
    g_requested = value > 0 ? (unsigned)value : 0;
    char scale[32];
    GetPrivateProfileStringA("hd_ships", "render_scale", "\001", scale, sizeof(scale), ini);
    if (scale[0] == 1 && !scale[1]) {
        char mode[32];
        GetPrivateProfileStringA("hd_ships", "mode", "sharp", mode, sizeof(mode), ini);
        g_render_scale = !_stricmp(mode, "sharp") ? 16 : (!_stricmp(mode, "pixel2") ? 2 : 0);
    } else {
        long requested = strtol(scale, NULL, 10);
        g_render_scale = requested < 0 ? 0 : (requested > 16 ? 16 : (int)requested);
    }
    log_line(g_render_scale ? "hd_ships: requested_render_scale=%d dump_step=%u"
                            : "hd_ships: render_scale=%d dump_step=%u",
             g_render_scale, g_requested);
}

static int write_file(const char *name, const void *data, size_t bytes)
{
    char path[MAX_PATH];
    if (sprintf_s(path, sizeof(path), "%s\\%s", g_root, name) < 0) {
        return 0;
    }
    FILE *file = NULL;
    if (fopen_s(&file, path, "wb") || !file) {
        return 0;
    }
    int ok = fwrite(data, 1, bytes, file) == bytes;
    if (fclose(file)) {
        ok = 0;
    }
    return ok;
}

static int write_geometry(void)
{
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "%s\\geometry.bin", g_root);
    FILE *file = NULL;
    if (fopen_s(&file, path, "wb") || !file) {
        return 0;
    }
    hd_header *h = &g_data->header;
    int ok = fwrite(h, sizeof(*h), 1, file) == 1 &&
             fwrite(g_data->passes, sizeof(hd_pass), h->passes, file) == h->passes &&
             fwrite(g_data->draws, sizeof(hd_draw), h->draws, file) == h->draws &&
             fwrite(g_data->textures, sizeof(hd_texture), h->textures, file) == h->textures &&
             fwrite(g_data->geometry, 1, h->geometry_bytes, file) == h->geometry_bytes;
    if (fclose(file)) {
        ok = 0;
    }
    return ok;
}

static int write_json(void)
{
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "%s\\draws.json", g_root);
    FILE *file = NULL;
    if (fopen_s(&file, path, "wb") || !file) {
        return 0;
    }
    fprintf(file, "{\"draws\":[\n");
    for (unsigned i = 0; i < g_data->header.draws; ++i) {
        hd_draw *d = &g_data->draws[i];
        fprintf(file,
                "%s{\"sequence\":%u,\"pass\":%u,\"polyset\":%u,\"material\":%u,"
                "\"damage\":%u,\"vertices\":%u,\"indices\":%u,\"texture\":%u,\"result\":%ld,\"rs\":[",
                i ? ",\n" : "", i, d->pass, d->polyset, d->material, d->damage, d->vertices, d->indices,
                d->texture, (long)d->result);
        for (unsigned s = 0; s < HD_RS; ++s) {
            fprintf(file, "%s[%u,%u,%ld]", s ? "," : "", s, d->rs[s], (long)d->rs_hr[s]);
        }
        fprintf(file, "],\"ls\":[");
        for (unsigned s = 0; s < HD_LS; ++s) {
            fprintf(file, "%s[%u,%u,%ld]", s ? "," : "", s, d->ls[s], (long)d->ls_hr[s]);
        }
        fprintf(file,
                "],\"vertex_count\":%u,\"index_count\":%u,\"flags\":%u,\"part_class\":%u,"
                "\"preclip_count\":%u,\"preclip_offset\":%u,\"viewport_hr\":%ld,\"viewport_dwords\":[",
                d->vertex_count, d->index_count, d->flags, d->part_class, d->preclip_count, d->preclip,
                (long)d->viewport_hr);
        for (unsigned v = 0; v < 11; ++v) {
            fprintf(file, "%s%u", v ? "," : "", d->viewport[v]);
        }
        fprintf(file, "],\"transforms\":[");
        for (unsigned m = 0; m < 3; ++m) {
            fprintf(file, "%s{\"hr\":%ld,\"matrix\":[", m ? "," : "", (long)d->transform_hr[m]);
            for (unsigned v = 0; v < 16; ++v) {
                float value = d->transforms[m][v];
                if (value == value && value >= -3.402823466e38f && value <= 3.402823466e38f) {
                    fprintf(file, "%s%.9g", v ? "," : "", value);
                } else {
                    fprintf(file, "%snull", v ? "," : "");
                }
            }
            fprintf(file, "]}");
        }
        fprintf(file, "]}");
    }
    fprintf(file, "\n]}\n");
    int ok = !ferror(file);
    if (fclose(file)) {
        ok = 0;
    }
    return ok;
}

static int write_manifest(void)
{
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "%s\\manifest.json", g_root);
    FILE *file = NULL;
    if (fopen_s(&file, path, "wb") || !file) {
        return 0;
    }
    hd_header *h = &g_data->header;
    fprintf(file,
            "{\"version\":1,\"step\":%u,\"exe_sha256\":\"%s\",\"text_sha256\":\"%s\","
            "\"incomplete\":%u,\"camera_fallback\":\"u=72+144*x; v=72-144*(cos65*y+sin65*z)\","
            "\"part_matrices\":\"not captured\",\"passes\":[\n",
            h->step, h->exe_sha256, h->text_sha256, h->incomplete);
    for (unsigned i = 0; i < h->passes; ++i) {
        hd_pass *p = &g_data->passes[i];
        fprintf(file,
                "%s{\"sequence\":%u,\"id\":%u,\"object\":%u,\"image\":%u,\"type\":%u,"
                "\"owner\":%u,\"pass\":%u,\"pose_xy_course_heading\":[%.17g,%.17g,%.17g,%.17g],"
                "\"handler\":%u,\"queue\":%u,\"blend_additive\":%u,\"reverse\":%u,\"main_first\":%u,"
                "\"key\":%u,\"source\":[%ld,%ld,%ld,%ld],\"dest\":[%ld,%ld,%ld,%ld],"
                "\"first_draw\":%u,\"draw_count\":%u,\"reference\":\"reference/"
                "%u.rgb565\",\"reference_valid\":%u}",
                i ? ",\n" : "", i, p->id, p->object, p->image, p->type, p->owner, p->pass, p->pose[0],
                p->pose[1], p->pose[2], p->pose[3], p->handler, p->queue, p->blend, p->reverse, p->main_first,
                p->key, (long)p->source[0], (long)p->source[1], (long)p->source[2], (long)p->source[3],
                (long)p->dest[0], (long)p->dest[1], (long)p->dest[2], (long)p->dest[3], p->first_draw,
                p->draw_count, i, p->reference_valid);
    }
    fprintf(file, "\n],\"textures\":[");
    for (unsigned i = 0; i < h->textures; ++i) {
        hd_texture *t = &g_data->textures[i];
        fprintf(file,
                "%s{\"file\":\"textures/%u.bin\",\"device\":%u,\"handle\":%u,\"surface\":%u,"
                "\"width\":%u,\"height\":%u,\"pitch\":%ld,\"row_bytes\":%u,\"flags\":%u,"
                "\"bpp\":%u,\"masks_rgba\":[%u,%u,%u,%u],\"palette_valid\":%u}",
                i ? "," : "", i, t->device, t->handle, t->surface, t->width, t->height, (long)t->pitch,
                t->row_bytes, t->flags, t->bpp, t->rmask, t->gmask, t->bmask, t->amask, t->palette_valid);
    }
    fprintf(file, "],\"draws\":%u,\"binary_directory\":\"ship_dump.h v1\"}\n", h->draws);
    int ok = !ferror(file);
    if (fclose(file)) {
        ok = 0;
    }
    return ok;
}

static void save_capture(void)
{
    char base[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, base, sizeof(base));
    if (!length || length >= MAX_PATH || length > MAX_PATH - 64) {
        log_line("hd_ships: executable path too long; dump refused");
        return;
    }
    char *slash = strrchr(base, '\\');
    if (!slash) {
        return;
    }
    *slash = 0;
    if (sprintf_s(g_root, sizeof(g_root), "%s\\hd_dump", base) < 0) {
        return;
    }
    CreateDirectoryA(g_root, NULL);
    if (sprintf_s(g_root, sizeof(g_root), "%s\\hd_dump\\step_%u", base, g_requested) < 0 ||
        !CreateDirectoryA(g_root, NULL)) {
        log_line("hd_ships: output directory exists or cannot be created; dump refused");
        return;
    }
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "%s\\textures", g_root);
    int ok = CreateDirectoryA(path, NULL) != 0;
    sprintf_s(path, sizeof(path), "%s\\reference", g_root);
    ok = CreateDirectoryA(path, NULL) && ok;
    for (unsigned i = 0; i < g_data->header.passes; ++i) {
        char name[64];
        sprintf_s(name, sizeof(name), "reference\\%u.rgb565", i);
        ok = write_file(name, g_data->reference[i], sizeof(g_data->reference[i])) && ok;
    }
    for (unsigned i = 0; i < g_data->header.textures; ++i) {
        char name[64];
        sprintf_s(name, sizeof(name), "textures\\%u.bin", i);
        ok = write_file(name, g_data->pixels + g_data->texture_offsets[i], g_data->textures[i].bytes) && ok;
    }
    ok = write_geometry() && write_json() && ok;
    /* Publish manifest last: a missing manifest denotes an interrupted dump. */
    hd_header *h = &g_data->header;
    if (ok) {
        ok = write_manifest();
    }
    unsigned triangles = 0;
    unsigned ships = 0;
    for (unsigned i = 0; i < h->draws; ++i) {
        hd_draw *d = &g_data->draws[i];
        triangles += (d->index_count ? d->index_count : d->vertex_count) / 3;
    }
    for (unsigned i = 0; i < h->passes; ++i) {
        unsigned j = 0;
        while (j < i && g_data->passes[j].id != g_data->passes[i].id) {
            ++j;
        }
        ships += j == i;
    }
    log_line("hd_ships: dump step=%u ships=%u draws=%u triangles=%u textures=%u dir=%s", h->step, ships,
             h->draws, triangles, h->textures, g_root);
    if (!ok || h->incomplete) {
        log_line("hd_ships: dump incomplete io=%d capture=%u", !ok, h->incomplete);
    }
}

int ship_hd_render_scale(void)
{
    return g_render_scale;
}

static unsigned live_bytes(hd_live_record *out, unsigned source, unsigned bytes)
{
    if (source > g_data->header.geometry_bytes || bytes > g_data->header.geometry_bytes - source ||
        bytes > HD_LIVE_GEOMETRY - out->geometry_bytes) {
        return UINT32_MAX;
    }
    unsigned offset = out->geometry_bytes;
    memcpy(out->geometry + offset, g_data->geometry + source, bytes);
    out->geometry_bytes += bytes;
    return offset;
}

static unsigned live_texture(hd_live_record *out, unsigned source, unsigned *map)
{
    if (source == UINT32_MAX) {
        return source;
    }
    if (source >= g_data->header.textures) {
        return UINT32_MAX;
    }
    if (map[source] != UINT32_MAX) {
        return map[source];
    }
    const hd_texture *t = &g_data->textures[source];
    unsigned offset = g_data->texture_offsets[source];
    if (!t->bytes || offset > g_data->pixel_bytes || t->bytes > g_data->pixel_bytes - offset ||
        out->textures == HD_LIVE_TEXTURES || t->bytes > HD_LIVE_PIXELS - out->pixel_bytes) {
        return UINT32_MAX;
    }
    unsigned index = out->textures++;
    out->texture[index] = *t;
    out->offsets[index] = out->pixel_bytes;
    memcpy(out->pixels + out->pixel_bytes, g_data->pixels + g_data->texture_offsets[source], t->bytes);
    out->pixel_bytes += t->bytes;
    map[source] = index;
    return index;
}

/* Called on the game thread before the scene record enters the SRW mailbox.
 * Capacity failure poisons just this pass; earlier completed ships remain usable. */
void ship_capture_publish(hd_live_record *out)
{
    if (!out) {
        return;
    }
    out->passes = 0;
    out->draws = 0;
    out->textures = 0;
    out->geometry_bytes = 0;
    out->pixel_bytes = 0;
    out->capture_ms = 0;
    if (!g_active || !g_render_scale || !g_data) {
        return;
    }
    unsigned map[HD_TEXTURES];
    LONGLONG start = capture_clock();
    memset(map, 0xff, sizeof(map));
    for (unsigned i = 0; i < g_data->header.passes && out->passes < HD_LIVE_PASSES; ++i) {
        hd_pass *p = &out->pass[out->passes++];
        *p = g_data->passes[i];
        unsigned first = p->first_draw;
        p->first_draw = out->draws;
        p->reference_valid = 0;
        if (g_data->invalid[i] || !g_data->passes[i].reference_valid ||
            p->draw_count > HD_LIVE_DRAWS - out->draws) {
            continue;
        }
        unsigned geometry_start = out->geometry_bytes;
        unsigned j = 0;
        for (; j < p->draw_count; ++j) {
            hd_draw d = g_data->draws[first + j];
            d.pass = out->passes - 1;
            d.vertices = live_bytes(out, d.vertices, d.vertex_count * sizeof(hd_vertex));
            d.indices = live_bytes(out, d.indices, d.index_count * sizeof(WORD));
            d.preclip = 0;
            d.preclip_count = 0;
            unsigned texture = live_texture(out, d.texture, map);
            if (d.vertices == UINT32_MAX || d.indices == UINT32_MAX ||
                (d.texture != UINT32_MAX && texture == UINT32_MAX)) {
                break;
            }
            d.texture = texture;
            out->draw[out->draws++] = d;
        }
        if (j == p->draw_count) {
            p->reference_valid = 1;
        } else {
            out->draws = p->first_draw;
            out->geometry_bytes = geometry_start;
        }
    }
    out->capture_ms =
        g_frequency ? (double)(g_data->capture_ticks + capture_clock() - start) * 1000 / (double)g_frequency
                    : 0;
}

/* Called before the actual world update; observers never call game code. */
static void __stdcall world_observe(const DWORD *saved)
{
    (void)saved;
    DWORD error = GetLastError();
    if (g_active && g_steps == g_requested && !g_dump_done) {
        save_capture();
        g_dump_done = 1;
        if (!g_render_scale) {
            free(g_data);
            g_data = NULL;
        }
    }
    g_active = 0;
    g_current = -1;
    g_identity_valid = 0;
    __try {
        int state = *(int *)DRAW_STATE;
        if (g_data && (state == 2 || state == 3)) {
            ++g_steps;
            if (g_render_scale || (!g_dump_done && g_steps == g_requested)) {
                g_data->header.step = g_steps;
                g_data->header.passes = 0;
                g_data->header.draws = 0;
                g_data->header.textures = 0;
                g_data->header.geometry_bytes = 0;
                g_data->header.incomplete = 0;
                g_data->pixel_bytes = 0;
                g_data->capture_ticks = 0;
                memset(g_data->invalid, 0, sizeof(g_data->invalid));
                memset(g_data->texture_attempted, 0, sizeof(g_data->texture_attempted));
                g_active = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("hd_ships: world state unreadable");
    }
    SetLastError(error);
}

static void identity_observe(const DWORD *saved, unsigned pass)
{
    if (!g_active) {
        return;
    }
    DWORD error = GetLastError();
    g_identity_valid = ship_capture_identity((const void *)saved[6], &g_identity);
    if (!g_identity_valid) {
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
    }
    g_identity.pass = pass;
    SetLastError(error);
}

static void __stdcall hull_observe(const DWORD *saved)
{
    identity_observe(saved, 1);
}

static void __stdcall other_observe(const DWORD *saved)
{
    identity_observe(saved, 0);
}

static void __stdcall queue_observe(const DWORD *saved)
{
    if (!g_active || !g_identity_valid) {
        return;
    }
    DWORD error = GetLastError();
    g_current = -1;
    __try {
        if (g_data->header.passes == g_data->pass_capacity) {
            g_data->header.incomplete = 1;
            if (g_current >= 0) {
                g_data->invalid[g_current] = 1;
            }
            __leave;
        }
        const DWORD *args = saved + 11;
        hd_pass *p = &g_data->passes[g_data->header.passes];
        *p = g_identity;
        p->draw_count = 0;
        p->handler = saved[6];
        p->queue = args[0];
        p->blend = args[4] & 255;
        p->reverse = args[5] & 255;
        p->main_first = args[6] & 255;
        memcpy(p->source, (const void *)args[2], 8);
        memcpy(p->dest, (const void *)args[3], 16);
        /* a4 is the source origin; a5 is the destination rectangle. */
        p->source[2] = p->source[0] + p->dest[2] - p->dest[0];
        p->source[3] = p->source[1] + p->dest[3] - p->dest[1];
        p->key = p->blend ? 0 : *(DWORD *)(p->handler + HANDLER_KEY);
        p->first_draw = g_data->header.draws;
        g_viewport_cached = 0;
        g_current = (int)g_data->header.passes++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
    }
    SetLastError(error);
}

static int copy_surface(IDirectDrawSurface *surface, BYTE *dest, unsigned capacity, hd_texture *out)
{
    hd_texture pending = *out;
    hd_texture *published = out;
    out = &pending;
    DDSURFACEDESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (!surface ||
        FAILED(surface->lpVtbl->Lock(
            surface, NULL, &desc,
            DDLOCK_READONLY | ((g_requested == g_steps && !g_dump_done) ? DDLOCK_WAIT : DDLOCK_DONOTWAIT),
            NULL))) {
        return 0;
    }
    int ok = 0;
    __try {
        unsigned bpp = desc.ddpfPixelFormat.dwRGBBitCount;
        unsigned row = (desc.dwWidth * bpp + 7) / 8;
        if (!desc.lpSurface || !desc.dwWidth || desc.dwWidth > 2048 || !desc.dwHeight ||
            desc.dwHeight > 2048 || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) ||
            desc.lPitch < -65536 || desc.lPitch > 65536 || (unsigned)abs(desc.lPitch) < row ||
            row > capacity / desc.dwHeight) {
            __leave;
        }
        out->width = desc.dwWidth;
        out->height = desc.dwHeight;
        out->pitch = desc.lPitch;
        out->row_bytes = row;
        out->bytes = row * desc.dwHeight;
        out->flags = desc.ddpfPixelFormat.dwFlags;
        out->bpp = bpp;
        out->rmask = desc.ddpfPixelFormat.dwRBitMask;
        out->gmask = desc.ddpfPixelFormat.dwGBitMask;
        out->bmask = desc.ddpfPixelFormat.dwBBitMask;
        out->amask = desc.ddpfPixelFormat.dwRGBAlphaBitMask;
        for (unsigned y = 0; y < desc.dwHeight; ++y) {
            memcpy(dest + y * row, (BYTE *)desc.lpSurface + (int)y * desc.lPitch, row);
        }
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (FAILED(surface->lpVtbl->Unlock(surface, desc.lpSurface))) {
        ok = 0;
    }
    if (ok) {
        IDirectDrawPalette *palette = NULL;
        if (SUCCEEDED(surface->lpVtbl->GetPalette(surface, &palette))) {
            out->palette_valid =
                SUCCEEDED(palette->lpVtbl->GetEntries(palette, 0, 0, 256, (PALETTEENTRY *)out->palette));
            palette->lpVtbl->Release(palette);
        }
        DDCOLORKEY key;
        out->key_valid = SUCCEEDED(surface->lpVtbl->GetColorKey(surface, DDCKEY_SRCBLT, &key));
        if (out->key_valid) {
            out->key_low = key.dwColorSpaceLowValue;
            out->key_high = key.dwColorSpaceHighValue;
        }
    }
    if (ok) {
        *published = pending;
    }
    return ok;
}

static void __stdcall queue_leave(const DWORD *saved)
{
    (void)saved;
    if (!g_active || g_current < 0) {
        return;
    }
    DWORD error = GetLastError();
    LONGLONG start = capture_clock();
    __try {
        hd_pass *p = &g_data->passes[g_current];
        if ((g_requested == g_steps && !g_dump_done) || (g_render_scale && p->blend)) {
            hd_texture desc;
            memset(&desc, 0, sizeof(desc));
            IDirectDrawSurface *surface = *(IDirectDrawSurface **)(p->handler + HANDLER_TARGET);
            p->reference_valid =
                copy_surface(surface, (BYTE *)g_data->reference[g_requested ? g_current : 0],
                             sizeof(g_data->reference[g_requested ? g_current : 0]), &desc) &&
                desc.width == HD_SIZE && desc.height == HD_SIZE && desc.bpp == 16 && desc.rmask == 0xf800 &&
                desc.gmask == 0x7e0 && desc.bmask == 0x1f;
            if (!p->reference_valid) {
                g_data->header.incomplete = 1;
                if (g_current >= 0) {
                    g_data->invalid[g_current] = 1;
                }
            }
            if (g_render_scale && p->blend) {
                scene_capture_additive(p, p->reference_valid ? g_data->reference[g_requested ? g_current : 0]
                                                             : NULL);
            }
        }
        p->reference_valid = !g_data->invalid[g_current];
        /* Texture locks are delayed until EndScene, before any live surface can go away. */
        for (unsigned i = 0; i < g_data->header.textures; ++i) {
            hd_texture *t = &g_data->textures[i];
            if (g_data->texture_attempted[i]) {
                continue;
            }
            g_data->texture_attempted[i] = 1;
            if (!copy_surface((IDirectDrawSurface *)t->surface, g_data->pixels + g_data->pixel_bytes,
                              g_data->pixel_capacity - g_data->pixel_bytes, t)) {
                g_data->header.incomplete = 1;
                if (g_current >= 0) {
                    g_data->invalid[g_current] = 1;
                }
                continue;
            }
            g_data->texture_offsets[i] = g_data->pixel_bytes;
            g_data->pixel_bytes += t->bytes;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
    }
    g_current = -1;
    g_poly = 0;
    g_material = 0;
    g_identity_valid = 0;
    g_data->capture_ticks += capture_clock() - start;
    SetLastError(error);
}

static void __stdcall poly_observe(const DWORD *saved)
{
    if (!g_active || g_current < 0) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        g_poly = saved[11];
        DWORD index = *(int *)(g_poly + POLY_DAMAGE) > 0 ? DAMAGE_INDEX : MATERIAL_INDEX;
        BYTE *materials = *(BYTE **)(saved[6] + HANDLER_MATERIALS);
        g_material = (*(DWORD **)(materials + 4))[*(unsigned *)(g_poly + index)];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_poly = 0;
        g_material = 0;
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
    }
    SetLastError(error);
}

static unsigned capture_texture(IDirect3DDevice2 *device, DWORD handle)
{
    if (!handle) {
        return UINT32_MAX;
    }
    if (!g_material || *(DWORD *)(g_material + MATERIAL_TEXTURE) != handle) {
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
        return UINT32_MAX;
    }
    DWORD surface = *(DWORD *)(g_material + MATERIAL_SURFACE);
    for (unsigned i = 0; i < g_data->header.textures; ++i) {
        hd_texture *t = &g_data->textures[i];
        if (t->device == (DWORD)device && t->handle == handle && t->surface == surface) {
            return i;
        }
    }
    if (g_data->header.textures == g_data->texture_capacity) {
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
        return UINT32_MAX;
    }
    unsigned index = g_data->header.textures;
    hd_texture *t = &g_data->textures[index];
    memset(t, 0, sizeof(*t));
    t->device = (DWORD)device;
    t->handle = handle;
    t->surface = surface;
    ++g_data->header.textures;
    return index;
}

static void capture_states(IDirect3DDevice2 *device, hd_draw *d)
{
    static const unsigned required[] = {1, 3, 9, 15, 17, 18, 19, 20, 21, 22, 24, 25, 27, 28, 29, 41, 44, 45};
    int dump = g_requested == g_steps && !g_dump_done;
    for (unsigned s = 0; s < HD_RS; ++s) {
        d->rs_hr[s] = E_INVALIDARG;
    }
    for (unsigned s = 0; s < HD_LS; ++s) {
        d->ls_hr[s] = E_INVALIDARG;
    }
    unsigned count = dump ? HD_RS - 1 : sizeof(required) / sizeof(required[0]);
    for (unsigned i = 0; i < count; ++i) {
        unsigned s = dump ? i + 1 : required[i];
        d->rs_hr[s] = device->lpVtbl->GetRenderState(device, (D3DRENDERSTATETYPE)s, (DWORD *)&d->rs[s]);
    }
    if (dump) {
        for (unsigned s = 1; s < HD_LS; ++s) {
            d->ls_hr[s] = device->lpVtbl->GetLightState(device, (D3DLIGHTSTATETYPE)s, (DWORD *)&d->ls[s]);
        }
    }
    for (unsigned s = 0; s < 3; ++s) {
        d->transform_hr[s] = device->lpVtbl->GetTransform(device, (D3DTRANSFORMSTATETYPE)(s + 1),
                                                          (D3DMATRIX *)d->transforms[s]);
    }
    if (!dump && g_viewport_cached && g_viewport_device == (DWORD)device) {
        d->viewport_hr = g_viewport_hr;
        memcpy(d->viewport, g_viewport, sizeof(d->viewport));
    } else {
        IDirect3DViewport2 *viewport = NULL;
        d->viewport_hr = device->lpVtbl->GetCurrentViewport(device, &viewport);
        if (SUCCEEDED(d->viewport_hr) && viewport) {
            D3DVIEWPORT2 v;
            memset(&v, 0, sizeof(v));
            v.dwSize = sizeof(v);
            d->viewport_hr = viewport->lpVtbl->GetViewport2(viewport, &v);
            memcpy(d->viewport, &v, sizeof(v));
            viewport->lpVtbl->Release(viewport);
        }
        if (SUCCEEDED(d->viewport_hr) && !viewport) {
            d->viewport_hr = E_FAIL;
        }
        g_viewport_device = (DWORD)device;
        g_viewport_cached = 1;
        g_viewport_hr = d->viewport_hr;
        memcpy(g_viewport, d->viewport, sizeof(g_viewport));
    }
    d->texture = capture_texture(device, d->rs[D3DRENDERSTATE_TEXTUREHANDLE]);
}

void ship_capture_draw(void *object, unsigned primitive, unsigned vtype, const void *vertices, unsigned count,
                       const WORD *indices, unsigned index_count, unsigned flags, HRESULT result)
{
    if (!g_active || g_current < 0) {
        return;
    }
    DWORD error = GetLastError();
    LONGLONG start = capture_clock();
    __try {
        if (vtype != D3DVT_LVERTEX || primitive != D3DPT_TRIANGLELIST || count > 65536 ||
            index_count > 196608 || g_data->header.draws == g_data->draw_capacity) {
            g_data->header.incomplete = 1;
            if (g_current >= 0) {
                g_data->invalid[g_current] = 1;
            }
            __leave;
        }
        hd_draw *d = &g_data->draws[g_data->header.draws++];
        memset(d, 0, sizeof(*d));
        d->pass = (unsigned)g_current;
        ++g_data->passes[g_current].draw_count;
        d->polyset = g_poly;
        d->material = g_material;
        d->primitive = primitive;
        d->flags = flags;
        d->result = result;
        d->vertex_count = count;
        d->index_count = index_count;
        d->vertices = copy_geometry(vertices, count * sizeof(hd_vertex));
        d->indices = copy_geometry(indices, index_count * sizeof(WORD));
        if (g_poly) {
            d->damage = *(unsigned *)(g_poly + POLY_DAMAGE);
            d->part_class = *(unsigned *)(g_poly + POLY_CLASS);
            unsigned triangles = *(unsigned *)(g_poly + POLY_TRIANGLES);
            if (g_steps == g_requested && !g_dump_done && triangles <= 65536 / 3) {
                d->preclip_count = triangles * 3;
                d->preclip =
                    copy_geometry(*(void **)(g_poly + POLY_VERTICES), d->preclip_count * sizeof(hd_vertex));
            }
        }
        capture_states((IDirect3DDevice2 *)object, d);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_data->header.incomplete = 1;
        if (g_current >= 0) {
            g_data->invalid[g_current] = 1;
        }
    }
    g_data->capture_ticks += capture_clock() - start;
    SetLastError(error);
}

/* PUSHAD layout matches scene.c; preserve flags, GPR, LastError and x87/SSE.
 * Tail observers see original ECX and stack. Queue wrapper copies seven args,
 * original consumes 28 bytes, wrapper RET 28 consumes the caller's originals. */
// clang-format off
#define BRIDGE(name, observer) \
    __declspec(naked) static void name(void) { \
        __asm { pushfd } \
        __asm { pushad } \
        __asm { mov eax, esp } \
        __asm { sub esp, 528 } \
        __asm { and esp, -16 } \
        __asm { fxsave [esp] } \
        __asm { fninit } \
        __asm { push 1f80h } \
        __asm { ldmxcsr [esp] } \
        __asm { add esp, 4 } \
        __asm { push eax } \
        __asm { push eax } \
        __asm { cld } \
        __asm { call observer } \
        __asm { pop eax } \
        __asm { fxrstor [esp] } \
        __asm { mov esp, eax } \
        __asm { popad } \
        __asm { popfd } \
        __asm { ret } \
    }
BRIDGE(world_bridge, world_observe)
BRIDGE(hull_bridge, hull_observe)
BRIDGE(other_bridge, other_observe)
BRIDGE(queue_bridge, queue_observe)
BRIDGE(queue_leave_bridge, queue_leave)
BRIDGE(poly_bridge, poly_observe)

__declspec(naked) static void world_thunk(void)
{
    __asm { call world_bridge }
    __asm { jmp dword ptr [g_world_fn] }
}
__declspec(naked) static void hull_thunk(void)
{
    __asm { call hull_bridge }
    __asm { jmp dword ptr [g_hull_resume] }
}
__declspec(naked) static void other_thunk(void)
{
    __asm { call other_bridge }
    __asm { jmp dword ptr [g_other_resume] }
}
__declspec(naked) static void poly_thunk(void)
{
    __asm { call poly_bridge }
    __asm { jmp dword ptr [g_poly_fn] }
}
__declspec(naked) static void queue_thunk(void)
{
    __asm {
        call queue_bridge
        push [esp + 28]
        push [esp + 28]
        push [esp + 28]
        push [esp + 28]
        push [esp + 28]
        push [esp + 28]
        push [esp + 28]
        call dword ptr [g_queue_fn]
        call queue_leave_bridge
        ret 28
    }
}

// clang-format on

static int hook_call(DWORD site, DWORD target, void *thunk, int install)
{
    BYTE bytes[5] = {0xe8};
    DWORD relative = target - site - 5;
    memcpy(bytes + 1, &relative, 4);
    return install ? patch_call((void *)site, bytes, thunk) : patch_check((void *)site, bytes, 5);
}

static int hook_entry(DWORD site, const BYTE *bytes, unsigned length, void *thunk, void **resume)
{
    BYTE *code = VirtualAlloc(NULL, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!code) {
        return 0;
    }
    memcpy(code, bytes, length);
    code[length] = 0xe9;
    DWORD relative = site + length - (DWORD)(code + length + 5);
    memcpy(code + length + 1, &relative, 4);
    DWORD old;
    if (!VirtualProtect(code, length + 5, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(code, 0, MEM_RELEASE);
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), code, length + 5);
    *resume = code;
    return patch_jmp_padded((void *)site, bytes, length, thunk);
}

void ship_capture_apply(const char *text_hash)
{
    if (!ship_capture_enabled()) {
        return;
    }
    static const BYTE hull[] = {0x51, 0xa1, 0xe0, 0xdd, 0x4c, 0};
    static const BYTE other[] = {0x51, 0x53, 0x8b, 0xc1, 0x8b, 0x0d, 0xe4, 0xdd, 0x4c, 0};
    static const DWORD queues[] = {0x456d23, 0x456e26};
    static const DWORD polys[] = {0x414aed, 0x414b12, 0x414b30, 0x414c44};
    if (!patch_check((void *)HULL_ENTRY, hull, sizeof(hull)) ||
        !patch_check((void *)OTHER_ENTRY, other, sizeof(other)) ||
        !hook_call(WORLD_CALL, WORLD_FN, NULL, 0)) {
        log_line("hd_ships: hook bytes rejected");
        return;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!hook_call(queues[i], QUEUE_FN, NULL, 0)) {
            return;
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!hook_call(polys[i], POLY_FN, NULL, 0)) {
            return;
        }
    }
    unsigned passes = g_requested ? HD_PASSES : HD_LIVE_PASSES;
    unsigned draws = g_requested ? HD_DRAWS : HD_LIVE_DRAWS;
    unsigned textures = g_requested ? HD_TEXTURES : HD_LIVE_TEXTURES;
    unsigned geometry = g_requested ? HD_GEOMETRY : HD_LIVE_GEOMETRY;
    unsigned pixels = g_requested ? HD_PIXELS : HD_LIVE_PIXELS;
    unsigned references = g_requested ? HD_PASSES : 1;
    size_t bytes = sizeof(capture) + passes * sizeof(hd_pass) + draws * sizeof(hd_draw) +
                   textures * (sizeof(hd_texture) + sizeof(uint32_t)) + geometry + pixels +
                   references * HD_SIZE * HD_SIZE * sizeof(WORD);
    g_data = calloc(1, bytes);
    if (!g_data) {
        log_line("hd_ships: allocation failed");
        return;
    }
    BYTE *cursor = (BYTE *)(g_data + 1);
    g_data->passes = (hd_pass *)cursor;
    cursor += passes * sizeof(hd_pass);
    g_data->draws = (hd_draw *)cursor;
    cursor += draws * sizeof(hd_draw);
    g_data->textures = (hd_texture *)cursor;
    cursor += textures * sizeof(hd_texture);
    g_data->texture_offsets = (uint32_t *)cursor;
    cursor += textures * sizeof(uint32_t);
    g_data->geometry = cursor;
    cursor += geometry;
    g_data->pixels = cursor;
    cursor += pixels;
    g_data->reference = (WORD(*)[HD_SIZE * HD_SIZE]) cursor;
    g_data->pass_capacity = passes;
    g_data->draw_capacity = draws;
    g_data->texture_capacity = textures;
    g_data->geometry_capacity = geometry;
    g_data->pixel_capacity = pixels;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart;
    g_data->header.magic = HD_MAGIC;
    g_data->header.version = HD_VERSION;
    strcpy_s(g_data->header.text_sha256, sizeof(g_data->header.text_sha256), text_hash);
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    sha256_file(exe, g_data->header.exe_sha256);
    int ok = hook_entry(HULL_ENTRY, hull, sizeof(hull), (void *)(uintptr_t)hull_thunk, &g_hull_resume) &&
             hook_entry(OTHER_ENTRY, other, sizeof(other), (void *)(uintptr_t)other_thunk, &g_other_resume);
    for (unsigned i = 0; i < 2 && ok; ++i) {
        ok = hook_call(queues[i], QUEUE_FN, (void *)(uintptr_t)queue_thunk, 1);
    }
    for (unsigned i = 0; i < 4 && ok; ++i) {
        ok = hook_call(polys[i], POLY_FN, (void *)(uintptr_t)poly_thunk, 1);
    }
    ok = ok && hook_call(WORLD_CALL, WORLD_FN, (void *)(uintptr_t)world_thunk, 1);
    if (!ok) {
        /* Installed observers are inert; original game calls still execute. */
        free(g_data);
        g_data = NULL;
    }
    log_line("hd_ships: armed step=%u ready=%d memory=%u", g_requested, ok, (unsigned)bytes);
}
