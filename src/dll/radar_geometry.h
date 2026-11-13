#ifndef TOW_RADAR_GEOMETRY_H
#define TOW_RADAR_GEOMETRY_H
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#define RADAR_SIDE         128
#define RADAR_PIXELS       (RADAR_SIDE * RADAR_SIDE)
#define RADAR_MARKERS      256
#define RADAR_ART_SIDE     8
#define RADAR_POINTER_SIDE 13
#define RADAR_POINTERS     2

/* The fallback bits retain the original step-wise pointer pixels. */
enum { RADAR_FRAME_MASK = 1, RADAR_WIND_MASK = 2, RADAR_HEADING_MASK = 4, RADAR_UI_MASK = 8 };

enum { RADAR_WIND, RADAR_HEADING };

/* Full sail can move several tiles per step. Twice a generous five-tile
 * step is a teleport; the one-tile terrain quantization is not a speed limit. */
#define RADAR_MAX_STEP (5 * 64.0)
#define RADAR_TELEPORT (2 * RADAR_MAX_STEP)

enum {
    RADAR_OK,
    RADAR_SNAP,
    RADAR_COVERAGE,
    RADAR_IDENTITY,
    RADAR_SEAM,
    RADAR_INCOMPLETE,
    RADAR_REPLAY,
    RADAR_GPU,
    RADAR_VIEWPORT,
    RADAR_ART,
    RADAR_OVERFLOW,
    RADAR_READ,
    RADAR_OCCLUDED,
    RADAR_REASON_COUNT
};

typedef struct {
    uintptr_t ptr;
    unsigned id;
    unsigned kind;
    double x;
    double y;
    POINT world;
    POINT projected;
    uintptr_t source;
    RECT source_rect;
    RECT dest;
    WORD key;
    WORD pixels[RADAR_ART_SIDE * RADAR_ART_SIDE];
} radar_marker;

typedef struct {
    int present;
    int valid;
    double angle;
    double radius;
    POINT centre;
    uintptr_t follow;
    unsigned follow_id;
    unsigned frame;
    uintptr_t source;
    RECT source_rect;
    RECT dest;
    WORD pixels[RADAR_POINTER_SIDE * RADAR_POINTER_SIDE];
} radar_pointer;

typedef struct {
    int valid;
    unsigned failure;
    unsigned count;
    uintptr_t follow;
    unsigned follow_id;
    RECT world;
    POINT tile;
    double left;
    double top;
    double centre_x;
    double centre_y;
    radar_marker markers[RADAR_MARKERS];
    /* One complete draw of hidden identities, for motion pairing only. */
    unsigned history_count;
    radar_marker history[RADAR_MARKERS];
    radar_pointer pointers[RADAR_POINTERS];
    WORD terrain[RADAR_PIXELS];
    WORD final[RADAR_PIXELS];
    /* Frame pixels before pointers overwrite them in the original final C. */
    WORD overlay[RADAR_PIXELS];
    WORD compass[RADAR_PIXELS];
    BYTE compass_mask[RADAR_PIXELS];
    BYTE overlay_mask[RADAR_PIXELS];
} radar_capture;

static inline double radar_angle_delta(double before, double now)
{
    double delta = fmod(now - before, 360.0);
    if (delta > 180) {
        delta -= 360;
    }
    if (delta < -180) {
        delta += 360;
    }
    return delta;
}

static inline void radar_pointer_position(const radar_pointer *old, const radar_pointer *now, double alpha,
                                          double *x, double *y)
{
    double angle = now->angle;
    if (old && old->valid && now->valid && old->follow == now->follow && old->follow_id == now->follow_id &&
        old->radius == now->radius && old->centre.x == now->centre.x && old->centre.y == now->centre.y) {
        double delta = radar_angle_delta(old->angle, now->angle);
        if (fabs(delta) <= 90) {
            angle = old->angle + delta * alpha;
        }
    }
    double radians = angle * (3.14159265358979323846 / 180.0);
    *x = now->centre.x + now->radius * sin(radians) - RADAR_POINTER_SIDE / 2;
    *y = now->centre.y - now->radius * cos(radians) - RADAR_POINTER_SIDE / 2;
}

