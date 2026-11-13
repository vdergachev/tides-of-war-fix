#include "smooth_radar.h"
#include "patch.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

/* Supported EXE, IDA read-only: every site is a five-byte direct CALL.
 * Only the displaced callees execute game code. Observers only copy memory. */
#define RADAR_CALL          0x42b583
#define RADAR_PANEL_CALL    0x42b81d
#define RADAR_DRAW          0x46edc0
#define TERRAIN_CALL        0x48f08d
#define TERRAIN_DRAW        0x46f0f0
#define MOBILE_PROJECT_CALL 0x4906fa
#define PORT_PROJECT_CALL   0x490c44
#define PROJECT_POINT       0x4900c0
#define MOBILE_CALL         0x4908f1
#define MOBILE_DRAW         0x491000
#define PORT_CALL           0x490cb2
#define PORT_DRAW           0x491310
#define MOBILE_BLIT         0x4912b3
#define PORT_BLIT           0x491434
#define SURFACE_BLIT        0x4179e0
#define WIND_CALL           0x46ec5c
#define WIND_DRAW           0x43ec30
#define HEADING_CALL        0x46ed98
/* Pointer circle globals and fields in CrowsNest/ship, verified in 0x46EA20. */
#ifndef POINTER_RADIUS_ADDRESS
#define POINTER_RADIUS_ADDRESS 0x4cfae8
#endif
#ifndef POINTER_CENTRE_ADDRESS
#define POINTER_CENTRE_ADDRESS 0x4cfaf0
#endif
#ifndef NIGHT_ADDRESS
#define NIGHT_ADDRESS 0x4cdd70
#endif
#define RADAR_WIND_ANGLE     0x48
#define RADAR_WIND_WIDTH     0xf4
#define SHIP_HEADING_SOURCE  0x10
#define HEADING_SOURCE_FLAG  0x0a
#define HEADING_SOURCE_ANGLE 0x10
#define POSE_HEADING         0x18
#ifndef FRAME_ADDRESS
#define FRAME_ADDRESS 0x4cde34
#endif
#ifndef TABLE_ADDRESS
#define TABLE_ADDRESS 0x4cddd8
#endif
#ifndef RADAR_ADDRESS
#define RADAR_ADDRESS 0x4cde10
#endif
#define RADAR_VIEW 0x5c
#ifndef WORLD_WIDTH
#define WORLD_WIDTH 0x4cf2e8
#endif
#ifndef WORLD_HEIGHT
#define WORLD_HEIGHT 0x4cf2ec
#endif
#ifndef RADAR_RECT
#define RADAR_RECT 0x4cfad0
#endif
#ifndef TILE_COUNT
#define TILE_COUNT 0x4cfb08
#endif
#define DIB_WIDTH        8
#define DIB_HEIGHT       0x0c
#define DIB_BPP          0x42
#define DIB_BITS         0x60
#define DIB_PITCH        0x64
#define TABLE_ARRAY      0xa40
#define TABLE_COUNT      0xa44
#define SHIP_POSE        0x28
#define SHIP_POINT       0x4c
#define CAMERA_FOLLOW    0x48
#define CAMERA_CENTRE    0x38
#define CAMERA_RECT      8
#define CAMERA_SECONDARY 0x18
#define OBJECT_LIMIT     4096

/* First four calls are compass art; the remainder are frame/night art.
 * The last call draws the heading pointer; wind uses SpriteFrameColorKey. */
static const DWORD OVERLAY_CALLS[] = {0x48f131, 0x48f1ac, 0x48f227, 0x48f2a2, 0x48f348,
                                      0x48f3e2, 0x48f47e, 0x48f505, 0x48f58b, 0x48f615,
                                      0x48f6a3, 0x48f726, 0x48f7b4, 0x48f83b, 0x46ed98};
static DWORD g_radar_fn = RADAR_DRAW;
static radar_capture g_capture;
static radar_capture g_last;
/* Latest original radar pixels can come from a partial draw as well. */
static WORD g_last_final[RADAR_PIXELS];
static uintptr_t g_table;
static uintptr_t g_radar;
static BYTE *g_last_frame;
static int g_redrawn;
static int g_attempted;
static int g_have_final;
static int g_pointer_failed;
static LONG g_epoch;
static LONG g_reused;
static LONG g_partial;
static LONG g_draws;
static LONG g_no_terrain;
static LONG g_no_compass;
static LONG g_no_frame;
static LONG g_reasons[RADAR_REASON_COUNT];
static DWORD g_terrain_fn = TERRAIN_DRAW;
static DWORD g_project_fn = PROJECT_POINT;
static DWORD g_mobile_fn = MOBILE_DRAW;
static DWORD g_port_fn = PORT_DRAW;
static DWORD g_blit_fn = SURFACE_BLIT;
static DWORD g_wind_fn = WIND_DRAW;
static int g_enabled;
static int g_pointers_enabled = 1;
static int g_ready;
static int g_complete;
static int g_terrain;
static unsigned g_frame_calls;
static unsigned g_compass_calls;
static scene_record *g_record;
static BYTE *g_frame;
static radar_marker g_pending;
static radar_marker g_admitted;
static int g_has_pending;
static int g_has_admitted;
static WORD g_replay[RADAR_PIXELS];
static LONG g_captured;
static LONG g_fallback;
static LONG g_pointer_captured;
static LONG g_pointer_fallback;
static LONG g_pose_unavailable;
static DWORD g_read_site;

