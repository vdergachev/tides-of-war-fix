#ifndef TOW_BALL_GEOMETRY_H
#define TOW_BALL_GEOMETRY_H
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#define BALL_LIMIT      1024
#define BALL_TRAIL      16
#define BALL_TILE       33
#define BALL_ATLAS_SIDE 32
#define BALL_ATLAS      (BALL_TILE * BALL_ATLAS_SIDE)
#define BALL_HASH_SIZE  (BALL_LIMIT * 2)

enum { BALL_BIRTH, BALL_END };

typedef struct {
    uintptr_t ptr;
    uint64_t generation;
    unsigned kind;
} scene_ball_event;

typedef struct {
    uintptr_t ptr;
    uint64_t generation;
    double x;
    double y;
    double vx;
    double vy;
    double start_x;
    double start_y;
    unsigned life;
    unsigned half_life;
    unsigned type;
} scene_ball;

/* Same sparse RGB565 palette and descending Bresenham table as 0x419030.
 * Zero-initialize the original routine's uninitialized palette base word. */
static inline unsigned ball_trail(const scene_ball *ball, double x, double y, int offset_x, int offset_y,
                                  int *px, int *py, uint16_t *colors)
{
    int head_x = (int)round(x) - offset_x;
    int head_y = (int)round(y) - offset_y;

    /* Original clamps unsigned start coordinates to the DIB bounds. */
    unsigned sx = (unsigned)((int)floor(ball->start_x + .5) - offset_x);
    unsigned sy = (unsigned)((int)floor(ball->start_y + .5) - offset_y);
    int end_x = sx >= 640 ? 639 : (int)sx;
    int end_y = sy >= 480 ? 479 : (int)sy;
    int dx = 2 * abs(end_x - head_x);
    int dy = 2 * abs(end_y - head_y);
    int step_x = end_x < head_x ? -1 : 1;
    int step_y = end_y < head_y ? -1 : 1;
    int major = dx > dy ? dx : dy;
    int minor = dx > dy ? dy : dx;
    int error = minor - major / 2;
    int length = major / 2;
    if (length > BALL_TRAIL) {
        length = BALL_TRAIL;
    }

    int phase = -abs((int)ball->life - (int)ball->half_life);
    unsigned r = (uint8_t)(10 * phase - 1);
    unsigned g = (uint8_t)(12 * phase - 61);
    unsigned b = (uint8_t)(2 * (phase + 26));
    uint16_t palette[17] = {0, 0x514a, 0, 0, 0, 0, 0x4108, 0, 0, 0, 0x2084, 0, 0x1042, 0, 0, 0x0821, 0};
    palette[16] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    unsigned count = 0;
    for (int i = length; i > 0; --i) {
        if (palette[i]) {
            px[count] = head_x;
            py[count] = head_y;
            colors[count++] = palette[i];
        }

        if (error >= 0) {
            error -= major;
            if (dx > dy) {
                head_y += step_y;
            } else {
                head_x += step_x;
            }
        }

        error += minor;
        if (dx > dy) {
            head_x += step_x;
        } else {
            head_y += step_y;
        }
    }

    return count;
}

typedef struct {
    unsigned slots[BALL_HASH_SIZE];
} ball_lookup;

static inline unsigned ball_hash(uintptr_t ptr, uint64_t generation)
{
    uint64_t key = (uint64_t)ptr ^ (generation * UINT64_C(0x9e3779b97f4a7c15));
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    return (unsigned)key & (BALL_HASH_SIZE - 1);
}

static inline void ball_index(ball_lookup *index, const scene_ball *balls, unsigned count)
{
    for (unsigned i = 0; i < BALL_HASH_SIZE; ++i) {
        index->slots[i] = 0;
    }

    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = ball_hash(balls[i].ptr, balls[i].generation);
        while (index->slots[slot]) {
            slot = (slot + 1) & (BALL_HASH_SIZE - 1);
        }
        index->slots[slot] = i + 1;
    }
}

static inline const scene_ball *ball_lookup_find(const ball_lookup *index, const scene_ball *balls,
                                                 const scene_ball *ball)
{
    unsigned slot = ball_hash(ball->ptr, ball->generation);
    while (index->slots[slot]) {
        const scene_ball *found = &balls[index->slots[slot] - 1];
        if (found->ptr == ball->ptr && found->generation == ball->generation) {
            return found;
        }

        slot = (slot + 1) & (BALL_HASH_SIZE - 1);
    }

    return NULL;
}
#endif