static inline int radar_pointer_rect_valid(const radar_pointer *pointer)
{
    if (!isfinite(pointer->angle) || !isfinite(pointer->radius) || pointer->radius <= 0 ||
        pointer->radius > RADAR_SIDE / 2 || pointer->dest.right - pointer->dest.left != RADAR_POINTER_SIDE ||
        pointer->dest.bottom - pointer->dest.top != RADAR_POINTER_SIDE) {
        return 0;
    }
    double x;
    double y;
    radar_pointer_position(NULL, pointer, 1, &x, &y);
    return fabs(x - pointer->dest.left) <= 1 && fabs(y - pointer->dest.top) <= 1;
}

/* Replace both geometry and fallback pixels when a partial draw observed a
 * fresh pointer. A skipped draw deliberately keeps the last displayed angle. */
static inline void radar_reuse_pointers(radar_capture *out, const radar_capture *fresh)
{
    for (unsigned i = 0; i < RADAR_POINTERS; ++i) {
        const radar_pointer *pointer = &fresh->pointers[i];
        if (!pointer->present) {
            continue;
        }
        unsigned mask = RADAR_WIND_MASK << i;
        for (unsigned p = 0; p < RADAR_PIXELS; ++p) {
            out->overlay_mask[p] &= (BYTE)~mask;
        }
        out->pointers[i] = *pointer;
        if (!pointer->valid) {
            for (unsigned y = 0; y < RADAR_POINTER_SIDE; ++y) {
                for (unsigned x = 0; x < RADAR_POINTER_SIDE; ++x) {
                    if (pointer->pixels[y * RADAR_POINTER_SIDE + x] != 0x07e0) {
                        unsigned p = (pointer->dest.top + y) * RADAR_SIDE + pointer->dest.left + x;
                        out->overlay_mask[p] |= (BYTE)mask;
                    }
                }
            }
        }
    }
}

/* Refresh positions without changing the captured projection anchor or art. */
static inline void radar_update_marker(radar_capture *capture, uintptr_t ptr, unsigned id, double x, double y)
{
    if (!isfinite(x) || !isfinite(y) || fabs(x) >= 1e8 || fabs(y) >= 1e8) {
        return;
    }
    for (unsigned i = 0; i < capture->count; ++i) {
        radar_marker *marker = &capture->markers[i];
        if (!marker->kind && marker->ptr == ptr && marker->id == id) {
            marker->x = x;
            marker->y = y;
        }
    }
}

/* Partial painting is never mixed into the last complete layer set. */
static inline int radar_reuse(radar_capture *out, const radar_capture *last)
{
    if (!last->valid) {
        return 0;
    }
    *out = *last;
    out->failure = RADAR_OK;
    return 1;
}

/* Apply only to the published copy so transient UI never enters the layer cache. */
static inline void radar_mask_occlusion(radar_capture *out, const WORD *baseline, const WORD *final,
                                        unsigned stride)
{
    for (unsigned y = 0; y < RADAR_SIDE; ++y) {
        for (unsigned x = 0; x < RADAR_SIDE; ++x) {
            unsigned p = y * RADAR_SIDE + x;
            WORD pixel = final[y * stride + x];
            if (pixel != baseline[p]) {
                out->overlay_mask[p] |= RADAR_UI_MASK;
                out->final[p] = pixel;
            }
        }
    }
}

/* All geometry consumes owned captures, never memory belonging to the game. */
static inline const radar_marker *radar_find(const radar_capture *old, const radar_marker *marker)
{
    if (old) {
        for (unsigned i = 0; i < old->count + old->history_count; ++i) {
            const radar_marker *candidate = i < old->count ? &old->markers[i] : &old->history[i - old->count];
            if (candidate->ptr == marker->ptr && candidate->id == marker->id &&
                candidate->kind == marker->kind && fabs(candidate->x - marker->x) <= RADAR_TELEPORT &&
                fabs(candidate->y - marker->y) <= RADAR_TELEPORT) {
                return candidate;
            }
        }
    }
    return NULL;
}

