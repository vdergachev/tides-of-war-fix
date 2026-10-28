#ifndef TOW_SCENE_H
#define TOW_SCENE_H

#include <windows.h>
#include <stdint.h>
#include "diag.h"

#define SCENE_WIDTH  640
#define SCENE_HEIGHT 480
#define SCENE_PIXELS (SCENE_WIDTH * SCENE_HEIGHT)
#define SCENE_SHIPS  8
#define SCENE_RING   4
#define SCENE_CLOUDS 5
/* One transfer per ship per pass; static objects 9/10 add one each.
 * Keep capacity at 32; overflow uses the plain frame. */
#define SCENE_DRAWS         (SCENE_SHIPS * 4)
#define SCENE_SPRITE_SIZE   144
#define SCENE_SPRITE_PIXELS (SCENE_SPRITE_SIZE * SCENE_SPRITE_SIZE)

enum { SCENE_A0, SCENE_A1, SCENE_A, SCENE_B, SCENE_C, SCENE_D, SCENE_E, SCENE_LAYERS };

typedef struct {
    unsigned ticks;
    unsigned image;
    int valid;
    int width;
    int height;
    POINT dest;
    double x;
    double y;
    double vx;
    double vy;
} scene_cloud;

typedef struct {
    uintptr_t ptr;
    unsigned id;
    double x;
    double y;
    double course;
    POINT point;
    RECT box;
    double wake_stern;
} scene_ship;

/* Source copied while locked, normalized to packed rows; stride retains the
 * signed source pitch, box retains the clipped screen destination. */
typedef struct {
    unsigned ship;
    unsigned id;
    unsigned pass;
    int width;
    int height;
    int stride;
    WORD key;
    RECT box;
    WORD pixels[SCENE_SPRITE_PIXELS];
} scene_draw;

typedef struct {
    LONG epoch;
    unsigned step;
    unsigned count;
    unsigned follow;
    unsigned layers;
    int valid;
    POINT camera;
    RECT view;
    LONGLONG qpc;
    scene_ship ships[SCENE_SHIPS];
    unsigned cloud_flags;
    scene_cloud clouds[SCENE_CLOUDS];
    unsigned draw_count;
    scene_draw draws[SCENE_DRAWS];
    WORD pixels[SCENE_LAYERS][SCENE_PIXELS];
} scene_record;

#ifdef __cplusplus
extern "C" {
#endif

diag_frame *scene_diagnostic(const scene_record *record);
WORD (*scene_drawing(const scene_record *record))[SCENE_PIXELS];
int scene_init(SRWLOCK *lock);
void scene_present(HDC source, LONG epoch, LONG serial);
/* Worker owns *record; exchange returns the oldest complete record without copying. */
int scene_take(scene_record **record);
void scene_capture_log(void);

#ifdef __cplusplus
}
#endif

#endif