typedef struct {
    uintptr_t ptr;
    unsigned id;
} mobile_entry;

static mobile_entry g_mobiles[OBJECT_LIMIT];
static unsigned g_mobile_count;
static int g_mobile_lookup_ready;

void smooth_radar_configure(const char *path)
{
    g_enabled = GetPrivateProfileIntA("smooth_radar", "enabled", 1, path) != 0;
    g_pointers_enabled = GetPrivateProfileIntA("smooth_radar", "pointers", 1, path) != 0;
    log_line("[smooth_radar] enabled=%d pointers=%d", g_enabled, g_pointers_enabled);
}

void smooth_radar_begin(scene_record *record, BYTE *frame)
{
    g_record = record;
    g_frame = frame;
    memset(&record->radar, 0, sizeof(record->radar));
    g_redrawn = 0;
    g_attempted = 0;
}

void smooth_radar_reset(void)
{
    g_record = NULL;
    g_complete = 0;
    g_last.valid = 0;
}

/* Capture the actual radar draw independently of step/scene boundaries. */
static void __stdcall radar_enter(const DWORD *saved)
{
    g_frame = (BYTE *)saved[11];
    memset(&g_capture, 0, sizeof(g_capture));
    g_complete = g_enabled && g_ready;
    if (!g_complete) {
        return;
    }
    g_attempted = 1;
    g_redrawn = 0;
    g_have_final = 0;
    g_pointer_failed = 0;
    g_terrain = 0;
    g_frame_calls = 0;
    g_compass_calls = 0;
    g_has_pending = 0;
    g_has_admitted = 0;
    g_mobile_lookup_ready = 0;
}

static int frame_pixels(WORD *pixels)
{
    if (!g_frame || g_frame != *(BYTE **)FRAME_ADDRESS || *(int *)(g_frame + DIB_WIDTH) != SCENE_WIDTH ||
        *(int *)(g_frame + DIB_HEIGHT) != SCENE_HEIGHT || *(WORD *)(g_frame + DIB_BPP) != 16) {
        return 0;
    }
    const BYTE *bits = *(const BYTE **)(g_frame + DIB_BITS);
    int pitch = *(int *)(g_frame + DIB_PITCH);
    if (!bits || pitch < -65536 || pitch > 65536 || abs(pitch) < SCENE_WIDTH * 2 || !GdiFlush()) {
        return 0;
    }
    for (int y = 0; y < RADAR_SIDE; ++y) {
        memcpy(pixels + y * RADAR_SIDE, bits + y * pitch, RADAR_SIDE * sizeof(WORD));
    }
    return 1;
}

static int compare_mobile(const void *left, const void *right)
{
    const mobile_entry *a = left;
    const mobile_entry *b = right;
    if (a->ptr != b->ptr) {
        return a->ptr > b->ptr ? 1 : -1;
    }
    return (a->id > b->id) - (a->id < b->id);
}

static void build_mobile_lookup(void)
{
    g_mobile_count = 0;
    const BYTE *table = *(const BYTE **)TABLE_ADDRESS;
    unsigned count = *(const unsigned *)(table + TABLE_COUNT);
    if (count <= OBJECT_LIMIT) {
        const uintptr_t *objects = *(const uintptr_t **)(table + TABLE_ARRAY);
        for (unsigned i = 0; i < count; ++i) {
            g_mobiles[i].ptr = objects[i];
            g_mobiles[i].id = i;
        }
        g_mobile_count = count;
        qsort(g_mobiles, count, sizeof(*g_mobiles), compare_mobile);
    }
    g_mobile_lookup_ready = 1;
}