/* Never draw hidden objects or retain them indefinitely. Art and relation are
 * deliberately excluded from identity; their discrete changes keep motion. */
static inline void radar_remember(radar_capture *now, const radar_capture *old)
{
    now->history_count = 0;
    if (!old->valid || old->follow != now->follow || old->follow_id != now->follow_id) {
        return;
    }
    for (unsigned i = 0; i < old->count; ++i) {
        const radar_marker *marker = &old->markers[i];
        int visible = 0;
        for (unsigned j = 0; j < now->count; ++j) {
            const radar_marker *current = &now->markers[j];
            if (current->ptr == marker->ptr && current->id == marker->id && current->kind == marker->kind) {
                visible = 1;
                break;
            }
        }
        if (!visible && now->history_count < RADAR_MARKERS) {
            now->history[now->history_count++] = *marker;
        }
    }
}

static inline double radar_terrain_shift(int tile, double origin)
{
    return (tile * 64.0 - origin) * (3.0 / 64.0);
}

static inline void radar_marker_position(const radar_capture *old, const radar_capture *now, unsigned index,
                                         double alpha, double *x, double *y)
{
    const radar_marker *marker = &now->markers[index];
    const radar_marker *before = radar_find(old, marker);
    double wx = before ? before->x + (marker->x - before->x) * alpha : marker->x;
    double wy = before ? before->y + (marker->y - before->y) * alpha : marker->y;
    double left = old->left + (now->left - old->left) * alpha;
    double top = old->top + (now->top - old->top) * alpha;
    *x = (wx - left) / 21.0 + marker->dest.left - marker->projected.x;
    *y = (wy - top) / 21.0 + marker->dest.top - marker->projected.y;
}

/* Different captures must describe identical tiles in their overlapping area.
 * Reject animation/unknown painting rather than introduce a seam between them. */
static inline unsigned radar_pair_reason(const radar_capture *old, const radar_capture *now)
{
    if (!old || !now || !old->valid || !now->valid) {
        return RADAR_INCOMPLETE;
    }
    if (old->follow != now->follow || old->follow_id != now->follow_id) {
        return RADAR_IDENTITY;
    }
    if (fabs(old->left - now->left) > RADAR_TELEPORT || fabs(old->top - now->top) > RADAR_TELEPORT) {
        return RADAR_SNAP;
    }
    int dx = (now->tile.x - old->tile.x) * 3;
    int dy = (now->tile.y - old->tile.y) * 3;
    if (abs(dx) >= 126 || abs(dy) >= 126) {
        return RADAR_COVERAGE;
    }
    for (int y = 1; y < 127; ++y) {
        for (int x = 1; x < 127; ++x) {
            int ox = x + dx;
            int oy = y + dy;
            if (ox >= 1 && ox < 127 && oy >= 1 && oy < 127 &&
                now->terrain[y * RADAR_SIDE + x] != old->terrain[oy * RADAR_SIDE + ox]) {
                return RADAR_SEAM;
            }
        }
    }
    return RADAR_OK;
}

static inline int radar_pair_valid(const radar_capture *old, const radar_capture *now)
{
    return radar_pair_reason(old, now) == RADAR_OK;
}

static inline int radar_covers(const radar_capture *capture, double left, double top, int x, int y)
{
    double sx = x - radar_terrain_shift(capture->tile.x, left);
    double sy = y - radar_terrain_shift(capture->tile.y, top);
    /* Conservative half-pixel margin at every output scale. */
    return sx >= 1.5 && sx <= 126.5 && sy >= 1.5 && sy <= 126.5;
}

static inline int radar_coverage(const radar_capture *old, const radar_capture *now, double alpha)
{
    double left = old->left + (now->left - old->left) * alpha;
    double top = old->top + (now->top - old->top) * alpha;
    for (int y = 0; y < RADAR_SIDE; ++y) {
        for (int x = 0; x < RADAR_SIDE; ++x) {
            if (!now->overlay_mask[y * RADAR_SIDE + x] && !radar_covers(old, left, top, x, y) &&
                !radar_covers(now, left, top, x, y)) {
                return 0;
            }
        }
    }
    return 1;
}
#endif
