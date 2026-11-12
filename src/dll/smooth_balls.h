#ifndef TOW_SMOOTH_BALLS_H
#define TOW_SMOOTH_BALLS_H
#include "scene.h"
#ifdef __cplusplus
extern "C" {
#endif
void smooth_balls_configure(const char *path);
void smooth_balls_install(void);
void smooth_balls_begin(scene_record *record, BYTE *frame);
void smooth_balls_finish(void);
void smooth_balls_log(void);
#ifdef __cplusplus
}
#endif
#endif
