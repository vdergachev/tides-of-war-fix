#ifndef TOW_WAKE_GEOMETRY_H
#define TOW_WAKE_GEOMETRY_H
#include "scene.h"
#include <math.h>

/* Measure the rear hull support from the hotspot, excluding padded pixels.
 * Call only for the A0/A1 interval; sails must not extend the emitter. */
static inline double wake_hull_stern(const scene_ship *ship, const scene_draw *draw, POINT camera, RECT view)
{
    double ux = sin(ship->course * 3.141592653589793 / 180);
    double uy = -cos(ship->course * 3.141592653589793 / 180);
    double anchor_x = ship->point.x - camera.x + view.left;
    double anchor_y = ship->point.y - camera.y + view.top;
    double stern = ship->wake_stern;
    for (int y = 0; y < draw->height; ++y) {
        for (int x = 0; x < draw->width; ++x) {
            if (draw->pixels[y * draw->width + x] == draw->key) {
                continue;
            }
            double back =
                -(draw->box.left + x + .5 - anchor_x) * ux - (draw->box.top + y + .5 - anchor_y) * uy;
            if (back > stern) {
                stern = back;
            }
        }
    }
    return stern;
}
#endif