static unsigned mobile_id(uintptr_t ptr)
{
    if (!g_mobile_lookup_ready) {
        build_mobile_lookup();
    }
    unsigned first = 0;
    unsigned last = g_mobile_count;
    while (first < last) {
        unsigned middle = first + (last - first) / 2;
        if (g_mobiles[middle].ptr < ptr) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    /* Sorting equal pointers by ID preserves the original first-cell match. */
    if (first < g_mobile_count && g_mobiles[first].ptr == ptr) {
        return g_mobiles[first].id;
    }
    return UINT_MAX;
}

/* The integer point is the game's authoritative projection input. Pose is
 * optional: dead/hidden objects need not have readable simulation storage. */
static void mobile_position(radar_marker *marker, POINT point)
{
    marker->x = point.x;
    marker->y = point.y;
    __try {
        const BYTE *object = (const BYTE *)marker->ptr;
        const double *pose = *(const double *const *)(object + SHIP_POSE);
        if (pose && isfinite(pose[0]) && isfinite(pose[1]) && fabs(pose[0] - point.x) <= .500001 &&
            fabs(pose[1] - point.y) <= .500001) {
            marker->x = pose[0];
            marker->y = pose[1];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_pose_unavailable);
    }
}

static void refresh_mobile(radar_marker *marker)
{
    if (marker->kind) {
        return;
    }
    __try {
        const BYTE *table = *(const BYTE **)TABLE_ADDRESS;
        unsigned count = *(const unsigned *)(table + TABLE_COUNT);
        const uintptr_t *objects = *(const uintptr_t **)(table + TABLE_ARRAY);
        if (count <= OBJECT_LIMIT && marker->id < count && objects[marker->id] == marker->ptr) {
            const BYTE *object = (const BYTE *)marker->ptr;
            mobile_position(marker, *(const POINT *)(object + SHIP_POINT));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_pose_unavailable);
    }
}

static int viewport(radar_capture *out, const BYTE *radar)
{
    const BYTE *camera = radar + RADAR_VIEW;
    RECT secondary = *(const RECT *)(camera + CAMERA_SECONDARY);
    RECT screen = *(const RECT *)RADAR_RECT;
    out->world = *(const RECT *)(camera + CAMERA_RECT);
    int world_width = *(int *)WORLD_WIDTH;
    int world_height = *(int *)WORLD_HEIGHT;
    if (secondary.right > secondary.left || out->world.left <= 0 || out->world.top <= 0 ||
        out->world.right >= world_width || out->world.bottom >= world_height) {
        out->failure = RADAR_SEAM;
    }
    if (radar != *(const BYTE **)RADAR_ADDRESS || screen.left || screen.top || screen.right != RADAR_SIDE ||
        screen.bottom != RADAR_SIDE || *(int *)TILE_COUNT != 42 || secondary.right > secondary.left ||
        world_width <= 0 || world_width > 100000000 || world_height <= 0 || world_height > 100000000 ||
        out->world.left <= 0 || out->world.top <= 0 || out->world.right <= out->world.left ||
        out->world.bottom <= out->world.top || out->world.right >= world_width ||
        out->world.bottom >= world_height || out->world.right - out->world.left != RADAR_SIDE * 21 ||
        out->world.bottom - out->world.top != RADAR_SIDE * 21) {
        return 0;
    }
    out->tile.x = out->world.left >> 6;
    out->tile.y = out->world.top >> 6;
    out->left = out->world.left;
    out->top = out->world.top;
    out->follow_id = *(const WORD *)(camera + CAMERA_FOLLOW);
    const BYTE *table = *(const BYTE **)TABLE_ADDRESS;
    unsigned count = *(const unsigned *)(table + TABLE_COUNT);
    if (count > OBJECT_LIMIT || out->follow_id >= count) {
        return 0;
    }
    const BYTE *ship = (*(const BYTE *const *const *)(table + TABLE_ARRAY))[out->follow_id];
    out->follow = (uintptr_t)ship;
    const POINT *point = (const POINT *)(ship + SHIP_POINT);
    const POINT *centre = (const POINT *)(camera + CAMERA_CENTRE);
    const double *pose = *(const double *const *)(ship + SHIP_POSE);
    if (!isfinite(pose[0]) || !isfinite(pose[1])) {
        return 0;
    }
    if (point->x == centre->x && fabs(pose[0] - point->x) <= .500001) {
        out->left += pose[0] - point->x;
    }
    if (point->y == centre->y && fabs(pose[1] - point->y) <= .500001) {
        out->top += pose[1] - point->y;
    }
    out->centre_x = out->left + RADAR_SIDE * 21.0 / 2;
    out->centre_y = out->top + RADAR_SIDE * 21.0 / 2;
    return 1;
}

static void __stdcall terrain_enter(const DWORD *saved)
{
    if (!g_complete) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        g_capture.failure = RADAR_VIEWPORT;
        g_complete =
            !g_terrain && (BYTE *)saved[11] == g_frame && viewport(&g_capture, (const BYTE *)saved[6]);
        if (g_complete) {
            g_capture.failure = RADAR_OK;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_read_site = TERRAIN_CALL;
        g_capture.failure = RADAR_READ;
        g_complete = 0;
    }
    SetLastError(error);
}

static void __stdcall terrain_leave(const DWORD *saved)
{
    (void)saved;
    if (!g_complete) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        g_complete = frame_pixels(g_capture.terrain);
        if (!g_complete) {
            g_read_site = TERRAIN_CALL;
            g_capture.failure = RADAR_READ;
        }
        memcpy(g_replay, g_capture.terrain, sizeof(g_replay));
        g_terrain = g_complete;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_read_site = TERRAIN_CALL;
        g_capture.failure = RADAR_READ;
        g_complete = 0;
    }
    SetLastError(error);
}

