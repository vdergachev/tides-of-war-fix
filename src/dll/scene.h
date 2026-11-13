#ifndef TOW_SCENE_H
#define TOW_SCENE_H

#include <windows.h>
#include <stdint.h>
#include "diag.h"
#include "ship_live.h"
#include "ball_geometry.h"
#include "radar_geometry.h"

#define SCENE_WIDTH  640
#define SCENE_HEIGHT 480
#define SCENE_PIXELS (SCENE_WIDTH * SCENE_HEIGHT)
/* Shared moving-object slots: ships plus Launch/Canoe boats. */
#define SCENE_SHIPS  16
#define SCENE_RING   4
#define SCENE_CLOUDS 5
/* Keep the existing transfer/pixel budget; overflow uses the plain frame. */
#define SCENE_DRAWS         32
#define SCENE_SPRITE_SIZE   144
#define SCENE_SPRITE_PIXELS (SCENE_SPRITE_SIZE * SCENE_SPRITE_SIZE)
#define SCENE_EFFECTS       128

enum {
    SCENE_A0,
    SCENE_A1,
    SCENE_A,
    SCENE_B,
    SCENE_C,
    SCENE_D,
    SCENE_E,
    SCENE_NO_BALLS,
    SCENE_NO_FIRE,
    SCENE_LAYERS
};

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
    unsigned type;
    double heading;
    int heading_valid;
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
    unsigned additive;
    RECT box;
    WORD pixels[SCENE_SPRITE_PIXELS];
} scene_draw;

/* Ordered MID transfers, including stationary smoke to preserve overlap. */
typedef struct {
    unsigned ship;
    unsigned id;
    uintptr_t source;
    unsigned frame;
    RECT source_rect;
    RECT relative;
    scene_draw draw;
} scene_effect;

typedef struct {
    LONG epoch;
    /* Plain capture serial published with this record (shown-step mapping). */
    LONG serial;
    unsigned step;
    unsigned count;
    unsigned follow;
    int night;
    unsigned layers;
    int valid;
    POINT camera;
    RECT view;
    LONGLONG qpc;
    unsigned ball_count;
    unsigned ball_births;
    unsigned ball_ends;
    unsigned ball_event_count;
    int balls_valid;
    POINT ball_offset;
    scene_ball balls[BALL_LIMIT];
    scene_ball_event ball_events[BALL_LIMIT * 2];
    scene_ship ships[SCENE_SHIPS];
    unsigned cloud_flags;
    scene_cloud clouds[SCENE_CLOUDS];
    unsigned draw_count;
    scene_draw draws[SCENE_DRAWS];
    int fire_valid;
    unsigned effect_count;
    scene_effect effects[SCENE_EFFECTS];
    radar_capture radar;
    hd_live_record *hd;
    BYTE ball_mid_mask[SCENE_PIXELS];
    WORD pixels[SCENE_LAYERS][SCENE_PIXELS];
} scene_record;

#ifdef __cplusplus
extern "C" {
#endif

diag_frame *scene_diagnostic(const scene_record *record);
WORD (*scene_drawing(const scene_record *record))[SCENE_PIXELS];
int scene_init(SRWLOCK *lock);
void scene_capture_additive(const hd_pass *pass, const WORD *pixels);
void scene_present(HDC source, LONG epoch, LONG serial);
/* Worker owns *record; exchange returns the oldest complete record without copying. */
int scene_take(scene_record **record);
void scene_capture_log(void);

#ifdef __cplusplus
}
#endif

#endif
