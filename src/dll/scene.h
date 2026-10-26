#ifndef TOW_SCENE_H
#define TOW_SCENE_H

#include <windows.h>
#include <stdint.h>

#define SCENE_WIDTH  640
#define SCENE_HEIGHT 480
#define SCENE_PIXELS (SCENE_WIDTH * SCENE_HEIGHT)
#define SCENE_SHIPS  8
#define SCENE_RING   4
#define SCENE_CLOUDS 5

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
} scene_ship;

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
    WORD pixels[SCENE_LAYERS][SCENE_PIXELS];
} scene_record;

int scene_init(SRWLOCK *lock);
void scene_present(HDC source, LONG epoch);
/* Worker owns *record; exchange returns the oldest complete record without copying. */
int scene_take(scene_record **record);
void scene_capture_log(void);

#endif