/* Projection is stdcall with three stack arguments, mutating the input POINT.
 * ESI is the owner at both verified callers; record world coordinates before it. */
static void __stdcall project_enter(const DWORD *saved)
{
    g_has_pending = 0;
    if (!g_complete || !g_terrain) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        unsigned kind = saved[10] == MOBILE_PROJECT_CALL + 5 ? 0 : 1;
        const BYTE *object = (const BYTE *)saved[1];
        memset(&g_pending, 0, sizeof(g_pending));
        g_pending.ptr = (uintptr_t)object;
        g_pending.kind = kind;
        g_pending.world = *(const POINT *)saved[11];
        g_pending.x = g_pending.world.x;
        g_pending.y = g_pending.world.y;
        g_pending.id = kind ? saved[24] : 0;
        /* Do not inspect pose before the game's projection/visibility checks. */
        g_has_pending = g_pending.id != UINT_MAX && fabs(g_pending.x) < 1e8 && fabs(g_pending.y) < 1e8;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_read_site = saved[10] - 5;
        g_capture.failure = RADAR_READ;
        g_complete = 0;
    }
    SetLastError(error);
}

static void __stdcall marker_enter(const DWORD *saved)
{
    g_has_admitted = 0;
    if (!g_complete || !g_terrain) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        unsigned kind = saved[10] == MOBILE_CALL + 5 ? 0 : 1;
        RECT clip = *(const RECT *)saved[12];
        if ((BYTE *)saved[11] != g_frame || clip.left || clip.top || clip.right != RADAR_SIDE ||
            clip.bottom != RADAR_SIDE || (saved[15] & 255) || !g_has_pending || g_pending.kind != kind ||
            g_pending.ptr != saved[1]) {
            g_capture.failure = RADAR_ART;
            g_complete = 0;
            __leave;
        }
        g_admitted = g_pending;
        /* This call is reached only after the game's visibility check. */
        if (!kind) {
            g_admitted.id = mobile_id(g_admitted.ptr);
            if (g_admitted.id == UINT_MAX) {
                g_capture.failure = RADAR_IDENTITY;
                g_complete = 0;
                __leave;
            }
            mobile_position(&g_admitted, g_admitted.world);
        }
        g_admitted.projected = *(const POINT *)saved[13];
        g_has_admitted = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_read_site = saved[10] - 5;
        g_capture.failure = RADAR_READ;
        g_complete = 0;
    }
    SetLastError(error);
}

/* A bad optional angle must not invalidate the terrain/marker capture. */
static void capture_pointer_angle(radar_pointer *out, unsigned index)
{
    __try {
        const BYTE *radar = *(const BYTE **)RADAR_ADDRESS;
        out->centre = *(const POINT *)POINTER_CENTRE_ADDRESS;
        out->radius = *(const double *)POINTER_RADIUS_ADDRESS;
        if (*(const BYTE *)NIGHT_ADDRESS) {
            out->radius -= *(const int *)(radar + RADAR_WIND_WIDTH);
        }
        out->follow = g_capture.follow;
        out->follow_id = g_capture.follow_id;
        if (!out->follow) {
            return;
        }
        if (index == RADAR_WIND) {
            unsigned half_degrees = *(const unsigned *)(radar + RADAR_WIND_ANGLE);
            if (half_degrees >= 720) {
                return;
            }
            out->angle = half_degrees * .5;
        } else {
            const BYTE *ship = (const BYTE *)out->follow;
            const BYTE *source = *(const BYTE *const *)(ship + SHIP_HEADING_SOURCE);
            if (source && !source[HEADING_SOURCE_FLAG]) {
                out->angle = *(const double *)(source + HEADING_SOURCE_ANGLE);
            } else {
                const BYTE *pose = *(const BYTE *const *)(ship + SHIP_POSE);
                out->angle = *(const double *)(pose + POSE_HEADING);
            }
        }
        out->valid = g_pointers_enabled && radar_pointer_rect_valid(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->valid = 0;
    }
}

static int capture_blit(const DWORD *saved)
{
    const DWORD *args = saved + 11;
    const BYTE *dib = (const BYTE *)saved[6];
    int width = *(const int *)args[4];
    int height = *(const int *)args[5];
    int sx = *(const int *)args[6];
    int sy = *(const int *)args[7];
    int dx = *(const int *)args[8];
    int dy = *(const int *)args[9];
    if ((BYTE *)args[0] != g_frame || width <= 0 || height <= 0 || width > RADAR_SIDE ||
        height > RADAR_SIDE || sx < 0 || sy < 0 || sx > RADAR_SIDE || sy > RADAR_SIDE || dx < 0 || dy < 0 ||
        dx > RADAR_SIDE || dy > RADAR_SIDE || dx + width > RADAR_SIDE || dy + height > RADAR_SIDE ||
        sx + width > *(int *)(dib + DIB_WIDTH) || sy + height > *(int *)(dib + DIB_HEIGHT) ||
        *(WORD *)(dib + DIB_BPP) != 16) {
        return 0;
    }
    const BYTE *bits = *(const BYTE *const *)(dib + DIB_BITS);
    int pitch = *(int *)(dib + DIB_PITCH);
    if (!bits || pitch < -65536 || pitch > 65536 || abs(pitch) < (sx + width) * 2) {
        return 0;
    }
    WORD key = (WORD)((*(const BYTE *)args[3] >> 3) | (8 * (*(const BYTE *)args[2] & 0xfc)) |
                      (256 * (*(const BYTE *)args[1] & 0xf8)));
    unsigned call = saved[10] - 5;
    int marker = call == MOBILE_BLIT || call == PORT_BLIT;
    int compass = call <= OVERLAY_CALLS[3] && call >= OVERLAY_CALLS[0];
    int pointer = call == HEADING_CALL || call == WIND_CALL;
    radar_capture *out = &g_capture;
    radar_marker *art = NULL;
    radar_pointer pointer_art;
    radar_pointer *arrow = NULL;
    unsigned pointer_mask = 0;
    if (marker) {
        if (!g_has_admitted || width != RADAR_ART_SIDE || height != RADAR_ART_SIDE ||
            out->count == RADAR_MARKERS || g_admitted.kind != (unsigned)(call == PORT_BLIT)) {
            out->failure = out->count == RADAR_MARKERS ? RADAR_OVERFLOW : RADAR_ART;
            return 0;
        }
        art = &out->markers[out->count++];
        *art = g_admitted;
        art->source = (uintptr_t)dib;
        SetRect(&art->source_rect, sx, sy, sx + width, sy + height);
        SetRect(&art->dest, dx, dy, dx + width, dy + height);
        art->key = key;
        g_has_admitted = 0;
    } else {
        if (key != 0x07e0) {
            return 0;
        }
        g_compass_calls += compass;
        g_frame_calls += !compass && call != OVERLAY_CALLS[14] && call != WIND_CALL;
        if (pointer && width == RADAR_POINTER_SIDE && height == RADAR_POINTER_SIDE &&
            *(int *)(dib + DIB_HEIGHT) == RADAR_POINTER_SIDE &&
            *(int *)(dib + DIB_WIDTH) == (call == WIND_CALL ? 65 : RADAR_POINTER_SIDE)) {
            unsigned index = call == WIND_CALL ? RADAR_WIND : RADAR_HEADING;
            arrow = &pointer_art;
            memset(arrow, 0, sizeof(*arrow));
            arrow->present = 1;
            arrow->source = (uintptr_t)dib;
            arrow->frame = (unsigned)sx / RADAR_POINTER_SIDE;
            SetRect(&arrow->source_rect, sx, sy, sx + width, sy + height);
            SetRect(&arrow->dest, dx, dy, dx + width, dy + height);
            capture_pointer_angle(arrow, index);
            pointer_mask = RADAR_WIND_MASK << index;
        }
        if (pointer && !arrow) {
            g_pointer_failed = 1;
            return 0;
        }
    }
    for (int y = 0; y < height; ++y) {
        const WORD *row = (const WORD *)(bits + (sy + y) * pitch) + sx;
        for (int x = 0; x < width; ++x) {
            WORD pixel = row[x];
            int p = (dy + y) * RADAR_SIDE + dx + x;
            if (art) {
                art->pixels[y * width + x] = pixel;
            }
            if (arrow) {
                arrow->pixels[y * width + x] = pixel;
            }
            if (pixel != key) {
                g_replay[p] = pixel;
                if (!marker) {
                    if (compass) {
                        out->compass_mask[p] = 1;
                        out->compass[p] = pixel;
                    } else if (arrow) {
                        if (!arrow->valid) {
                            out->overlay_mask[p] |= (BYTE)pointer_mask;
                        }
                    } else {
                        out->overlay_mask[p] |= RADAR_FRAME_MASK;
                        out->overlay[p] = pixel;
                    }
                }
            }
        }
    }
    if (arrow) {
        out->pointers[call == WIND_CALL ? RADAR_WIND : RADAR_HEADING] = *arrow;
    }
    return 1;
}

static void __stdcall blit_enter(const DWORD *saved)
{
    int pointer = saved[10] == HEADING_CALL + 5;
    if ((!g_complete && !pointer) || !g_terrain || !g_attempted) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        int was_complete = g_complete;
        unsigned failure = g_capture.failure;
        g_capture.failure = RADAR_ART;
        int captured = capture_blit(saved);
        if (pointer && !captured) {
            g_pointer_failed = 1;
        }
        g_complete = was_complete && captured;
        if (g_complete) {
            g_capture.failure = RADAR_OK;
        } else if (!was_complete) {
            g_capture.failure = failure;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pointer_failed |= pointer;
        g_read_site = saved[10] - 5;
        g_capture.failure = RADAR_READ;
        g_complete = 0;
    }
    SetLastError(error);
}

/* SpriteFrameColorKey embeds a CDib at +4 and offsets source X by frame width.
 * Convert its eleven arguments to the verified ten-argument blit observation. */
static void __stdcall wind_enter(const DWORD *saved)
{
    if (!g_terrain || !g_attempted) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        DWORD translated[21];
        memcpy(translated, saved, 11 * sizeof(DWORD));
        const DWORD *sprite = (const DWORD *)saved[6];
        unsigned frame = *(const unsigned *)saved[11];
        unsigned sx = *(const unsigned *)saved[18] + frame * sprite[27];
        if (!sprite[29] || frame >= sprite[30] || sprite[27] > 128 || sx > 1024) {
            g_pointer_failed = 1;
            g_capture.failure = RADAR_ART;
            g_complete = 0;
            __leave;
        }
        translated[6] += 4;
        memcpy(translated + 11, saved + 12, 10 * sizeof(DWORD));
        translated[17] = (DWORD)&sx;
        int was_complete = g_complete;
        unsigned failure = g_capture.failure;
        g_capture.failure = RADAR_ART;
        int captured = capture_blit(translated);
        if (!captured) {
            g_pointer_failed = 1;
        }
        g_complete = was_complete && captured;
        if (g_complete) {
            g_capture.failure = RADAR_OK;
        } else if (!was_complete) {
            g_capture.failure = failure;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pointer_failed = 1;
        g_read_site = WIND_CALL;
        g_capture.failure = RADAR_READ;
        g_complete = 0;
    }
    SetLastError(error);
}

/* Validate before tooltip/UI painting, at the end of the actual radar draw. */
static void __stdcall radar_leave(const DWORD *saved)
{
    (void)saved;
    InterlockedIncrement(&g_draws);
    if (!g_terrain) {
        InterlockedIncrement(&g_no_terrain);
    }
    if (g_compass_calls != 4) {
        InterlockedIncrement(&g_no_compass);
    }
    if (!g_frame_calls) {
        InterlockedIncrement(&g_no_frame);
    }
    DWORD error = GetLastError();
    __try {
        g_have_final = frame_pixels(g_capture.final);
        if (!g_have_final) {
            g_read_site = RADAR_DRAW;
            g_capture.failure = RADAR_READ;
        } else {
            memcpy(g_last_final, g_capture.final, sizeof(g_last_final));
        }
        if (g_complete && g_terrain && g_frame_calls && g_compass_calls == 4 && g_have_final) {
            g_capture.failure = RADAR_REPLAY;
            if (!memcmp(g_replay, g_capture.final, sizeof(g_replay))) {
                g_capture.valid = 1;
                g_capture.failure = RADAR_OK;
                radar_remember(&g_capture, &g_last);
                g_last = g_capture;
                g_table = *(uintptr_t *)TABLE_ADDRESS;
                g_radar = *(uintptr_t *)RADAR_ADDRESS;
                g_last_frame = g_frame;
                g_redrawn = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_read_site = RADAR_DRAW;
        g_capture.failure = RADAR_READ;
    }
    g_complete = 0;
    SetLastError(error);
}

static void publish_radar(void)
{
    radar_capture *out = &g_record->radar;
    if (g_table != *(uintptr_t *)TABLE_ADDRESS || g_radar != *(uintptr_t *)RADAR_ADDRESS ||
        g_last_frame != g_frame) {
        g_last.valid = 0;
    }
    if (!g_record->valid || !g_enabled || !g_ready) {
        g_last.valid = 0;
        out->failure = RADAR_INCOMPLETE;
        return;
    }
    if (g_attempted && !g_redrawn && g_pointer_failed) {
        /* Unknown pixels cannot safely inherit an earlier moving pointer. */
        g_last.valid = 0;
        out->failure = g_capture.failure ? g_capture.failure : RADAR_ART;
        return;
    }
    if (g_attempted && !g_redrawn && g_capture.failure && g_capture.failure != RADAR_READ &&
        g_capture.failure != RADAR_ART) {
        out->failure = g_capture.failure;
        return;
    }
    if (!radar_reuse(out, &g_last)) {
        out->failure = g_attempted && g_capture.failure ? g_capture.failure : RADAR_INCOMPLETE;
        return;
    }
    if (g_attempted && !g_redrawn) {
        radar_reuse_pointers(out, &g_capture);
    }
    POINT tile = out->tile;
    uintptr_t follow = out->follow;
    unsigned follow_id = out->follow_id;
    if (!viewport(out, (const BYTE *)g_radar)) {
        out->valid = 0;
        if (!out->failure) {
            out->failure = RADAR_VIEWPORT;
        }
        return;
    }
    if (follow != out->follow || follow_id != out->follow_id) {
        out->valid = 0;
        out->failure = RADAR_IDENTITY;
        return;
    }
    /* Terrain stays anchored to the tiles actually captured. */
    out->tile = tile;
    /* Radar objects extend beyond the 3D scene. Refresh every admitted identity
     * from the same current game state as the viewport, including offscreen foes. */
    for (unsigned i = 0; i < out->count; ++i) {
        refresh_mobile(&out->markers[i]);
    }
    for (unsigned i = 0; i < out->history_count; ++i) {
        refresh_mobile(&out->history[i]);
    }
    g_last = *out;
    radar_mask_occlusion(out, g_last_final, g_record->pixels[SCENE_C], SCENE_WIDTH);
    if (!g_redrawn) {
        InterlockedIncrement(&g_reused);
        if (g_attempted) {
            InterlockedIncrement(&g_partial);
        }
    }
}

void smooth_radar_finish(LONG epoch)
{
    if (!g_record) {
        return;
    }
    DWORD error = GetLastError();
    if (epoch != g_epoch && !g_redrawn) {
        g_last.valid = 0;
    }
    g_epoch = epoch;
    __try {
        publish_radar();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_record->radar.valid = 0;
        g_record->radar.failure = RADAR_READ;
        g_last.valid = 0;
    }
    radar_capture *out = &g_record->radar;
    for (unsigned i = 0; i < RADAR_POINTERS; ++i) {
        InterlockedIncrement(out->valid && out->pointers[i].valid ? &g_pointer_captured
                                                                  : &g_pointer_fallback);
    }
    InterlockedIncrement(out->valid ? &g_captured : &g_fallback);
    if (!out->valid && out->failure < RADAR_REASON_COUNT) {
        InterlockedIncrement(&g_reasons[out->failure]);
    }
    g_record = NULL;
    g_complete = 0;
    SetLastError(error);
}

/* PUSHAD: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX,flags,bridge return,
 * thunk return,args. Preserve GPRs, flags, LastError and x87/SSE. */
// clang-format off
#define RADAR_BRIDGE(name, observer) \
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
RADAR_BRIDGE(radar_enter_bridge, radar_enter)
RADAR_BRIDGE(radar_leave_bridge, radar_leave)
RADAR_BRIDGE(terrain_enter_bridge, terrain_enter)
RADAR_BRIDGE(terrain_leave_bridge, terrain_leave)
RADAR_BRIDGE(project_bridge, project_enter)
RADAR_BRIDGE(marker_bridge, marker_enter)
RADAR_BRIDGE(blit_bridge, blit_enter)
RADAR_BRIDGE(wind_bridge, wind_enter)

/* Three thiscall arguments, copied from the moving ESP+12 slot.
 * Preserve the original return and result registers/flags. */
__declspec(naked) static void radar_thunk(void)
{
    __asm {
        call radar_enter_bridge
        push [esp + 12]
        push [esp + 12]
        push [esp + 12]
        call dword ptr [g_radar_fn]
        call radar_leave_bridge
        ret 12
    }
}
/* One argument copied for the original thiscall; RET 4 consumes the original. */
__declspec(naked) static void terrain_thunk(void)
{
    __asm {
        call terrain_enter_bridge
        push [esp + 4]
        call dword ptr [g_terrain_fn]
        call terrain_leave_bridge
        ret 4
    }
}
/* Tail thunks leave thiscall ECX, stack arguments and original return intact. */
__declspec(naked) static void project_thunk(void)
{
    __asm {
        call project_bridge
        jmp dword ptr [g_project_fn]
    }
}
__declspec(naked) static void mobile_thunk(void)
{
    __asm {
        call marker_bridge
        jmp dword ptr [g_mobile_fn]
    }
}
__declspec(naked) static void port_thunk(void)
{
    __asm {
        call marker_bridge
        jmp dword ptr [g_port_fn]
    }
}
__declspec(naked) static void blit_thunk(void)
{
    __asm {
        call blit_bridge
        jmp dword ptr [g_blit_fn]
    }
}
__declspec(naked) static void wind_thunk(void)
{
    __asm {
        call wind_bridge
        jmp dword ptr [g_wind_fn]
    }
}

// clang-format on

typedef struct {
    DWORD site;
    DWORD target;
    void (*thunk)(void);
} radar_hook;

static void call_bytes(BYTE *out, DWORD site, DWORD target)
{
    out[0] = 0xe8;
    DWORD relative = target - site - 5;
    memcpy(out + 1, &relative, sizeof(relative));
}

void smooth_radar_install(void)
{
    if (!g_enabled) {
        return;
    }
    const radar_hook hooks[] = {{RADAR_CALL, RADAR_DRAW, radar_thunk},
                                {RADAR_PANEL_CALL, RADAR_DRAW, radar_thunk},
                                {TERRAIN_CALL, TERRAIN_DRAW, terrain_thunk},
                                {MOBILE_PROJECT_CALL, PROJECT_POINT, project_thunk},
                                {PORT_PROJECT_CALL, PROJECT_POINT, project_thunk},
                                {MOBILE_CALL, MOBILE_DRAW, mobile_thunk},
                                {PORT_CALL, PORT_DRAW, port_thunk},
                                {MOBILE_BLIT, SURFACE_BLIT, blit_thunk},
                                {PORT_BLIT, SURFACE_BLIT, blit_thunk},
                                {WIND_CALL, WIND_DRAW, wind_thunk}};
    BYTE expected[5];
    /* Preflight the entire set before mutation; partial installation stays inert. */
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(*hooks); ++i) {
        call_bytes(expected, hooks[i].site, hooks[i].target);
        if (!patch_check((void *)(uintptr_t)hooks[i].site, expected, sizeof(expected))) {
            log_line("smooth_radar: unexpected hook bytes site=%08lx", hooks[i].site);
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(OVERLAY_CALLS) / sizeof(*OVERLAY_CALLS); ++i) {
        call_bytes(expected, OVERLAY_CALLS[i], SURFACE_BLIT);
        if (!patch_check((void *)(uintptr_t)OVERLAY_CALLS[i], expected, sizeof(expected))) {
            log_line("smooth_radar: unexpected overlay bytes site=%08lx", OVERLAY_CALLS[i]);
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(*hooks); ++i) {
        call_bytes(expected, hooks[i].site, hooks[i].target);
        if (!patch_call((void *)(uintptr_t)hooks[i].site, expected, (void *)(uintptr_t)hooks[i].thunk)) {
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(OVERLAY_CALLS) / sizeof(*OVERLAY_CALLS); ++i) {
        call_bytes(expected, OVERLAY_CALLS[i], SURFACE_BLIT);
        if (!patch_call((void *)(uintptr_t)OVERLAY_CALLS[i], expected, (void *)(uintptr_t)blit_thunk)) {
            return;
        }
    }
    g_ready = 1;
    log_line("smooth_radar: capture ready markers=%u", RADAR_MARKERS);
}

void smooth_radar_log(void)
{
    if (g_ready) {
        log_line("smooth_radar: captured=%ld fallback=%ld reused=%ld partial_reused=%ld "
                 "pointer_captured=%ld pointer_fallback=%ld",
                 InterlockedExchange(&g_captured, 0), InterlockedExchange(&g_fallback, 0),
                 InterlockedExchange(&g_reused, 0), InterlockedExchange(&g_partial, 0),
                 InterlockedExchange(&g_pointer_captured, 0), InterlockedExchange(&g_pointer_fallback, 0));
        log_line("smooth_radar: draws=%ld no_terrain=%ld no_compass=%ld no_frame=%ld",
                 InterlockedExchange(&g_draws, 0), InterlockedExchange(&g_no_terrain, 0),
                 InterlockedExchange(&g_no_compass, 0), InterlockedExchange(&g_no_frame, 0));
        log_line("smooth_radar: pose_unavailable=%ld last_read_site=%08lx",
                 InterlockedExchange(&g_pose_unavailable, 0), g_read_site);
        log_line(
            "smooth_radar: capture_incomplete=%ld viewport=%ld art=%ld overflow=%ld read=%ld "
            "replay=%ld occluded=%ld identity=%ld seam=%ld",
            InterlockedExchange(&g_reasons[RADAR_INCOMPLETE], 0),
            InterlockedExchange(&g_reasons[RADAR_VIEWPORT], 0), InterlockedExchange(&g_reasons[RADAR_ART], 0),
            InterlockedExchange(&g_reasons[RADAR_OVERFLOW], 0),
            InterlockedExchange(&g_reasons[RADAR_READ], 0), InterlockedExchange(&g_reasons[RADAR_REPLAY], 0),
            InterlockedExchange(&g_reasons[RADAR_OCCLUDED], 0),
            InterlockedExchange(&g_reasons[RADAR_IDENTITY], 0),
            InterlockedExchange(&g_reasons[RADAR_SEAM], 0));
    }
}
